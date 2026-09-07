#pragma once

#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>
#include "neon/Models.hpp"

namespace neon {

// Disk preparation is independent of themes, visible pages and texture eviction.
class LibraryArtworkPreparer {
public:
    using Prepare = std::function<void(const Track&, const std::filesystem::path&, std::stop_token)>;
    struct Progress { std::size_t pending{}, active{}, prepared{}, reused{}; };
    explicit LibraryArtworkPreparer(Prepare prepare = {});
    ~LibraryArtworkPreparer();
    LibraryArtworkPreparer(const LibraryArtworkPreparer&) = delete;
    LibraryArtworkPreparer& operator=(const LibraryArtworkPreparer&) = delete;
    void start(const std::filesystem::path& libraryRoot);
    void enqueue(std::span<const Track> tracks);
    void prioritize(std::span<const Track* const> tracks);
    std::vector<Track> takeCompleted();
    Progress progress() const;
    void stop();
private:
    struct State;
    std::shared_ptr<State> state_;
    Prepare prepare_;
};

} // namespace neon
