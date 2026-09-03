#include "neon/Utils.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace neon {

std::string toUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Unable to convert UTF-16 to UTF-8");
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        output.data(), count, nullptr, nullptr);
    return output;
}

std::wstring fromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Unable to convert UTF-8 to UTF-16");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        output.data(), count);
    return output;
}

std::string pathToUtf8(const std::filesystem::path& path) { return toUtf8(path.wstring()); }
std::filesystem::path pathFromUtf8(std::string_view value) { return std::filesystem::path(fromUtf8(value)); }

std::string normalizeForSearch(std::string_view value) {
    const std::wstring wide = fromUtf8(value);
    if (wide.empty()) return {};
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
                                    wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                    nullptr, nullptr, 0);
    if (count <= 0) return std::string(value);
    std::wstring lowered(static_cast<std::size_t>(count), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
                  wide.data(), static_cast<int>(wide.size()), lowered.data(), count,
                  nullptr, nullptr, 0);
    return toUtf8(lowered);
}

std::string hexEncode(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    return output;
}

std::vector<std::uint8_t> hexDecode(std::string_view value) {
    if (value.size() % 2 != 0) return {};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> output(value.size() / 2);
    for (std::size_t i = 0; i < output.size(); ++i) {
        const int hi = nibble(value[i * 2]);
        const int lo = nibble(value[i * 2 + 1]);
        if (hi < 0 || lo < 0) return {};
        output[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return output;
}

std::string makeStableId(std::string_view value) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<std::uint8_t, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("SHA-256 failed");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hexEncode(std::span<const std::uint8_t>(digest.data(), 16));
}

std::string randomId() {
    std::array<std::uint8_t, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("Random ID generation failed");
    }
    return hexEncode(bytes);
}

bool isSupportedAudioFile(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), ::towlower);
    return extension == L".mp3" || extension == L".ogg" || extension == L".flac" || extension == L".wav";
}

bool isSupportedVideoFile(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), ::towlower);
    return extension == L".mp4" || extension == L".m4v" || extension == L".mov" ||
           extension == L".avi" || extension == L".wmv" || extension == L".mkv" ||
           extension == L".webm" || extension == L".mpeg" || extension == L".mpg";
}

std::int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string formatDuration(std::int64_t milliseconds) {
    const auto seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    std::ostringstream stream;
    stream << seconds / 60 << ':' << std::setw(2) << std::setfill('0') << seconds % 60;
    return stream.str();
}

}  // namespace neon
