#include "neon/HttpRequest.hpp"
#include "neon/Utils.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace neon::detail {
namespace {
struct Handle {
    HINTERNET value{};
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
struct Pending {
    std::mutex mutex;
    std::condition_variable changed;
    DWORD completion{};
    DWORD bytes{};
    bool failed{};
    std::array<std::uint8_t, 16 * 1024> buffer{};
    std::wstring headers;
};
using Binding = std::shared_ptr<Pending>;

void CALLBACK completed(HINTERNET, DWORD_PTR context, DWORD status, void*, DWORD length) {
    if (!context) return;
    auto* binding = reinterpret_cast<Binding*>(context);
    // WinHTTP's final callback releases the buffer/context even if the caller
    // already returned after cancellation. No detached application worker.
    auto pending = *binding;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete binding; return; }
    {
        std::lock_guard lock(pending->mutex);
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) pending->failed = true;
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                 status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                 status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            pending->completion = status;
            pending->bytes = length;
        }
    }
    pending->changed.notify_all();
}
}

HttpResponse httpGet(HINTERNET session, std::string_view url, std::wstring_view accept,
                     std::size_t maximumBytes, const std::atomic_bool* cancel) {
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_acquire); };
    HttpResponse response;
    if (!session || cancelled()) return response;
    const auto wideUrl = fromUtf8(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) return response;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Handle connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    if (!connection.value || cancelled()) return response;
    Handle request{WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.value) return response;
    auto pending = std::make_shared<Pending>();
    auto binding = std::make_unique<Binding>(pending);
    auto context = reinterpret_cast<DWORD_PTR>(binding.get());
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE,
                          &context, sizeof(context))) return response;
    if (WinHttpSetStatusCallback(request.value, completed,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return response;
    binding.release(); // Owned by HANDLE_CLOSING from here onward.
    const auto wait = [&](DWORD expected) {
        std::unique_lock lock(pending->mutex);
        while (!cancelled() && !pending->failed && pending->completion != expected)
            pending->changed.wait_for(lock, std::chrono::milliseconds(20));
        if (cancelled() || pending->failed) return false;
        pending->completion = 0;
        return true;
    };
    pending->headers = L"Accept: " + std::wstring(accept) + L"\r\n";
    if (cancelled() || !WinHttpSendRequest(request.value, pending->headers.c_str(),
        static_cast<DWORD>(pending->headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, context) ||
        !wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) ||
        !WinHttpReceiveResponse(request.value, nullptr) || !wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE))
        return response;
    DWORD statusBytes = sizeof(response.status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) return response;
    response.received = true;
    if (response.status != 200) return response;
    for (;;) {
        if (cancelled() || !WinHttpReadData(request.value, pending->buffer.data(),
            static_cast<DWORD>(pending->buffer.size()), nullptr) || !wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) {
            response.received = false;
            response.body.clear();
            return response;
        }
        const auto bytes = pending->bytes; // Completion synchronized by wait's mutex.
        if (!bytes) return response;
        if (response.body.size() + bytes > maximumBytes) {
            response.received = false;
            response.body.clear();
            return response;
        }
        response.body.insert(response.body.end(), pending->buffer.begin(), pending->buffer.begin() + bytes);
    }
}
}
