#pragma once

#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/raster/signature_probe.hpp"

#include <QColorSpace>
#include <QImage>
#include <QString>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vove::handlers::raster {

inline constexpr std::uint32_t kMaximumCanonicalRasterEdge = 2048;
inline constexpr std::uint64_t kMaximumQtRasterSourceBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumQtRasterSourcePixels = 64'000'000ULL;

enum class QtRasterDecodeErrorCode : std::uint8_t {
    none,
    invalid_limits,
    source_limit_exceeded,
    source_io_error,
    source_disconnected,
    truncated_input,
    malformed_input,
    unsupported_format,
    unsupported_heif,
    decoder_unavailable,
    dimension_limit_exceeded,
    color_profile_required,
    unsupported_color,
    decode_failed,
    output_limit_exceeded,
};

struct QtRasterDecodeError {
    QtRasterDecodeErrorCode code{QtRasterDecodeErrorCode::none};
    QString detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != QtRasterDecodeErrorCode::none;
    }
};

struct QtRasterDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumQtRasterSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumQtRasterSourcePixels};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{static_cast<std::uint64_t>(kMaximumCanonicalRasterEdge) *
                                       kMaximumCanonicalRasterEdge * 4ULL};
    std::span<const std::byte> fallback_cmyk_icc;
    bool apply_orientation{true};
};

struct QtRasterMetadata {
    RasterFormat format{RasterFormat::Unknown};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::optional<std::uint32_t> page_count;
    QColorSpace source_color_space;
    QColorSpace output_color_space;
    QString color_summary;
    std::string source_profile_fingerprint;
    bool assumed_srgb{};
};

struct QtRasterImage {
    QImage rgba8;
    QtRasterMetadata metadata;
};

struct QtRasterDecodeResult {
    QtRasterImage image;
    QtRasterDecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && !image.rgba8.isNull();
    }
};

// Decodes only from the already-opened, bounded native source. No path or file name is used.
[[nodiscard]] QtRasterDecodeResult decode_qt_raster(const NativeSource &source,
                                                    const QtRasterDecodeLimits &limits = {});

// Decodes a bounded embedded PNG/BMP already extracted by another isolated handler.
[[nodiscard]] QtRasterDecodeResult decode_qt_raster_bytes(std::span<const std::byte> encoded,
                                                          const QtRasterDecodeLimits &limits = {});

} // namespace vove::handlers::raster
