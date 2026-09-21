#pragma once

#include "vove/cache/artifact_store.hpp"
#include "vove/cache/artifact_index.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace vove::cache {

struct DiskCacheLimits {
    std::uint64_t maximum_entries{100'000};
    std::uint64_t maximum_bytes{10ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t minimum_free_bytes{0};
    std::size_t maximum_orphan_scan_entries{250'000};
    std::size_t maximum_space_probes{256};
};

enum class PruneErrorCode : std::uint8_t {
    none,
    invalid_configuration,
    index_unavailable,
    orphan_scan_failed,
    artifact_removal_failed,
    index_removal_failed,
    lru_made_no_progress,
    space_probe_failed,
    space_probe_limit_reached,
    free_space_target_unmet,
};

struct SpaceProbeResult {
    std::uint64_t available_bytes{};
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return !error;
    }
};

using SpaceProbe = std::function<SpaceProbeResult(const std::filesystem::path &)>;

struct PruneResult {
    std::uint64_t entries_before{};
    std::uint64_t entries_after{};
    std::uint64_t bytes_before{};
    std::uint64_t bytes_after{};
    std::size_t inspected{};
    std::size_t index_records_removed{};
    std::size_t artifacts_removed{};
    std::size_t orphan_artifacts_found{};
    std::size_t orphan_artifacts_removed{};
    std::uint64_t orphan_bytes_found{};
    std::uint64_t orphan_bytes_removed{};
    std::uint64_t free_bytes_before{};
    std::uint64_t free_bytes_after{};
    std::size_t space_probes{};
    bool free_space_observed{};
    bool minimum_free_space_reached{true};
    PruneErrorCode error_code{PruneErrorCode::none};
    std::error_code space_error;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error_code == PruneErrorCode::none && error.empty();
    }
};

[[nodiscard]] PruneResult prune_disk_cache(ArtifactStore &store, ArtifactIndex &index,
                                           DiskCacheLimits limits, std::size_t batch_limit = 256,
                                           SpaceProbe space_probe = {}, bool scan_orphans = true);

} // namespace vove::cache
