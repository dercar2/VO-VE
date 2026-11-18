#pragma once

#include "vove/core/directory_entry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vove::search {

inline constexpr std::size_t kSearchBatchSize = 64;
inline constexpr std::size_t kMaximumSearchResults = 100'000;
inline constexpr std::size_t kMaximumSearchRoots = 64;

enum class SearchStatus : std::uint8_t {
    success,
    provider_unavailable,
    scope_not_indexed,
    invalid_request,
    io_error,
    timed_out,
    cancelled,
};

struct SearchRequest {
    std::uint64_t generation{};
    std::string query_utf8;
    std::vector<std::string> allowed_roots_utf8;
    std::size_t maximum_results{kMaximumSearchResults};
};

struct SearchBatch {
    std::uint64_t generation{};
    std::vector<core::DirectoryEntry> entries;
    std::uint64_t total_matches{};
    std::int64_t provider_index_modified_unix_ns{};
    SearchStatus status{SearchStatus::success};
    std::string message_utf8;
    bool is_final{};
    bool truncated{};
};

[[nodiscard]] std::vector<std::string>
normalize_allowed_roots(std::span<const std::string> roots_utf8);

[[nodiscard]] bool path_is_within_allowed_roots(const std::string &path_utf8,
                                                std::span<const std::string> normalized_roots_utf8);

// Owned by one search request. Preparation and membership checks are purely lexical.
class AllowedRootFilter {
  public:
    AllowedRootFilter() = default;
    explicit AllowedRootFilter(std::span<const std::string> roots_utf8);
    [[nodiscard]] const std::vector<std::string> &normalized_roots() const noexcept { return roots_; }
    [[nodiscard]] bool contains(const std::string &path_utf8) const;

  private:
    std::vector<std::string> roots_;
#ifdef _WIN32
    std::vector<std::wstring> wide_roots_;
#endif
};

} // namespace vove::search
