#include "neon/Video.hpp"
#include "neon/BackgroundIo.hpp"
#include "neon/Utils.hpp"
#include "neon/VideoSourceCache.hpp"

#include <SDL3/SDL.h>

#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mfplay.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>

namespace neon {
namespace {

using Microsoft::WRL::ComPtr;

struct VideoCallTrace {
    const char* name;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    bool enabled{SDL_getenv("NEON_VIDEO_TRACE") != nullptr};
    explicit VideoCallTrace(const char* operation) : name(operation) {
        if (enabled) std::clog << "Video enter " << name << std::endl;
    }
    ~VideoCallTrace() {
        if (enabled) std::clog << "Video leave " << name << " " <<
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << " ms" << std::endl;
    }
};

// Some removable-drive drivers do not complete cancellation promptly. A copy
// owns its state and handles independently of the player/window; teardown never
// joins it. Bound outstanding copies so repeated Skip cannot create an I/O storm.
struct SourcePreparation {
    std::atomic_bool cancel{};
    std::atomic_bool finished{};
    std::atomic_int percent{-1};
    std::atomic<ULONGLONG> activity{GetTickCount64()};
    VideoSourceResult result;
};

std::shared_ptr<std::atomic_int> preparationCount(const Track& track) {
    // A stuck removable-drive request must not consume the network drive's slots.
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<std::atomic_int>> counts;
    std::lock_guard lock(mutex);
    const auto key = normalizeForSearch(pathToUtf8(track.path.root_path()));
    auto count = counts[key].lock();
    if (!count) { count = std::make_shared<std::atomic_int>(0); counts[key] = count; }
    return count;
}

bool sameSource(const Track& a, const Track& b) {
    return a.path == b.path && a.fileSize == b.fileSize && a.modifiedTicks == b.modifiedTicks;
}

std::shared_ptr<SourcePreparation> prepareSource(const Track& track,
    const std::filesystem::path& root, const VideoEngine::SourceLoader& loader) {
    const auto job = std::make_shared<SourcePreparation>();
    try {
        std::thread([job, track, root, loader] {
            const auto cancelled = [job] { return job->cancel.load(); };
            const auto progress = [job](int percent) {
                job->percent.store(percent);
                job->activity.store(GetTickCount64());
            };
            try {
                BackgroundIoCancellation cancelIo(cancelled);
                const auto cached = videoSourceCachePath(track, root);
                std::error_code error;
                const auto size = std::filesystem::file_size(cached, error);
                if (!error && track.fileSize > 0 && size == track.fileSize) {
                    std::filesystem::last_write_time(cached, std::filesystem::file_time_type::clock::now(), error);
                    job->result.path = cached;
                } else if (!cancelled() && videoSourceNeedsCache(track.path)) {
                    const auto count = preparationCount(track);
                    bool acquired = false;
                    const auto deadline = GetTickCount64() + 30'000;
                    while (!cancelled()) {
                        auto active = count->load();
                        if (active < 2 && count->compare_exchange_weak(active, active + 1)) {
                            acquired = true;
                            break;
                        }
                        if (GetTickCount64() >= deadline) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    if (acquired) {
                        struct Slot {
                            std::shared_ptr<std::atomic_int> count;
                            ~Slot() { --*count; }
                        } slot{count};
                        progress(0);
                        job->result = loader(track, root, cancelled, progress);
                    } else if (!cancelled()) {
                        job->result.error = "Video source is still responding to an earlier cancelled read";
                    }
                } else if (!cancelled()) job->result.path = track.path;
            } catch (const std::exception& error) { job->result.error = error.what(); }
            catch (...) { job->result.error = "Video preparation failed"; }
            job->finished.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::exception& error) {
        job->result.error = error.what();
        job->finished.store(true, std::memory_order_release);
    }
    return job;
}

std::string hresultMessage(std::string_view operation, HRESULT result) {
    std::ostringstream message;
    message << operation << " failed (0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result) << ')';
    return message.str();
}

struct SurfaceState {
    std::atomic_bool touchReleased{};
    std::atomic_bool repaint{};
};

LRESULT CALLBACK videoWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* state = reinterpret_cast<SurfaceState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    auto* touchReleased = state ? &state->touchReleased : nullptr;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        if (state) state->repaint.store(true, std::memory_order_release);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SIZE && state) state->repaint.store(true, std::memory_order_release);
    if (message == WM_POINTERUP && touchReleased) {
        POINTER_INPUT_TYPE pointerType{};
        if (GetPointerType(GET_POINTERID_WPARAM(wparam), &pointerType) &&
            (pointerType == PT_TOUCH || pointerType == PT_PEN)) {
            touchReleased->store(true, std::memory_order_release);
            return 0;
        }
    }
    if (message == WM_TOUCH) {
        const UINT count = LOWORD(wparam);
        std::vector<TOUCHINPUT> inputs(count);
        if (touchReleased && count > 0 &&
            GetTouchInputInfo(reinterpret_cast<HTOUCHINPUT>(lparam), count,
                              inputs.data(), sizeof(TOUCHINPUT))) {
            const bool released = std::ranges::any_of(inputs, [](const TOUCHINPUT& input) {
                return (input.dwFlags & TOUCHEVENTF_UP) != 0;
            });
            if (released) touchReleased->store(true, std::memory_order_release);
        }
        CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lparam));
        return 0;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(reinterpret_cast<HDC>(wparam), &bounds,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool registerVideoWindowClass() {
    static const bool registered = [] {
        WNDCLASSEXW descriptor{};
        descriptor.cbSize = sizeof(descriptor);
        descriptor.lpfnWndProc = videoWindowProc;
        descriptor.hInstance = GetModuleHandleW(nullptr);
        descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        descriptor.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        descriptor.lpszClassName = L"NeonJukeboxVideoSurface";
        if (RegisterClassExW(&descriptor)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

struct EventState {
    std::mutex mutex;
    std::condition_variable changed;
    bool mediaReady{};
    bool failed{};
    HRESULT failure{S_OK};
    ComPtr<IMFPMediaItem> createdItem;
    std::atomic_bool playing{};
    std::atomic_bool paused{};
    std::atomic_bool finished{};
};

class LoopbackAnalyzer {
public:
    ~LoopbackAnalyzer() { stop(); }

    void start() {
        stop();
        stopping_.store(false, std::memory_order_release);
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stopping_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    AudioVisualizationFrame frame() { return analyzer_.frame(); }

private:
    static bool isFloatFormat(const WAVEFORMATEX& format) {
        if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
        if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
            format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        return IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }

    static bool isPcmFormat(const WAVEFORMATEX& format) {
        if (format.wFormatTag == WAVE_FORMAT_PCM) return true;
        if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
            format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        return IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }

    void pushPacket(const WAVEFORMATEX& format, const BYTE* data, UINT32 frames,
                    DWORD flags, std::vector<float>& converted) {
        const int channels = std::max(1, static_cast<int>(format.nChannels));
        const std::size_t sampleCount = static_cast<std::size_t>(frames) *
                                        static_cast<std::size_t>(channels);
        if (sampleCount == 0) return;
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = channels;
        spec.freq = static_cast<int>(format.nSamplesPerSec);

        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
            converted.assign(sampleCount, 0.0F);
            analyzer_.push(spec, converted.data(), static_cast<int>(converted.size()));
            return;
        }

        const std::size_t bytesPerSample = format.nBlockAlign /
                                           static_cast<std::size_t>(channels);
        if (isFloatFormat(format) && bytesPerSample == sizeof(float)) {
            analyzer_.push(spec, reinterpret_cast<const float*>(data),
                           static_cast<int>(sampleCount));
            return;
        }

        converted.resize(sampleCount);
        if (isFloatFormat(format) && bytesPerSample == sizeof(double)) {
            const auto* source = reinterpret_cast<const double*>(data);
            for (std::size_t i = 0; i < sampleCount; ++i) {
                converted[i] = std::clamp(static_cast<float>(source[i]), -1.0F, 1.0F);
            }
        } else if (isPcmFormat(format) && bytesPerSample == 1) {
            for (std::size_t i = 0; i < sampleCount; ++i) {
                converted[i] = (static_cast<float>(data[i]) - 128.0F) / 128.0F;
            }
        } else if (isPcmFormat(format) && bytesPerSample == 2) {
            const auto* source = reinterpret_cast<const std::int16_t*>(data);
            for (std::size_t i = 0; i < sampleCount; ++i) {
                converted[i] = static_cast<float>(source[i]) / 32768.0F;
            }
        } else if (isPcmFormat(format) && bytesPerSample == 3) {
            for (std::size_t i = 0; i < sampleCount; ++i) {
                const auto* source = data + i * 3;
                std::int32_t value = static_cast<std::int32_t>(source[0]) |
                    (static_cast<std::int32_t>(source[1]) << 8) |
                    (static_cast<std::int32_t>(source[2]) << 16);
                if ((value & 0x00800000) != 0) value |= static_cast<std::int32_t>(0xFF000000);
                converted[i] = static_cast<float>(value) / 8388608.0F;
            }
        } else if (isPcmFormat(format) && bytesPerSample == 4) {
            const auto* source = reinterpret_cast<const std::int32_t*>(data);
            for (std::size_t i = 0; i < sampleCount; ++i) {
                converted[i] = static_cast<float>(static_cast<double>(source[i]) / 2147483648.0);
            }
        } else {
            return;
        }
        analyzer_.push(spec, converted.data(), static_cast<int>(converted.size()));
    }

    bool captureSession() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) return false;

        ComPtr<IMMDevice> device;
        result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (FAILED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result)) return false;

        ComPtr<IAudioClient> client;
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(result)) return false;

        WAVEFORMATEX* rawFormat{};
        result = client->GetMixFormat(&rawFormat);
        if (FAILED(result) || !rawFormat) return false;
        const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>
            format(rawFormat, &CoTaskMemFree);

        constexpr REFERENCE_TIME bufferDuration = 1'000'000;  // 100 ms.
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufferDuration, 0, format.get(), nullptr);
        if (FAILED(result)) return false;

        ComPtr<IAudioCaptureClient> capture;
        result = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(result) || FAILED(client->Start())) return false;

        std::vector<float> converted;
        bool healthy = true;
        while (!stopping_.load(std::memory_order_acquire)) {
            UINT32 packetFrames{};
            result = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(result)) { healthy = false; break; }
            while (packetFrames > 0 && !stopping_.load(std::memory_order_acquire)) {
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(result)) { healthy = false; break; }
                pushPacket(*format, data, frames, flags, converted);
                if (FAILED(capture->ReleaseBuffer(frames))) { healthy = false; break; }
                result = capture->GetNextPacketSize(&packetFrames);
                if (FAILED(result)) { healthy = false; break; }
            }
            if (!healthy) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        client->Stop();
        return healthy;
    }

    void run() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) return;
        const bool ownsCom = SUCCEEDED(comResult);
        while (!stopping_.load(std::memory_order_acquire)) {
            if (captureSession()) break;
            for (int wait = 0; wait < 50 && !stopping_.load(std::memory_order_acquire); ++wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (ownsCom) CoUninitialize();
    }

    SpectrumAnalyzer analyzer_;
    std::atomic_bool stopping_{true};
    std::thread worker_;
};

class MediaPlayerCallback final : public IMFPMediaPlayerCallback {
public:
    explicit MediaPlayerCallback(std::shared_ptr<EventState> state) : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID identifier, void** object) override {
        if (!object) return E_POINTER;
        if (identifier == __uuidof(IUnknown) || identifier == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        if (!event) return;
        if (FAILED(event->hrEvent) || event->eEventType == MFP_EVENT_TYPE_ERROR) {
            {
                std::lock_guard lock(state_->mutex);
                state_->failed = true;
                state_->failure = FAILED(event->hrEvent) ? event->hrEvent : E_FAIL;
            }
            state_->playing.store(false, std::memory_order_release);
            state_->finished.store(true, std::memory_order_release);
            state_->changed.notify_all();
            return;
        }
        switch (event->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
            {
                std::lock_guard lock(state_->mutex);
                state_->createdItem = MFP_GET_MEDIAITEM_CREATED_EVENT(event)->pMediaItem;
            }
            state_->changed.notify_all();
            break;
        case MFP_EVENT_TYPE_MEDIAITEM_SET:
            {
                std::lock_guard lock(state_->mutex);
                state_->mediaReady = true;
            }
            state_->changed.notify_all();
            break;
        case MFP_EVENT_TYPE_PLAY:
            state_->playing.store(true, std::memory_order_release);
            state_->paused.store(false, std::memory_order_release);
            break;
        case MFP_EVENT_TYPE_PAUSE:
            state_->playing.store(false, std::memory_order_release);
            state_->paused.store(true, std::memory_order_release);
            break;
        case MFP_EVENT_TYPE_STOP:
            state_->playing.store(false, std::memory_order_release);
            state_->paused.store(false, std::memory_order_release);
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            state_->playing.store(false, std::memory_order_release);
            state_->paused.store(false, std::memory_order_release);
            state_->finished.store(true, std::memory_order_release);
            break;
        default:
            break;
        }
    }

private:
    std::atomic<ULONG> references_{1};
    std::shared_ptr<EventState> state_;
};

std::int64_t propVariantToMilliseconds(const PROPVARIANT& value) {
    LONGLONG ticks{};
    return SUCCEEDED(PropVariantToInt64(value, &ticks)) ? ticks / 10'000 : 0;
}

}  // namespace

struct VideoEngine::Impl {
    struct Request {
        Track track;
        std::int64_t startMs{};
        std::int64_t durationMs{};
        std::shared_ptr<SourcePreparation> preparation;
    };
    struct Snapshot {
        bool active{};
        bool loading{};
        bool paused{};
        bool finished{};
        std::int64_t positionMs{};
        std::int64_t durationMs{};
        std::string error;
        int preparationPercent{-1};
    };

    HWND parent{};
    HWND surface{};
    HWND overlay{};
    std::uint64_t overlayRevision{};
    RECT overlayBounds{};
    bool initialized{};
    RECT lastDestination{-1, -1, -1, -1};
    SurfaceState surfaceState;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<const Request> request;
    Snapshot snapshot;
    float volume{0.8F};
    bool wantPause{};
    std::int64_t seekMs{};
    std::uint64_t seekRevision{};
    std::atomic<std::uint64_t> generation{};
    std::atomic_bool stopping{};
    std::atomic_bool exited{true};
    std::thread worker;
    std::filesystem::path cacheRoot;
    SourceLoader sourceLoader;
    Track preloadedTrack;
    std::shared_ptr<SourcePreparation> preloaded;

    // These COM objects belong exclusively to the media worker. No UI method
    // calls into a decoder, waits for media readiness or tears down a player.
    ComPtr<IMFPMediaPlayer> player;
    ComPtr<IMFPMediaPlayerCallback> callback;
    std::shared_ptr<EventState> events;
    LoopbackAnalyzer loopback;
    bool loopbackRunning{};

    bool cancelled(std::uint64_t id) const {
        return stopping.load(std::memory_order_acquire) ||
               generation.load(std::memory_order_acquire) != id;
    }

    void releasePlayer() {
        VideoCallTrace trace("Shutdown");
        if (player) player->Shutdown();
        player.Reset();
        callback.Reset();
        events.reset();
    }

    void fail(std::uint64_t id, std::string error) {
        std::lock_guard lock(mutex);
        if (cancelled(id)) return;
        snapshot.active = snapshot.loading = false;
        snapshot.finished = true;
        snapshot.error = std::move(error);
    }

    bool setPosition(std::int64_t milliseconds) {
        VideoCallTrace trace("SetPosition");
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = milliseconds * 10'000;
        return SUCCEEDED(player->SetPosition(MFP_POSITIONTYPE_100NS, &position));
    }

    bool open(const Request& item, std::uint64_t id) {
        const auto preparation = item.preparation ? item.preparation : prepareSource(item.track, cacheRoot, sourceLoader);
        int previous = -2;
        while (!preparation->finished.load(std::memory_order_acquire)) {
            if (cancelled(id)) { preparation->cancel.store(true); return false; }
            const auto percent = preparation->percent.load();
            if (percent != previous) {
                previous = percent;
                std::lock_guard lock(mutex);
                if (!cancelled(id)) snapshot.preparationPercent = percent;
            }
            if (GetTickCount64() - preparation->activity.load() > 30'000) {
                preparation->cancel.store(true);
                fail(id, "Video source stopped responding while preparing playback");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (cancelled(id)) return false;
        if (preparation->result.path.empty()) {
            fail(id, preparation->result.error.empty() ? "Video source preparation was cancelled" : preparation->result.error);
            return false;
        }
        const auto source = preparation->result.path;
        { std::lock_guard lock(mutex); if (!cancelled(id)) snapshot.preparationPercent = 100; }
        events = std::make_shared<EventState>();
        callback.Attach(new MediaPlayerCallback(events));
        HRESULT result;
        {
            VideoCallTrace trace("CreateMediaPlayer");
            result = MFPCreateMediaPlayer(nullptr, FALSE,
                MFP_OPTION_FREE_THREADED_CALLBACK, callback.Get(), surface, &player);
        }
        if (cancelled(id)) return false;
        if (FAILED(result)) { fail(id, hresultMessage("Video file open", result)); return false; }

        {
            VideoCallTrace trace("CreateMediaItemAsync");
            result = player->CreateMediaItemFromURL(source.c_str(), FALSE, 0, nullptr);
        }
        if (FAILED(result)) { fail(id, hresultMessage("Video file load", result)); return false; }

        // The decoder opens a complete local file. Time spent preparing a slow
        // USB/network source is separate from this decoder deadline.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        ComPtr<IMFPMediaItem> created;
        {
            std::unique_lock lock(events->mutex);
            while (!events->createdItem && !events->failed && !cancelled(id) &&
                   std::chrono::steady_clock::now() < deadline)
                events->changed.wait_for(lock, std::chrono::milliseconds(20));
            if (cancelled(id)) return false;
            if (!events->createdItem || events->failed) {
                const auto failure = events->failure;
                lock.unlock();
                fail(id, failure == S_OK ? "Video file load timed out" : hresultMessage("Video file load", failure));
                return false;
            }
            created.Swap(events->createdItem);
        }
        {
            VideoCallTrace trace("SetMediaItem");
            result = player->SetMediaItem(created.Get());
        }
        if (FAILED(result)) { fail(id, hresultMessage("Video media selection", result)); return false; }
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        {
            std::unique_lock lock(events->mutex);
            while (!events->mediaReady && !events->failed && !cancelled(id) &&
                   std::chrono::steady_clock::now() < deadline)
                events->changed.wait_for(lock, std::chrono::milliseconds(20));
            if (cancelled(id)) return false;
            if (!events->mediaReady || events->failed) {
                const auto failure = events->failure;
                lock.unlock();
                fail(id, failure == S_OK ? "Video decoder timed out" : hresultMessage("Video decoder", failure));
                return false;
            }
        }

        player->SetBorderColor(RGB(0, 0, 0));
        player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
        float level;
        { std::lock_guard lock(mutex); level = volume; }
        player->SetVolume(level);
        PROPVARIANT duration{};
        auto durationMs = item.durationMs;
        if (SUCCEEDED(player->GetDuration(MFP_POSITIONTYPE_100NS, &duration)))
            durationMs = propVariantToMilliseconds(duration);
        PropVariantClear(&duration);
        if (cancelled(id)) return false;
        if (item.startMs > 0 && !setPosition(item.startMs)) {
            fail(id, "Video position could not be restored");
            return false;
        }
        if (cancelled(id)) return false;
        { VideoCallTrace trace("Play"); result = player->Play(); }
        if (FAILED(result)) { fail(id, hresultMessage("Video playback", result)); return false; }
        if (cancelled(id)) return false;
        if (!loopbackRunning) { loopback.start(); loopbackRunning = true; }
        {
            std::lock_guard lock(mutex);
            if (cancelled(id)) return false;
            snapshot.loading = false;
            snapshot.durationMs = durationMs;
        }
        surfaceState.repaint.store(true, std::memory_order_release);
        return true;
    }

    void run() {
        const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::uint64_t currentGeneration{};
        bool actualPaused{};
        float actualVolume{-1};
        std::uint64_t appliedSeek{};
        auto lastPosition = std::chrono::steady_clock::now();
        try {
            while (!stopping.load(std::memory_order_acquire)) {
                std::shared_ptr<const Request> desired;
                std::uint64_t id, seekVersion;
                std::int64_t position;
                bool pause;
                float level;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(20));
                    desired = request;
                    id = generation.load(std::memory_order_acquire);
                    pause = wantPause;
                    level = volume;
                    position = seekMs;
                    seekVersion = seekRevision;
                }
                if (stopping.load(std::memory_order_acquire)) break;
                if (id != currentGeneration) {
                    currentGeneration = id;
                    releasePlayer();
                    if (cancelled(id)) continue;
                    if (!desired) {
                        if (loopbackRunning) { loopback.stop(); loopbackRunning = false; }
                        continue;
                    }
                    if (FAILED(comResult)) {
                        fail(id, hresultMessage("Video COM initialization", comResult));
                        continue;
                    }
                    actualPaused = false;
                    actualVolume = -1;
                    appliedSeek = 0;
                    if (!open(*desired, id)) { releasePlayer(); continue; }
                    // Read commands again: pause/seek/skip may have changed while opening.
                    continue;
                }
                if (!player || cancelled(id)) continue;

                bool failed;
                HRESULT failure;
                {
                    std::lock_guard lock(events->mutex);
                    failed = events->failed;
                    failure = events->failure;
                }
                if (failed) {
                    fail(id, hresultMessage("Video playback", failure));
                    releasePlayer();
                    continue;
                }
                if (events->finished.exchange(false, std::memory_order_acq_rel)) {
                    std::lock_guard lock(mutex);
                    if (!cancelled(id)) {
                        snapshot.active = snapshot.loading = false;
                        snapshot.finished = true;
                    }
                }

                if (actualVolume != level) { player->SetVolume(level); actualVolume = level; }
                if (appliedSeek != seekVersion) {
                    if (!setPosition(position)) fail(id, "Video seek failed");
                    appliedSeek = seekVersion;
                    surfaceState.repaint.store(true, std::memory_order_release);
                }
                if (actualPaused != pause) {
                    const auto result = pause ? player->Pause() : player->Play();
                    if (FAILED(result)) fail(id, hresultMessage("Video pause/resume", result));
                    actualPaused = pause;
                }
                if (cancelled(id)) continue;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastPosition >= std::chrono::milliseconds(50)) {
                    PROPVARIANT current{};
                    VideoCallTrace trace("GetPosition");
                    const auto result = player->GetPosition(MFP_POSITIONTYPE_100NS, &current);
                    const auto milliseconds = propVariantToMilliseconds(current);
                    PropVariantClear(&current);
                    if (SUCCEEDED(result)) {
                        std::lock_guard lock(mutex);
                        if (!cancelled(id) && seekRevision == appliedSeek)
                            snapshot.positionMs = milliseconds;
                    }
                    lastPosition = now;
                }
                // MF draws moving frames itself. Repaint only on exposure/resize,
                // not once for every SDL frame.
                if (!cancelled(id) && surfaceState.repaint.exchange(false, std::memory_order_acq_rel)) {
                    VideoCallTrace trace("UpdateVideo");
                    player->UpdateVideo();
                }
            }
        } catch (const std::exception& error) {
            fail(generation.load(std::memory_order_acquire), error.what());
        } catch (...) {
            fail(generation.load(std::memory_order_acquire), "Unexpected video worker failure");
        }
        releasePlayer();
        loopback.stop();
        loopbackRunning = false;
        if (SUCCEEDED(comResult)) CoUninitialize();
        exited.store(true, std::memory_order_release);
    }
};

VideoEngine::VideoEngine(SourceLoader sourceLoader) : impl_(std::make_unique<Impl>()) {
    impl_->sourceLoader = sourceLoader ? std::move(sourceLoader) : SourceLoader{
        [](const Track& track, const std::filesystem::path& root,
           const std::function<bool()>& cancelled, const std::function<void(int)>& progress) {
            return cacheVideoSource(track, root, cancelled, progress);
        }};
}
VideoEngine::~VideoEngine() { shutdown(); }

bool VideoEngine::initialize(SDL_Window* window, std::string& error) {
    if (impl_->initialized) return true;
    if (!window) { error = "Video window is unavailable"; return false; }
    impl_->parent = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!impl_->parent) { error = "Windows video handle is unavailable"; return false; }
    if (!registerVideoWindowClass()) { error = "Video surface registration failed"; return false; }
    impl_->surface = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
        L"NeonJukeboxVideoSurface", L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1, impl_->parent, nullptr, GetModuleHandleW(nullptr), &impl_->surfaceState);
    if (!impl_->surface) { error = "Video surface creation failed"; return false; }
    RegisterTouchWindow(impl_->surface, TWF_FINETOUCH);
    impl_->stopping.store(false, std::memory_order_release);
    impl_->exited.store(false, std::memory_order_release);
    impl_->cacheRoot = pathFromUtf8(SDL_GetBasePath()) / L"library" / L"video-media";
    try { impl_->worker = std::thread([this] { impl_->run(); }); }
    catch (const std::exception& exception) {
        error = exception.what();
        impl_->exited.store(true, std::memory_order_release);
        DestroyWindow(impl_->surface);
        impl_->surface = nullptr;
        return false;
    }
    impl_->initialized = true;
    return true;
}

void VideoEngine::requestShutdown() {
    if (!impl_) return;
    stop();
    cancelPreload();
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake.notify_all();
}

void VideoEngine::shutdown() {
    if (!impl_) return;
    requestShutdown();
    if (impl_->worker.joinable()) {
        // A renderer may send window messages while releasing its native sink.
        // Keep the owning window thread pumping until media teardown completes.
        while (!impl_->exited.load(std::memory_order_acquire)) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        impl_->worker.join();
    }
    if (impl_->surface) {
        UnregisterTouchWindow(impl_->surface);
        DestroyWindow(impl_->surface);
    }
    if (impl_->overlay) DestroyWindow(impl_->overlay);
    impl_->overlay = nullptr;
    impl_->overlayRevision = 0;
    impl_->surface = nullptr;
    impl_->parent = nullptr;
    impl_->initialized = false;
}

bool VideoEngine::play(const Track& track, std::int64_t startMs, std::string& error) {
    if (!impl_->initialized || impl_->exited.load(std::memory_order_acquire)) {
        error = "Video engine is unavailable";
        return false;
    }
    error.clear();
    auto request = std::make_shared<Impl::Request>();
    request->track = track;
    request->startMs = std::max<std::int64_t>(0, startMs);
    request->durationMs = track.durationMs;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->preloaded && sameSource(track, impl_->preloadedTrack)) {
            request->preparation = std::move(impl_->preloaded);
        } else if (impl_->preloaded) {
            impl_->preloaded->cancel.store(true);
            impl_->preloaded.reset();
        }
        if (impl_->request && impl_->request->preparation)
            impl_->request->preparation->cancel.store(true);
        impl_->request = std::move(request);
        impl_->wantPause = false;
        impl_->seekRevision = 0;
        impl_->snapshot = {true, true, false, false, std::max<std::int64_t>(0, startMs), track.durationMs, {}};
        ++impl_->generation;
    }
    hide();
    impl_->lastDestination = {-1, -1, -1, -1};
    impl_->wake.notify_all();
    return true;
}

void VideoEngine::preload(const Track& track) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->initialized || impl_->stopping.load()) return;
    if (impl_->preloaded && sameSource(track, impl_->preloadedTrack)) return;
    if (impl_->preloaded) impl_->preloaded->cancel.store(true);
    impl_->preloadedTrack = track;
    impl_->preloaded = prepareSource(track, impl_->cacheRoot, impl_->sourceLoader);
}

void VideoEngine::cancelPreload() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->preloaded) impl_->preloaded->cancel.store(true);
    impl_->preloaded.reset();
}

VideoEngine::PreloadState VideoEngine::preloadState() const {
    std::lock_guard lock(impl_->mutex);
    const auto& job = impl_->preloaded;
    if (!job) return PreloadState::None;
    if (job->finished.load(std::memory_order_acquire))
        return job->result.path.empty() ? PreloadState::Failed : PreloadState::Ready;
    if (GetTickCount64() - job->activity.load() > 30'000) {
        job->cancel.store(true);
        return PreloadState::Failed;
    }
    return PreloadState::Preparing;
}

std::string VideoEngine::preloadError() const {
    std::lock_guard lock(impl_->mutex);
    const auto& job = impl_->preloaded;
    if (!job) return {};
    if (job->finished.load(std::memory_order_acquire)) return job->result.error;
    if (GetTickCount64() - job->activity.load() > 30'000)
        return "Video source stopped responding while preparing the next clip";
    return {};
}

void VideoEngine::stop() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->request || impl_->snapshot.active) {
            if (impl_->request && impl_->request->preparation)
                impl_->request->preparation->cancel.store(true);
            impl_->request.reset();
            impl_->snapshot = {};
            ++impl_->generation;
        }
    }
    hide();
    impl_->wake.notify_all();
}

bool VideoEngine::pause() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->snapshot.active || impl_->wantPause) return false;
    impl_->wantPause = impl_->snapshot.paused = true;
    impl_->wake.notify_all();
    return true;
}

bool VideoEngine::resume() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->snapshot.active || !impl_->wantPause) return false;
    impl_->wantPause = impl_->snapshot.paused = false;
    impl_->wake.notify_all();
    return true;
}

bool VideoEngine::seek(std::int64_t milliseconds) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->snapshot.active) return false;
    impl_->seekMs = std::max<std::int64_t>(0, milliseconds);
    impl_->snapshot.positionMs = impl_->seekMs;
    ++impl_->seekRevision;
    impl_->wake.notify_all();
    return true;
}

void VideoEngine::setVolume(float volume) {
    std::lock_guard lock(impl_->mutex);
    impl_->volume = std::clamp(volume, 0.0F, 1.0F);
    impl_->wake.notify_all();
}

void VideoEngine::render(const SDL_FRect& logicalDestination) {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->snapshot.active || impl_->snapshot.loading) return;
    }
    if (!impl_->surface || !impl_->parent) return;
    RECT client{};
    if (!GetClientRect(impl_->parent, &client)) return;
    const float clientWidth = static_cast<float>(client.right - client.left);
    const float clientHeight = static_cast<float>(client.bottom - client.top);
    const float scale = std::min(clientWidth / 1920.0F, clientHeight / 1080.0F);
    const float offsetX = (clientWidth - 1920.0F * scale) * 0.5F;
    const float offsetY = (clientHeight - 1080.0F * scale) * 0.5F;
    RECT destination{
        static_cast<LONG>(std::lround(offsetX + logicalDestination.x * scale)),
        static_cast<LONG>(std::lround(offsetY + logicalDestination.y * scale)),
        static_cast<LONG>(std::lround(offsetX + (logicalDestination.x + logicalDestination.w) * scale)),
        static_cast<LONG>(std::lround(offsetY + (logicalDestination.y + logicalDestination.h) * scale))};
    if (!EqualRect(&destination, &impl_->lastDestination)) {
        SetWindowPos(impl_->surface, HWND_TOP, destination.left, destination.top,
                     destination.right - destination.left, destination.bottom - destination.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        impl_->lastDestination = destination;
        impl_->surfaceState.repaint.store(true, std::memory_order_release);
    } else if (!IsWindowVisible(impl_->surface)) {
        ShowWindow(impl_->surface, SW_SHOWNOACTIVATE);
    }
}

void VideoEngine::renderOverlay(SDL_Surface* image, std::uint64_t revision) {
    if (!image || !impl_->parent || !impl_->surface || !IsWindowVisible(impl_->surface) ||
        IsIconic(impl_->parent) || GetForegroundWindow() != impl_->parent) {
        hideOverlay();
        return;
    }
    RECT client{};
    if (!GetClientRect(impl_->parent, &client)) { hideOverlay(); return; }
    const float scale = std::min(static_cast<float>(client.right) / 1920.0F,
                                 static_cast<float>(client.bottom) / 1080.0F);
    POINT origin{
        static_cast<LONG>(std::lround((client.right - 1920.0F * scale) / 2 + 144 * scale)),
        static_cast<LONG>(std::lround((client.bottom + 1080.0F * scale) / 2 - 72 * scale - image->h))};
    ClientToScreen(impl_->parent, &origin);
    const RECT bounds{origin.x, origin.y, origin.x + image->w, origin.y + image->h};
    if (!impl_->overlay) {
        // An owned, non-activating window stays above the native video child.
        // Per-pixel alpha paints only glyphs; clicks pass through to the video.
        impl_->overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"STATIC", L"Neon Jukebox Video Info", WS_POPUP, origin.x, origin.y, image->w, image->h,
            impl_->parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        impl_->overlayRevision = 0;
    }
    if (!impl_->overlay) return;
    if (impl_->overlayRevision != revision || !EqualRect(&bounds, &impl_->overlayBounds)) {
        SDL_Surface* pixels = SDL_ConvertSurface(image, SDL_PIXELFORMAT_BGRA32);
        if (!pixels) { hideOverlay(); return; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = pixels->w;
        info.bmiHeader.biHeight = -pixels->h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits{};
        const HDC memory = CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool uploaded = false;
        if (memory && bitmap && bits) {
            for (int y = 0; y < pixels->h; ++y)
                std::memcpy(static_cast<std::uint8_t*>(bits) + static_cast<std::size_t>(y) * pixels->w * 4,
                    static_cast<std::uint8_t*>(pixels->pixels) + static_cast<std::size_t>(y) * pixels->pitch,
                    static_cast<std::size_t>(pixels->w) * 4);
            const auto previous = SelectObject(memory, bitmap);
            SIZE size{pixels->w, pixels->h};
            POINT source{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            uploaded = UpdateLayeredWindow(impl_->overlay, nullptr, &origin, &size, memory, &source,
                0, &blend, ULW_ALPHA) != FALSE;
            SelectObject(memory, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        SDL_DestroySurface(pixels);
        if (!uploaded) { hideOverlay(); return; }
        impl_->overlayRevision = revision;
        impl_->overlayBounds = bounds;
    }
    if (!IsWindowVisible(impl_->overlay)) ShowWindow(impl_->overlay, SW_SHOWNOACTIVATE);
}

void VideoEngine::hideOverlay() {
    if (impl_->overlay && IsWindowVisible(impl_->overlay)) ShowWindow(impl_->overlay, SW_HIDE);
}

void VideoEngine::hide() {
    hideOverlay();
    if (impl_->surface && IsWindowVisible(impl_->surface)) ShowWindow(impl_->surface, SW_HIDE);
}

bool VideoEngine::initialized() const { return impl_->initialized; }
bool VideoEngine::playing() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot.active && !impl_->snapshot.paused;
}
bool VideoEngine::loading() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot.loading;
}

std::string VideoEngine::loadingStatus() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->snapshot.loading) return {};
    const auto percent = impl_->snapshot.preparationPercent;
    return percent >= 0 && percent < 100 ? "PREPARING VIDEO  " + std::to_string(percent) + "%" : "OPENING VIDEO...";
}
bool VideoEngine::paused() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot.active && impl_->snapshot.paused;
}
bool VideoEngine::takeFinished() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->snapshot.finished, false);
}
std::string VideoEngine::takeError() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->snapshot.error, {});
}
bool VideoEngine::takeSurfaceTouch() {
    return impl_->surfaceState.touchReleased.exchange(false, std::memory_order_acq_rel);
}
std::int64_t VideoEngine::positionMs() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot.positionMs;
}
std::int64_t VideoEngine::durationMs() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot.durationMs;
}
float VideoEngine::volume() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->volume;
}
AudioVisualizationFrame VideoEngine::visualization() {
    return playing() && !loading() ? impl_->loopback.frame() : AudioVisualizationFrame{};
}

}  // namespace neon
