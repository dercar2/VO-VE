#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::raster {

inline constexpr std::uint64_t kMaximumHeicSourceBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumHeicSourcePixels = 64'000'000ULL;
inline constexpr std::uint32_t kMaximumHeicOutputEdge = 2048;
inline constexpr std::uint64_t kMaximumHeicOutputBytes =
    static_cast<std::uint64_t>(kMaximumHeicOutputEdge) * kMaximumHeicOutputEdge * 4ULL;

enum class HeicDecodeErrorCode : std::uint8_t {
    none,
    invalid_limits,
    source_limit_exceeded,
    source_io_error,
    source_disconnected,
    truncated_input,
    malformed_input,
    unsupported_format,
    unsupported_sequence,
    decoder_unavailable,
    dimension_limit_exceeded,
    unsupported_color,
    decode_failed,
    resource_limit_exceeded,
    output_limit_exceeded,
};

struct HeicDecodeError {
    HeicDecodeErrorCode code{HeicDecodeErrorCode::none};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != HeicDecodeErrorCode::none;
    }
};

struct HeicDecodeLimits {
    std::uint64_t maximum_source_bytes{kMaximumHeicSourceBytes};
    std::uint64_t maximum_source_pixels{kMaximumHeicSourcePixels};
    std::uint32_t maximum_output_edge{512};
    std::uint64_t maximum_output_bytes{kMaximumHeicOutputBytes};
};

enum class HeicColorModel : std::uint8_t {
    unknown,
    grayscale,
    rgb,
};

struct HeicMetadata {
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint32_t page_count{};
    HeicColorModel source_color_model{HeicColorModel::unknown};
    std::string source_color_profile;
    std::string source_profile_fingerprint;
    bool used_embedded_icc{};
    bool has_alpha{};
};

struct HeicImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    HeicMetadata metadata;
};

struct HeicDecodeResult {
    HeicImage image;
    HeicDecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error && image.width != 0 && image.height != 0 && !image.rgba8.empty();
    }
};

// HEVC HEIF and AVIF still images from an already-opened source; no paths or credentials.
[[nodiscard]] HeicDecodeResult decode_heic(const NativeSource &source,
                                           const HeicDecodeLimits &limits = {});

} // namespace vove::handlers::raster
