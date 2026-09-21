#pragma once

#include "directory_enumerator.hpp"
#include "vove/core/reserved_names.hpp"

#include <chrono>
#include <utility>

namespace vove::platform::detail {

// Apply before opening children, including TMP-named directories (subtree pruning).
[[nodiscard]] inline bool recursive_name_excluded(const std::filesystem::path &name) {
    const auto text = name.u8string();
    const std::u8string_view view(text);
    return view.empty() || view.front() == u8'.' || core::is_internal_filename(view) ||
           (view.size() >= 4 && core::ascii_iequal(view.substr(view.size() - 4),
                                                  std::u8string_view(u8".tmp")));
}

[[nodiscard]] inline std::size_t recursive_metadata_cost(const core::DirectoryEntry &entry) {
    // Include decoder search keys, string capacity slack, and model bookkeeping.
    return sizeof(entry) + 256U +
           2U * (entry.name_utf8.size() + entry.path_utf8.size() + entry.source_revision_utf8.size());
}

class RecursiveTraversal {
  public:
    RecursiveTraversal(const catalog::CatalogRequest &request, const EntryCallback &on_entry,
                       const ProgressCallback &on_progress)
        : request(request), on_entry_(on_entry), on_progress_(on_progress),
          deadline_(std::chrono::steady_clock::now() + request.total_timeout) {}

    [[nodiscard]] bool tick() {
        if (stopped_) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline_) {
            result.error = {.kind = catalog::CatalogErrorKind::timed_out,
                            .message_utf8 = "Recursive catalog total time limit reached"};
            result.truncated = true;
            stopped_ = true;
            return false;
        }
        if (on_progress_ && now >= next_progress_) {
            next_progress_ = now + catalog::kRecursiveCatalogProgressInterval;
            if (!on_progress_(result.directories_visited)) {
                result.error = {.kind = catalog::CatalogErrorKind::cancelled, .message_utf8 = {}};
                result.truncated = true;
                stopped_ = true;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool enter(const std::uint32_t depth) {
        if (!tick()) {
            return false;
        }
        if (depth > request.maximum_depth) {
            result.truncated = true;
            return false;
        }
        if (result.directories_visited >= request.maximum_directories) {
            result.truncated = true;
            stopped_ = true;
            return false;
        }
        ++result.directories_visited;
        return true;
    }

    void skip(catalog::CatalogError error = {}, const bool root = false) {
        result.truncated = true;
        if (!result.error || root) {
            result.error = std::move(error);
        }
        result.root_failed = result.root_failed || root;
    }

    [[nodiscard]] bool emit(core::DirectoryEntry entry) {
        if (!tick()) {
            return false;
        }
        const auto cost = recursive_metadata_cost(entry);
        if (entry.path_utf8.size() > catalog::kRecursiveCatalogMaximumPathBytes ||
            files_ >= catalog::recursive_entry_limit(request) ||
            cost > catalog::kRecursiveCatalogMaximumMetadataBytes - metadata_bytes_) {
            result.truncated = true;
            stopped_ = true;
            return false;
        }
        metadata_bytes_ += cost;
        ++files_;
        if (!on_entry_(std::move(entry))) {
            result.error = {.kind = catalog::CatalogErrorKind::cancelled, .message_utf8 = {}};
            result.truncated = true;
            stopped_ = true;
            return false;
        }
        return true;
    }

    const catalog::CatalogRequest &request;
    EnumerationResult result;

  private:
    const EntryCallback &on_entry_;
    const ProgressCallback &on_progress_;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::time_point next_progress_{};
    std::size_t files_{};
    std::size_t metadata_bytes_{};
    bool stopped_{};
};

} // namespace vove::platform::detail
