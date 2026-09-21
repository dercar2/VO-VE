#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vove::handlers::raster {

inline constexpr std::size_t kMaxSignatureProbeBytes = 4096;

enum class RasterFormat : std::uint8_t {
    Unknown,
    Jpeg,
    Png,
    Bmp,
    Gif,
    Ico,
    Tiff,
    Webp,
    Heif,
    Avif,
    Psd,
    Jpegxl,
};

enum class ProbeStatus : std::uint8_t {
    Matched,
    Unsupported,
    Truncated,
    Malformed,
};

struct SignatureProbeResult {
    RasterFormat format{RasterFormat::Unknown};
    ProbeStatus status{ProbeStatus::Unsupported};

    // Total prefix size needed to continue a truncated probe; zero for terminal results.
    std::size_t required_bytes{};

    [[nodiscard]] bool matched() const noexcept {
        return status == ProbeStatus::Matched;
    }
};

// Bytes beyond kMaxSignatureProbeBytes are deliberately ignored.
[[nodiscard]] SignatureProbeResult
probe_raster_signature(std::span<const std::byte> prefix) noexcept;

} // namespace vove::handlers::raster
