#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vove::handlers::xcf {

inline constexpr std::uint64_t kMaximumXcfSourceBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumXcfSourcePixels = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumXcfOutputEdge = 4'096;
inline constexpr std::uint64_t kMaximumXcfOutputBytes =
    static_cast<std::uint64_t>(kMaximumXcfOutputEdge) * kMaximumXcfOutputEdge * 4ULL;

enum class XcfDecodeErrorCode : std::uint8_t {
    none,
    source_io_error,
    source_disconnected,
    source_limit_exceeded,
    dimension_limit_exceeded,
    output_limit_exceeded,
    decoder_resource_limit_exceeded,
    malformed_input,
    compression_not_supported,
    unsupported_version,
    decode_failed,
    invalid_limits,
};

struct XcfDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumXcfSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumXcfSourcePixels};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{kMaximumXcfOutputBytes};
};

struct XcfDecodeError {
    XcfDecodeErrorCode code{XcfDecodeErrorCode::none};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != XcfDecodeErrorCode::none;
    }
};

struct XcfDecodedImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::string source_color_profile;
    std::string source_profile_fingerprint;
};

struct XcfDecodeResult {
    XcfDecodedImage image;
    XcfDecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && image.width != 0 && image.height != 0 && !image.rgba8.empty();
    }
};

[[nodiscard]] XcfDecodeResult decode_xcf(const raster::NativeSource &source,
                                         const XcfDecodeLimits &limits = {});

} // namespace vove::handlers::xcf
