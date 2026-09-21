#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::raster {

inline constexpr std::uint64_t kMaximumRawSourceBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL;
inline constexpr std::uint64_t kMaximumRawMetadataBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumRawPreviewBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumRawMetadataReads = 65'536;

enum class RawPreviewErrorCode : std::uint8_t {
    none,
    invalid_limits,
    not_raw,
    unsupported_format,
    embedded_preview_unavailable,
    malformed_input,
    resource_limit_exceeded,
    source_io_error,
    source_disconnected,
    source_changed,
};

struct RawPreviewError {
    RawPreviewErrorCode code{RawPreviewErrorCode::none};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != RawPreviewErrorCode::none;
    }
};

struct RawPreviewLimits {
    std::uint64_t maximum_source_bytes{kMaximumRawSourceBytes};
    std::uint64_t maximum_metadata_bytes{kMaximumRawMetadataBytes};
    std::uint64_t maximum_preview_bytes{kMaximumRawPreviewBytes};
    std::uint32_t maximum_metadata_reads{kMaximumRawMetadataReads};
};

struct RawEmbeddedPreview {
    std::vector<std::byte> jpeg;
    std::uint16_t width{};
    std::uint16_t height{};
    // Zero means the container did not provide an orientation and the JPEG decoder must use EXIF.
    std::uint16_t orientation{};
    bool adobe_rgb{};
};

struct RawPreviewResult {
    // PIEX exposes at most a full preview and a thumbnail. Preserve their preference order so the
    // platform decoder can fall back when a candidate has a valid JPEG signature but broken data.
    std::vector<RawEmbeddedPreview> previews;
    RawPreviewError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && !previews.empty();
    }
};

// Locates and copies only a JPEG preview already stored in a camera RAW container.
// Full sensor decoding and demosaicing are deliberately outside this contract.
[[nodiscard]] RawPreviewResult extract_raw_embedded_preview(const NativeSource &source,
                                                            const RawPreviewLimits &limits = {});

} // namespace vove::handlers::raster
