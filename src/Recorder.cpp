#include "neon/Recorder.hpp"

#include <Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace neon {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint16_t waveFormatPcm = 1;
constexpr std::uint16_t waveChannels = recorderChannels;
constexpr std::uint32_t waveRate = recorderSampleRate;
constexpr std::uint16_t waveBits = recorderBitsPerSample;
constexpr std::uint16_t waveBlockAlign = waveChannels * waveBits / 8;
constexpr std::uint32_t waveByteRate = waveRate * waveBlockAlign;

std::wstring windowsError(HRESULT result, const wchar_t* context) {
    wchar_t* message{};
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                          FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, static_cast<DWORD>(result), 0,
                                      reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring output = context;
    wchar_t code[16]{};
    swprintf_s(code, L" (0x%08X)", static_cast<unsigned int>(result));
    output += code;
    if (size && message) {
        output += L": ";
        output.append(message, size);
        while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n')) {
            output.pop_back();
        }
    }
    if (message) LocalFree(message);
    return output;
}

std::wstring lastError(const wchar_t* context) {
    return windowsError(HRESULT_FROM_WIN32(GetLastError()), context);
}

std::string toUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), output.data(), count, nullptr, nullptr);
    return output;
}

std::string toSystemAnsi(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_ACP, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
                        output.data(), count, nullptr, nullptr);
    return output;
}

void writeU16(std::ostream& stream, std::uint16_t value) {
    const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8)};
    stream.write(bytes, sizeof(bytes));
}

void writeU32(std::ostream& stream, std::uint32_t value) {
    const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8),
                       static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    stream.write(bytes, sizeof(bytes));
}

void writeFourCc(std::ostream& stream, const char (&value)[5]) {
    stream.write(value, 4);
}

struct InfoField {
    const char* id;
    std::string value;
};

std::vector<InfoField> metadataFields(const RecordingMetadata& metadata) {
    std::vector<InfoField> fields;
    auto append = [&fields](const char* id, std::wstring_view value) {
        if (!value.empty()) fields.push_back({id, toSystemAnsi(value)});
    };
    append("INAM", metadata.title);
    append("IART", metadata.artist);
    append("IPRD", metadata.album);
    append("ICRD", metadata.albumYear);
    fields.push_back({"ISFT", "Neon Recorder"});
    fields.push_back({"ICMT", "44.1 kHz / 16-bit stereo PCM"});
    return fields;
}

std::uint32_t infoListSize(const std::vector<InfoField>& fields) {
    std::uint64_t size = 4;
    for (const auto& field : fields) {
        const std::uint64_t payload = field.value.size() + 1;
        size += 8 + payload + (payload & 1U);
    }
    return static_cast<std::uint32_t>(size);
}

void appendSyncSafe(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 21) & 0x7FU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 14) & 0x7FU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7FU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0x7FU));
}

void appendId3TextFrame(std::vector<std::uint8_t>& tag, const char (&id)[5],
                        std::wstring_view value) {
    if (value.empty()) return;
    const std::string text = toUtf8(value);
    const std::uint32_t payloadSize = static_cast<std::uint32_t>(text.size() + 1);
    tag.insert(tag.end(), id, id + 4);
    appendSyncSafe(tag, payloadSize);
    tag.push_back(0);
    tag.push_back(0);
    tag.push_back(3);  // ID3 text encoding: UTF-8
    tag.insert(tag.end(), text.begin(), text.end());
}

std::string artworkMimeType(const RecordingMetadata& metadata) {
    if (!metadata.artworkMimeType.empty()) return metadata.artworkMimeType;
    if (metadata.artwork.size() >= 8 &&
        std::equal(metadata.artwork.begin(), metadata.artwork.begin() + 8,
                   std::array<std::uint8_t, 8>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}.begin())) {
        return "image/png";
    }
    return "image/jpeg";
}

void appendId3PictureFrame(std::vector<std::uint8_t>& tag,
                           const RecordingMetadata& metadata) {
    if (metadata.artwork.empty()) return;
    const auto mime = artworkMimeType(metadata);
    const std::uint64_t payloadSize64 = 1 + mime.size() + 1 + 1 + 1 + metadata.artwork.size();
    if (payloadSize64 > 0x0FFFFFFFU) return;

    tag.insert(tag.end(), {'A', 'P', 'I', 'C'});
    appendSyncSafe(tag, static_cast<std::uint32_t>(payloadSize64));
    tag.push_back(0);
    tag.push_back(0);
    tag.push_back(3);  // Description encoding: UTF-8.
    tag.insert(tag.end(), mime.begin(), mime.end());
    tag.push_back(0);
    tag.push_back(3);  // Front cover.
    tag.push_back(0);  // Empty UTF-8 description.
    tag.insert(tag.end(), metadata.artwork.begin(), metadata.artwork.end());
}

std::vector<std::uint8_t> makeId3Tag(const RecordingMetadata& metadata) {
    std::vector<std::uint8_t> frames;
    appendId3TextFrame(frames, "TIT2", metadata.title);
    appendId3TextFrame(frames, "TPE1", metadata.artist);
    appendId3TextFrame(frames, "TALB", metadata.album);
    appendId3TextFrame(frames, "TDRC", metadata.albumYear);
    appendId3TextFrame(frames, "TENC", L"Neon Recorder");
    appendId3PictureFrame(frames, metadata);

    std::vector<std::uint8_t> tag;
    tag.reserve(10 + frames.size());
    tag.insert(tag.end(), {'I', 'D', '3', 4, 0, 0});
    appendSyncSafe(tag, static_cast<std::uint32_t>(frames.size()));
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

std::filesystem::path partialPath(const std::filesystem::path& directory,
                                  std::wstring_view suffix) {
    static std::atomic_uint64_t sequence{};
    return directory /
           (L"NeonRecorderTemp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) +
            std::wstring(suffix));
}

bool checkMediaResult(HRESULT result, const wchar_t* context, std::wstring& error) {
    if (SUCCEEDED(result)) return true;
    error = windowsError(result, context);
    return false;
}

struct AudibleRange {
    std::uint64_t startFrame{};
    std::uint64_t frameCount{};
};

bool findAudibleRange(const std::filesystem::path& rawPcmPath,
                      std::uint64_t totalFrames,
                      AudibleRange& range,
                      std::wstring& error) {
    if (totalFrames == 0) {
        error = L"Kayıtta ses algılanmadı.";
        return false;
    }
    std::ifstream input(rawPcmPath, std::ios::binary);
    if (!input) {
        error = L"Geçici kayıt verisi açılamadı: " + rawPcmPath.wstring();
        return false;
    }

    constexpr std::uint64_t analysisFrames = waveRate / 100;  // 10 ms
    constexpr std::uint32_t qualifyingBlocks = 3;             // Ignore brief clicks.
    constexpr double audibleRms = 0.001;                      // -60 dBFS.
    constexpr std::uint64_t edgePadding = waveRate * 30 / 1000;  // Preserve attacks/tails.
    std::vector<std::int16_t> samples(static_cast<std::size_t>(analysisFrames) * waveChannels);

    std::uint64_t firstAny = totalFrames;
    std::uint64_t lastAny{};
    std::uint64_t firstQualified = totalFrames;
    std::uint64_t lastQualified{};
    std::uint64_t runStart{};
    std::uint32_t runLength{};

    for (std::uint64_t offset = 0; offset < totalFrames; offset += analysisFrames) {
        const std::uint64_t frames = std::min(analysisFrames, totalFrames - offset);
        const auto bytes = static_cast<std::streamsize>(frames * waveBlockAlign);
        input.read(reinterpret_cast<char*>(samples.data()), bytes);
        if (input.gcount() != bytes) {
            error = L"Geçici kayıt verisi eksik veya bozuk.";
            return false;
        }
        double squareSum{};
        const std::size_t sampleCount = static_cast<std::size_t>(frames) * waveChannels;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const double normalized = static_cast<double>(samples[index]) / 32768.0;
            squareSum += normalized * normalized;
        }
        const double rms = std::sqrt(squareSum / static_cast<double>(sampleCount));
        if (rms >= audibleRms) {
            if (firstAny == totalFrames) firstAny = offset;
            lastAny = offset + frames;
            if (runLength == 0) runStart = offset;
            ++runLength;
            if (runLength >= qualifyingBlocks) {
                if (firstQualified == totalFrames) firstQualified = runStart;
                lastQualified = offset + frames;
            }
        } else {
            runLength = 0;
        }
    }

    const bool hasQualifiedAudio = firstQualified != totalFrames;
    const std::uint64_t detectedStart = hasQualifiedAudio ? firstQualified : firstAny;
    const std::uint64_t detectedEnd = hasQualifiedAudio ? lastQualified : lastAny;
    if (detectedStart == totalFrames || detectedEnd <= detectedStart) {
        error = L"Kayıtta ses algılanmadı. Parça çalarken yeniden kayıt yapın.";
        return false;
    }
    const std::uint64_t paddedStart = detectedStart > edgePadding ? detectedStart - edgePadding : 0;
    const std::uint64_t paddedEnd = std::min(totalFrames, detectedEnd + edgePadding);
    range = {paddedStart, paddedEnd - paddedStart};
    return true;
}

std::filesystem::path uniqueTemporaryPcm(const std::filesystem::path& recordingRoot,
                                         std::wstring& error) {
    if (recordingRoot.empty()) {
        error = L"Geçici kayıt klasörü için ana kayıt klasörü seçilmedi.";
        return {};
    }

    const auto directory = recordingRoot / L"_TMP";
    std::error_code folderError;
    std::filesystem::create_directories(directory, folderError);
    if (folderError) {
        error = L"Geçici kayıt klasörü oluşturulamadı: " + directory.wstring();
        return {};
    }

    static std::atomic_uint64_t sequence{};
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        const auto fileName = L"NeonRecorder-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64()) + L"-" +
                              std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) +
                              L".pcm";
        const auto file = directory / fileName;
        const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_TEMPORARY |
                                              FILE_FLAG_SEQUENTIAL_SCAN,
                                          nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return file;
        }
        const DWORD createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) {
            SetLastError(createError);
            error = lastError(L"Geçici kayıt dosyası oluşturulamadı");
            return {};
        }
    }
    error = L"Benzersiz geçici kayıt dosyası adı oluşturulamadı.";
    return {};
}

enum class SampleEncoding { Pcm, Float, Unsupported };

struct CaptureFormat {
    SampleEncoding encoding{SampleEncoding::Unsupported};
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::uint16_t containerBits{};
    std::uint16_t validBits{};
    std::uint16_t blockAlign{};
    std::uint32_t channelMask{};
};

CaptureFormat describeFormat(const WAVEFORMATEX* format) {
    CaptureFormat result;
    if (!format) return result;
    result.sampleRate = format->nSamplesPerSec;
    result.channels = format->nChannels;
    result.containerBits = format->wBitsPerSample;
    result.validBits = format->wBitsPerSample;
    result.blockAlign = format->nBlockAlign;
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        result.encoding = SampleEncoding::Pcm;
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        result.encoding = SampleEncoding::Float;
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        result.validBits = extended->Samples.wValidBitsPerSample;
        result.channelMask = extended->dwChannelMask;
        if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            result.encoding = SampleEncoding::Pcm;
        } else if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            result.encoding = SampleEncoding::Float;
        }
    }
    return result;
}

float decodeSample(const std::uint8_t* source, const CaptureFormat& format) {
    if (format.encoding == SampleEncoding::Float) {
        if (format.containerBits == 32) {
            float value{};
            std::memcpy(&value, source, sizeof(value));
            return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
        }
        if (format.containerBits == 64) {
            double value{};
            std::memcpy(&value, source, sizeof(value));
            return std::isfinite(value) ? static_cast<float>(std::clamp(value, -1.0, 1.0)) : 0.0F;
        }
        return 0.0F;
    }
    if (format.containerBits == 8) {
        return (static_cast<float>(*source) - 128.0F) / 128.0F;
    }
    if (format.containerBits == 16) {
        std::int16_t value{};
        std::memcpy(&value, source, sizeof(value));
        return static_cast<float>(value) / 32768.0F;
    }
    if (format.containerBits == 24) {
        std::int32_t value = static_cast<std::int32_t>(source[0]) |
                             (static_cast<std::int32_t>(source[1]) << 8) |
                             (static_cast<std::int32_t>(source[2]) << 16);
        if (value & 0x00800000) value |= static_cast<std::int32_t>(0xFF000000);
        return static_cast<float>(value) / 8388608.0F;
    }
    if (format.containerBits == 32) {
        std::int32_t value{};
        std::memcpy(&value, source, sizeof(value));
        const std::uint16_t valid = std::clamp<std::uint16_t>(format.validBits, 1, 32);
        if (valid < 32) value >>= (32 - valid);
        return static_cast<float>(static_cast<double>(value) /
                                  std::ldexp(1.0, static_cast<int>(valid - 1)));
    }
    return 0.0F;
}

class CdPcmConverter {
public:
    explicit CdPcmConverter(CaptureFormat source)
        : source_(source), step_(static_cast<double>(source.sampleRate) / waveRate) {}

    bool valid() const {
        return source_.encoding != SampleEncoding::Unsupported && source_.sampleRate > 0 &&
               source_.channels > 0 && source_.blockAlign >= source_.channels * source_.containerBits / 8 &&
               (source_.containerBits == 8 || source_.containerBits == 16 ||
                source_.containerBits == 24 || source_.containerBits == 32 ||
                (source_.encoding == SampleEncoding::Float && source_.containerBits == 64));
    }

    void append(const std::uint8_t* source, std::uint32_t frames, bool silent,
                std::vector<std::int16_t>& output) {
        const std::size_t sampleBytes = source_.containerBits / 8;
        pending_.reserve(pending_.size() + static_cast<std::size_t>(frames) * 2);
        std::vector<float> channels(source_.channels);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            if (silent) {
                std::fill(channels.begin(), channels.end(), 0.0F);
            } else {
                const auto* frameData = source + static_cast<std::size_t>(frame) * source_.blockAlign;
                for (std::uint16_t channel = 0; channel < source_.channels; ++channel) {
                    channels[channel] = decodeSample(frameData + channel * sampleBytes, source_);
                }
            }
            const auto [left, right] = stereo(channels);
            pending_.push_back(left);
            pending_.push_back(right);
        }
        render(output, false);
    }

    void finish(std::vector<std::int16_t>& output) { render(output, true); }

private:
    std::pair<float, float> stereo(std::span<const float> channels) const {
        if (channels.size() == 1) return {channels[0], channels[0]};
        if (channels.size() == 2) return {channels[0], channels[1]};

        float left{};
        float right{};
        float leftWeight{};
        float rightWeight{};
        auto mix = [&](float sample, float leftGain, float rightGain) {
            left += sample * leftGain;
            right += sample * rightGain;
            leftWeight += leftGain;
            rightWeight += rightGain;
        };

        // WAVEFORMATEXTENSIBLE channel order follows the ascending speaker-mask
        // bits. This preserves front stereo and folds centre/surround channels
        // down without clipping. The fallback matches the common 5.1 order.
        if (source_.channelMask) {
            std::size_t index{};
            for (std::uint32_t bit = 1; bit && index < channels.size(); bit <<= 1) {
                if (!(source_.channelMask & bit)) continue;
                const float sample = channels[index++];
                switch (bit) {
                    case SPEAKER_FRONT_LEFT: mix(sample, 1.0F, 0.0F); break;
                    case SPEAKER_FRONT_RIGHT: mix(sample, 0.0F, 1.0F); break;
                    case SPEAKER_FRONT_CENTER: mix(sample, 0.707F, 0.707F); break;
                    case SPEAKER_LOW_FREQUENCY: mix(sample, 0.25F, 0.25F); break;
                    case SPEAKER_BACK_LEFT:
                    case SPEAKER_SIDE_LEFT:
                    case SPEAKER_FRONT_LEFT_OF_CENTER: mix(sample, 0.707F, 0.0F); break;
                    case SPEAKER_BACK_RIGHT:
                    case SPEAKER_SIDE_RIGHT:
                    case SPEAKER_FRONT_RIGHT_OF_CENTER: mix(sample, 0.0F, 0.707F); break;
                    case SPEAKER_BACK_CENTER: mix(sample, 0.5F, 0.5F); break;
                    default: mix(sample, 0.25F, 0.25F); break;
                }
            }
        } else {
            mix(channels[0], 1.0F, 0.0F);
            mix(channels[1], 0.0F, 1.0F);
            for (std::size_t index = 2; index < channels.size(); ++index) {
                mix(channels[index], 0.35F, 0.35F);
            }
        }
        return {std::clamp(left / std::max(1.0F, leftWeight), -1.0F, 1.0F),
                std::clamp(right / std::max(1.0F, rightWeight), -1.0F, 1.0F)};
    }

    float dither() {
        auto random = [this]() {
            randomState_ ^= randomState_ << 13;
            randomState_ ^= randomState_ >> 17;
            randomState_ ^= randomState_ << 5;
            return static_cast<float>(randomState_ & 0xFFFFU) / 65535.0F;
        };
        return (random() - random()) / 65536.0F;
    }

    std::int16_t quantize(float sample) {
        const float adjusted = std::clamp(sample + dither(), -1.0F, 0.999969F);
        return static_cast<std::int16_t>(std::lrint(adjusted * 32768.0F));
    }

    void render(std::vector<std::int16_t>& output, bool finishing) {
        std::size_t frames = pending_.size() / 2;
        if (frames == 0) return;
        while (nextPosition_ + 1.0 < static_cast<double>(frames) ||
               (finishing && nextPosition_ < static_cast<double>(frames))) {
            const auto first = static_cast<std::size_t>(nextPosition_);
            const auto second = std::min(first + 1, frames - 1);
            const float fraction = static_cast<float>(nextPosition_ - static_cast<double>(first));
            const float left = std::lerp(pending_[first * 2], pending_[second * 2], fraction);
            const float right = std::lerp(pending_[first * 2 + 1], pending_[second * 2 + 1], fraction);
            output.push_back(quantize(left));
            output.push_back(quantize(right));
            nextPosition_ += step_;
        }
        const auto consumed = std::min(static_cast<std::size_t>(nextPosition_), frames);
        if (consumed) {
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed * 2));
            nextPosition_ -= static_cast<double>(consumed);
        }
    }

    CaptureFormat source_;
    double step_{};
    double nextPosition_{};
    std::vector<float> pending_;
    std::uint32_t randomState_{0x4E454F4EU};
};

class MultimediaThread {
public:
    MultimediaThread() { handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &index_); }
    ~MultimediaThread() { if (handle_) AvRevertMmThreadCharacteristics(handle_); }
private:
    DWORD index_{};
    HANDLE handle_{};
};

}  // namespace

bool measureAudibleRecording(const std::filesystem::path& rawPcmPath,
                             std::uint64_t sampleFrames,
                             std::uint64_t& audibleFrames,
                             std::wstring& error) {
    AudibleRange range;
    audibleFrames = 0;
    error.clear();
    if (!findAudibleRange(rawPcmPath, sampleFrames, range, error)) return false;
    audibleFrames = range.frameCount;
    return true;
}

struct LoopbackRecorder::Impl {
    ~Impl() {
        requestStop.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
        removeTemporary();
    }

    void removeTemporary() {
        std::error_code ignored;
        if (!temporaryPath.empty()) std::filesystem::remove(temporaryPath, ignored);
        temporaryPath.clear();
    }

    void finishStartup(bool success, std::wstring message = {}) {
        {
            std::lock_guard lock(mutex);
            startupComplete = true;
            startupSucceeded = success;
            if (!message.empty()) error = std::move(message);
        }
        condition.notify_all();
    }

    void failDuringCapture(std::wstring message) {
        std::lock_guard lock(mutex);
        error = std::move(message);
    }

    void capture() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            finishStartup(false, windowsError(comResult, L"Windows ses sistemi başlatılamadı"));
            return;
        }
        struct CoCleanup { ~CoCleanup() { CoUninitialize(); } } coCleanup;
        MultimediaThread multimediaThread;

        std::ofstream raw(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!raw) {
            finishStartup(false, L"Geçici kayıt dosyası açılamadı.");
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) {
            finishStartup(false, windowsError(result, L"Ses aygıtları açılamadı"));
            return;
        }
        ComPtr<IMMDevice> device;
        result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result)) {
            finishStartup(false, windowsError(result, L"Varsayılan ses çıkışı bulunamadı"));
            return;
        }
        ComPtr<IAudioClient> client;
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(result)) {
            finishStartup(false, windowsError(result, L"Ses çıkışı dinlenemedi"));
            return;
        }

        WAVEFORMATEX* allocatedFormat{};
        result = client->GetMixFormat(&allocatedFormat);
        if (FAILED(result) || !allocatedFormat) {
            finishStartup(false, windowsError(result, L"Ses biçimi okunamadı"));
            return;
        }
        struct FormatCleanup {
            WAVEFORMATEX* value;
            ~FormatCleanup() { CoTaskMemFree(value); }
        } formatCleanup{allocatedFormat};

        const CaptureFormat captureFormat = describeFormat(allocatedFormat);
        CdPcmConverter converter(captureFormat);
        if (!converter.valid()) {
            finishStartup(false, L"Windows ses çıkışının örnek biçimi desteklenmiyor.");
            return;
        }

        constexpr REFERENCE_TIME bufferDuration = 1'000'000;  // 100 ms
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufferDuration, 0, allocatedFormat, nullptr);
        if (FAILED(result)) {
            finishStartup(false, windowsError(result, L"Sistem sesi kaydı başlatılamadı"));
            return;
        }
        ComPtr<IAudioCaptureClient> captureClient;
        result = client->GetService(IID_PPV_ARGS(&captureClient));
        if (FAILED(result)) {
            finishStartup(false, windowsError(result, L"Kayıt akışı açılamadı"));
            return;
        }
        result = client->Start();
        if (FAILED(result)) {
            finishStartup(false, windowsError(result, L"Kayıt başlatılamadı"));
            return;
        }
        finishStartup(true);

        std::vector<std::int16_t> converted;
        converted.reserve(8192);
        bool failed{};
        while (!requestStop.load(std::memory_order_acquire)) {
            UINT32 packetFrames{};
            result = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(result)) {
                failDuringCapture(windowsError(result, L"Kayıt akışı kesildi"));
                failed = true;
                break;
            }
            if (!packetFrames) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            while (packetFrames) {
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                result = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(result)) {
                    failDuringCapture(windowsError(result, L"Ses verisi okunamadı"));
                    failed = true;
                    break;
                }
                converted.clear();
                converter.append(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, converted);
                if (!converted.empty()) {
                    raw.write(reinterpret_cast<const char*>(converted.data()),
                              static_cast<std::streamsize>(converted.size() * sizeof(std::int16_t)));
                    if (!raw) {
                        captureClient->ReleaseBuffer(frames);
                        failDuringCapture(L"Kayıt diske yazılamadı. Boş alanı kontrol edin.");
                        failed = true;
                        break;
                    }
                    float packetLeft{};
                    float packetRight{};
                    for (std::size_t index = 0; index + 1 < converted.size(); index += 2) {
                        packetLeft = std::max(packetLeft, std::abs(static_cast<float>(converted[index]) / 32768.0F));
                        packetRight = std::max(packetRight, std::abs(static_cast<float>(converted[index + 1]) / 32768.0F));
                    }
                    peakLeft.store(packetLeft, std::memory_order_relaxed);
                    peakRight.store(packetRight, std::memory_order_relaxed);
                    sampleFrames.fetch_add(converted.size() / 2, std::memory_order_relaxed);
                }
                captureClient->ReleaseBuffer(frames);
                if (failed) break;
                result = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(result)) {
                    failDuringCapture(windowsError(result, L"Kayıt akışı kesildi"));
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        }

        client->Stop();
        converted.clear();
        converter.finish(converted);
        if (!converted.empty() && raw) {
            raw.write(reinterpret_cast<const char*>(converted.data()),
                      static_cast<std::streamsize>(converted.size() * sizeof(std::int16_t)));
            sampleFrames.fetch_add(converted.size() / 2, std::memory_order_relaxed);
        }
        raw.flush();
        if (!raw && !failed) failDuringCapture(L"Kayıt dosyası tamamlanamadı.");
        raw.close();
        recording.store(false, std::memory_order_release);
        hasRecording.store(sampleFrames.load(std::memory_order_relaxed) > 0,
                           std::memory_order_release);
        peakLeft.store(0.0F, std::memory_order_relaxed);
        peakRight.store(0.0F, std::memory_order_relaxed);
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    std::filesystem::path temporaryPath;
    std::atomic_bool requestStop{};
    std::atomic_bool recording{};
    std::atomic_bool hasRecording{};
    std::atomic<std::uint64_t> sampleFrames{};
    std::atomic<float> peakLeft{};
    std::atomic<float> peakRight{};
    bool startupComplete{};
    bool startupSucceeded{};
    std::wstring error;
};

LoopbackRecorder::LoopbackRecorder() : impl_(std::make_unique<Impl>()) {}
LoopbackRecorder::~LoopbackRecorder() = default;

bool LoopbackRecorder::start(const std::filesystem::path& recordingRoot,
                             std::wstring& error) {
    discard();
    error.clear();
    impl_->temporaryPath = uniqueTemporaryPcm(recordingRoot, error);
    if (impl_->temporaryPath.empty()) return false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->startupComplete = false;
        impl_->startupSucceeded = false;
        impl_->error.clear();
    }
    impl_->requestStop.store(false, std::memory_order_release);
    impl_->recording.store(true, std::memory_order_release);
    impl_->hasRecording.store(false, std::memory_order_release);
    impl_->sampleFrames.store(0, std::memory_order_relaxed);
    impl_->peakLeft.store(0.0F, std::memory_order_relaxed);
    impl_->peakRight.store(0.0F, std::memory_order_relaxed);
    impl_->worker = std::thread([implementation = impl_.get()] { implementation->capture(); });

    std::unique_lock lock(impl_->mutex);
    const bool signalled = impl_->condition.wait_for(lock, std::chrono::seconds(10),
                                                     [this] { return impl_->startupComplete; });
    const bool success = signalled && impl_->startupSucceeded;
    if (!success) {
        error = signalled ? impl_->error : L"Ses sistemi zamanında yanıt vermedi.";
        lock.unlock();
        impl_->requestStop.store(true, std::memory_order_release);
        if (impl_->worker.joinable()) impl_->worker.join();
        impl_->recording.store(false, std::memory_order_release);
        impl_->removeTemporary();
        return false;
    }
    return true;
}

bool LoopbackRecorder::stop(std::wstring& error) {
    error.clear();
    impl_->requestStop.store(true, std::memory_order_release);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->recording.store(false, std::memory_order_release);
    {
        std::lock_guard lock(impl_->mutex);
        error = impl_->error;
    }
    const bool hasAudio = impl_->hasRecording.load(std::memory_order_acquire);
    if (!hasAudio && error.empty()) error = L"Kaydedilmiş ses bulunamadı.";
    return hasAudio;
}

void LoopbackRecorder::discard() {
    impl_->requestStop.store(true, std::memory_order_release);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->recording.store(false, std::memory_order_release);
    impl_->hasRecording.store(false, std::memory_order_release);
    impl_->sampleFrames.store(0, std::memory_order_relaxed);
    impl_->peakLeft.store(0.0F, std::memory_order_relaxed);
    impl_->peakRight.store(0.0F, std::memory_order_relaxed);
    impl_->removeTemporary();
    std::lock_guard lock(impl_->mutex);
    impl_->error.clear();
}

CapturedRecording LoopbackRecorder::takeRecording() {
    if (impl_->recording.load(std::memory_order_acquire) ||
        !impl_->hasRecording.load(std::memory_order_acquire)) {
        return {};
    }
    CapturedRecording result{impl_->temporaryPath,
                             impl_->sampleFrames.load(std::memory_order_relaxed)};
    impl_->temporaryPath.clear();
    impl_->hasRecording.store(false, std::memory_order_release);
    impl_->sampleFrames.store(0, std::memory_order_relaxed);
    impl_->peakLeft.store(0.0F, std::memory_order_relaxed);
    impl_->peakRight.store(0.0F, std::memory_order_relaxed);
    std::lock_guard lock(impl_->mutex);
    impl_->error.clear();
    return result;
}

RecorderSnapshot LoopbackRecorder::snapshot() const {
    RecorderSnapshot result;
    result.recording = impl_->recording.load(std::memory_order_acquire);
    result.hasRecording = impl_->hasRecording.load(std::memory_order_acquire);
    result.sampleFrames = impl_->sampleFrames.load(std::memory_order_relaxed);
    result.peakLeft = impl_->peakLeft.load(std::memory_order_relaxed);
    result.peakRight = impl_->peakRight.load(std::memory_order_relaxed);
    std::lock_guard lock(impl_->mutex);
    result.error = impl_->error;
    return result;
}

std::filesystem::path LoopbackRecorder::temporaryPcmPath() const {
    return impl_->temporaryPath;
}

bool writeCdQualityWave(const std::filesystem::path& rawPcmPath,
                        std::uint64_t sampleFrames,
                        const std::filesystem::path& destination,
                        const RecordingMetadata& metadata,
                        std::wstring& error) {
    error.clear();
    std::error_code fileError;
    const std::uint64_t availableBytes = std::filesystem::file_size(rawPcmPath, fileError);
    const std::uint64_t originalDataBytes = sampleFrames * waveBlockAlign;
    if (fileError || availableBytes < originalDataBytes) {
        error = L"Geçici kayıt verisi bulunamadı veya tamamlanmamış.\nYol: " +
                rawPcmPath.wstring() + L"\nBeklenen: " +
                std::to_wstring(originalDataBytes) + L" bayt\nBulunan: " +
                std::to_wstring(fileError ? 0 : availableBytes) + L" bayt";
        return false;
    }
    AudibleRange audible;
    if (!findAudibleRange(rawPcmPath, sampleFrames, audible, error)) return false;
    const std::uint64_t dataBytes64 = audible.frameCount * waveBlockAlign;
    const auto fields = metadataFields(metadata);
    const std::uint32_t listSize = infoListSize(fields);
    const auto id3 = makeId3Tag(metadata);
    const std::uint64_t riffSize64 = 4 + (8 + 16) + (8 + dataBytes64 + (dataBytes64 & 1U)) +
                                     (8 + 8) +
                                     (8 + listSize + (listSize & 1U)) +
                                     (8 + id3.size() + (id3.size() & 1U));
    if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() ||
        riffSize64 > std::numeric_limits<std::uint32_t>::max()) {
        error = L"Kayıt klasik WAV sınırı olan 4 GB'ı aşıyor.";
        return false;
    }

    const auto partial = partialPath(destination.parent_path(), L".part.wav");
    std::ifstream input(rawPcmPath, std::ios::binary);
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    auto cleanPartial = [&] {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
    };
    if (!input || !output) {
        cleanPartial();
        error = L"Seçilen klasörde dosya oluşturulamadı.";
        return false;
    }
    input.seekg(static_cast<std::streamoff>(audible.startFrame * waveBlockAlign));
    if (!input) {
        cleanPartial();
        error = L"Geçici kayıt verisinde kırpılacak konuma ulaşılamadı.";
        return false;
    }

    writeFourCc(output, "RIFF");
    writeU32(output, static_cast<std::uint32_t>(riffSize64));
    writeFourCc(output, "WAVE");
    writeFourCc(output, "fmt ");
    writeU32(output, 16);
    writeU16(output, waveFormatPcm);
    writeU16(output, waveChannels);
    writeU32(output, waveRate);
    writeU32(output, waveByteRate);
    writeU16(output, waveBlockAlign);
    writeU16(output, waveBits);
    writeFourCc(output, "data");
    writeU32(output, static_cast<std::uint32_t>(dataBytes64));

    std::vector<char> copyBuffer(1024 * 1024);
    std::uint64_t remaining = dataBytes64;
    while (remaining) {
        const auto request = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, copyBuffer.size()));
        input.read(copyBuffer.data(), request);
        if (input.gcount() != request) {
            cleanPartial();
            error = L"Geçici kayıt verisi eksik veya bozuk.";
            return false;
        }
        output.write(copyBuffer.data(), request);
        remaining -= static_cast<std::uint64_t>(request);
    }
    if (dataBytes64 & 1U) output.put('\0');

    // Windows Explorer reads WAV LIST/INFO through the system ANSI code page.
    // CSET records that code page explicitly for other RIFF-aware readers.
    writeFourCc(output, "CSET");
    writeU32(output, 8);
    writeU16(output, static_cast<std::uint16_t>(GetACP()));
    writeU16(output, 0);      // Country
    writeU16(output, 0);      // Language
    writeU16(output, 0);      // Dialect

    writeFourCc(output, "LIST");
    writeU32(output, listSize);
    writeFourCc(output, "INFO");
    for (const auto& field : fields) {
        output.write(field.id, 4);
        const auto payload = static_cast<std::uint32_t>(field.value.size() + 1);
        writeU32(output, payload);
        output.write(field.value.data(), static_cast<std::streamsize>(field.value.size()));
        output.put('\0');
        if (payload & 1U) output.put('\0');
    }
    if (listSize & 1U) output.put('\0');

    // An ID3 chunk gives WAV files the same Unicode/year/artwork metadata as
    // MP3. TagLib (used by Neon Jukebox) recognizes APIC from this chunk.
    writeFourCc(output, "ID3 ");
    writeU32(output, static_cast<std::uint32_t>(id3.size()));
    output.write(reinterpret_cast<const char*>(id3.data()),
                 static_cast<std::streamsize>(id3.size()));
    if (id3.size() & 1U) output.put('\0');
    output.flush();
    if (!output) {
        cleanPartial();
        error = L"WAV dosyası tamamlanamadı. Disk alanını kontrol edin.";
        return false;
    }
    output.close();
    input.close();
    if (!MoveFileExW(partial.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto message = lastError(L"WAV dosyası hedefe taşınamadı");
        cleanPartial();
        error = message;
        return false;
    }
    return true;
}

bool writeHighQualityMp3(const std::filesystem::path& rawPcmPath,
                         std::uint64_t sampleFrames,
                         const std::filesystem::path& destination,
                         const RecordingMetadata& metadata,
                         std::wstring& error) {
    error.clear();
    std::error_code fileError;
    const std::uint64_t availableBytes = std::filesystem::file_size(rawPcmPath, fileError);
    const std::uint64_t dataBytes = sampleFrames * waveBlockAlign;
    if (fileError || availableBytes < dataBytes || sampleFrames == 0) {
        error = L"Geçici kayıt verisi bulunamadı veya tamamlanmamış.\nYol: " +
                rawPcmPath.wstring() + L"\nBeklenen: " +
                std::to_wstring(dataBytes) + L" bayt\nBulunan: " +
                std::to_wstring(fileError ? 0 : availableBytes) + L" bayt";
        return false;
    }
    AudibleRange audible;
    if (!findAudibleRange(rawPcmPath, sampleFrames, audible, error)) return false;
    sampleFrames = audible.frameCount;

    // Media Foundation still has MAX_PATH-sensitive code paths. Keep its
    // temporary output name short and under the raw recording's _TMP folder
    // instead of repeating a potentially long artist/album/title path.
    const auto encodedPath = partialPath(rawPcmPath.parent_path(), L".encoding.mp3");
    const auto taggedPath = partialPath(destination.parent_path(), L".part.mp3");
    auto removeFile = [](const std::filesystem::path& path) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    };
    removeFile(encodedPath);
    removeFile(taggedPath);

    HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (!checkMediaResult(result, L"MP3 kodlayıcısı başlatılamadı", error)) return false;

    const bool encoded = [&]() {
        ComPtr<IMFAttributes> attributes;
        result = MFCreateAttributes(&attributes, 2);
        if (!checkMediaResult(result, L"MP3 ayarları oluşturulamadı", error)) return false;
        attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        ComPtr<IMFSinkWriter> writer;
        constexpr unsigned int createAttempts = 5;
        for (unsigned int attempt = 0; attempt < createAttempts; ++attempt) {
            result = MFCreateSinkWriterFromURL(encodedPath.c_str(), nullptr,
                                               attributes.Get(), &writer);
            if (SUCCEEDED(result)) break;
            removeFile(encodedPath);
            if (attempt + 1 < createAttempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
        if (FAILED(result)) {
            const std::wstring context = L"MP3 geçici dosyası oluşturulamadı: " +
                                         encodedPath.wstring();
            return checkMediaResult(result, context.c_str(), error);
        }

        ComPtr<IMFCollection> availableTypes;
        result = MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_MP3, MFT_ENUM_FLAG_ALL,
                                                         nullptr, &availableTypes);
        if (!checkMediaResult(result, L"MP3 kodlama profilleri okunamadı", error)) return false;
        DWORD typeCount{};
        availableTypes->GetElementCount(&typeCount);
        ComPtr<IMFMediaType> outputType;
        for (DWORD index = 0; index < typeCount; ++index) {
            ComPtr<IUnknown> unknown;
            if (FAILED(availableTypes->GetElement(index, &unknown))) continue;
            ComPtr<IMFMediaType> candidate;
            if (FAILED(unknown.As(&candidate))) continue;
            UINT32 channels{};
            UINT32 rate{};
            UINT32 bytesPerSecond{};
            if (SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) &&
                SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) &&
                SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytesPerSecond)) &&
                channels == waveChannels && rate == waveRate && bytesPerSecond == 40'000) {
                outputType = std::move(candidate);
                break;
            }
        }
        if (!outputType) {
            error = L"Windows'ta 44,1 kHz stereo 320 kbps MP3 profili bulunamadı.";
            return false;
        }

        DWORD streamIndex{};
        result = writer->AddStream(outputType.Get(), &streamIndex);
        if (!checkMediaResult(result, L"320 kbps MP3 biçimi kullanılamıyor", error)) return false;

        ComPtr<IMFMediaType> inputType;
        result = MFCreateMediaType(&inputType);
        if (!checkMediaResult(result, L"PCM giriş biçimi oluşturulamadı", error)) return false;
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, waveChannels);
        inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, waveRate);
        inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, waveBits);
        inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, waveBlockAlign);
        inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, waveByteRate);
        inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        result = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
        if (!checkMediaResult(result, L"PCM kayıt MP3 kodlayıcısına bağlanamadı", error)) return false;
        result = writer->BeginWriting();
        if (!checkMediaResult(result, L"MP3 kodlama başlatılamadı", error)) return false;

        std::ifstream input(rawPcmPath, std::ios::binary);
        if (!input) {
            error = L"Geçici kayıt verisi açılamadı: " + rawPcmPath.wstring();
            return false;
        }
        input.seekg(static_cast<std::streamoff>(audible.startFrame * waveBlockAlign));
        if (!input) {
            error = L"Geçici kayıt verisinde kırpılacak konuma ulaşılamadı.";
            return false;
        }
        constexpr std::uint32_t chunkFrames = waveRate;
        std::vector<std::uint8_t> pcm(static_cast<std::size_t>(chunkFrames) * waveBlockAlign);
        std::uint64_t frameOffset{};
        while (frameOffset < sampleFrames) {
            const auto frames = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(chunkFrames, sampleFrames - frameOffset));
            const DWORD bytes = frames * waveBlockAlign;
            input.read(reinterpret_cast<char*>(pcm.data()), bytes);
            if (input.gcount() != static_cast<std::streamsize>(bytes)) {
                error = L"Geçici kayıt verisi eksik veya bozuk.";
                return false;
            }

            ComPtr<IMFMediaBuffer> buffer;
            result = MFCreateMemoryBuffer(bytes, &buffer);
            if (!checkMediaResult(result, L"MP3 kodlama belleği ayrılamadı", error)) return false;
            BYTE* bufferBytes{};
            result = buffer->Lock(&bufferBytes, nullptr, nullptr);
            if (!checkMediaResult(result, L"MP3 kodlama belleği açılamadı", error)) return false;
            std::memcpy(bufferBytes, pcm.data(), bytes);
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);

            ComPtr<IMFSample> sample;
            result = MFCreateSample(&sample);
            if (!checkMediaResult(result, L"MP3 ses örneği oluşturulamadı", error)) return false;
            sample->AddBuffer(buffer.Get());
            const LONGLONG start = static_cast<LONGLONG>(frameOffset * 10'000'000ULL / waveRate);
            const LONGLONG end = static_cast<LONGLONG>((frameOffset + frames) * 10'000'000ULL / waveRate);
            sample->SetSampleTime(start);
            sample->SetSampleDuration(end - start);
            result = writer->WriteSample(streamIndex, sample.Get());
            if (!checkMediaResult(result, L"Ses MP3 dosyasına kodlanamadı", error)) return false;
            frameOffset += frames;
        }
        result = writer->Finalize();
        return checkMediaResult(result, L"MP3 dosyası tamamlanamadı", error);
    }();
    MFShutdown();
    if (!encoded) {
        removeFile(encodedPath);
        return false;
    }

    std::ifstream mp3(encodedPath, std::ios::binary);
    std::ofstream tagged(taggedPath, std::ios::binary | std::ios::trunc);
    if (!mp3 || !tagged) {
        removeFile(encodedPath);
        removeFile(taggedPath);
        error = L"MP3 etiketleri yazılamadı.";
        return false;
    }
    const auto id3 = makeId3Tag(metadata);
    tagged.write(reinterpret_cast<const char*>(id3.data()),
                 static_cast<std::streamsize>(id3.size()));
    std::vector<char> copyBuffer(1024 * 1024);
    while (mp3) {
        mp3.read(copyBuffer.data(), static_cast<std::streamsize>(copyBuffer.size()));
        if (mp3.gcount() > 0) tagged.write(copyBuffer.data(), mp3.gcount());
    }
    tagged.flush();
    if (!tagged) {
        mp3.close();
        tagged.close();
        removeFile(encodedPath);
        removeFile(taggedPath);
        error = L"MP3 dosyası diske yazılamadı. Boş alanı kontrol edin.";
        return false;
    }
    mp3.close();
    tagged.close();
    removeFile(encodedPath);
    if (!MoveFileExW(taggedPath.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = lastError(L"MP3 dosyası hedefe taşınamadı");
        removeFile(taggedPath);
        return false;
    }
    return true;
}

std::wstring safeRecordingFileName(std::wstring value) {
    static constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    for (auto& character : value) {
        if (character < 32 || invalid.find(character) != std::wstring_view::npos) character = L'_';
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'.')) value.pop_back();
    const auto first = value.find_first_not_of(L' ');
    if (first == std::wstring::npos) return L"Yeni Kayıt";
    value.erase(0, first);
    if (value.empty()) return L"Yeni Kayıt";
    return value;
}

std::filesystem::path automaticRecordingFolder(const std::filesystem::path& root,
                                                std::wstring artist,
                                                std::wstring album) {
    if (artist.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        artist = L"Sanatçısı Bilinmeyenler";
    }
    if (album.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        album = L"Albümü Bilinmeyenler";
    }
    return root / safeRecordingFileName(std::move(artist)) /
           safeRecordingFileName(std::move(album));
}

namespace {

std::wstring lowerCase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool hasCompleteRecordingIdentity(const RecordingMetadata& metadata) {
    const auto hasText = [](std::wstring_view value) {
        return value.find_first_not_of(L" \t\r\n") != std::wstring_view::npos;
    };
    return hasText(metadata.title) && hasText(metadata.artist) && hasText(metadata.album);
}

std::vector<std::filesystem::path> automaticRecordingCandidates(
    const std::filesystem::path& root,
    const RecordingMetadata& metadata) {
    std::vector<std::filesystem::path> candidates;
    if (!hasCompleteRecordingIdentity(metadata)) return candidates;

    const auto folder = automaticRecordingFolder(root, metadata.artist, metadata.album);
    const auto base = lowerCase(safeRecordingFileName(metadata.artist + L" - " + metadata.title));
    const auto numberedPrefix = base + L" (";
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const auto extension = lowerCase(iterator->path().extension().wstring());
        if (extension != L".mp3" && extension != L".wav") continue;
        const auto stem = lowerCase(iterator->path().stem().wstring());
        const bool baseName = stem == base;
        bool numberedCopy = stem.starts_with(numberedPrefix) && stem.ends_with(L')');
        if (numberedCopy) {
            const auto number = std::wstring_view(stem).substr(
                numberedPrefix.size(), stem.size() - numberedPrefix.size() - 1);
            numberedCopy = !number.empty() && std::ranges::all_of(number, [](wchar_t character) {
                return std::iswdigit(character) != 0;
            });
        }
        if (baseName || numberedCopy) candidates.push_back(iterator->path());
    }
    return candidates;
}

std::optional<std::uint64_t> mediaFileDurationTicks(const std::filesystem::path& path) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return std::nullopt;
    PROPVARIANT duration;
    PropVariantInit(&duration);
    const HRESULT result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
    std::optional<std::uint64_t> ticks;
    if (SUCCEEDED(result)) {
        if (duration.vt == VT_UI8) ticks = duration.uhVal.QuadPart;
        else if (duration.vt == VT_I8 && duration.hVal.QuadPart > 0) {
            ticks = static_cast<std::uint64_t>(duration.hVal.QuadPart);
        }
    }
    PropVariantClear(&duration);
    return ticks;
}

}  // namespace

bool automaticRecordingAlreadyExists(const std::filesystem::path& root,
                                     const RecordingMetadata& metadata) {
    return !automaticRecordingCandidates(root, metadata).empty();
}

bool automaticRecordingMatchesDuration(const std::filesystem::path& root,
                                       const RecordingMetadata& metadata,
                                       std::uint64_t expectedDurationTicks) {
    if (expectedDurationTicks == 0) return false;
    const auto candidates = automaticRecordingCandidates(root, metadata);
    if (candidates.empty() || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;

    bool matched{};
    for (const auto& candidate : candidates) {
        const auto actual = mediaFileDurationTicks(candidate);
        if (!actual || *actual == 0) continue;
        if (automaticRecordingDurationMatches(*actual, expectedDurationTicks)) {
            matched = true;
            break;
        }
    }
    MFShutdown();
    return matched;
}

void removeSupersededAutomaticRecordings(const std::filesystem::path& root,
                                         const RecordingMetadata& metadata,
                                         const std::filesystem::path& keep) {
    const auto normalizedKeep = lowerCase(keep.lexically_normal().wstring());
    for (const auto& candidate : automaticRecordingCandidates(root, metadata)) {
        if (lowerCase(candidate.lexically_normal().wstring()) == normalizedKeep) continue;
        std::error_code ignored;
        std::filesystem::remove(candidate, ignored);
    }
}

}  // namespace neon
