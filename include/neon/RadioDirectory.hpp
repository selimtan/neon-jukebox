#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "neon/Models.hpp"

namespace neon {

struct RadioDirectoryResult {
    std::vector<Track> tracks;
    std::string error; // Empty on success. Cancellation returns no tracks and an error.
};

class RadioDirectory {
public:
    // Synchronous worker operation, with one async WinHTTP session per call.
    // No shared state, owned threads, UI callbacks, or implicit cache writes.
    [[nodiscard]] RadioDirectoryResult fetchTurkey(const std::atomic_bool* cancel = nullptr) const;

    // Radio Browser station array, restricted to checked, playable TR stations.
    // Malformed entries are skipped; malformed/oversized documents report errors.
    // Empty arrays succeed. Duplicate UUIDs, stream URLs and normalized station
    // names select the lexically smallest UUID, independently of response order.
    [[nodiscard]] static RadioDirectoryResult parseStations(std::string_view json);
    // Also validates restored cached URLs before handing them to Windows playback.
    [[nodiscard]] static bool validStreamUrl(std::string_view url);
};

}  // namespace neon
