#include <winsock2.h>
#include <ws2tcpip.h>
#include "neon/BackgroundIo.hpp"
#include "neon/HttpRequest.hpp"
#include "neon/Library.hpp"

#include <future>
#include <iostream>
#include <string>

namespace {
int failures{};
#define CHECK(value) do { if (!(value)) { std::cerr << "FAIL line " << __LINE__ << ": " << #value << '\n'; ++failures; } } while (false)
using namespace std::chrono_literals;

class Server {
public:
    explicit Server(int mode) {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        int size = sizeof(address);
        CHECK(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/cover";
        CHECK(listen(socket_, 1) == 0);
        u_long nonblocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonblocking);
        worker_ = std::jthread([this, mode](std::stop_token stop) {
            SOCKET client = INVALID_SOCKET;
            while (!stop.stop_requested() && client == INVALID_SOCKET) {
                client = accept(socket_, nullptr, nullptr);
                if (client == INVALID_SOCKET) std::this_thread::sleep_for(2ms);
            }
            if (client == INVALID_SOCKET) return;
            // Receive the request before responding, then deliberately stall
            // either headers or body. No public network/provider is contacted.
            char request[4096];
            while (!stop.stop_requested() && recv(client, request, sizeof(request), 0) <= 0)
                std::this_thread::sleep_for(2ms);
            connected = true;
            const std::string reply = mode == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello" :
                mode == 2 ? "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nx" : "";
            if (!reply.empty()) send(client, reply.data(), static_cast<int>(reply.size()), 0);
            while (!stop.stop_requested()) std::this_thread::sleep_for(2ms);
            closesocket(client);
        });
    }
    ~Server() { worker_.request_stop(); worker_.join(); closesocket(socket_); }
    std::string url;
    std::atomic_bool connected{};
private:
    SOCKET socket_{};
    std::jthread worker_;
};

void networkCancellation() {
    WSADATA data{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const auto session = WinHttpOpen(L"Jukebox cancellation test", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    CHECK(session != nullptr);
    WinHttpSetTimeouts(session, 1000, 1000, 4000, 4000);
    for (int mode : {0, 1, 2, 1, 2}) {
        Server server(mode);
        std::atomic_bool cancel{};
        auto request = std::async(std::launch::async, [&] {
            return neon::detail::httpGet(session, server.url, L"application/json", 1024, &cancel);
        });
        if (mode == 0) {
            const auto response = request.get();
            CHECK(response.received && response.status == 200);
            CHECK(std::string(response.body.begin(), response.body.end()) == "hello");
        } else {
            const auto until = std::chrono::steady_clock::now() + 3s;
            while (!server.connected && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(2ms);
            CHECK(server.connected);
            std::this_thread::sleep_for(100ms);
            const auto started = std::chrono::steady_clock::now();
            cancel = true;
            CHECK(request.wait_for(500ms) == std::future_status::ready);
            const auto response = request.get();
            CHECK(!response.received && response.body.empty());
            std::cout << "Cancelled HTTP " << (mode == 1 ? "headers" : "body") << ": "
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
                << " ms\n";
        }
    }
    WinHttpCloseHandle(session);
    WSACleanup();
}

void diskCancellation() {
    const auto name = L"\\\\.\\pipe\\neon-cancel-test-" + std::to_wstring(GetCurrentProcessId());
    const auto pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
        1, 1024, 1024, 0, nullptr);
    CHECK(pipe != INVALID_HANDLE_VALUE);
    std::atomic_bool cancel{}, started{};
    auto reader = std::async(std::launch::async, [&] {
        neon::BackgroundIoCancellation cancelIo([&] { return cancel.load(); });
        started = true;
        const auto result = ConnectNamedPipe(pipe, nullptr);
        return !result && GetLastError() == ERROR_OPERATION_ABORTED;
    });
    while (!started) std::this_thread::sleep_for(2ms);
    std::this_thread::sleep_for(30ms);
    cancel = true;
    CHECK(reader.wait_for(500ms) == std::future_status::ready);
    CHECK(reader.get());
    CloseHandle(pipe);

    // An already cancelled scan must not probe cached remote paths at all.
    neon::LibraryIndex cached;
    neon::Track track;
    track.path = L"\\\\192.0.2.1\\offline\\record.mp3";
    cached.tracks.assign(1000, track);
    const auto began = std::chrono::steady_clock::now();
    const auto result = neon::LibraryScanner{}.scan({}, {}, cached, {}, {}, &cancel);
    CHECK(result.tracks.empty());
    CHECK(std::chrono::steady_clock::now() - began < 100ms);
}
}

int main() {
    networkCancellation();
    diskCancellation();
    std::cout << "Shutdown I/O checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
