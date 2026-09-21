#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::raster {

inline constexpr std::uint64_t kMaximumWebpSourceBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumWebpSourcePixels = 64'000'000ULL;
inline constexpr std::uint32_t kMaximumWebpOutputEdge = 2048;
inline constexpr std::uint32_t kMaximumWebpFrames = 4096;
inline constexpr std::uint64_t kMaximumWebpOutputBytes =
    static_cast<std::uint64_t>(kMaximumWebpOutputEdge) * kMaximumWebpOutputEdge * 4ULL;

enum class WebpDecodeErrorCode : std::uint8_t {
    none,
    invalid_limits,
    source_limit_exceeded,
    source_io_error,
    source_disconnected,
    source_changed,
    truncated_input,
    malformed_input,
    unsupported_format,
    decoder_unavailable,
    dimension_limit_exceeded,
    unsupported_color,
    decode_failed,
    resource_limit_exceeded,
    output_limit_exceeded,
};

struct WebpDecodeError {
    WebpDecodeErrorCode code{WebpDecodeErrorCode::none};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != WebpDecodeErrorCode::none;
    }
};

struct WebpDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumWebpSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumWebpSourcePixels};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{kMaximumWebpOutputBytes};
};

struct WebpMetadata {
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint32_t page_count{};
    std::string source_color_model;
    std::string source_color_profile;
    std::string source_profile_fingerprint;
    bool used_embedded_icc{};
    bool has_alpha{};
};

struct WebpImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    WebpMetadata metadata;
};

struct WebpDecodeResult {
    WebpImage image;
    WebpDecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && image.width != 0 && image.height != 0 && !image.rgba8.empty();
    }
};

// Decodes only from an already-opened source. Paths and credentials never cross this API.
[[nodiscard]] WebpDecodeResult decode_webp(const NativeSource &source,
                                           const WebpDecodeLimits &limits = {});

} // namespace vove::handlers::raster
