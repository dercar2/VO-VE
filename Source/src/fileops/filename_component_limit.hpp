#pragma once

#include <cstddef>
#include <filesystem>
#include <system_error>

namespace vove::fileops::detail {

struct FilenameComponentLimitResult {
    std::size_t maximum_units{};
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return maximum_units != 0 && !error;
    }
};

[[nodiscard]] FilenameComponentLimitResult
query_filename_component_limit(const std::filesystem::path &directory) noexcept;
[[nodiscard]] std::size_t filename_component_units(const std::filesystem::path &filename) noexcept;

} // namespace vove::fileops::detail
