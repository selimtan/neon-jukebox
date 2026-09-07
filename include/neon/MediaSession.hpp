#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neon {

enum class MediaSource {
    // Values are persisted in Recorder's settings; keep existing IDs stable.
    Spotify = 0,
    Chrome = 1,
    Automatic = 2,
    YouTube = 3
};

struct MediaSessionInfo {
    bool apiAvailable{};
    bool sessionAvailable{};
    bool playing{};
    bool nextEnabled{};
    MediaSource requestedSource{MediaSource::Spotify};
    MediaSource source{MediaSource::Spotify};
    std::wstring sourceAppId;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    // GSMTC does not expose an album release year. Artwork, when the source
    // publishes it, is copied from the session thumbnail without a web lookup.
    std::vector<std::uint8_t> artwork;
    std::string artworkMimeType;
    bool timelineAvailable{};
    std::int64_t positionTicks{};
    std::int64_t endTicks{};
    std::int64_t durationTicks{};
    std::wstring error;

    [[nodiscard]] bool operator==(const MediaSessionInfo&) const = default;
};

// A stable identity for automatic recording boundaries. Artist is deliberately
// excluded because Spotify may publish it just after the title.
[[nodiscard]] std::wstring mediaTrackIdentity(const MediaSessionInfo& media);

// YouTube follows Chrome's media session, including YouTube Music. Windows
// identifies the browser, so this preset cannot filter individual sites/tabs.
// Watches Spotify, Chrome/YouTube, or whichever supported Windows media session is
// active on a background MTA thread. The callback only signals that a new
// snapshot exists; callers read the thread-safe latest() value on their UI thread.
class MediaSessionWatcher {
public:
    using ChangedCallback = std::function<void()>;

    MediaSessionWatcher();
    ~MediaSessionWatcher();
    MediaSessionWatcher(const MediaSessionWatcher&) = delete;
    MediaSessionWatcher& operator=(const MediaSessionWatcher&) = delete;

    void start(ChangedCallback callback);
    void stop();
    void setSource(MediaSource source);
    void requestSkipNext();
    [[nodiscard]] MediaSessionInfo latest() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neon
