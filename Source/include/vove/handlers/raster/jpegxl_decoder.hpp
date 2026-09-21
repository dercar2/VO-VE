#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::raster {

inline constexpr std::uint64_t kMaximumJpegxlSourceBytes = 512ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaximumJpegxlSourcePixels = 64ULL * 1024 * 1024;
inline constexpr std::uint32_t kMaximumJpegxlOutputEdge = 2048;
inline constexpr std::uint64_t kMaximumJpegxlOutputBytes = 2048ULL * 2048 * 4;

enum class JpegxlDecodeErrorCode : std::uint8_t {
    none, invalid_limits, source_limit_exceeded, source_io_error, source_disconnected,
    source_changed, truncated_input, malformed_input, unsupported_format, decoder_unavailable,
    dimension_limit_exceeded, unsupported_color, decode_failed, resource_limit_exceeded,
    output_limit_exceeded,
};

struct JpegxlDecodeError {
    JpegxlDecodeErrorCode code{JpegxlDecodeErrorCode::none};
    std::string detail;
    [[nodiscard]] explicit operator bool() const noexcept {
        return code != JpegxlDecodeErrorCode::none;
    }
};

struct JpegxlDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumJpegxlSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumJpegxlSourcePixels};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{kMaximumJpegxlOutputBytes};
};

struct JpegxlMetadata {
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint32_t page_count{1};
    std::string source_color_model;
    std::string source_color_profile;
    std::string source_profile_fingerprint;
    bool used_embedded_icc{};
    bool has_alpha{};
};

struct JpegxlImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    JpegxlMetadata metadata;
};

struct JpegxlDecodeResult {
    JpegxlImage image;
    JpegxlDecodeError error;
    [[nodiscard]] bool ok() const noexcept {
        return !error && image.width != 0 && image.height != 0 && !image.rgba8.empty();
    }
};

// Read only the supplied source; return the first coalesced, oriented SDR image.
[[nodiscard]] JpegxlDecodeResult decode_jpegxl(const NativeSource &source,
                                              const JpegxlDecodeLimits &limits = {});

} // namespace vove::handlers::raster
