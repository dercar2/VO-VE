#pragma once

#include "vove/fileops/trash_transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace vove::fileops {

inline constexpr std::uintmax_t kTrashLowSpaceWarningBytes = std::uintmax_t{1024} * 1024U * 1024U;

enum class TrashQuotaState : std::uint8_t {
    normal,
    warning,
    exceeded,
    unknown,
};

struct TrashStorageUsage {
    std::filesystem::path root;
    std::size_t total_items{};
    std::uint64_t total_bytes{};
    std::uintmax_t capacity_bytes{};
    std::uintmax_t available_bytes{};
    bool space_known{};
    bool totals_saturated{};
};

enum class TrashCatalogStatus : std::uint8_t {
    success,
    cancelled,
    io_error,
};

struct TrashManifestRecord {
    std::filesystem::path manifest_path;
    TrashTransaction transaction;
};

struct TrashStorageFilter {
    std::filesystem::path root;
    std::string storage_identity_utf8;
    std::string source_revision_utf8;
};

struct TrashCatalogResult {
    TrashCatalogStatus status{TrashCatalogStatus::io_error};
    std::vector<TrashManifestRecord> manifests;
    std::size_t corrupt_manifests{};
    std::size_t unreadable_recovery_roots{};
    std::size_t low_space_roots{};
    std::size_t space_check_failures{};
    std::size_t total_items{};
    std::uint64_t total_bytes{};
    std::int64_t oldest_unix_ns{};
    bool totals_saturated{};
    std::vector<TrashStorageUsage> storage_usage;
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == TrashCatalogStatus::success;
    }
};

[[nodiscard]] TrashCatalogResult
read_trash_catalog(const std::filesystem::path &manifest_directory,
                   std::span<const std::filesystem::path> recovery_roots = {},
                   const std::stop_token &stop = {},
                   const std::optional<TrashStorageFilter> &storage_filter = std::nullopt);

[[nodiscard]] std::filesystem::path trash_storage_root_for(const std::filesystem::path &path);
[[nodiscard]] std::uintmax_t effective_trash_limit(const TrashStorageUsage &usage,
                                                   std::uintmax_t absolute_limit) noexcept;
[[nodiscard]] TrashQuotaState classify_trash_quota(const TrashStorageUsage &usage,
                                                   std::uintmax_t absolute_limit,
                                                   std::uintmax_t additional_bytes = 0) noexcept;

} // namespace vove::fileops
