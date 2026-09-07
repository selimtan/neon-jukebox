#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "neon/Audio.hpp"
#include "neon/Models.hpp"
#include "neon/Utils.hpp"

namespace {

int failures{};

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #expression "\n"; \
        ++failures; \
    } \
} while (false)

neon::Track fixture(const std::filesystem::path& path) {
    neon::Track track;
    track.id = path.filename().string();
    track.path = path;
    track.title = track.id;
    track.durationMs = 0;
    return track;
}

neon::AudioVisualizationFrame analyzedSine(float amplitude) {
    neon::SpectrumAnalyzer analyzer;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 48'000;
    std::vector<float> samples(neon::SpectrumAnalyzer::sampleCount * 2);
    constexpr double pi = 3.14159265358979323846;
    for (int pass = 0; pass < 8; ++pass) {
        for (std::size_t frame = 0; frame < neon::SpectrumAnalyzer::sampleCount; ++frame) {
            const float value = amplitude * static_cast<float>(
                std::sin(2.0 * pi * 1000.0 * static_cast<double>(frame) / spec.freq));
            samples[frame * 2] = value;
            samples[frame * 2 + 1] = value;
        }
        analyzer.push(spec, samples.data(), static_cast<int>(samples.size()));
        (void)analyzer.frame();
    }
    return analyzer.frame();
}

float effectPeak(float volume) {
    neon::AudioEngine audio;
    std::string error;
    CHECK(audio.initialize(error, NEON_EFFECT_FIXTURES));
    CHECK(audio.takeEffectError().empty());
    CHECK(!audio.playEffect(neon::UiSoundEffect::Coin)); // Neon/default is silent.
    audio.setEffectsEnabled(true);
    audio.setVolume(volume);
    CHECK(audio.playEffect(neon::UiSoundEffect::Coin));
    float peak{};
    for (int frame = 0; frame < 85; ++frame) {
        SDL_Delay(10);
        const auto frameData = audio.visualization();
        // Measure PCM samples; the visual peak meter intentionally boosts RMS.
        for (const auto sample : frameData.leftWaveform) peak = std::max(peak, std::abs(sample));
    }
    CHECK(!audio.playing());
    CHECK(!audio.takeFinished()); // An effect ending is never a song completion.
    return peak;
}

void verifyEffects() {
    const auto loud = effectPeak(1.0F);
    const auto quiet = effectPeak(0.25F);
    std::cout << "Effect output peaks: full=" << loud << " quarter=" << quiet << '\n';
    CHECK(loud > 0.20F && loud < 0.48F); // 35% gain, allowing sample-rate conversion overshoot.
    CHECK(quiet > loud * 0.18F && quiet < loud * 0.32F);
    CHECK(effectPeak(0.0F) == 0.0F);

    const auto root = std::filesystem::temp_directory_path() / ("neon-effects-" + neon::randomId());
    std::filesystem::create_directories(root);
    for (const auto* name : {"coin.wav", "page-turn.wav"})
        std::filesystem::copy_file(std::filesystem::path(NEON_EFFECT_FIXTURES) / name, root / name);
    {
        neon::AudioEngine audio;
        std::string error;
        CHECK(audio.initialize(error, root));
        CHECK(audio.takeEffectError().empty());
        audio.setEffectsEnabled(true);
        // Removing the source proves effects have been decoded before clicks.
        for (const auto* name : {"coin.wav", "page-turn.wav"}) CHECK(std::filesystem::remove(root / name));
        CHECK(audio.play(fixture(std::filesystem::path(NEON_MIXER_FIXTURES) / "music.mp3"), 0, error));
        CHECK(audio.pause());
        const auto position = audio.positionMs();
        const auto duration = audio.durationMs();
        for (int i = 0; i < 20; ++i) CHECK(audio.playEffect(neon::UiSoundEffect::Coin));
        CHECK(audio.playEffect(neon::UiSoundEffect::PageTurn));
        SDL_Delay(160);
        CHECK(audio.visualization().peakLeft > 0.01F);
        CHECK(audio.paused());
        CHECK(audio.positionMs() == position);
        CHECK(audio.durationMs() == duration);
        CHECK(!audio.takeFinished());
        CHECK(audio.resume());
        audio.setEffectsEnabled(false);
        CHECK(!audio.playEffect(neon::UiSoundEffect::PageTurn));
        CHECK(audio.playing());
        audio.shutdown();
        // Missing files remain a nonfatal, once-readable warning after recovery.
        CHECK(audio.initialize(error, root));
        CHECK(!audio.takeEffectError().empty());
        CHECK(audio.takeEffectError().empty());
        audio.setEffectsEnabled(true);
        CHECK(!audio.playEffect(neon::UiSoundEffect::Coin));
        CHECK(audio.play(fixture(std::filesystem::path(NEON_MIXER_FIXTURES) / "music.mp3"), 0, error));
    }
    std::filesystem::remove(root);
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        std::cerr << "SDL audio initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }

    // Spectrum height must follow signal level instead of saturating every
    // frequency bin regardless of input amplitude.
    const auto quietTone = analyzedSine(0.02F);
    const auto loudTone = analyzedSine(0.80F);
    const float quietMaximum = *std::ranges::max_element(quietTone.bands);
    const float loudMaximum = *std::ranges::max_element(loudTone.bands);
    CHECK(loudMaximum > quietMaximum + 0.20F);
    CHECK(loudMaximum > 0.70F);
    CHECK(std::ranges::count_if(loudTone.bands,
        [](float value) { return value > 0.95F; }) < 12);
    verifyEffects();

    {
        neon::AudioEngine audio;
        std::string error;
        CHECK(audio.initialize(error));

        const std::filesystem::path mixerFixtures = NEON_MIXER_FIXTURES;
        const std::filesystem::path taglibFixtures = NEON_TAGLIB_FIXTURES;
        const std::array<std::filesystem::path, 4> formats{
            mixerFixtures / "music.mp3",
            taglibFixtures / "test.ogg",
            taglibFixtures / "sinewave.flac",
            mixerFixtures / "sword.wav"
        };

        for (const auto& path : formats) {
            error.clear();
            CHECK(std::filesystem::is_regular_file(path));
            CHECK(audio.play(fixture(path), 0, error));
            CHECK(audio.durationMs() > 0);
            CHECK(audio.playing());
            CHECK(audio.seek(audio.durationMs() / 2));
            CHECK(audio.pause());
            CHECK(audio.paused());
            CHECK(audio.resume());
            audio.stop();
        }

        error.clear();
        CHECK(audio.play(fixture(mixerFixtures / "sword.wav"), 0, error));
        SDL_Delay(180);
        const auto visualization = audio.visualization();
        CHECK(std::ranges::any_of(visualization.bands, [](float value) { return value > 0.001F; }));
        CHECK(visualization.rmsLeft > 0.001F);
        CHECK(visualization.rmsRight > 0.001F);
        CHECK(std::ranges::any_of(visualization.leftWaveform,
                                  [](float value) { return std::abs(value) > 0.001F; }));

        bool finished{};
        const std::uint64_t deadline = SDL_GetTicks() + 5000;
        while (SDL_GetTicks() < deadline && !(finished = audio.takeFinished())) SDL_Delay(10);
        CHECK(finished);
        audio.shutdown();
    }

    SDL_Quit();
    if (failures == 0) std::cout << "All Neon Jukebox audio integration tests passed.\n";
    return failures == 0 ? 0 : 1;
}
