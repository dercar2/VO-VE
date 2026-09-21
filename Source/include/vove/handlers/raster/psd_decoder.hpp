#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vove::handlers::raster {

inline constexpr std::uint64_t kMaximumPsdSourceBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumPsbSourceBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumPsdSourcePixels = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumPsdDimension = 30'000;
inline constexpr std::uint32_t kMaximumPsbDimension = 300'000;
inline constexpr std::uint32_t kMaximumPsdOutputEdge = 2048;
inline constexpr std::uint64_t kMaximumPsdOutputBytes =
    static_cast<std::uint64_t>(kMaximumPsdOutputEdge) * kMaximumPsdOutputEdge * 4ULL;
inline constexpr std::uint64_t kMaximumPsdZipBytes = 256ULL * 1024ULL * 1024ULL;

enum class PsdDecodeErrorCode : std::uint8_t {
    none,
    invalid_limits,
    source_limit_exceeded,
    source_io_error,
    source_disconnected,
    source_changed,
    truncated_input,
    malformed_input,
    unsupported_version,
    unsupported_depth,
    unsupported_color_mode,
    unsupported_compression,
    dimension_limit_exceeded,
    resource_limit_exceeded,
    color_profile_required,
    invalid_color_profile,
    color_transform_failed,
    output_limit_exceeded,
};

struct PsdDecodeError {
    PsdDecodeErrorCode code{PsdDecodeErrorCode::none};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != PsdDecodeErrorCode::none;
    }
};

struct PsdDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumPsbSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumPsdSourcePixels};
    std::uint32_t maximum_dimension{kMaximumPsbDimension};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{kMaximumPsdOutputBytes};
    std::uint64_t maximum_zip_bytes{kMaximumPsdZipBytes};
    std::span<const std::byte> fallback_cmyk_icc;
};

enum class PsdColorModel : std::uint8_t {
    grayscale,
    rgb,
    cmyk,
    lab,
};

struct PsdMetadata {
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint16_t source_depth{};
    std::uint16_t source_channels{};
    PsdColorModel source_color_model{PsdColorModel::rgb};
    std::string source_color_profile;
    std::string source_profile_fingerprint;
    bool used_embedded_icc{};
    bool used_fallback_icc{};
    bool has_alpha{};
};

struct PsdImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    PsdMetadata metadata;
};

struct PsdDecodeResult {
    PsdImage image;
    PsdDecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && image.width != 0 && image.height != 0 && !image.rgba8.empty();
    }
};

// Decodes the flattened PSD/PSB composite from an already-opened source. Layer data and paths are never
// interpreted, and no source path or credential crosses this boundary.
[[nodiscard]] PsdDecodeResult decode_psd_composite(const NativeSource &source,
                                                   const PsdDecodeLimits &limits = {});

} // namespace vove::handlers::raster
