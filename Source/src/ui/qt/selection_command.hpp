#pragma once

#include "vove/core/directory_entry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace vove::ui {

struct SelectionRecord {
    std::size_t order{};
    std::uint64_t id{};
    core::EntryKind kind{core::EntryKind::file};
    std::string original_path_utf8;
    std::string name_utf8;
    std::string source_revision_utf8;
    std::uint64_t size_bytes{};
    std::int64_t modified_unix_ns{};
};

// Owns original-file metadata only. No model indexes, UI objects, previews or lazy reads.
class SelectionSnapshot final {
  public:
    explicit SelectionSnapshot(std::span<const core::DirectoryEntry> ordered_entries = {});
    [[nodiscard]] const std::vector<SelectionRecord> &records() const noexcept { return records_; }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

  private:
    std::vector<SelectionRecord> records_;
};

// Synchronous invocation transfers a value; a consumer may retain it for later read-only work.
// The consumer must revalidate revisions before opening originals; this is not backend approval.
using SelectionCommand = std::function<void(SelectionSnapshot)>;
[[nodiscard]] bool dispatch_selection_command(SelectionSnapshot snapshot,
                                              const SelectionCommand &command);

} // namespace vove::ui
