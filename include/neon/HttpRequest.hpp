#pragma once

#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

namespace neon::detail {
struct HttpResponse {
    bool received{};
    DWORD status{};
    std::vector<std::uint8_t> body;
};
// The session must use WINHTTP_FLAG_ASYNC. Only the caller's worker waits;
// cancellation aborts the request without waiting for a network timeout.
HttpResponse httpGet(HINTERNET session, std::string_view url, std::wstring_view accept,
                     std::size_t maximumBytes, const std::atomic_bool* cancel);
}
