#include "neon/MediaSession.hpp"

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <optional>
#include <thread>

namespace neon {
namespace {

using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using PlaybackStatus =
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using Microsoft::WRL::ComPtr;

struct SessionArtworkCache {
    std::wstring mediaKey;
    std::vector<std::uint8_t> bytes;
    std::string mimeType;
    std::chrono::steady_clock::time_point checkedAt{};
};

std::wstring asWide(const winrt::hstring& value) {
    return {value.c_str(), value.size()};
}

std::wstring normalizedSourceId(const Session& session) {
    std::wstring source = asWide(session.SourceAppUserModelId());
    std::ranges::transform(source, source.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return source;
}

std::optional<MediaSource> sessionSource(const Session& session) {
    const auto source = normalizedSourceId(session);
    if (source.find(L"spotify") != std::wstring::npos) return MediaSource::Spotify;
    if (source.find(L"chrome") != std::wstring::npos) return MediaSource::Chrome;
    return std::nullopt;
}

bool matchesSource(const Session& session, MediaSource requested) {
    const auto source = sessionSource(session);
    // YouTube publishes its playback and metadata through Chrome's session.
    // GSMTC supplies the app ID, not the page URL, so do not guess the site
    // from titles or artists (which can contain arbitrary user content).
    if (requested == MediaSource::YouTube) return source == MediaSource::Chrome;
    return source && (requested == MediaSource::Automatic || *source == requested);
}

bool isPlaying(const Session& session) {
    return session.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
}

Session selectSession(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager& manager,
    MediaSource requested) {
    Session selected{nullptr};
    // In automatic mode the current Windows media session wins when it is
    // playing. For an explicit source, the first playing matching session wins.
    const auto current = manager.GetCurrentSession();
    if (current && matchesSource(current, requested) && isPlaying(current)) selected = current;
    if (!selected) {
        for (const auto& session : manager.GetSessions()) {
            if (matchesSource(session, requested) && isPlaying(session)) {
                selected = session;
                break;
            }
        }
    }
    // Keep a paused session selected so Playing -> Paused can close a capture
    // and its last published metadata remains visible.
    if (!selected && current && matchesSource(current, requested)) selected = current;
    if (!selected) {
        for (const auto& session : manager.GetSessions()) {
            if (matchesSource(session, requested)) {
                selected = session;
                break;
            }
        }
    }
    return selected;
}

void readArtwork(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties&
        properties,
    SessionArtworkCache& cache) {
    constexpr std::uint64_t maximumArtworkBytes = 8ULL * 1024 * 1024;
    cache.bytes.clear();
    cache.mimeType.clear();
    const auto thumbnail = properties.Thumbnail();
    if (!thumbnail) return;
    const auto stream = thumbnail.OpenReadAsync().get();
    if (!stream || stream.Size() == 0 || stream.Size() > maximumArtworkBytes) return;

    const auto size = static_cast<std::uint32_t>(stream.Size());
    winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
    const auto loaded = reader.LoadAsync(size).get();
    if (loaded == 0) return;
    cache.bytes.resize(loaded);
    reader.ReadBytes(winrt::array_view<std::uint8_t>(cache.bytes));
    cache.mimeType = winrt::to_string(stream.ContentType());
}

bool hasSpotifyBrandingLayout(const std::vector<std::uint8_t>& pixels,
                              std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t stride,
                              std::uint32_t artworkSide) {
    if (width != height || width < 128 || artworkSide >= height) return false;
    const std::uint32_t margin = (width - artworkSide) / 2;
    if (margin == 0) return false;

    struct PixelCounts {
        std::uint64_t black{};
        std::uint64_t light{};
        std::uint64_t total{};
    };
    const auto count = [&](std::uint32_t x0, std::uint32_t y0,
                           std::uint32_t x1, std::uint32_t y1) {
        PixelCounts counts;
        const std::uint32_t step = std::max<std::uint32_t>(1, width / 150);
        for (std::uint32_t y = y0; y < y1; y += step) {
            for (std::uint32_t x = x0; x < x1; x += step) {
                const auto offset = static_cast<std::size_t>(y) * stride + x * 4;
                const auto blue = pixels[offset];
                const auto green = pixels[offset + 1];
                const auto red = pixels[offset + 2];
                ++counts.total;
                if (red < 48 && green < 48 && blue < 48) ++counts.black;
                if (red > 180 && green > 180 && blue > 180) ++counts.light;
            }
        }
        return counts;
    };

    const auto left = count(0, 0, margin, artworkSide);
    const auto right = count(margin + artworkSide, 0, width, artworkSide);
    const auto bottom = count(0, artworkSide, width, height);
    const auto darkRatio = [](const PixelCounts& value) {
        return value.total == 0 ? 0.0 : static_cast<double>(value.black) / value.total;
    };
    const auto lightRatio = [](const PixelCounts& value) {
        return value.total == 0 ? 0.0 : static_cast<double>(value.light) / value.total;
    };
    return darkRatio(left) > 0.85 && darkRatio(right) > 0.85 &&
           darkRatio(bottom) > 0.55 && lightRatio(bottom) > 0.07;
}

bool cropSpotifyBranding(std::vector<std::uint8_t>& artwork, std::string& mimeType) {
    if (artwork.empty()) return false;
    ComPtr<IStream> input;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &input))) return false;
    ULONG written{};
    if (FAILED(input->Write(artwork.data(), static_cast<ULONG>(artwork.size()), &written)) ||
        written != artwork.size()) return false;
    LARGE_INTEGER zero{};
    if (FAILED(input->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(input.Get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT width{};
    UINT height{};
    if (FAILED(frame->GetSize(&width, &height)) || width != height || width < 128) return false;
    const UINT artworkSide = width * 78 / 100;
    const UINT margin = (width - artworkSide) / 2;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) return false;
    const UINT stride = width * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                     pixels.data())) ||
        !hasSpotifyBrandingLayout(pixels, width, height, stride, artworkSide)) return false;

    WICRect crop{static_cast<INT>(margin), 0,
                 static_cast<INT>(artworkSide), static_cast<INT>(artworkSide)};
    ComPtr<IWICBitmapClipper> clipper;
    if (FAILED(factory->CreateBitmapClipper(&clipper)) ||
        FAILED(clipper->Initialize(frame.Get(), &crop))) return false;

    ComPtr<IStream> output;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &output))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> outputFrame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(&outputFrame, &properties)) ||
        FAILED(outputFrame->Initialize(properties.Get())) ||
        FAILED(outputFrame->SetSize(artworkSide, artworkSide))) return false;
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(outputFrame->SetPixelFormat(&pixelFormat)) ||
        FAILED(outputFrame->WriteSource(clipper.Get(), nullptr)) ||
        FAILED(outputFrame->Commit()) || FAILED(encoder->Commit())) return false;

    STATSTG statistics{};
    if (FAILED(output->Stat(&statistics, STATFLAG_NONAME)) || statistics.cbSize.QuadPart == 0 ||
        statistics.cbSize.QuadPart > 8ULL * 1024 * 1024 ||
        FAILED(output->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(statistics.cbSize.QuadPart));
    ULONG read{};
    if (FAILED(output->Read(cropped.data(), static_cast<ULONG>(cropped.size()), &read)) ||
        read != cropped.size()) return false;
    artwork = std::move(cropped);
    mimeType = "image/png";
    return true;
}

MediaSessionInfo readSession(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager& manager,
    MediaSource requested,
    SessionArtworkCache& artworkCache) {
    MediaSessionInfo result;
    result.apiAvailable = true;
    result.requestedSource = requested;
    result.source = requested;
    const Session selected = selectSession(manager, requested);
    if (!selected) return result;

    result.sessionAvailable = true;
    result.sourceAppId = asWide(selected.SourceAppUserModelId());
    if (requested != MediaSource::YouTube) {
        if (const auto source = sessionSource(selected)) result.source = *source;
    }
    const auto playback = selected.GetPlaybackInfo();
    result.playing = playback.PlaybackStatus() == PlaybackStatus::Playing;
    result.nextEnabled = playback.Controls().IsNextEnabled();
    try {
        const auto timeline = selected.GetTimelineProperties();
        result.timelineAvailable = true;
        result.positionTicks = timeline.Position().count();
        result.endTicks = timeline.EndTime().count();
        const auto startTicks = timeline.StartTime().count();
        result.durationTicks = std::max<std::int64_t>(0, result.endTicks - startTicks);
    } catch (const winrt::hresult_error&) {
        // Some sites publish metadata without a playback timeline. Title and
        // artist changes still provide the primary track boundary signal.
    }
    try {
        const auto properties = selected.TryGetMediaPropertiesAsync().get();
        if (properties) {
            result.title = asWide(properties.Title());
            result.artist = asWide(properties.Artist());
            if (result.artist.empty()) result.artist = asWide(properties.AlbumArtist());
            result.album = asWide(properties.AlbumTitle());

            const std::wstring artworkKey = result.sourceAppId + L"\n" + result.title + L"\n" +
                                            result.artist + L"\n" + result.album;
            const auto now = std::chrono::steady_clock::now();
            const bool retryMissingArtwork = artworkCache.bytes.empty() &&
                (artworkCache.checkedAt.time_since_epoch().count() == 0 ||
                 now - artworkCache.checkedAt >= std::chrono::seconds(2));
            if (artworkKey != artworkCache.mediaKey || retryMissingArtwork) {
                artworkCache.mediaKey = artworkKey;
                artworkCache.checkedAt = now;
                readArtwork(properties, artworkCache);
                if (result.source == MediaSource::Spotify) {
                    cropSpotifyBranding(artworkCache.bytes, artworkCache.mimeType);
                }
            }
            result.artwork = artworkCache.bytes;
            result.artworkMimeType = artworkCache.mimeType;
        }
    } catch (const winrt::hresult_error&) {
        // Playback transitions can invalidate the old media item between the
        // playback and metadata calls. The next 100 ms poll will retry it.
    }
    return result;
}

}  // namespace

std::wstring mediaTrackIdentity(const MediaSessionInfo& media) {
    if (media.title.empty()) return {};
    return media.sourceAppId + L"\n" + media.title;
}

struct MediaSessionWatcher::Impl {
    ~Impl() { stop(); }

    MediaSource sourcePreference() const {
        std::lock_guard lock(mutex);
        return desiredSource;
    }

    void publish(MediaSessionInfo value) {
        bool changed{};
        ChangedCallback notification;
        {
            std::lock_guard lock(mutex);
            changed = !hasValue || value != current;
            current = std::move(value);
            hasValue = true;
            notification = callback;
        }
        if (changed && notification) notification();
    }

    void run() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            MediaSessionInfo value;
            value.requestedSource = sourcePreference();
            value.source = value.requestedSource;
            value.error = L"Windows medya oturumu başlatılamadı.";
            publish(std::move(value));
            return;
        }
        try {
            const auto manager = winrt::Windows::Media::Control::
                GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            SessionArtworkCache artworkCache;
            while (!stopping.load(std::memory_order_acquire)) {
                try {
                    const auto requested = sourcePreference();
                    if (skipNextRequested.exchange(false, std::memory_order_acq_rel)) {
                        const auto session = selectSession(manager, requested);
                        if (session && session.GetPlaybackInfo().Controls().IsNextEnabled()) {
                            session.TrySkipNextAsync().get();
                        }
                    }
                    publish(readSession(manager, requested, artworkCache));
                } catch (const winrt::hresult_error& exception) {
                    MediaSessionInfo value;
                    value.apiAvailable = true;
                    value.requestedSource = sourcePreference();
                    value.source = value.requestedSource;
                    value.error = asWide(exception.message());
                    publish(std::move(value));
                }
                std::unique_lock lock(waitMutex);
                waitCondition.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return stopping.load(std::memory_order_acquire);
                });
            }
        } catch (const winrt::hresult_error& exception) {
            MediaSessionInfo value;
            value.requestedSource = sourcePreference();
            value.source = value.requestedSource;
            value.error = L"Medya bilgilerine erişilemedi: " + asWide(exception.message());
            publish(std::move(value));
        }
        CoUninitialize();
    }

    void stop() {
        stopping.store(true, std::memory_order_release);
        waitCondition.notify_all();
        if (worker.joinable()) worker.join();
    }

    mutable std::mutex mutex;
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::thread worker;
    std::atomic_bool stopping{};
    std::atomic_bool skipNextRequested{};
    ChangedCallback callback;
    MediaSessionInfo current;
    MediaSource desiredSource{MediaSource::Spotify};
    bool hasValue{};
};

MediaSessionWatcher::MediaSessionWatcher() : impl_(std::make_unique<Impl>()) {}
MediaSessionWatcher::~MediaSessionWatcher() = default;

void MediaSessionWatcher::start(ChangedCallback callback) {
    stop();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->callback = std::move(callback);
        impl_->hasValue = false;
        impl_->current = {};
    }
    impl_->stopping.store(false, std::memory_order_release);
    impl_->skipNextRequested.store(false, std::memory_order_release);
    impl_->worker = std::thread([implementation = impl_.get()] { implementation->run(); });
}

void MediaSessionWatcher::stop() { impl_->stop(); }

void MediaSessionWatcher::setSource(MediaSource source) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->desiredSource = source;
    }
    impl_->waitCondition.notify_all();
}

void MediaSessionWatcher::requestSkipNext() {
    impl_->skipNextRequested.store(true, std::memory_order_release);
    impl_->waitCondition.notify_all();
}

MediaSessionInfo MediaSessionWatcher::latest() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current;
}

}  // namespace neon
