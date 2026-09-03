#include "neon/Audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "neon/Utils.hpp"

namespace neon {
namespace {
constexpr double pi = 3.14159265358979323846;
}

void SpectrumAnalyzer::push(const SDL_AudioSpec& spec, const float* pcm, int samples) noexcept {
    if (!pcm || samples <= 0) return;
    const int channels = std::max(1, spec.channels);
    sampleRate_.store(spec.freq > 0 ? spec.freq : 48000, std::memory_order_relaxed);
    for (int offset = 0; offset + channels <= samples; offset += channels) {
        float mono{};
        for (int channel = 0; channel < channels; ++channel) mono += pcm[offset + channel];
        working_.mono[workingCount_] = mono / static_cast<float>(channels);
        working_.left[workingCount_] = pcm[offset];
        working_.right[workingCount_] = channels > 1 ? pcm[offset + 1] : pcm[offset];
        ++workingCount_;
        if (workingCount_ == sampleCount) {
            int slot = producerSlot_;
            const int reading = readingSlot_.load(std::memory_order_acquire);
            const int current = publishedSlot_.load(std::memory_order_relaxed);
            for (int attempt = 0; attempt < 3 && (slot == reading || slot == current); ++attempt) slot = (slot + 1) % 3;
            if (slot != reading) {
                published_[slot] = working_;
                publishedSlot_.store(slot, std::memory_order_release);
                publishedSequence_.fetch_add(1, std::memory_order_release);
                producerSlot_ = (slot + 1) % 3;
            }
            workingCount_ = 0;
        }
    }
}

AudioVisualizationFrame SpectrumAnalyzer::frame() {
    const auto sequence = publishedSequence_.load(std::memory_order_acquire);
    if (sequence == 0 || sequence == consumedSequence_) {
        for (auto& band : smoothed_) band *= 0.88F;
        for (auto& sample : frame_.leftWaveform) sample *= 0.82F;
        for (auto& sample : frame_.rightWaveform) sample *= 0.82F;
        frame_.rmsLeft *= 0.86F;
        frame_.rmsRight *= 0.86F;
        frame_.peakLeft = std::max(frame_.rmsLeft, frame_.peakLeft - 0.025F);
        frame_.peakRight = std::max(frame_.rmsRight, frame_.peakRight - 0.025F);
        frame_.bands = smoothed_;
        return frame_;
    }
    consumedSequence_ = sequence;
    const int slot = publishedSlot_.load(std::memory_order_acquire);
    if (slot < 0) return frame_;
    readingSlot_.store(slot, std::memory_order_release);
    const auto samples = published_[slot];
    readingSlot_.store(-1, std::memory_order_release);

    const double sampleRate = static_cast<double>(sampleRate_.load(std::memory_order_relaxed));
    for (std::size_t band = 0; band < bandCount; ++band) {
        const double fraction = static_cast<double>(band) / static_cast<double>(bandCount - 1);
        // 45 Hz..18 kHz on a logarithmic axis gives hi-fi style equal visual
        // weight to bass octaves without throwing away cymbal/air content.
        const double frequency = 45.0 * std::pow(400.0, fraction);
        const double omega = 2.0 * pi * std::min(frequency, sampleRate * 0.45) / sampleRate;
        const double coefficient = 2.0 * std::cos(omega);
        double previous{};
        double previous2{};
        for (std::size_t i = 0; i < samples.mono.size(); ++i) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) /
                                                       static_cast<double>(samples.mono.size() - 1));
            const double value = static_cast<double>(samples.mono[i]) * window + coefficient * previous - previous2;
            previous2 = previous;
            previous = value;
        }
        const double power = previous2 * previous2 + previous * previous -
                             coefficient * previous * previous2;
        // Goertzel magnitude grows with the analysis-window length. The old
        // unnormalised value therefore pinned strong low-frequency bins at 1.0
        // almost permanently. Compensate for the Hann coherent gain, then map
        // the resulting amplitude from -72 dBFS to 0 dBFS. The gentle curve
        // keeps quiet detail visible without turning normal music into a solid
        // wall of fully-lit LEDs.
        constexpr double hannSum = static_cast<double>(sampleCount - 1) * 0.5;
        const double amplitude = 2.0 * std::sqrt(std::max(0.0, power)) / hannSum;
        const double decibels = 20.0 * std::log10(std::max(amplitude, 0.0000001));
        const float dbLevel = static_cast<float>(std::clamp((decibels + 72.0) / 72.0,
                                                            0.0, 1.0));
        const float magnitude = std::pow(dbLevel, 1.45F);
        const float rate = magnitude > smoothed_[band] ? 0.55F : 0.12F;
        smoothed_[band] += (magnitude - smoothed_[band]) * rate;
    }

    double leftPower{};
    double rightPower{};
    float leftPeak{};
    float rightPeak{};
    for (std::size_t i = 0; i < sampleCount; ++i) {
        leftPower += static_cast<double>(samples.left[i]) * samples.left[i];
        rightPower += static_cast<double>(samples.right[i]) * samples.right[i];
        leftPeak = std::max(leftPeak, std::abs(samples.left[i]));
        rightPeak = std::max(rightPeak, std::abs(samples.right[i]));
    }
    const float leftRms = std::clamp(static_cast<float>(std::sqrt(leftPower / sampleCount)) * 2.8F, 0.0F, 1.0F);
    const float rightRms = std::clamp(static_cast<float>(std::sqrt(rightPower / sampleCount)) * 2.8F, 0.0F, 1.0F);
    frame_.rmsLeft += (leftRms - frame_.rmsLeft) * (leftRms > frame_.rmsLeft ? 0.58F : 0.16F);
    frame_.rmsRight += (rightRms - frame_.rmsRight) * (rightRms > frame_.rmsRight ? 0.58F : 0.16F);
    frame_.peakLeft = std::max(std::clamp(leftPeak * 1.3F, 0.0F, 1.0F), frame_.peakLeft - 0.018F);
    frame_.peakRight = std::max(std::clamp(rightPeak * 1.3F, 0.0F, 1.0F), frame_.peakRight - 0.018F);
    for (std::size_t i = 0; i < AudioVisualizationFrame::waveformSampleCount; ++i) {
        const std::size_t source = i * sampleCount / AudioVisualizationFrame::waveformSampleCount;
        frame_.leftWaveform[i] = std::clamp(samples.left[source], -1.0F, 1.0F);
        frame_.rightWaveform[i] = std::clamp(samples.right[source], -1.0F, 1.0F);
    }
    frame_.bands = smoothed_;
    return frame_;
}

std::array<float, SpectrumAnalyzer::bandCount> SpectrumAnalyzer::bands() { return frame().bands; }

AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::shutdown() {
    const bool wasInitialized = mixer_ != nullptr;
    if (mixer_) MIX_SetPostMixCallback(mixer_, nullptr, nullptr);
    if (track_) MIX_SetTrackStoppedCallback(track_, nullptr, nullptr);
    acceptFinished_.store(false, std::memory_order_release);
    if (track_) MIX_DestroyTrack(track_);
    if (audio_) MIX_DestroyAudio(audio_);
    if (mixer_) MIX_DestroyMixer(mixer_);
    track_ = nullptr;
    audio_ = nullptr;
    mixer_ = nullptr;
    finished_.store(false, std::memory_order_release);
    paused_ = false;
    durationMs_ = 0;
    if (wasInitialized) MIX_Quit();
}

bool AudioEngine::initialize(std::string& error) {
    if (mixer_) return true;
    if (!MIX_Init()) {
        error = SDL_GetError();
        return false;
    }
    mixer_ = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!mixer_) {
        error = SDL_GetError();
        MIX_Quit();
        return false;
    }
    track_ = MIX_CreateTrack(mixer_);
    if (!track_ || !MIX_SetTrackStoppedCallback(track_, &AudioEngine::stoppedCallback, this) ||
        !MIX_SetPostMixCallback(mixer_, &AudioEngine::postMixCallback, this)) {
        error = SDL_GetError();
        if (track_) MIX_DestroyTrack(track_);
        MIX_DestroyMixer(mixer_);
        track_ = nullptr;
        mixer_ = nullptr;
        MIX_Quit();
        return false;
    }
    setVolume(volume_);
    return true;
}

bool AudioEngine::play(const Track& selected, std::int64_t startMs, std::string& error) {
    if (!mixer_ && !initialize(error)) return false;
    stop();
    audio_ = MIX_LoadAudio(mixer_, pathToUtf8(selected.path).c_str(), false);
    if (!audio_) { error = SDL_GetError(); return false; }
    if (!MIX_SetTrackAudio(track_, audio_)) {
        error = SDL_GetError();
        MIX_DestroyAudio(audio_);
        audio_ = nullptr;
        return false;
    }
    SDL_PropertiesID options = 0;
    if (startMs > 0) {
        options = SDL_CreateProperties();
        SDL_SetNumberProperty(options, MIX_PROP_PLAY_START_MILLISECOND_NUMBER, startMs);
    }
    finished_.store(false, std::memory_order_release);
    acceptFinished_.store(true, std::memory_order_release);
    const bool started = MIX_PlayTrack(track_, options);
    if (options) SDL_DestroyProperties(options);
    if (!started) {
        acceptFinished_.store(false, std::memory_order_release);
        error = SDL_GetError();
        MIX_SetTrackAudio(track_, nullptr);
        MIX_DestroyAudio(audio_);
        audio_ = nullptr;
        return false;
    }
    durationMs_ = selected.durationMs > 0 ? selected.durationMs : MIX_AudioFramesToMS(audio_, MIX_GetAudioDuration(audio_));
    paused_ = false;
    return true;
}

void AudioEngine::stop() {
    if (!track_) return;
    acceptFinished_.store(false, std::memory_order_release);
    MIX_StopTrack(track_, 0);
    MIX_SetTrackAudio(track_, nullptr);
    if (audio_) { MIX_DestroyAudio(audio_); audio_ = nullptr; }
    finished_.store(false, std::memory_order_release);
    paused_ = false;
    durationMs_ = 0;
}

bool AudioEngine::pause() {
    if (!track_ || !playing() || paused_) return false;
    paused_ = MIX_PauseTrack(track_);
    return paused_;
}

bool AudioEngine::resume() {
    if (!track_ || !paused_) return false;
    if (!MIX_ResumeTrack(track_)) return false;
    paused_ = false;
    return true;
}

bool AudioEngine::seek(std::int64_t milliseconds) {
    if (!track_ || !audio_) return false;
    return MIX_SetTrackPlaybackPosition(track_, MIX_TrackMSToFrames(track_, std::max<std::int64_t>(0, milliseconds)));
}

void AudioEngine::setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0F, 1.0F);
    if (mixer_) MIX_SetMixerGain(mixer_, volume_);
}

bool AudioEngine::playing() const { return track_ && MIX_TrackPlaying(track_); }

bool AudioEngine::takeFinished() { return finished_.exchange(false, std::memory_order_acq_rel); }

std::int64_t AudioEngine::positionMs() const {
    if (!track_ || !audio_) return 0;
    const auto frames = MIX_GetTrackPlaybackPosition(track_);
    return frames < 0 ? 0 : MIX_TrackFramesToMS(track_, frames);
}

std::array<float, SpectrumAnalyzer::bandCount> AudioEngine::spectrum() { return analyzer_.bands(); }

AudioVisualizationFrame AudioEngine::visualization() { return analyzer_.frame(); }

void SDLCALL AudioEngine::stoppedCallback(void* userdata, MIX_Track*) {
    auto* self = static_cast<AudioEngine*>(userdata);
    if (self->acceptFinished_.load(std::memory_order_acquire)) self->finished_.store(true, std::memory_order_release);
}

void SDLCALL AudioEngine::postMixCallback(void* userdata, MIX_Mixer*, const SDL_AudioSpec* spec,
                                          float* pcm, int samples) {
    if (spec) static_cast<AudioEngine*>(userdata)->analyzer_.push(*spec, pcm, samples);
}

}  // namespace neon
