#include "vove/cache/cache_maintenance.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace vove::cache {
namespace {

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

[[nodiscard]] SpaceProbeResult probe_filesystem_space(const std::filesystem::path &root) {
    std::error_code error;
    const auto info = std::filesystem::space(root, error);
    if (error) {
        return {.available_bytes = 0, .error = error};
    }
    return {.available_bytes = static_cast<std::uint64_t>(info.available), .error = {}};
}

void set_error(PruneResult &result, const PruneErrorCode code, std::string message) {
    result.error_code = code;
    result.error = std::move(message);
}

} // namespace

PruneResult prune_disk_cache(ArtifactStore &store, ArtifactIndex &index,
                             const DiskCacheLimits limits, const std::size_t batch_limit,
                             SpaceProbe space_probe, const bool scan_orphans) {
    PruneResult result;
    if (!index.ready()) {
        set_error(result, PruneErrorCode::index_unavailable,
                  index.last_error().empty() ? "SQLite cache index is not ready"
                                             : index.last_error());
        return result;
    }
    if (batch_limit == 0 || batch_limit > 10'000) {
        set_error(result, PruneErrorCode::invalid_configuration,
                  "cache prune batch limit must be 1..10000");
        return result;
    }
    if (scan_orphans && limits.maximum_orphan_scan_entries == 0) {
        set_error(result, PruneErrorCode::invalid_configuration,
                  "cache orphan scan limit must be non-zero");
        return result;
    }
    if (limits.minimum_free_bytes != 0 && limits.maximum_space_probes == 0) {
        set_error(result, PruneErrorCode::invalid_configuration,
                  "cache space probe limit must be non-zero when a free-space reserve is set");
        return result;
    }

    if (!space_probe) {
        space_probe = probe_filesystem_space;
    }
    const auto observe_space = [&]() -> bool {
        if (result.space_probes >= limits.maximum_space_probes) {
            set_error(result, PruneErrorCode::space_probe_limit_reached,
                      "cache space probe limit was reached before the reserve was restored");
            return false;
        }
        const auto observation = space_probe(store.root());
        ++result.space_probes;
        if (!observation.ok()) {
            result.space_error = observation.error;
            set_error(result, PruneErrorCode::space_probe_failed,
                      "failed to inspect free space for the cache root: " +
                          observation.error.message());
            return false;
        }
        if (!result.free_space_observed) {
            result.free_bytes_before = observation.available_bytes;
            result.free_space_observed = true;
        }
        result.free_bytes_after = observation.available_bytes;
        result.minimum_free_space_reached =
            observation.available_bytes >= limits.minimum_free_bytes;
        return true;
    };

    if (limits.minimum_free_bytes != 0 && !observe_space()) {
        return result;
    }

    const auto scanned = scan_orphans
                             ? store.scan_digest_artifacts(limits.maximum_orphan_scan_entries)
                             : DigestScanResult{};
    if (scanned.error) {
        set_error(result, PruneErrorCode::orphan_scan_failed, scanned.error.message);
        return result;
    }
    const auto orphan_scan_incomplete = scanned.inspection_limit_reached;
    for (const auto &artifact : scanned.artifacts) {
        if (index.find(artifact.digest_hex)) {
            continue;
        }
        if (!index.last_error().empty()) {
            set_error(result, PruneErrorCode::index_unavailable, index.last_error());
            return result;
        }
        ++result.orphan_artifacts_found;
        result.orphan_bytes_found = saturating_add(result.orphan_bytes_found, artifact.bytes);
        const auto removal = store.remove_digest_checked(artifact.digest_hex);
        if (!removal.ok()) {
            set_error(result, PruneErrorCode::artifact_removal_failed, removal.error.message);
            return result;
        }
        if (removal.status == RemoveStatus::removed) {
            ++result.orphan_artifacts_removed;
            ++result.artifacts_removed;
            result.orphan_bytes_removed =
                saturating_add(result.orphan_bytes_removed, artifact.bytes);
        }
    }

    if (limits.minimum_free_bytes != 0 && result.orphan_artifacts_removed != 0 &&
        !result.minimum_free_space_reached && !observe_space()) {
        return result;
    }

    const auto indexed_entries_before = index.count();
    const auto indexed_bytes_before = index.total_bytes();
    result.entries_before = saturating_add(indexed_entries_before, result.orphan_artifacts_found);
    result.bytes_before = saturating_add(indexed_bytes_before, result.orphan_bytes_found);
    auto entries = indexed_entries_before;
    auto bytes = indexed_bytes_before;

    while (entries > limits.maximum_entries || bytes > limits.maximum_bytes ||
           !result.minimum_free_space_reached) {
        const auto excess_entries =
            entries > limits.maximum_entries ? entries - limits.maximum_entries : std::uint64_t{0};
        const auto requested = excess_entries == 0
                                   ? batch_limit
                                   : static_cast<std::size_t>(std::min<std::uint64_t>(
                                         batch_limit, std::max<std::uint64_t>(1, excess_entries)));
        const auto victims = index.oldest(requested);
        if (victims.empty()) {
            if (!index.last_error().empty()) {
                set_error(result, PruneErrorCode::index_unavailable, index.last_error());
            } else if (!result.minimum_free_space_reached) {
                set_error(result, PruneErrorCode::free_space_target_unmet,
                          "cache is empty but the requested free-space reserve is not available");
            } else {
                set_error(result, PruneErrorCode::lru_made_no_progress,
                          "cache LRU returned no records while limits are exceeded");
            }
            break;
        }
        std::size_t records_removed_in_batch = 0;
        for (const auto &victim : victims) {
            ++result.inspected;
            const auto removal = store.remove_digest_checked(victim.digest_hex);
            if (!removal.ok()) {
                set_error(result, PruneErrorCode::artifact_removal_failed, removal.error.message);
                break;
            }
            if (removal.status == RemoveStatus::removed) {
                ++result.artifacts_removed;
            }
            if (!index.erase(victim.digest_hex)) {
                set_error(result, PruneErrorCode::index_removal_failed,
                          index.last_error().empty()
                              ? "failed to erase a selected cache index record"
                              : index.last_error());
                break;
            }
            ++result.index_records_removed;
            ++records_removed_in_batch;
        }
        if (!result.error.empty()) {
            break;
        }
        if (records_removed_in_batch == 0) {
            set_error(result, PruneErrorCode::lru_made_no_progress,
                      "cache LRU batch removed no records");
            break;
        }
        entries = index.count();
        bytes = index.total_bytes();
        if (!result.minimum_free_space_reached && !observe_space()) {
            break;
        }
    }
    result.entries_after = index.count();
    result.bytes_after = index.total_bytes();
    if (orphan_scan_incomplete && result.ok()) {
        set_error(result, PruneErrorCode::orphan_scan_failed,
                  "cache orphan scan limit was reached before the artifact tree ended");
    }
    return result;
}

} // namespace vove::cache
