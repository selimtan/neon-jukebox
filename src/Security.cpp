#include "neon/Security.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <vector>

#include "neon/Utils.hpp"

namespace neon {
namespace {

std::vector<std::uint8_t> derive(std::string_view pin, std::span<const std::uint8_t> salt,
                                 std::uint32_t iterations) {
    BCRYPT_ALG_HANDLE algorithm{};
    std::vector<std::uint8_t> output(32);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        throw std::runtime_error("Unable to initialize PIN hashing");
    }
    const auto status = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(pin.data())),
        static_cast<ULONG>(pin.size()), const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()), iterations, output.data(),
        static_cast<ULONG>(output.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Unable to hash PIN");
    return output;
}

bool constantTimeEqual(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) {
    if (left.size() != right.size()) return false;
    std::uint8_t difference{};
    for (std::size_t i = 0; i < left.size(); ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

}  // namespace

bool PinGuard::validFormat(std::string_view pin) {
    return pin.size() >= 4 && pin.size() <= 8 &&
           std::ranges::all_of(pin, [](unsigned char value) { return std::isdigit(value) != 0; });
}

PinRecord PinGuard::create(std::string_view pin) {
    if (!validFormat(pin)) throw std::invalid_argument("PIN must contain 4-8 digits");
    std::array<std::uint8_t, 16> salt{};
    if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("Unable to create PIN salt");
    }
    PinRecord record;
    record.saltHex = hexEncode(salt);
    record.hashHex = hexEncode(derive(pin, salt, record.iterations));
    return record;
}

bool PinGuard::verify(std::string_view pin, const PinRecord& record) {
    if (!validFormat(pin) || !record.configured()) return false;
    const auto salt = hexDecode(record.saltHex);
    const auto expected = hexDecode(record.hashHex);
    if (salt.empty() || expected.empty()) return false;
    return constantTimeEqual(derive(pin, salt, record.iterations), expected);
}

bool PinGuard::attempt(std::string_view pin, const PinRecord& record) {
    if (locked()) return false;
    if (verify(pin, record)) {
        resetFailures();
        return true;
    }
    ++failedAttempts_;
    if (failedAttempts_ >= 5) {
        lockedUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        failedAttempts_ = 0;
    }
    return false;
}

bool PinGuard::locked() const { return std::chrono::steady_clock::now() < lockedUntil_; }

int PinGuard::secondsRemaining() const {
    if (!locked()) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
        lockedUntil_ - std::chrono::steady_clock::now()).count()) + 1;
}

void PinGuard::resetFailures() {
    failedAttempts_ = 0;
    lockedUntil_ = {};
}

}  // namespace neon
