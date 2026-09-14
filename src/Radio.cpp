#include "neon/Radio.hpp"
#include "neon/RadioDirectory.hpp"

#include <SDL3/SDL.h>

#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <roapi.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace neon {
namespace {

using winrt::Windows::Foundation::Uri;
using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Media::Playback::MediaPlaybackSession;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// The media worker owns its apartment, including WinRT exception cleanup.
struct Apartment {
    Apartment() : result(RoInitialize(RO_INIT_MULTITHREADED)) {}
    ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
    HRESULT result{};
};

// Windows MediaPlayer does not expose decoded PCM samples. Capture the default
// render endpoint in loopback mode so radio uses the same visualizer data as
// local music and video playback.
class RadioLoopbackAnalyzer {
public:
    ~RadioLoopbackAnalyzer() { stop(); }

    void start() {
        stop();
        stopping_.store(false, std::memory_order_release);
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            // Visualization is optional; a capture-thread failure must not
            // interrupt radio playback.
            stopping_.store(true, std::memory_order_release);
        }
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

std::string currentError(const char* context) {
    try {
        throw;
    } catch (const winrt::hresult_error& exception) {
        return std::string(context) + ": " + winrt::to_string(exception.message()) +
            " (HRESULT " + std::to_string(static_cast<std::int32_t>(exception.code())) + ")";
    } catch (const std::exception& exception) {
        return std::string(context) + ": " + exception.what();
    } catch (...) {
        return std::string(context) + ": unknown error";
    }
}

bool validateUrl(const std::string& url, std::string& error) {
    // Validation does not create WinRT objects on the caller's thread. All
    // activation factories and media objects stay inside the worker apartment.
    if (RadioDirectory::validStreamUrl(url)) return true;
    error = "Radio stream URL must be an absolute HTTP or HTTPS URL with a host.";
    return false;
}

struct RadioState {
    std::mutex mutex;
    std::condition_variable wake;
    std::uint64_t generation{};
    std::uint64_t revision{};
    bool initialized{};
    bool starting{};
    bool stopping{};
    bool exited{true};
    bool active{};
    bool opened{};
    bool playing{};
    bool loading{};
    bool paused{};
    bool finished{};
    float volume{0.8F};
    std::string url;
    std::string error;
    std::string startupError;

    // Requires mutex. Public stop/switch/shutdown discard all pending signals.
    void clearPlayback() {
        ++generation;
        ++revision;
        active = opened = playing = loading = paused = finished = false;
        url.clear();
        error.clear();
    }
    bool accepts(std::uint64_t value) const {
        return !stopping && active && generation == value;
    }
};

void finish(const std::shared_ptr<RadioState>& state, std::uint64_t generation,
            std::string error = {}) {
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepts(generation)) return;
        state->active = state->opened = state->playing = state->loading = state->paused = false;
        state->finished = true;
        state->error = std::move(error);
        ++state->revision;
    }
    state->wake.notify_all();
}

// The worker alone owns projected objects and revokers. Events only acquire
// RadioState::mutex, and no WinRT call or event revocation holds that mutex.
struct Player {
    MediaPlayer media{nullptr};
    MediaSource source{nullptr};
    MediaPlaybackSession session{nullptr};
    MediaPlayer::MediaOpened_revoker opened;
    MediaPlayer::MediaEnded_revoker ended;
    MediaPlayer::MediaFailed_revoker failed;
    MediaPlaybackSession::PlaybackStateChanged_revoker changed;

    ~Player() { close(); }

    void close() noexcept {
        opened.revoke();
        ended.revoke();
        failed.revoke();
        changed.revoke();
        // Close can involve a driver or media pipeline. This always runs on the
        // worker; callers never join it without a deadline.
        if (media) {
            try { media.IsMuted(true); } catch (...) {}
            try { media.Pause(); } catch (...) {}
            try { media.Source(nullptr); } catch (...) {}
            try { media.Close(); } catch (...) {}
        }
        if (source) { try { source.Close(); } catch (...) {} }
        session = nullptr;
        source = nullptr;
        media = nullptr;
    }

    void create(float volume) {
        media = MediaPlayer();
        media.AutoPlay(false);
        media.IsLoopingEnabled(false);
        media.Volume(volume);
        media.IsMuted(volume == 0.0F);
        // Opt out before assigning a source. Unpackaged desktop apps may not
        // expose manual SMTC; automatic integration must still be disabled.
        media.CommandManager().IsEnabled(false);
        try { media.SystemMediaTransportControls().IsEnabled(false); }
        catch (const winrt::hresult_error&) {}
        session = media.PlaybackSession();
    }

    void open(const std::shared_ptr<RadioState>& state, std::uint64_t generation,
              const std::string& url) {
        const std::weak_ptr<RadioState> weak = state;
        opened = media.MediaOpened(winrt::auto_revoke, [weak, generation](auto const&, auto const&) noexcept {
            if (const auto shared = weak.lock()) {
                std::lock_guard lock(shared->mutex);
                if (!shared->accepts(generation)) return;
                shared->opened = true;
                shared->loading = false;
                ++shared->revision;
                shared->wake.notify_all();
            }
        });
        ended = media.MediaEnded(winrt::auto_revoke, [weak, generation](auto const&, auto const&) noexcept {
            if (const auto shared = weak.lock()) finish(shared, generation);
        });
        failed = media.MediaFailed(winrt::auto_revoke, [weak, generation](auto const&, auto const& args) noexcept {
            if (const auto shared = weak.lock()) {
                try {
                    auto message = winrt::to_string(args.ErrorMessage());
                    if (message.empty()) message = "The stream could not be opened or decoded.";
                    finish(shared, generation, "Radio playback failed: " + message + " (HRESULT " +
                        std::to_string(static_cast<std::int32_t>(args.ExtendedErrorCode())) + ")");
                } catch (...) {
                    finish(shared, generation, "Radio playback failed.");
                }
            }
        });
        changed = session.PlaybackStateChanged(winrt::auto_revoke,
            [weak, generation](auto const& sender, auto const&) noexcept {
                if (const auto shared = weak.lock()) {
                    try {
                        const auto value = sender.PlaybackState();
                        std::lock_guard lock(shared->mutex);
                        if (!shared->accepts(generation)) return;
                        shared->playing = value == MediaPlaybackState::Playing;
                        if (value == MediaPlaybackState::Opening || value == MediaPlaybackState::Buffering) {
                            shared->loading = true;
                        } else if (value == MediaPlaybackState::Playing || shared->opened) {
                            shared->loading = false;
                        }
                    } catch (...) {
                        // Closing a player can race an already dispatched event.
                    }
                }
            });
        source = MediaSource::CreateFromUri(Uri(winrt::to_hstring(url)));
        // There is no download job, file source, persistent cache, seek or
        // duration access. MediaPlayer resolves HTTP MP3/AAC/HLS asynchronously.
        media.Source(source);
    }
};

struct Request {
    std::uint64_t generation{};
    std::uint64_t revision{};
    bool stopping{};
    bool active{};
    bool opened{};
    bool paused{};
    float volume{};
    std::string url;
};

Request request(const std::shared_ptr<RadioState>& state) {
    std::lock_guard lock(state->mutex);
    return {state->generation, state->revision, state->stopping, state->active,
            state->opened, state->paused, state->volume, state->url};
}

void run(const std::shared_ptr<RadioState>& state) noexcept {
    {
        Apartment apartment;
        try {
            winrt::check_hresult(apartment.result);
            Player player;
            player.create(request(state).volume);
            {
                std::lock_guard lock(state->mutex);
                state->starting = false;
                if (!state->stopping) state->initialized = true;
            }
            state->wake.notify_all();
            std::uint64_t generation{};
            bool hasSource{};
            bool appliedPause{};
            bool appliedOpened{};
            bool controlsApplied{};
            float appliedVolume{-1.0F};
            Clock::time_point deadline{};
            for (;;) {
                auto next = request(state);
                if (next.stopping) break;
                if (next.generation != generation || (!next.active && hasSource)) {
                    // A separate player per generation makes even delayed WinRT
                    // events unambiguously belong to their original station.
                    player.close();
                    generation = next.generation;
                    hasSource = controlsApplied = false;
                    appliedVolume = -1.0F;
                }
                if (next.active) {
                    try {
                        if (!hasSource) {
                            if (!player.media) player.create(next.volume);
                            // AutoPlay remains false if cancellation arrives inside
                            // Source assignment; re-read the request before Play.
                            player.open(state, generation, next.url);
                            hasSource = true;
                            deadline = Clock::now() + 30s;
                            next = request(state);
                            if (next.stopping || !next.active || next.generation != generation) continue;
                        }
                        if (!next.opened && Clock::now() >= deadline) {
                            finish(state, generation, "Radio stream did not open within 30 seconds.");
                            continue;
                        }
                        if (appliedVolume != next.volume) {
                            // Mute before lowering to zero; set gain before unmuting.
                            if (next.volume == 0.0F) player.media.IsMuted(true);
                            player.media.Volume(next.volume);
                            player.media.IsMuted(next.volume == 0.0F);
                            appliedVolume = next.volume;
                        }
                        if (!controlsApplied || appliedPause != next.paused || appliedOpened != next.opened) {
                            if (next.paused) player.media.Pause();
                            else player.media.Play();
                            appliedPause = next.paused;
                            appliedOpened = next.opened;
                            controlsApplied = true;
                        }
                    } catch (...) {
                        finish(state, generation, currentError("Radio playback failed"));
                        // Also release a partially constructed player/source.
                        player.close();
                        hasSource = controlsApplied = false;
                        appliedVolume = -1.0F;
                    }
                }
                std::unique_lock lock(state->mutex);
                state->wake.wait_for(lock, 50ms, [&] {
                    return state->stopping || state->revision != next.revision;
                });
            }
        } catch (...) {
            const auto message = currentError("Could not initialize Windows radio playback");
            std::lock_guard lock(state->mutex);
            state->startupError = message;
            if (!state->stopping) {
                state->error = message;
                state->finished = state->active;
                state->active = state->playing = state->loading = state->paused = false;
            }
        }
    } // Tear down COM before advertising that the worker can be joined.
    {
        std::lock_guard lock(state->mutex);
        state->initialized = state->starting = false;
        state->exited = true;
    }
    state->wake.notify_all();
}

}  // namespace

struct RadioEngine::Impl {
    // Serializes public calls; the worker and callbacks never acquire this lock.
    mutable std::mutex mutex;
    std::shared_ptr<RadioState> state = std::make_shared<RadioState>();
    std::thread worker;
    RadioLoopbackAnalyzer loopback;

    void shutdown() {
        loopback.stop();
        {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
            state->initialized = false;
            state->clearPlayback();
        }
        state->wake.notify_all();
        if (!worker.joinable()) return;
        std::unique_lock lock(state->mutex);
        const bool exited = state->wake.wait_for(lock, 250ms, [&] { return state->exited; });
        lock.unlock();
        if (exited) worker.join();
    }

    ~Impl() {
        shutdown();
        // A hung system Close cannot retain the engine or dereference it later.
        // The worker owns its apartment and shared state until cleanup completes.
        if (worker.joinable()) worker.detach();
    }
};

RadioEngine::RadioEngine() : impl_(std::make_unique<Impl>()) {}
RadioEngine::~RadioEngine() = default;

bool RadioEngine::initialize(std::string& error) {
    std::lock_guard apiLock(impl_->mutex);
    error.clear();
    float level{};
    {
        std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->initialized) return true;
        if (!impl_->state->exited) {
            error = "Radio playback is still shutting down; retry initialization shortly.";
            return false;
        }
        level = impl_->state->volume;
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    auto state = std::make_shared<RadioState>();
    state->volume = level;
    state->starting = true;
    state->exited = false;
    impl_->state = state;
    try {
        impl_->worker = std::thread([state] { run(state); });
    } catch (...) {
        state->starting = false;
        state->exited = true;
        error = currentError("Could not start radio worker");
        return false;
    }
    std::unique_lock lock(state->mutex);
    const bool ready = state->wake.wait_for(lock, 3s, [&] { return !state->starting; });
    if (ready && state->initialized) return true;
    error = ready ? state->startupError : "Windows radio initialization timed out.";
    lock.unlock();
    impl_->shutdown();
    return false;
}

void RadioEngine::shutdown() {
    std::lock_guard lock(impl_->mutex);
    impl_->shutdown();
}

bool RadioEngine::play(const Track& track, std::string& error) {
    std::lock_guard apiLock(impl_->mutex);
    error.clear();
    if (track.mediaKind != MediaKind::Radio) {
        error = "Radio playback requires a radio track.";
        return false;
    }
    if (!validateUrl(track.streamUrl, error)) return false;
    const auto state = impl_->state;
    {
        std::lock_guard lock(state->mutex);
        if (!state->initialized || state->stopping) {
            error = "Radio playback is not initialized.";
            return false;
        }
        state->clearPlayback();
        state->url = track.streamUrl;
        state->active = state->loading = true;
    }
    impl_->loopback.start();
    state->wake.notify_all();
    return true;
}

void RadioEngine::stop() {
    std::lock_guard apiLock(impl_->mutex);
    {
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->clearPlayback();
    }
    impl_->state->wake.notify_all();
    impl_->loopback.stop();
}

bool RadioEngine::pause() {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    auto& state = *impl_->state;
    if (!state.active || state.paused) return false;
    state.paused = true;
    ++state.revision;
    state.wake.notify_all();
    return true;
}

bool RadioEngine::resume() {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    auto& state = *impl_->state;
    if (!state.active || !state.paused) return false;
    state.paused = false;
    state.playing = false; // Wait for WinRT's actual Playing notification.
    ++state.revision;
    state.wake.notify_all();
    return true;
}

void RadioEngine::setVolume(float volume) {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    impl_->state->volume = std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
    ++impl_->state->revision;
    impl_->state->wake.notify_all();
}

bool RadioEngine::initialized() const {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->initialized;
}

bool RadioEngine::playing() const {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->active && impl_->state->playing && !impl_->state->paused;
}

bool RadioEngine::loading() const {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->loading;
}

bool RadioEngine::paused() const {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->active && impl_->state->paused;
}

bool RadioEngine::takeFinished() {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return std::exchange(impl_->state->finished, false);
}

std::string RadioEngine::takeError() {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return std::exchange(impl_->state->error, {});
}

float RadioEngine::volume() const {
    std::lock_guard apiLock(impl_->mutex);
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->volume;
}

AudioVisualizationFrame RadioEngine::visualization() {
    std::lock_guard apiLock(impl_->mutex);
    {
        std::lock_guard lock(impl_->state->mutex);
        const auto& state = *impl_->state;
        if (!state.active || !state.playing || state.loading || state.paused) {
            return {};
        }
    }
    return impl_->loopback.frame();
}

}  // namespace neon
