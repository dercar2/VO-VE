#pragma once

#include "vove/core/directory_entry.hpp"
#include "vove/core/reserved_names.hpp"

#include <string_view>

namespace vove::core {

[[nodiscard]] inline bool visible_in_preview_catalog(const DirectoryEntry &entry) noexcept {
    const std::string_view name(entry.name_utf8);
    if (name.empty() || name.front() == '.') {
        return false;
    }
    constexpr std::string_view temporary_suffix = ".tmp";
    return entry.kind != EntryKind::file || name.size() < temporary_suffix.size() ||
           !ascii_iequal(name.substr(name.size() - temporary_suffix.size()), temporary_suffix);
}

} // namespace vove::core
