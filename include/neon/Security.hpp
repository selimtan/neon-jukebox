#pragma once

#include <chrono>
#include <string_view>

#include "neon/Models.hpp"

namespace neon {

class PinGuard {
public:
    static bool validFormat(std::string_view pin);
    static PinRecord create(std::string_view pin);
    static bool verify(std::string_view pin, const PinRecord& record);

    bool attempt(std::string_view pin, const PinRecord& record);
    [[nodiscard]] bool locked() const;
    [[nodiscard]] int secondsRemaining() const;
    [[nodiscard]] int failedAttempts() const { return failedAttempts_; }
    void resetFailures();

private:
    int failedAttempts_{};
    std::chrono::steady_clock::time_point lockedUntil_{};
};

}  // namespace neon
