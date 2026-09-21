#pragma once

#include <filesystem>
#include <string>

namespace vove::fileops::detail {

[[nodiscard]] std::string batch_rename_collision_key(const std::filesystem::path &path);

} // namespace vove::fileops::detail
