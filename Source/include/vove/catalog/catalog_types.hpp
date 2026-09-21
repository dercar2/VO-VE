#pragma once

#include "vove/core/directory_entry.hpp"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vove::catalog {

using RequestGeneration = std::uint64_t;
inline constexpr std::size_t kCatalogBatchSize = 64;
inline constexpr std::size_t kCatalogQueueCapacity = 8;
inline constexpr std::size_t kCatalogMaxConcurrentEnumerations = 4;
inline constexpr auto kCatalogInactivityTimeout = std::chrono::seconds(3);
// Finite production policy. Directory/depth/time budgets may be reduced, never disabled.
// The service's total clock starts at submission and includes helper startup and backpressure.
inline constexpr std::size_t kRecursiveCatalogMaximumEntries = 50'000;
inline constexpr std::uint32_t kRecursiveCatalogMaximumDirectories = 20'000;
inline constexpr std::uint32_t kRecursiveCatalogMaximumDepth = 64;
inline constexpr auto kRecursiveCatalogTotalTimeout = std::chrono::seconds(60);
inline constexpr auto kRecursiveCatalogProgressInterval = std::chrono::milliseconds(200);
inline constexpr std::size_t kRecursiveCatalogMaximumMetadataBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kRecursiveCatalogMaximumFrameBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kRecursiveCatalogMaximumPathBytes = 32U * 1024U;

// Zero means no entry limit only for ordinary listings. Recursive requests are always capped.
struct CatalogRequest {
    RequestGeneration generation{};
    std::filesystem::path path;
    std::size_t maximum_entries{100'000};
    // When set, return metadata for this one direct child instead of enumerating the directory.
    std::filesystem::path exact_entry_path{};
    bool recursive{false};
    // Root is depth zero and counts as one directory. Smaller budgets are useful for tests.
    std::uint32_t maximum_directories{kRecursiveCatalogMaximumDirectories};
    std::uint32_t maximum_depth{kRecursiveCatalogMaximumDepth};
    std::chrono::milliseconds total_timeout{kRecursiveCatalogTotalTimeout};
};

[[nodiscard]] inline std::size_t recursive_entry_limit(const CatalogRequest &request) noexcept {
    return request.maximum_entries == 0 || request.maximum_entries > kRecursiveCatalogMaximumEntries
               ? kRecursiveCatalogMaximumEntries : request.maximum_entries;
}

[[nodiscard]] inline bool valid_recursive_request(const CatalogRequest &request) noexcept {
    return !request.recursive ||
           (!request.path.empty() && request.path.native().size() <= kRecursiveCatalogMaximumPathBytes &&
            request.path.native().find(std::filesystem::path::value_type{}) ==
                std::filesystem::path::string_type::npos &&
            request.exact_entry_path.empty() && request.maximum_directories > 0 &&
            request.maximum_directories <= kRecursiveCatalogMaximumDirectories &&
            request.maximum_depth <= kRecursiveCatalogMaximumDepth &&
            request.total_timeout.count() > 0 &&
            request.total_timeout <= kRecursiveCatalogTotalTimeout);
}

enum class CatalogErrorKind : std::uint8_t {
    none,
    not_found,
    permission_denied,
    authentication_required,
    network_disconnected,
    timed_out,
    io_error,
    cancelled,
};

struct CatalogError {
    CatalogErrorKind kind{CatalogErrorKind::none};
    std::string message_utf8;
    std::int64_t platform_code{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return kind != CatalogErrorKind::none;
    }
};

struct CatalogBatch {
    RequestGeneration generation{};
    std::vector<core::DirectoryEntry> entries;
    std::string directory_revision_utf8;
    bool is_final{false};
    bool truncated{false};
    CatalogError error;
    std::uint32_t directories_visited{};
    // Recursive child errors/limits are partial success; only confirmed root failure is fatal.
    // A timeout (even before the first file) is unknown completeness, not confirmed root failure.
    bool root_failed{false};
};

class DirectorySource {
  public:
    virtual ~DirectorySource() = default;
    virtual void submit(CatalogRequest request) = 0;
    virtual void cancel(RequestGeneration generation) = 0;
    [[nodiscard]] virtual std::optional<CatalogBatch> poll() = 0;
};

class DirectorySink {
  public:
    virtual ~DirectorySink() = default;
    virtual void begin_catalog() = 0;
    virtual void replace_catalog(std::vector<core::DirectoryEntry> entries) = 0;
    virtual void append_catalog(std::vector<core::DirectoryEntry> entries) = 0;
    virtual void finish_catalog() = 0;
    virtual void abort_catalog() = 0;
};

} // namespace vove::catalog
