#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#include "neon/Models.hpp"

namespace neon {

[[nodiscard]] constexpr bool shouldOfferPlayNow(bool backgroundTrackPlaying,
                                                 bool requestQueueWasEmpty) noexcept {
    return backgroundTrackPlaying && requestQueueWasEmpty;
}

class CoinCreditBank {
public:
    void insert();
    [[nodiscard]] bool consume();
    [[nodiscard]] std::size_t available() const { return credits_; }

private:
    static constexpr std::size_t maximumCredits = 99;
    std::size_t credits_{};
};

class RequestQueue {
public:
    const QueueItem& enqueue(std::string_view trackId);
    [[nodiscard]] const QueueItem* front() const;
    std::optional<QueueItem> popFront();
    bool remove(std::string_view queueItemId);
    bool move(std::size_t from, std::size_t to);
    void clear();
    void restore(std::vector<QueueItem> items);

    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }
    [[nodiscard]] const std::deque<QueueItem>& items() const { return items_; }
    [[nodiscard]] std::vector<QueueItem> snapshot() const;

private:
    std::deque<QueueItem> items_;
};

class AmbientSelector {
public:
    std::optional<std::size_t> next(AmbientMode mode, bool repeat, std::size_t trackCount,
                                    std::mt19937& random);
    void reset();

private:
    AmbientMode mode_{AmbientMode::Off};
    bool repeat_{true};
    std::size_t trackCount_{};
    std::size_t cursor_{};
    std::vector<std::size_t> order_;
};

}  // namespace neon
