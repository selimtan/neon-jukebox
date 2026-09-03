#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neon {

std::string toUtf8(std::wstring_view value);
std::wstring fromUtf8(std::string_view value);
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8(std::string_view value);
std::string normalizeForSearch(std::string_view value);
std::string makeStableId(std::string_view value);
std::string randomId();
std::string hexEncode(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> hexDecode(std::string_view value);
bool isSupportedAudioFile(const std::filesystem::path& path);
bool isSupportedVideoFile(const std::filesystem::path& path);
std::int64_t nowUnixMs();
std::string formatDuration(std::int64_t milliseconds);

}  // namespace neon
