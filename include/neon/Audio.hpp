#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include <SDL3/SDL_audio.h>
#include <SDL3_mixer/SDL_mixer.h>

#include "neon/Models.hpp"

namespace neon {

struct AudioVisualizationFrame {
    static constexpr std::size_t bandCount = 64;
    static constexpr std::size_t waveformSampleCount = 256;

    std::array<float, bandCount> bands{};
    std::array<float, waveformSampleCount> leftWaveform{};
    std::array<float, waveformSampleCount> rightWaveform{};
    float rmsLeft{};
    float rmsRight{};
    float peakLeft{};
    float peakRight{};
};

class SpectrumAnalyzer {
public:
    static constexpr std::size_t sampleCount = 1024;
    static constexpr std::size_t bandCount = AudioVisualizationFrame::bandCount;

    void push(const SDL_AudioSpec& spec, const float* pcm, int samples) noexcept;
    [[nodiscard]] std::array<float, bandCount> bands();
    [[nodiscard]] AudioVisualizationFrame frame();

private:
    struct SampleWindow {
        std::array<float, sampleCount> mono{};
        std::array<float, sampleCount> left{};
        std::array<float, sampleCount> right{};
    };

    SampleWindow working_{};
    std::array<SampleWindow, 3> published_{};
    std::size_t workingCount_{};
    int producerSlot_{};
    std::atomic<int> publishedSlot_{-1};
    std::atomic<int> readingSlot_{-1};
    std::atomic<std::uint64_t> publishedSequence_{};
    std::uint64_t consumedSequence_{};
    std::atomic<int> sampleRate_{48000};
    std::array<float, bandCount> smoothed_{};
    AudioVisualizationFrame frame_{};
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool initialize(std::string& error);
    void shutdown();
    bool play(const Track& track, std::int64_t startMs, std::string& error);
    void stop();
    bool pause();
    bool resume();
    bool seek(std::int64_t milliseconds);
    void setVolume(float volume);

    [[nodiscard]] bool initialized() const { return mixer_ != nullptr; }
    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool paused() const { return paused_; }
    [[nodiscard]] bool takeFinished();
    [[nodiscard]] std::int64_t positionMs() const;
    [[nodiscard]] std::int64_t durationMs() const { return durationMs_; }
    [[nodiscard]] float volume() const { return volume_; }
    [[nodiscard]] std::array<float, SpectrumAnalyzer::bandCount> spectrum();
    [[nodiscard]] AudioVisualizationFrame visualization();

private:
    static void SDLCALL stoppedCallback(void* userdata, MIX_Track* track);
    static void SDLCALL postMixCallback(void* userdata, MIX_Mixer* mixer,
                                        const SDL_AudioSpec* spec, float* pcm, int samples);

    MIX_Mixer* mixer_{};
    MIX_Track* track_{};
    MIX_Audio* audio_{};
    std::atomic_bool finished_{};
    std::atomic_bool acceptFinished_{};
    bool paused_{};
    float volume_{0.8F};
    std::int64_t durationMs_{};
    SpectrumAnalyzer analyzer_;
};

}  // namespace neon
