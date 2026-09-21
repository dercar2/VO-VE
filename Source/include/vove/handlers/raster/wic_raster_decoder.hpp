#pragma once

#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/raster/signature_probe.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct IStream;

namespace vove::handlers::raster {

inline constexpr std::uint32_t kMaximumWicRasterEdge = 2048;
inline constexpr std::uint64_t kMaximumWicRasterSourceBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumWicRasterSourcePixels = 64'000'000ULL;
inline constexpr std::uint64_t kMaximumWicRasterOutputBytes =
    static_cast<std::uint64_t>(kMaximumWicRasterEdge) * kMaximumWicRasterEdge * 4ULL;

enum class WicRasterDecodeErrorCode : std::uint8_t {
    none,
    invalid_limits,
    source_limit_exceeded,
    source_io_error,
    source_disconnected,
    source_changed,
    truncated_input,
    malformed_input,
    unsupported_format,
    unsupported_webp,
    unsupported_heif,
    unsupported_avif,
    decoder_unavailable,
    dimension_limit_exceeded,
    unsupported_color,
    color_profile_required,
    invalid_color_profile,
    color_profile_mismatch,
    color_transform_failed,
    decode_failed,
    output_limit_exceeded,
};

struct WicRasterDecodeError {
    WicRasterDecodeErrorCode code{WicRasterDecodeErrorCode::none};
    std::uint32_t system_code{};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != WicRasterDecodeErrorCode::none;
    }
};

struct WicRasterDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumWicRasterSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumWicRasterSourcePixels};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{kMaximumWicRasterOutputBytes};
    std::span<const std::byte> fallback_cmyk_icc;
    bool apply_orientation{true};
    // RAW containers may authoritatively identify an otherwise unprofiled JPEG as Adobe RGB.
    // Generic raster callers must keep the default and reject external/unknown color contexts.
    bool allow_external_rgb_color_space{false};
};

struct WicRasterMetadata {
    RasterFormat format{RasterFormat::Unknown};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::optional<std::uint32_t> page_count;
    std::uint16_t exif_orientation{1};
    std::string color_summary;
    std::string source_profile_fingerprint;
    bool assumed_srgb{};
};

struct WicRasterImage {
    std::vector<std::byte> rgba8;
    std::uint32_t width{};
    std::uint32_t height{};
    WicRasterMetadata metadata;
};

struct WicRasterDecodeResult {
    WicRasterImage image;
    WicRasterDecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && image.width != 0 && image.height != 0 && !image.rgba8.empty();
    }
};

// Decodes only from the supplied already-opened source. No path or file name is consulted.
[[nodiscard]] WicRasterDecodeResult decode_wic_raster(const NativeSource &source,
                                                      const WicRasterDecodeLimits &limits = {});

namespace detail {

// Exposed only so the random-access COM stream contract can be tested independently of WIC.
// The caller owns one COM reference and must call Release().
[[nodiscard]] ::IStream *create_wic_source_stream(const NativeSource &source) noexcept;

} // namespace detail
} // namespace vove::handlers::raster
