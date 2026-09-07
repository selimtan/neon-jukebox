#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace neon {

inline constexpr std::uint32_t recorderSampleRate = 44'100;
inline constexpr std::uint16_t recorderChannels = 2;
inline constexpr std::uint16_t recorderBitsPerSample = 16;
inline constexpr std::uint64_t automaticMinimumRecordingSeconds = 2 * 60;
inline constexpr std::uint64_t automaticMinimumRecordingFrames =
    static_cast<std::uint64_t>(recorderSampleRate) * automaticMinimumRecordingSeconds;
inline constexpr std::uint32_t automaticDurationTolerancePercent = 10;

[[nodiscard]] constexpr bool automaticRecordingIsLongEnough(std::uint64_t frames) {
    return frames > automaticMinimumRecordingFrames;
}

// Compares a recorded-file duration with the current media-session duration.
// The tolerance is relative to the media-session duration.
[[nodiscard]] constexpr bool automaticRecordingDurationMatches(
    std::uint64_t actualDurationTicks,
    std::uint64_t expectedDurationTicks) {
    if (actualDurationTicks == 0 || expectedDurationTicks == 0) return false;
    const auto difference = actualDurationTicks > expectedDurationTicks
        ? actualDurationTicks - expectedDurationTicks
        : expectedDurationTicks - actualDurationTicks;
    return difference <= expectedDurationTicks * automaticDurationTolerancePercent / 100;
}

struct RecordingMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring albumYear;
    std::vector<std::uint8_t> artwork;
    std::string artworkMimeType;
};

struct RecorderSnapshot {
    bool recording{};
    bool hasRecording{};
    std::uint64_t sampleFrames{};
    float peakLeft{};
    float peakRight{};
    std::wstring error;
};

struct CapturedRecording {
    std::filesystem::path rawPcmPath;
    std::uint64_t sampleFrames{};

    [[nodiscard]] explicit operator bool() const {
        return !rawPcmPath.empty() && sampleFrames > 0;
    }
};

// Captures the audio currently being sent to the Windows default playback
// device. Captured audio is continuously converted to CD-format PCM and
// streamed to a temporary file, so a long recording does not consume RAM.
class LoopbackRecorder {
public:
    LoopbackRecorder();
    ~LoopbackRecorder();
    LoopbackRecorder(const LoopbackRecorder&) = delete;
    LoopbackRecorder& operator=(const LoopbackRecorder&) = delete;

    // Creates the raw PCM capture under <recordingRoot>\_TMP.
    bool start(const std::filesystem::path& recordingRoot, std::wstring& error);
    bool stop(std::wstring& error);
    void discard();
    // Transfers ownership of a stopped recording to an asynchronous saver.
    // The caller becomes responsible for deleting rawPcmPath.
    CapturedRecording takeRecording();

    [[nodiscard]] RecorderSnapshot snapshot() const;
    [[nodiscard]] std::filesystem::path temporaryPcmPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Measures the portion that remains after the recorder's leading/trailing
// silence detection and edge padding. Used to reject tiny automatic fragments.
bool measureAudibleRecording(const std::filesystem::path& rawPcmPath,
                             std::uint64_t sampleFrames,
                             std::uint64_t& audibleFrames,
                             std::wstring& error);

// Wraps a raw 44.1 kHz, 16-bit stereo PCM recording in a standards-compliant
// RIFF/WAVE file. Text is stored in LIST/INFO and an embedded ID3 tag is used
// for Unicode metadata and album artwork when available.
bool writeCdQualityWave(const std::filesystem::path& rawPcmPath,
                        std::uint64_t sampleFrames,
                        const std::filesystem::path& destination,
                        const RecordingMetadata& metadata,
                        std::wstring& error);

// Encodes the same CD-format temporary PCM recording as a 320 kbps stereo MP3
// and prepends UTF-8 ID3v2 metadata for broad player compatibility.
bool writeHighQualityMp3(const std::filesystem::path& rawPcmPath,
                         std::uint64_t sampleFrames,
                         const std::filesystem::path& destination,
                         const RecordingMetadata& metadata,
                         std::wstring& error);

std::wstring safeRecordingFileName(std::wstring value);

// Returns the artist/album subfolder used by automatic recording. Missing
// metadata is grouped predictably instead of mixing loose files into the root.
std::filesystem::path automaticRecordingFolder(const std::filesystem::path& root,
                                                std::wstring artist,
                                                std::wstring album);

// Checks both supported output formats in the exact artist/album destination.
// Incomplete metadata is never treated as a match.
bool automaticRecordingAlreadyExists(const std::filesystem::path& root,
                                     const RecordingMetadata& metadata);

// Returns true only when a matching named recording has a readable duration
// close to the duration reported by the current media session.
bool automaticRecordingMatchesDuration(const std::filesystem::path& root,
                                       const RecordingMetadata& metadata,
                                       std::uint64_t expectedDurationTicks);

// Removes older files with the same artist/album/title after a replacement was
// saved successfully. The newly written file is preserved.
void removeSupersededAutomaticRecordings(const std::filesystem::path& root,
                                         const RecordingMetadata& metadata,
                                         const std::filesystem::path& keep);

}  // namespace neon
