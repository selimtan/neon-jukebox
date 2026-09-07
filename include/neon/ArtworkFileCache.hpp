#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_map>

namespace neon {
// A visible page and the library sweep share the same extraction. Waiting is
// confined to their workers; cancellation never needs the lock owner to finish.
class ArtworkFileLease {
public:
    ArtworkFileLease(const std::filesystem::path& path, std::stop_token stop) {
        static std::mutex mutex;
        static std::unordered_map<std::wstring, std::weak_ptr<std::timed_mutex>> locks;
        {
            std::lock_guard guard(mutex);
            auto& entry = locks[path.native()];
            owner_ = entry.lock();
            if (!owner_) { owner_ = std::make_shared<std::timed_mutex>(); entry = owner_; }
            if (locks.size() > 1024) std::erase_if(locks, [](const auto& item) { return item.second.expired(); });
        }
        lock_ = std::unique_lock<std::timed_mutex>(*owner_, std::defer_lock);
        while (!stop.stop_requested() && !lock_.try_lock_for(std::chrono::milliseconds(20))) {}
    }
    explicit operator bool() const { return lock_.owns_lock(); }
private:
    std::shared_ptr<std::timed_mutex> owner_;
    std::unique_lock<std::timed_mutex> lock_;
};
}
