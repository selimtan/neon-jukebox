#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <objbase.h>
#include <roapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "neon/Radio.hpp"

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
int failures{};

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #expression "\n"; \
        ++failures; \
    } \
} while (false)

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    return predicate();
}

neon::Track station(std::string url) {
    neon::Track track;
    track.id = "radio-test";
    track.title = "Muted test stream";
    track.mediaKind = neon::MediaKind::Radio;
    track.streamUrl = std::move(url);
    return track;
}

void checkStopped(neon::RadioEngine& radio) {
    CHECK(!radio.playing());
    CHECK(!radio.loading());
    CHECK(!radio.paused());
    CHECK(!radio.takeFinished());
    CHECK(radio.takeError().empty());
    CHECK(!radio.pause());
    CHECK(!radio.resume());
}

// A generated, silent PCM WAV exercises real HTTP resolution and decoding
// without files, third-party codecs, a web server executable, or the internet.
std::string silentWave() {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint32_t dataSize = sampleRate * 2 * 4;
    std::string bytes;
    const auto u16 = [&](std::uint16_t value) {
        bytes.push_back(static_cast<char>(value));
        bytes.push_back(static_cast<char>(value >> 8));
    };
    const auto u32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
    };
    bytes += "RIFF";
    u32(36 + dataSize);
    bytes += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(sampleRate);
    u32(sampleRate * 2);
    u16(2);
    u16(16);
    bytes += "data";
    u32(dataSize);
    bytes.append(dataSize, '\0');
    return bytes;
}

class HttpFixture {
public:
    HttpFixture() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        try {
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == INVALID_SOCKET) throw std::runtime_error("socket failed");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
                listen(listener_, SOMAXCONN) == SOCKET_ERROR) throw std::runtime_error("loopback bind/listen failed");
            int size = sizeof(address);
            if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR) {
                throw std::runtime_error("getsockname failed");
            }
            port_ = ntohs(address.sin_port);
            u_long nonblocking = 1;
            if (ioctlsocket(listener_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                throw std::runtime_error("nonblocking socket failed");
            }
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            if (listener_ != INVALID_SOCKET) closesocket(listener_);
            WSACleanup();
            throw;
        }
    }
    ~HttpFixture() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        closesocket(listener_);
        WSACleanup();
    }
    std::string url(const char* path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    std::atomic_int stalledRequests{};
    std::atomic_int audioRequests{};
    std::atomic_bool releaseStalls{};

private:
    struct Client {
        SOCKET socket{INVALID_SOCKET};
        std::string request;
        std::string response;
        std::size_t sent{};
        bool stalled{};
    };

    std::string response(const std::string& request) {
        if (request.find(" /audio.wav") != std::string::npos) {
            ++audioRequests;
            std::size_t first{};
            bool partial{};
            auto lower = request;
            for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            const auto range = lower.find("\r\nrange: bytes=");
            if (range != std::string::npos) {
                try {
                    first = static_cast<std::size_t>(std::stoull(lower.substr(range + 15)));
                    partial = first < wave_.size();
                } catch (...) {}
            }
            if (!partial) first = 0;
            auto header = std::string(partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n") +
                "Content-Type: audio/wav\r\nAccept-Ranges: bytes\r\nContent-Length: " +
                std::to_string(wave_.size() - first) + "\r\n";
            if (partial) header += "Content-Range: bytes " + std::to_string(first) + "-" +
                std::to_string(wave_.size() - 1) + "/" + std::to_string(wave_.size()) + "\r\n";
            header += "Connection: close\r\n\r\n";
            return header + (request.starts_with("HEAD ") ? std::string{} : wave_.substr(first));
        }
        if (request.find(" /bad") != std::string::npos) {
            return "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: 12\r\n"
                "Connection: close\r\n\r\ninvalidaudio";
        }
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }

    void run() {
        std::vector<Client> clients;
        while (!stop_.load()) {
            fd_set reads;
            fd_set writes;
            FD_ZERO(&reads);
            FD_ZERO(&writes);
            FD_SET(listener_, &reads);
            for (auto& client : clients) {
                if (client.stalled && releaseStalls.load()) {
                    client.response = response("GET /missing HTTP/1.1\r\n\r\n");
                    client.stalled = false;
                }
                if (!client.response.empty()) FD_SET(client.socket, &writes);
                else FD_SET(client.socket, &reads);
            }
            timeval timeout{0, 20000};
            if (select(0, &reads, &writes, nullptr, &timeout) == SOCKET_ERROR) break;
            if (FD_ISSET(listener_, &reads)) {
                const auto accepted = accept(listener_, nullptr, nullptr);
                if (accepted != INVALID_SOCKET) {
                    u_long nonblocking = 1;
                    if (clients.size() >= FD_SETSIZE - 1 ||
                        ioctlsocket(accepted, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                        closesocket(accepted);
                    } else {
                        Client client;
                        client.socket = accepted;
                        clients.push_back(std::move(client));
                    }
                }
            }
            for (auto& client : clients) {
                bool closed{};
                if (FD_ISSET(client.socket, &reads)) {
                    char buffer[4096];
                    const auto received = recv(client.socket, buffer, sizeof(buffer), 0);
                    if (received > 0) {
                        client.request.append(buffer, static_cast<std::size_t>(received));
                        if (client.request.size() > 16384) closed = true;
                        else if (!client.stalled && client.request.find("\r\n\r\n") != std::string::npos) {
                            if (client.request.find(" /stall") != std::string::npos) {
                                client.stalled = true;
                                ++stalledRequests;
                            } else client.response = response(client.request);
                        }
                    } else if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) closed = true;
                }
                if (!closed && FD_ISSET(client.socket, &writes)) {
                    const auto sent = send(client.socket, client.response.data() + client.sent,
                        static_cast<int>(client.response.size() - client.sent), 0);
                    if (sent > 0) {
                        client.sent += static_cast<std::size_t>(sent);
                        closed = client.sent == client.response.size();
                    } else if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) closed = true;
                }
                if (closed) {
                    closesocket(client.socket);
                    client.socket = INVALID_SOCKET;
                }
            }
            std::erase_if(clients, [](const Client& client) { return client.socket == INVALID_SOCKET; });
        }
        for (const auto& client : clients) closesocket(client.socket);
    }

    SOCKET listener_{INVALID_SOCKET};
    unsigned short port_{};
    const std::string wave_ = silentWave();
    std::atomic_bool stop_{};
    std::thread worker_;
};

bool waitPlaying(neon::RadioEngine& radio, std::chrono::milliseconds timeout = 10s) {
    std::string error;
    const bool changed = waitUntil([&] {
        error = radio.takeError();
        return !error.empty() || radio.playing();
    }, timeout);
    if (!error.empty()) std::cerr << error << '\n';
    return changed && error.empty() && radio.playing();
}

void testStateAndValidation() {
    neon::RadioEngine radio;
    CHECK(!radio.initialized());
    CHECK(radio.volume() == 0.8F);
    checkStopped(radio);
    // Gain boundary checks happen while there is no source/player.
    radio.setVolume(2.0F);
    CHECK(radio.volume() == 1.0F);
    radio.setVolume(-1.0F);
    CHECK(radio.volume() == 0.0F);
    radio.setVolume(std::numeric_limits<float>::quiet_NaN());
    CHECK(radio.volume() == 0.0F);
    radio.setVolume(std::numeric_limits<float>::infinity());
    CHECK(radio.volume() == 0.0F);
    radio.setVolume(0.0F);
    std::string error;
    CHECK(!radio.play(station("http://127.0.0.1/audio"), error));
    CHECK(!error.empty());
    const std::vector<std::string> invalid{
        "", "not a URL", "radio.example/audio", "//radio.example/audio", "file:///C:/audio.mp3",
        "ftp://radio.example/audio", "data:audio/mpeg;base64,AA==", "http:", "http://",
        "https:///audio", "http://?q=x", "http://#fragment", "https://radio.example/a b",
        "https://radio.example/\\bad", "http://[broken/audio", "https://radio.example:bad/audio",
        "https://radio.example/%zz", "https://radio.example/%1", "https://radio.example/\r\n",
        std::string("http://localhost/\0hidden", 24)
    };
    // Validation works before initialization and does not trigger a network open.
    for (const auto& url : invalid) {
        CHECK(!radio.play(station(url), error));
        CHECK(!error.empty());
        checkStopped(radio);
    }
    CHECK(radio.initialize(error));
    if (!radio.initialized()) {
        std::cerr << error << '\n';
        return;
    }
    CHECK(error.empty());
    CHECK(radio.initialize(error));
    CHECK(radio.volume() == 0.0F);
    for (const auto& url : invalid) {
        CHECK(!radio.play(station(url), error));
        CHECK(!error.empty());
        checkStopped(radio);
    }
    auto music = station("http://127.0.0.1/audio.wav");
    music.mediaKind = neon::MediaKind::Music;
    CHECK(!radio.play(music, error));
    CHECK(!error.empty());
    checkStopped(radio);
    radio.stop();
    radio.stop();
    CHECK(radio.initialized());
    radio.shutdown();
    radio.shutdown();
    CHECK(!radio.initialized());
    checkStopped(radio);
    CHECK(radio.initialize(error));
    CHECK(radio.volume() == 0.0F);
    checkStopped(radio);
}

void testHttpPlayback() {
    std::cout << "HTTP fixture playback" << std::endl;
    HttpFixture fixture;
    neon::RadioEngine radio;
    radio.setVolume(0.0F); // Always mute before initializing or submitting a source.
    std::string error;
    CHECK(radio.initialize(error));
    if (!radio.initialized()) { std::cerr << error << '\n'; return; }
    CHECK(radio.play(station(fixture.url("/stall")), error));
    CHECK(waitUntil([&] { return fixture.stalledRequests.load() > 0; }));
    CHECK(radio.loading());
    CHECK(!radio.playing());
    CHECK(radio.pause());
    CHECK(radio.paused());
    CHECK(!radio.pause());
    CHECK(radio.resume());
    CHECK(!radio.resume());

    const auto switchStart = Clock::now();
    std::cout << "Switching from stalled source" << std::endl;
    CHECK(radio.play(station(fixture.url("/audio.wav")), error));
    CHECK(Clock::now() - switchStart < 500ms);
    CHECK(waitPlaying(radio));
    CHECK(fixture.audioRequests.load() > 0);
    CHECK(!radio.loading());
    CHECK(radio.volume() == 0.0F);
    fixture.releaseStalls.store(true); // A late old-source failure must be ignored.
    std::this_thread::sleep_for(100ms);
    CHECK(!radio.takeFinished());
    CHECK(radio.takeError().empty());
    CHECK(radio.pause());
    CHECK(radio.paused());
    CHECK(!radio.playing());
    std::this_thread::sleep_for(100ms);
    CHECK(radio.resume());
    CHECK(waitPlaying(radio));
    // Invalid replacement preserves current playback.
    CHECK(!radio.play(station("file:///C:/not-radio.wav"), error));
    CHECK(!error.empty());
    CHECK(radio.playing());
    CHECK(waitUntil([&] { return radio.takeFinished(); }));
    CHECK(!radio.takeFinished());
    CHECK(radio.takeError().empty());
    checkStopped(radio);

    for (const auto* path : {"/missing", "/bad"}) {
        CHECK(radio.play(station(fixture.url(path)), error));
        std::string failure;
        CHECK(waitUntil([&] { failure = radio.takeError(); return !failure.empty(); }));
        CHECK(!failure.empty());
        CHECK(radio.takeError().empty());
        CHECK(radio.takeFinished());
        CHECK(!radio.takeFinished());
        checkStopped(radio);
    }

    fixture.releaseStalls.store(false);
    auto requests = fixture.stalledRequests.load();
    CHECK(radio.play(station(fixture.url("/stall")), error));
    CHECK(waitUntil([&] { return fixture.stalledRequests.load() > requests; }));
    const auto stopStart = Clock::now();
    radio.stop();
    CHECK(Clock::now() - stopStart < 500ms);
    checkStopped(radio);
    fixture.releaseStalls.store(true);
    std::this_thread::sleep_for(100ms);
    checkStopped(radio);

    // Independent callers race queries, mute, stop and source changes. No COM
    // apartment or message pump is required on any public calling thread.
    std::atomic_bool reading{true};
    std::thread reader([&] {
        while (reading.load()) {
            (void)radio.initialized();
            (void)radio.playing();
            (void)radio.loading();
            (void)radio.paused();
            (void)radio.volume();
            radio.setVolume(0.0F);
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 16; ++i) {
        CHECK(radio.play(station(fixture.url(i % 2 ? "/missing" : "/audio.wav")), error));
        radio.stop();
    }
    reading.store(false);
    reader.join();
    radio.stop();
    std::this_thread::sleep_for(100ms);
    checkStopped(radio);

    fixture.releaseStalls.store(false);
    requests = fixture.stalledRequests.load();
    CHECK(radio.play(station(fixture.url("/stall")), error));
    CHECK(waitUntil([&] { return fixture.stalledRequests.load() > requests; }));
    const auto shutdownStart = Clock::now();
    radio.shutdown();
    CHECK(Clock::now() - shutdownStart < 750ms);
    CHECK(!radio.initialized());
    checkStopped(radio);
    fixture.releaseStalls.store(true);
    CHECK(waitUntil([&] { return radio.initialize(error); }, 3s));
    CHECK(radio.play(station(fixture.url("/audio.wav")), error));
    CHECK(waitPlaying(radio));
    radio.stop();
    checkStopped(radio);
}

void testOpenTimeout() {
    HttpFixture fixture;
    neon::RadioEngine radio;
    radio.setVolume(0.0F);
    std::string error;
    CHECK(radio.initialize(error));
    if (!radio.initialized()) { std::cerr << error << '\n'; return; }
    CHECK(radio.play(station(fixture.url("/stall")), error));
    CHECK(waitUntil([&] { return fixture.stalledRequests.load() > 0; }));
    const auto started = Clock::now();
    std::string failure;
    CHECK(waitUntil([&] { failure = radio.takeError(); return !failure.empty(); }, 33s));
    CHECK(Clock::now() - started < 33s);
    CHECK(!failure.empty());
    CHECK(!radio.loading());
    CHECK(radio.takeFinished());
    CHECK(radio.takeError().empty());
    checkStopped(radio);
}

void liveProbe(const std::string& url) {
    neon::RadioEngine radio;
    radio.setVolume(0.0F);
    std::string error;
    CHECK(radio.initialize(error));
    if (!radio.initialized()) { std::cerr << error << '\n'; return; }
    CHECK(radio.play(station(url), error));
    if (!error.empty()) { std::cerr << error << '\n'; return; }
    const bool playing = waitPlaying(radio, 35s);
    CHECK(playing);
    if (playing) {
        std::this_thread::sleep_for(2s);
        const auto failure = radio.takeError();
        if (!failure.empty()) std::cerr << failure << '\n';
        CHECK(failure.empty());
        // A public live station may briefly rebuffer after opening. It must
        // recover to actual playback without ending or reporting a decoder error.
        CHECK(waitPlaying(radio));
        CHECK(!radio.takeFinished());
        CHECK(radio.volume() == 0.0F);
        CHECK(radio.pause());
        std::this_thread::sleep_for(100ms);
        CHECK(radio.paused());
        CHECK(radio.resume());
        CHECK(waitPlaying(radio));
        std::cout << "Muted live probe reached actual playback: " << url << '\n';
    }
    const auto start = Clock::now();
    radio.shutdown();
    CHECK(Clock::now() - start < 750ms);
    checkStopped(radio);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--stream")) {
        std::cerr << "Usage: neon_radio_playback_tests [--stream URL]\n";
        return 2;
    }
    // Match the application's Windows Runtime lifetime. Letting every apartment
    // exit tells Windows MediaPlayer that this process is shutting down.
    const HRESULT apartment = RoInitialize(RO_INIT_SINGLETHREADED);
    CHECK(SUCCEEDED(apartment));
    try {
        if (argc == 3) liveProbe(argv[2]);
        else {
            std::cout << "Radio state and validation" << std::endl;
            testStateAndValidation();
            std::cout << "Radio HTTP playback" << std::endl;
            // Calls from the existing UI STA still use the dedicated media worker.
            testHttpPlayback();
            std::cout << "Testing the 30-second open deadline (muted)...\n";
            testOpenTimeout();
        }
    } catch (const std::exception& exception) {
        std::cerr << "Radio test exception: " << exception.what() << '\n';
        ++failures;
    }
    if (SUCCEEDED(apartment)) RoUninitialize();
    if (failures == 0) std::cout << "Radio playback tests passed (all playback muted).\n";
    return failures == 0 ? 0 : 1;
}
