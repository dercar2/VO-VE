#pragma once

#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/raster/signature_probe.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::dxf {

inline constexpr std::uint64_t kMaximumDxfScanBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumDxfPreviewBytes = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaximumDxfLineBytes = 4U * 1024U;
inline constexpr std::uint64_t kMaximumDxfPairs = 16ULL * 1024ULL * 1024ULL;

enum class DxfPreviewStatus : std::uint8_t {
    success,
    unsupported_container,
    no_preview,
    malformed,
    resource_limit,
    io_error,
};

struct DxfPreviewLimits {
    std::uint64_t maximum_scan_bytes{kMaximumDxfScanBytes};
    std::uint64_t maximum_preview_bytes{kMaximumDxfPreviewBytes};
    std::size_t maximum_line_bytes{kMaximumDxfLineBytes};
    std::uint64_t maximum_pairs{kMaximumDxfPairs};
};

struct DxfEmbeddedCandidate {
    raster::RasterFormat format{raster::RasterFormat::Unknown};
    std::string normalized_name;
    std::vector<std::byte> encoded_image;
};

struct DxfPreviewResult {
    DxfPreviewStatus status{DxfPreviewStatus::malformed};
    DxfEmbeddedCandidate candidate;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == DxfPreviewStatus::success && !candidate.encoded_image.empty();
    }
};

[[nodiscard]] DxfPreviewResult extract_dxf_embedded_preview(const raster::NativeSource &source,
                                                            const DxfPreviewLimits &limits = {});

} // namespace vove::handlers::dxf
