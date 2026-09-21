#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vove::preview {

struct ScaledRgbaImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && width != 0 && height != 0 && !rgba8.empty();
    }
};

// Fits an RGBA8 image into maximum_edge without upscaling. Interpolation is performed in
// premultiplied-alpha space to avoid dark fringes around transparent artwork.
[[nodiscard]] ScaledRgbaImage scale_rgba8_to_edge(std::span<const std::byte> source,
                                                  std::uint32_t width, std::uint32_t height,
                                                  std::uint32_t maximum_edge);

} // namespace vove::preview
