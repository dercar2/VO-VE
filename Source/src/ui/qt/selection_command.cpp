#include "selection_command.hpp"

#include <utility>

namespace vove::ui {

SelectionSnapshot::SelectionSnapshot(const std::span<const core::DirectoryEntry> entries) {
    records_.reserve(entries.size());
    for (const auto &entry : entries) {
        records_.push_back({.order = records_.size(),
                            .id = entry.id,
                            .kind = entry.kind,
                            .original_path_utf8 = entry.path_utf8,
                            .name_utf8 = entry.name_utf8,
                            .source_revision_utf8 = entry.source_revision_utf8,
                            .size_bytes = entry.size_bytes,
                            .modified_unix_ns = entry.modified_unix_ns});
    }
}

bool dispatch_selection_command(SelectionSnapshot snapshot, const SelectionCommand &command) {
    if (!command || snapshot.empty()) return false;
    command(std::move(snapshot));
    return true;
}

} // namespace vove::ui
