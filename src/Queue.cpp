#include "neon/Queue.hpp"

#include <algorithm>
#include <numeric>

#include "neon/Utils.hpp"

namespace neon {

void CoinCreditBank::insert() {
    if (credits_ < maximumCredits) ++credits_;
}

bool CoinCreditBank::consume() {
    if (credits_ == 0) return false;
    --credits_;
    return true;
}

const QueueItem& RequestQueue::enqueue(std::string_view trackId) {
    items_.push_back(QueueItem{randomId(), std::string(trackId), nowUnixMs()});
    return items_.back();
}

const QueueItem* RequestQueue::front() const { return items_.empty() ? nullptr : &items_.front(); }

std::optional<QueueItem> RequestQueue::popFront() {
    if (items_.empty()) return std::nullopt;
    QueueItem item = std::move(items_.front());
    items_.pop_front();
    return item;
}

bool RequestQueue::remove(std::string_view queueItemId) {
    const auto found = std::ranges::find(items_, queueItemId, &QueueItem::id);
    if (found == items_.end()) return false;
    items_.erase(found);
    return true;
}

bool RequestQueue::move(std::size_t from, std::size_t to) {
    if (from >= items_.size() || to >= items_.size() || from == to) return false;
    QueueItem item = std::move(items_[from]);
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(from));
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
    return true;
}

void RequestQueue::clear() { items_.clear(); }

void RequestQueue::restore(std::vector<QueueItem> items) {
    items_.assign(std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
}

std::vector<QueueItem> RequestQueue::snapshot() const { return {items_.begin(), items_.end()}; }

std::optional<std::size_t> AmbientSelector::next(AmbientMode mode, bool repeat,
                                                  std::size_t trackCount,
                                                  std::mt19937& random) {
    if (mode == AmbientMode::Off || trackCount == 0) {
        reset();
        return std::nullopt;
    }
    if (mode != mode_ || repeat != repeat_ || trackCount != trackCount_) {
        mode_ = mode;
        repeat_ = repeat;
        trackCount_ = trackCount;
        cursor_ = 0;
        order_.resize(trackCount);
        std::iota(order_.begin(), order_.end(), std::size_t{});
        if (mode_ == AmbientMode::Shuffle) std::shuffle(order_.begin(), order_.end(), random);
    }
    if (cursor_ == order_.size()) {
        if (!repeat_) return std::nullopt;
        cursor_ = 0;
        if (mode_ == AmbientMode::Shuffle) std::shuffle(order_.begin(), order_.end(), random);
    }
    return order_[cursor_++];
}

void AmbientSelector::reset() {
    mode_ = AmbientMode::Off;
    repeat_ = true;
    trackCount_ = 0;
    cursor_ = 0;
    order_.clear();
}

}  // namespace neon
