#pragma once

#include "vove/cache/cache_types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vove::cache {

struct QoiEncodeResult {
    std::vector<std::byte> bytes;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

struct QoiDecodeResult {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] QoiEncodeResult encode_qoi_rgba8(std::span<const std::byte> rgba8,
                                               std::uint32_t width, std::uint32_t height,
                                               std::size_t stride);

[[nodiscard]] QoiDecodeResult decode_qoi_rgba8(std::span<const std::byte> encoded);

} // namespace vove::cache
