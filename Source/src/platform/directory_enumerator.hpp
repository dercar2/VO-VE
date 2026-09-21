#pragma once

#include "vove/catalog/catalog_types.hpp"

#include <functional>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vove::platform::detail {

using EntryCallback = std::function<bool(core::DirectoryEntry)>;
using ProgressCallback = std::function<bool(std::uint32_t)>;

struct EnumerationResult {
    catalog::CatalogError error;
    bool truncated{false};
    std::uint32_t directories_visited{};
    bool root_failed{false};
};

struct EntryQueryResult {
    std::optional<core::DirectoryEntry> entry{};
    catalog::CatalogError error{};
};

[[nodiscard]] EnumerationResult enumerate_directory(const catalog::CatalogRequest &request,
                                                    const EntryCallback &on_entry,
                                                    const ProgressCallback &on_progress = {});
[[nodiscard]] EntryQueryResult query_entry(const std::filesystem::path &path);

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path);
[[nodiscard]] std::uint64_t stable_entry_id(std::string_view path_utf8);

} // namespace vove::platform::detail
