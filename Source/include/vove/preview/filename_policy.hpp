#pragma once

#include "vove/core/reserved_names.hpp"

#include <string_view>

namespace vove::preview {

[[nodiscard]] inline bool is_temporary_preview_source(const std::string_view path) noexcept {
    constexpr std::string_view suffix = ".tmp";
    return path.size() >= suffix.size() &&
           core::ascii_iequal(path.substr(path.size() - suffix.size()), suffix);
}

} // namespace vove::preview
