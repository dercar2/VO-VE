#include "vove/cache/qoi_codec.hpp"

#include <qoi.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace vove::cache {
namespace {

constexpr std::size_t kQoiHeaderBytes = 14;

[[nodiscard]] std::uint32_t read_be32(const std::span<const std::byte> bytes,
                                      const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] bool valid_dimensions(const std::uint32_t width,
                                    const std::uint32_t height) noexcept {
    return width != 0 && height != 0 && width <= kMaxArtifactDimension &&
           height <= kMaxArtifactDimension;
}

} // namespace

QoiEncodeResult
encode_qoi_rgba8(const std::span<const std::byte> rgba8, const std::uint32_t width,
                 const std::uint32_t height, // NOLINT(bugprone-easily-swappable-parameters)
                 const std::size_t stride) {
    QoiEncodeResult result;
    if (!valid_dimensions(width, height)) {
        result.error = "QOI dimensions are outside cache limits";
        return result;
    }
    const auto row_bytes = static_cast<std::size_t>(width) * 4U;
    const auto preceding_rows = static_cast<std::size_t>(height - 1U);
    if (stride < row_bytes ||
        preceding_rows > (std::numeric_limits<std::size_t>::max() - row_bytes) / stride ||
        rgba8.size() < preceding_rows * stride + row_bytes) {
        result.error = "RGBA8 source buffer is truncated";
        return result;
    }

    std::vector<std::byte> packed;
    const std::byte *source = rgba8.data();
    if (stride != row_bytes) {
        packed.resize(row_bytes * height);
        for (std::uint32_t row = 0; row < height; ++row) {
            std::copy_n(rgba8.data() + stride * row, row_bytes, packed.data() + row_bytes * row);
        }
        source = packed.data();
    }

    const qoi_desc descriptor{width, height, 4, QOI_SRGB};
    int encoded_size{};
    void *encoded = qoi_encode(source, &descriptor, &encoded_size);
    if (encoded == nullptr || encoded_size <= 0) {
        std::free(encoded);
        result.error = "QOI encoder rejected RGBA8 pixels";
        return result;
    }
    const auto size = static_cast<std::size_t>(encoded_size);
    if (size > kMaxArtifactBytes - kVvt1HeaderBytes) {
        std::free(encoded);
        result.error = "QOI payload exceeds cache artifact limit";
        return result;
    }
    const auto *first = static_cast<const std::byte *>(encoded);
    result.bytes.assign(first, first + size);
    std::free(encoded);
    return result;
}

QoiDecodeResult decode_qoi_rgba8(const std::span<const std::byte> encoded) {
    QoiDecodeResult result;
    if (encoded.size() < kQoiHeaderBytes || encoded.size() > kMaxArtifactBytes - kVvt1HeaderBytes) {
        result.error = "QOI payload size is outside cache limits";
        return result;
    }
    if (encoded[0] != std::byte{'q'} || encoded[1] != std::byte{'o'} ||
        encoded[2] != std::byte{'i'} || encoded[3] != std::byte{'f'}) {
        result.error = "QOI magic is invalid";
        return result;
    }
    const auto width = read_be32(encoded, 4);
    const auto height = read_be32(encoded, 8);
    if (!valid_dimensions(width, height) || (std::to_integer<unsigned>(encoded[12]) != 3U &&
                                             std::to_integer<unsigned>(encoded[12]) != 4U)) {
        result.error = "QOI header is outside cache limits";
        return result;
    }
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        result.error = "QOI payload cannot be represented by the decoder";
        return result;
    }

    qoi_desc descriptor{};
    void *decoded = qoi_decode(encoded.data(), static_cast<int>(encoded.size()), &descriptor, 4);
    if (decoded == nullptr || descriptor.width != width || descriptor.height != height) {
        std::free(decoded);
        result.error = "QOI payload is malformed";
        return result;
    }
    const auto pixel_bytes = static_cast<std::size_t>(width) * height * 4U;
    const auto *first = static_cast<const std::byte *>(decoded);
    result.width = width;
    result.height = height;
    result.rgba8.assign(first, first + pixel_bytes);
    std::free(decoded);
    return result;
}

} // namespace vove::cache
