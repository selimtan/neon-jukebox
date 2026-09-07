#include <Windows.h>
#include <ShObjIdl.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "neon/Recorder.hpp"

namespace {

int failures{};

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #expression "\n"; \
        ++failures; \
    } \
} while (false)

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool contains(const std::vector<std::uint8_t>& bytes, std::string_view value) {
    return std::search(bytes.begin(), bytes.end(), value.begin(), value.end(),
                       [](std::uint8_t left, char right) {
                           return left == static_cast<std::uint8_t>(right);
                       }) != bytes.end();
}

std::string systemAnsi(std::wstring_view value) {
    const int count = WideCharToMultiByte(CP_ACP, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(count, 0)), '\0');
    if (count > 0) {
        WideCharToMultiByte(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
                            result.data(), count, nullptr, nullptr);
    }
    return result;
}

std::wstring shellStringProperty(const std::filesystem::path& path,
                                 const PROPERTYKEY& key) {
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT,
                                                 IID_PPV_ARGS(&store)))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    if (FAILED(store->GetValue(key, &value))) return {};
    PWSTR text{};
    const HRESULT converted = PropVariantToStringAlloc(value, &text);
    PropVariantClear(&value);
    if (FAILED(converted) || !text) return {};
    std::wstring result(text);
    CoTaskMemFree(text);
    return result;
}

bool hasEmbeddedArtwork(const std::filesystem::path& path,
                        const std::vector<std::uint8_t>& expected) {
    TagLib::FileRef reference(TagLib::FileName(path.c_str()), false);
    if (reference.isNull()) return false;
    for (const auto& picture : reference.complexProperties("PICTURE")) {
        bool valid{};
        const auto data = picture.value("data").toByteVector(&valid);
        if (!valid || data.size() != expected.size()) continue;
        const auto* begin = reinterpret_cast<const std::uint8_t*>(data.data());
        if (std::equal(expected.begin(), expected.end(), begin)) return true;
    }
    return false;
}

unsigned int tagYear(const std::filesystem::path& path) {
    TagLib::FileRef reference(TagLib::FileName(path.c_str()), false);
    return reference.isNull() || !reference.tag() ? 0U : reference.tag()->year();
}

}  // namespace

int main() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(comResult));
    const auto folder = std::filesystem::temp_directory_path() /
                        (L"neon-recorder-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    const auto rawPath = folder / L"source.pcm";
    const auto wavePath = folder / L"recording.wav";
    const auto mp3Path = folder / L"recording.mp3";
    constexpr std::uint64_t leadingSilence = neon::recorderSampleRate * 2;
    constexpr std::uint64_t audibleFrames = neon::recorderSampleRate / 10;
    constexpr std::uint64_t trailingSilence = neon::recorderSampleRate * 3;
    constexpr std::uint64_t frames = leadingSilence + audibleFrames + trailingSilence;
    {
        std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            std::array<std::int16_t, 2> sample{};
            if (frame >= leadingSilence && frame < leadingSilence + audibleFrames) {
                const auto contentFrame = frame - leadingSilence;
                sample = {static_cast<std::int16_t>((contentFrame % 100) * 200),
                          static_cast<std::int16_t>(
                              -static_cast<std::int32_t>((contentFrame % 100) * 200))};
            }
            raw.write(reinterpret_cast<const char*>(sample.data()), sizeof(sample));
        }
    }

    const std::vector<std::uint8_t> artwork{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 'N', 'E', 'O', 'N'};
    neon::RecordingMetadata metadata{L"Gece Şarkısı", L"Deneme Sanatçı", L"Neon Albüm",
                                      L"1998", artwork, "image/png"};
    std::wstring error;
    std::uint64_t measuredAudibleFrames{};
    CHECK(neon::measureAudibleRecording(rawPath, frames, measuredAudibleFrames, error));
    CHECK(measuredAudibleFrames >= audibleFrames);
    CHECK(measuredAudibleFrames <= audibleFrames + neon::recorderSampleRate / 10);
    CHECK(error.empty());

    CHECK(neon::writeCdQualityWave(rawPath, frames, wavePath, metadata, error));
    CHECK(error.empty());
    CHECK(shellStringProperty(wavePath, PKEY_Title) == metadata.title);
    CHECK(shellStringProperty(wavePath, PKEY_Music_Artist) == metadata.artist);
    CHECK(tagYear(wavePath) == 1998);
    CHECK(hasEmbeddedArtwork(wavePath, artwork));

    std::ifstream wave(wavePath, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(wave),
                                          std::istreambuf_iterator<char>()};
    CHECK(bytes.size() > 44 + audibleFrames * 4);
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF");
    CHECK(std::string(bytes.begin() + 8, bytes.begin() + 12) == "WAVE");
    CHECK(readU16(bytes, 20) == 1);
    CHECK(readU16(bytes, 22) == neon::recorderChannels);
    CHECK(readU32(bytes, 24) == neon::recorderSampleRate);
    CHECK(readU16(bytes, 34) == neon::recorderBitsPerSample);
    CHECK(std::string(bytes.begin() + 36, bytes.begin() + 40) == "data");
    const std::uint64_t trimmedFrames = readU32(bytes, 40) / 4;
    CHECK(trimmedFrames >= audibleFrames);
    CHECK(trimmedFrames <= audibleFrames + neon::recorderSampleRate / 10);
    CHECK(trimmedFrames < frames / 2);
    CHECK(readU32(bytes, 4) + 8 == bytes.size());
    CHECK(contains(bytes, "LIST"));
    CHECK(contains(bytes, "INFO"));
    CHECK(contains(bytes, "CSET"));
    CHECK(contains(bytes, "ID3 "));
    CHECK(contains(bytes, "TDRC"));
    CHECK(contains(bytes, "APIC"));
    CHECK(contains(bytes, systemAnsi(L"Gece Ş")));
    CHECK(contains(bytes, systemAnsi(L"Deneme Sanatçı")));
    CHECK(contains(bytes, "Neon Alb"));

    error.clear();
    const bool mp3Written = neon::writeHighQualityMp3(rawPath, frames, mp3Path, metadata, error);
    if (!mp3Written) std::wcerr << L"MP3 error: " << error << L'\n';
    CHECK(mp3Written);
    CHECK(error.empty());
    std::ifstream mp3(mp3Path, std::ios::binary);
    const std::vector<std::uint8_t> mp3Bytes{std::istreambuf_iterator<char>(mp3),
                                             std::istreambuf_iterator<char>()};
    CHECK(mp3Bytes.size() > 1000);
    CHECK(mp3Bytes.size() >= 4);
    if (mp3Bytes.size() >= 4) {
        CHECK(std::string(mp3Bytes.begin(), mp3Bytes.begin() + 3) == "ID3");
        CHECK(mp3Bytes[3] == 4);
    }
    CHECK(contains(mp3Bytes, "TIT2"));
    CHECK(contains(mp3Bytes, "TPE1"));
    CHECK(contains(mp3Bytes, "TALB"));
    CHECK(contains(mp3Bytes, "TDRC"));
    CHECK(contains(mp3Bytes, "APIC"));
    CHECK(contains(mp3Bytes, "Gece \xC5\x9E"));
    CHECK(contains(mp3Bytes, "Deneme Sanat"));
    CHECK(shellStringProperty(mp3Path, PKEY_Title) == metadata.title);
    CHECK(shellStringProperty(mp3Path, PKEY_Music_Artist) == metadata.artist);
    CHECK(tagYear(mp3Path) == 1998);
    CHECK(hasEmbeddedArtwork(mp3Path, artwork));
    CHECK(std::adjacent_find(mp3Bytes.begin(), mp3Bytes.end(),
                             [](std::uint8_t first, std::uint8_t second) {
                                 return first == 0xFFU && (second & 0xE0U) == 0xE0U;
                             }) != mp3Bytes.end());

    // The final path remains below MAX_PATH, while the old temporary naming
    // scheme (which repeated this long stem plus an encoding suffix) did not.
    const std::size_t longStemLength = 245 - folder.wstring().size() - 1 - 4;
    const auto longMp3Path = folder / (std::wstring(longStemLength, L'L') + L".mp3");
    CHECK(longMp3Path.wstring().size() == 245);
    error.clear();
    CHECK(neon::writeHighQualityMp3(rawPath, frames, longMp3Path, metadata, error));
    CHECK(error.empty());
    CHECK(std::filesystem::exists(longMp3Path));

    const auto silentPath = folder / L"silent.pcm";
    const auto silentWavePath = folder / L"silent.wav";
    {
        std::ofstream silent(silentPath, std::ios::binary | std::ios::trunc);
        const std::vector<std::int16_t> silence(neon::recorderSampleRate * 2);
        silent.write(reinterpret_cast<const char*>(silence.data()),
                     static_cast<std::streamsize>(silence.size() * sizeof(std::int16_t)));
    }
    error.clear();
    measuredAudibleFrames = 0;
    CHECK(!neon::measureAudibleRecording(silentPath, neon::recorderSampleRate,
                                         measuredAudibleFrames, error));
    CHECK(measuredAudibleFrames == 0);
    CHECK(!error.empty());
    error.clear();
    CHECK(!neon::writeCdQualityWave(silentPath, neon::recorderSampleRate,
                                    silentWavePath, metadata, error));
    CHECK(!error.empty());
    CHECK(!std::filesystem::exists(silentWavePath));

    CHECK(neon::safeRecordingFileName(L"A: B? / C*") == L"A_ B_ _ C_");
    CHECK(neon::safeRecordingFileName(L"   ") == L"Yeni Kayıt");
    CHECK(neon::safeRecordingFileName(L"Test...  ") == L"Test");
    CHECK(!neon::automaticRecordingIsLongEnough(
        neon::automaticMinimumRecordingFrames - 1));
    CHECK(!neon::automaticRecordingIsLongEnough(
        neon::automaticMinimumRecordingFrames));
    CHECK(neon::automaticRecordingIsLongEnough(
        neon::automaticMinimumRecordingFrames + 1));
    constexpr std::uint64_t hundredSeconds = 100ULL * 10'000'000;
    CHECK(neon::automaticRecordingDurationMatches(90ULL * 10'000'000, hundredSeconds));
    CHECK(neon::automaticRecordingDurationMatches(110ULL * 10'000'000, hundredSeconds));
    CHECK(!neon::automaticRecordingDurationMatches(89ULL * 10'000'000, hundredSeconds));
    CHECK(!neon::automaticRecordingDurationMatches(111ULL * 10'000'000, hundredSeconds));
    CHECK(neon::automaticRecordingFolder(L"C:\\Music\\Neon Recorder", L"Haluk Levent",
                                          L"Bu Ateş Sönmez") ==
          std::filesystem::path(
              L"C:\\Music\\Neon Recorder\\Haluk Levent\\Bu Ateş Sönmez"));
    CHECK(neon::automaticRecordingFolder(L"C:\\Music\\Neon Recorder", L"  ", L"  ") ==
          std::filesystem::path(
              L"C:\\Music\\Neon Recorder\\Sanatçısı Bilinmeyenler\\Albümü Bilinmeyenler"));
    CHECK(neon::automaticRecordingFolder(L"C:\\Music\\Neon Recorder", L"Sanatçı: Canlı?",
                                          L"Albüm: Canlı?") ==
          std::filesystem::path(
              L"C:\\Music\\Neon Recorder\\Sanatçı_ Canlı_\\Albüm_ Canlı_"));

    const auto automaticRoot = folder / L"automatic";
    const auto metadataFolder = neon::automaticRecordingFolder(
        automaticRoot, metadata.artist, metadata.album);
    CHECK(!neon::automaticRecordingAlreadyExists(automaticRoot, metadata));
    std::filesystem::create_directories(metadataFolder, ignored);
    const auto existingMp3 = metadataFolder /
        (neon::safeRecordingFileName(metadata.artist + L" - " + metadata.title) + L".mp3");
    {
        std::ofstream existing(existingMp3, std::ios::binary);
        existing.put('\0');
    }
    CHECK(neon::automaticRecordingAlreadyExists(automaticRoot, metadata));
    constexpr std::uint64_t ticksPerSecond = 10'000'000;
    const std::uint64_t expectedDurationTicks =
        trimmedFrames * ticksPerSecond / neon::recorderSampleRate;
    CHECK(!neon::automaticRecordingMatchesDuration(
        automaticRoot, metadata, expectedDurationTicks));
    std::filesystem::remove(existingMp3, ignored);
    const auto existingWave = metadataFolder /
        (neon::safeRecordingFileName(metadata.artist + L" - " + metadata.title) + L".wav");
    std::filesystem::copy_file(wavePath, existingWave,
                               std::filesystem::copy_options::overwrite_existing, ignored);
    CHECK(!ignored);
    CHECK(neon::automaticRecordingMatchesDuration(
        automaticRoot, metadata, expectedDurationTicks));
    CHECK(!neon::automaticRecordingMatchesDuration(
        automaticRoot, metadata, expectedDurationTicks + 20 * ticksPerSecond));
    const auto numberedWave = metadataFolder /
        (neon::safeRecordingFileName(metadata.artist + L" - " + metadata.title) + L" (2).wav");
    std::filesystem::rename(existingWave, numberedWave, ignored);
    CHECK(!ignored);
    CHECK(neon::automaticRecordingMatchesDuration(
        automaticRoot, metadata, expectedDurationTicks));
    std::filesystem::copy_file(wavePath, existingWave,
                               std::filesystem::copy_options::overwrite_existing, ignored);
    CHECK(!ignored);
    neon::removeSupersededAutomaticRecordings(automaticRoot, metadata, existingWave);
    CHECK(std::filesystem::exists(existingWave));
    CHECK(!std::filesystem::exists(numberedWave));
    auto incompleteMetadata = metadata;
    incompleteMetadata.album.clear();
    CHECK(!neon::automaticRecordingAlreadyExists(automaticRoot, incompleteMetadata));

    std::filesystem::remove_all(folder, ignored);
    if (SUCCEEDED(comResult)) CoUninitialize();
    if (failures == 0) std::cout << "All Neon Recorder file tests passed.\n";
    return failures == 0 ? 0 : 1;
}
