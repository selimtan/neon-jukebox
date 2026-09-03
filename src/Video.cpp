#include "neon/Video.hpp"

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
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace neon {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresultMessage(std::string_view operation, HRESULT result) {
    std::ostringstream message;
    message << operation << " failed (0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result) << ')';
    return message.str();
}

LRESULT CALLBACK videoWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* touchReleased = reinterpret_cast<std::atomic_bool*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
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
    SDL_Window* window{};
    HWND parent{};
    HWND surface{};
    bool comOwned{};
    bool initialized{};
    bool active{};
    float volume{0.8F};
    std::int64_t durationMs{};
    RECT lastDestination{-1, -1, -1, -1};
    std::atomic_bool surfaceTouchReleased{};

    ComPtr<IMFPMediaPlayer> player;
    ComPtr<IMFPMediaPlayerCallback> callback;
    std::shared_ptr<EventState> events;
    LoopbackAnalyzer loopback;

    void releasePlayer() {
        active = false;
        loopback.stop();
        if (player) {
            player->Stop();
            player->Shutdown();
        }
        player.Reset();
        callback.Reset();
        events.reset();
        durationMs = 0;
        lastDestination = {-1, -1, -1, -1};
        if (surface) ShowWindow(surface, SW_HIDE);
    }
};

VideoEngine::VideoEngine() : impl_(std::make_unique<Impl>()) {}
VideoEngine::~VideoEngine() { shutdown(); }

bool VideoEngine::initialize(SDL_Window* window, std::string& error) {
    if (impl_->initialized) return true;
    if (!window) { error = "Video window is unavailable"; return false; }
    impl_->window = window;
    const auto properties = SDL_GetWindowProperties(window);
    impl_->parent = static_cast<HWND>(SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!impl_->parent) { error = "Windows video handle is unavailable"; return false; }

    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(result)) impl_->comOwned = true;
    else if (result != RPC_E_CHANGED_MODE) {
        error = hresultMessage("COM initialization", result);
        return false;
    }
    if (!registerVideoWindowClass()) {
        error = "Video surface registration failed";
        if (impl_->comOwned) CoUninitialize();
        impl_->comOwned = false;
        return false;
    }
    impl_->surface = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
        L"NeonJukeboxVideoSurface", L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1, impl_->parent, nullptr, GetModuleHandleW(nullptr),
        &impl_->surfaceTouchReleased);
    if (!impl_->surface) {
        error = "Video surface creation failed";
        if (impl_->comOwned) CoUninitialize();
        impl_->comOwned = false;
        return false;
    }
    // WM_TOUCH does not honor an HTTRANSPARENT hit-test result. Register the
    // native video child explicitly and forward a completed contact to App so
    // touch and mouse taps have identical expand/collapse behavior.
    RegisterTouchWindow(impl_->surface, TWF_FINETOUCH);
    impl_->initialized = true;
    return true;
}

void VideoEngine::shutdown() {
    if (!impl_) return;
    impl_->releasePlayer();
    if (impl_->surface) {
        UnregisterTouchWindow(impl_->surface);
        DestroyWindow(impl_->surface);
    }
    impl_->surface = nullptr;
    if (impl_->comOwned) CoUninitialize();
    impl_->comOwned = false;
    impl_->initialized = false;
    impl_->window = nullptr;
    impl_->parent = nullptr;
}

bool VideoEngine::play(const Track& track, std::int64_t startMs, std::string& error) {
    if (!impl_->initialized || !impl_->surface) {
        error = "Video engine is unavailable";
        return false;
    }
    impl_->releasePlayer();
    impl_->events = std::make_shared<EventState>();
    auto* callback = new MediaPlayerCallback(impl_->events);
    impl_->callback.Attach(callback);

    HRESULT result = MFPCreateMediaPlayer(
        track.path.c_str(), FALSE, MFP_OPTION_FREE_THREADED_CALLBACK,
        impl_->callback.Get(), impl_->surface, &impl_->player);
    if (FAILED(result)) {
        error = hresultMessage("Video file open", result);
        impl_->releasePlayer();
        return false;
    }

    bool ready{};
    HRESULT asynchronousFailure{S_OK};
    {
        std::unique_lock lock(impl_->events->mutex);
        const bool signalled = impl_->events->changed.wait_for(lock, std::chrono::seconds(8), [&] {
            return impl_->events->mediaReady || impl_->events->failed;
        });
        ready = signalled && !impl_->events->failed;
        asynchronousFailure = impl_->events->failure;
    }
    if (!ready) {
        error = asynchronousFailure == S_OK ? "Video decoder timed out"
                                            : hresultMessage("Video decoder", asynchronousFailure);
        impl_->releasePlayer();
        return false;
    }

    impl_->player->SetBorderColor(RGB(0, 0, 0));
    impl_->player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
    impl_->player->SetVolume(impl_->volume);
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(impl_->player->GetDuration(MFP_POSITIONTYPE_100NS, &duration))) {
        impl_->durationMs = propVariantToMilliseconds(duration);
    }
    PropVariantClear(&duration);
    if (startMs > 0 && !seek(startMs)) {
        error = "Video position could not be restored";
        impl_->releasePlayer();
        return false;
    }
    result = impl_->player->Play();
    if (FAILED(result)) {
        error = hresultMessage("Video playback", result);
        impl_->releasePlayer();
        return false;
    }
    impl_->events->finished.store(false, std::memory_order_release);
    impl_->events->playing.store(true, std::memory_order_release);
    impl_->events->paused.store(false, std::memory_order_release);
    impl_->active = true;
    impl_->loopback.start();
    return true;
}

void VideoEngine::stop() { impl_->releasePlayer(); }

bool VideoEngine::pause() {
    if (!impl_->active || !impl_->player || paused()) return false;
    if (FAILED(impl_->player->Pause())) return false;
    impl_->events->playing.store(false, std::memory_order_release);
    impl_->events->paused.store(true, std::memory_order_release);
    return true;
}

bool VideoEngine::resume() {
    if (!impl_->active || !impl_->player || !paused()) return false;
    if (FAILED(impl_->player->Play())) return false;
    impl_->events->playing.store(true, std::memory_order_release);
    impl_->events->paused.store(false, std::memory_order_release);
    return true;
}

bool VideoEngine::seek(std::int64_t milliseconds) {
    if (!impl_->player) return false;
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = std::max<std::int64_t>(0, milliseconds) * 10'000;
    const HRESULT result = impl_->player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    PropVariantClear(&position);
    return SUCCEEDED(result);
}

void VideoEngine::setVolume(float volume) {
    impl_->volume = std::clamp(volume, 0.0F, 1.0F);
    if (impl_->player) impl_->player->SetVolume(impl_->volume);
}

void VideoEngine::render(const SDL_FRect& logicalDestination) {
    if (!impl_->active || !impl_->surface || !impl_->parent) return;
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
    } else if (!IsWindowVisible(impl_->surface)) {
        ShowWindow(impl_->surface, SW_SHOWNOACTIVATE);
    }
    if (impl_->player) impl_->player->UpdateVideo();
}

void VideoEngine::hide() {
    if (impl_->surface && IsWindowVisible(impl_->surface)) ShowWindow(impl_->surface, SW_HIDE);
}

bool VideoEngine::initialized() const { return impl_->initialized; }

bool VideoEngine::playing() const {
    return impl_->active && impl_->events && impl_->events->playing.load(std::memory_order_acquire);
}

bool VideoEngine::paused() const {
    return impl_->active && impl_->events && impl_->events->paused.load(std::memory_order_acquire);
}

bool VideoEngine::takeFinished() {
    return impl_->events && impl_->events->finished.exchange(false, std::memory_order_acq_rel);
}

bool VideoEngine::takeSurfaceTouch() {
    return impl_->surfaceTouchReleased.exchange(false, std::memory_order_acq_rel);
}

std::int64_t VideoEngine::positionMs() const {
    if (!impl_->player) return 0;
    PROPVARIANT position;
    PropVariantInit(&position);
    const auto result = impl_->player->GetPosition(MFP_POSITIONTYPE_100NS, &position);
    const auto milliseconds = SUCCEEDED(result) ? propVariantToMilliseconds(position) : 0;
    PropVariantClear(&position);
    return milliseconds;
}

std::int64_t VideoEngine::durationMs() const { return impl_->durationMs; }
float VideoEngine::volume() const { return impl_->volume; }
AudioVisualizationFrame VideoEngine::visualization() { return impl_->loopback.frame(); }

}  // namespace neon
