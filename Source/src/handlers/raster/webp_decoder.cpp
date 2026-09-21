#include "vove/handlers/raster/webp_decoder.hpp"

#include "vove/color/color_transform.h"
#include "vove/handlers/raster/profile_fingerprint.hpp"
#include "vove/handlers/raster/signature_probe.hpp"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>

namespace vove::handlers::raster {
namespace {

constexpr std::size_t kMaximumDiagnosticBytes = 256;

struct DemuxDeleter {
    void operator()(WebPDemuxer *demux) const noexcept {
        WebPDemuxDelete(demux);
    }
};

struct FrameIterator final {
    WebPIterator value{};
    bool active{};

    ~FrameIterator() {
        if (active) {
            WebPDemuxReleaseIterator(&value);
        }
    }

    FrameIterator(const FrameIterator &) = delete;
    FrameIterator &operator=(const FrameIterator &) = delete;
    FrameIterator() = default;
};

struct ChunkIterator final {
    WebPChunkIterator value{};
    bool active{};

    ~ChunkIterator() {
        if (active) {
            WebPDemuxReleaseChunkIterator(&value);
        }
    }

    ChunkIterator(const ChunkIterator &) = delete;
    ChunkIterator &operator=(const ChunkIterator &) = delete;
    ChunkIterator() = default;
};

using Demux = std::unique_ptr<WebPDemuxer, DemuxDeleter>;

[[nodiscard]] WebpDecodeResult failure(const WebpDecodeErrorCode code,
                                       const std::string_view detail = {}) {
    WebpDecodeResult result;
    result.error = {.code = code, .detail = std::string(detail.substr(0, kMaximumDiagnosticBytes))};
    return result;
}

[[nodiscard]] bool valid_limits(const WebpDecodeLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 &&
           limits.maximum_source_bytes <= kMaximumWebpSourceBytes &&
           limits.maximum_source_pixels != 0 &&
           limits.maximum_source_pixels <= kMaximumWebpSourcePixels &&
           limits.maximum_output_edge != 0 &&
           limits.maximum_output_edge <= kMaximumWebpOutputEdge &&
           limits.maximum_output_bytes != 0 &&
           limits.maximum_output_bytes <= kMaximumWebpOutputBytes;
}

[[nodiscard]] bool multiply_exceeds(const std::uint64_t left, const std::uint64_t right,
                                    const std::uint64_t limit) noexcept {
    return left != 0 && right > limit / left;
}

[[nodiscard]] WebpDecodeErrorCode source_error_code(const NativeSourceError &error) noexcept {
    if (error.code == NativeSourceErrorCode::disconnected) {
        return WebpDecodeErrorCode::source_disconnected;
    }
    if (error.code == NativeSourceErrorCode::source_changed) {
        return WebpDecodeErrorCode::source_changed;
    }
    return WebpDecodeErrorCode::source_io_error;
}

[[nodiscard]] WebpDecodeResult validate_source(const NativeSource &source) {
    const auto validation = source.validate_unchanged();
    if (validation.unchanged && !validation.error) {
        return {};
    }
    return failure(source_error_code(validation.error), "WebP source changed during decode");
}

[[nodiscard]] WebpDecodeResult probe_source(const NativeSource &source) {
    const auto prefix_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), kMaxSignatureProbeBytes));
    if (prefix_size == 0) {
        return failure(WebpDecodeErrorCode::truncated_input, "empty WebP input");
    }
    std::array<std::byte, kMaxSignatureProbeBytes> prefix{};
    const auto read = source.read_at(0, std::span(prefix).first(prefix_size));
    if (!read.ok()) {
        return failure(source_error_code(read.error), "cannot read WebP signature");
    }
    if (read.bytes_read != prefix_size) {
        return failure(WebpDecodeErrorCode::truncated_input, "WebP signature is truncated");
    }
    const auto probe = probe_raster_signature(std::span(prefix).first(prefix_size));
    if (probe.status == ProbeStatus::Truncated) {
        return failure(WebpDecodeErrorCode::truncated_input, "WebP signature is truncated");
    }
    if (probe.status == ProbeStatus::Malformed) {
        return failure(WebpDecodeErrorCode::malformed_input, "invalid WebP signature");
    }
    if (!probe.matched() || probe.format != RasterFormat::Webp) {
        return failure(WebpDecodeErrorCode::unsupported_format, "input is not WebP");
    }
    return {};
}

[[nodiscard]] WebpDecodeResult read_source(const NativeSource &source,
                                           std::vector<std::byte> &bytes) {
    if (source.size() > std::numeric_limits<std::size_t>::max()) {
        return failure(WebpDecodeErrorCode::source_limit_exceeded, "WebP source is too large");
    }
    bytes.resize(static_cast<std::size_t>(source.size()));
    constexpr std::size_t kReadBlockBytes = std::size_t{1024} * 1024U;
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto count = std::min(kReadBlockBytes, bytes.size() - offset);
        const auto read = source.read_at(offset, std::span(bytes).subspan(offset, count));
        if (!read.ok()) {
            return failure(source_error_code(read.error), "cannot read WebP source");
        }
        if (read.bytes_read != count) {
            return failure(WebpDecodeErrorCode::truncated_input, "WebP source is truncated");
        }
        offset += count;
    }
    return validate_source(source);
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
scaled_size(const std::uint32_t width, const std::uint32_t height,
            const std::uint32_t maximum_edge) noexcept {
    if (width <= maximum_edge && height <= maximum_edge) {
        return {width, height};
    }
    const auto scale =
        static_cast<double>(maximum_edge) / static_cast<double>(std::max(width, height));
    const auto scaled_width = std::max(1U, static_cast<std::uint32_t>(std::lround(width * scale)));
    const auto scaled_height =
        std::max(1U, static_cast<std::uint32_t>(std::lround(height * scale)));
    return {std::min(scaled_width, maximum_edge), std::min(scaled_height, maximum_edge)};
}

struct ScaleCoordinate {
    std::uint32_t value{};
    std::uint32_t output_extent{};
    std::uint32_t source_extent{};
};

[[nodiscard]] std::uint32_t scaled_floor(const ScaleCoordinate coordinate) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(coordinate.value) * coordinate.output_extent) /
        coordinate.source_extent);
}

[[nodiscard]] std::uint32_t scaled_ceil(const ScaleCoordinate coordinate) noexcept {
    const auto numerator = static_cast<std::uint64_t>(coordinate.value) * coordinate.output_extent;
    return static_cast<std::uint32_t>((numerator + coordinate.source_extent - 1U) /
                                      coordinate.source_extent);
}

[[nodiscard]] WebpDecodeResult map_decode_status(const VP8StatusCode status) {
    switch (status) {
    case VP8_STATUS_NOT_ENOUGH_DATA:
    case VP8_STATUS_SUSPENDED:
        return failure(WebpDecodeErrorCode::truncated_input, "WebP frame is truncated");
    case VP8_STATUS_BITSTREAM_ERROR:
        return failure(WebpDecodeErrorCode::malformed_input, "invalid WebP bitstream");
    case VP8_STATUS_UNSUPPORTED_FEATURE:
        return failure(WebpDecodeErrorCode::unsupported_format, "unsupported WebP feature");
    case VP8_STATUS_OUT_OF_MEMORY:
        return failure(WebpDecodeErrorCode::resource_limit_exceeded,
                       "WebP decoder resource limit exceeded");
    case VP8_STATUS_INVALID_PARAM:
        return failure(WebpDecodeErrorCode::decode_failed, "invalid WebP decoder parameters");
    case VP8_STATUS_USER_ABORT:
        return failure(WebpDecodeErrorCode::decode_failed, "WebP decode aborted");
    case VP8_STATUS_OK:
        return {};
    }
    return failure(WebpDecodeErrorCode::decode_failed, "WebP decode failed");
}

[[nodiscard]] std::array<std::byte, 4> animation_background(const std::uint32_t packed) noexcept {
    return {std::byte{static_cast<unsigned char>((packed >> 16U) & 0xFFU)},
            std::byte{static_cast<unsigned char>((packed >> 8U) & 0xFFU)},
            std::byte{static_cast<unsigned char>(packed & 0xFFU)},
            std::byte{static_cast<unsigned char>((packed >> 24U) & 0xFFU)}};
}

void fill_canvas(std::vector<std::byte> &canvas, const std::array<std::byte, 4> color) noexcept {
    for (std::size_t offset = 0; offset < canvas.size(); offset += 4U) {
        std::copy(color.begin(), color.end(), canvas.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void composite_pixel(std::byte *destination, const std::byte *source) noexcept {
    const auto source_alpha = std::to_integer<unsigned>(source[3]);
    if (source_alpha == 255U) {
        std::copy_n(source, 4, destination);
        return;
    }
    if (source_alpha == 0U) {
        return;
    }
    const auto destination_alpha = std::to_integer<unsigned>(destination[3]);
    const auto inverse_source = 255U - source_alpha;
    const auto output_alpha = source_alpha + (destination_alpha * inverse_source + 127U) / 255U;
    if (output_alpha == 0U) {
        std::fill_n(destination, 4, std::byte{});
        return;
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto source_value = std::to_integer<unsigned>(source[channel]);
        const auto destination_value = std::to_integer<unsigned>(destination[channel]);
        const auto premultiplied = source_value * source_alpha * 255U +
                                   destination_value * destination_alpha * inverse_source;
        destination[channel] = std::byte{static_cast<unsigned char>(
            (premultiplied + output_alpha * 127U) / (output_alpha * 255U))};
    }
    destination[3] = std::byte{static_cast<unsigned char>(output_alpha)};
}

struct FramePlacement {
    std::uint32_t canvas_width{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool blend{};
};

void place_frame(std::span<std::byte> canvas, const FramePlacement placement,
                 const std::span<const std::byte> frame) noexcept {
    for (std::uint32_t row = 0; row < placement.height; ++row) {
        for (std::uint32_t column = 0; column < placement.width; ++column) {
            const auto destination_offset =
                (static_cast<std::size_t>(placement.y + row) * placement.canvas_width +
                 placement.x + column) *
                4U;
            const auto source_offset =
                (static_cast<std::size_t>(row) * placement.width + column) * 4U;
            if (placement.blend) {
                composite_pixel(canvas.data() + destination_offset, frame.data() + source_offset);
            } else {
                std::copy_n(frame.data() + source_offset, 4, canvas.data() + destination_offset);
            }
        }
    }
}

[[nodiscard]] WebpDecodeResult
transform_color(std::vector<std::byte> &rgba, const std::uint32_t width, const std::uint32_t height,
                const std::span<const std::byte> icc, WebpMetadata &metadata) {
    metadata.source_color_model = "RGB";
    metadata.source_profile_fingerprint = profile_sha256(icc);
    if (icc.empty()) {
        metadata.source_color_profile = "sRGB IEC61966-2.1 (assumed)";
        return {};
    }
    const auto transformed =
        vove::color::to_srgb_rgba8({.width = width,
                                    .height = height,
                                    .source_stride = static_cast<std::size_t>(width) * 4U,
                                    .source_format = vove::color::PixelFormat::rgba8,
                                    .source_pixels = rgba,
                                    .embedded_icc = icc,
                                    .fallback_cmyk_icc = {}});
    if (!transformed.ok()) {
        if (transformed.error == vove::color::TransformError::resource_limit) {
            return failure(WebpDecodeErrorCode::resource_limit_exceeded,
                           "WebP ICC transform exceeds resource limit");
        }
        return failure(WebpDecodeErrorCode::unsupported_color, transformed.detail.empty()
                                                                   ? "unsupported WebP ICC profile"
                                                                   : transformed.detail);
    }
    rgba = transformed.rgba_pixels;
    metadata.source_color_model = transformed.metadata.source_model;
    metadata.source_color_profile = transformed.metadata.source_profile;
    metadata.used_embedded_icc = transformed.metadata.used_embedded_profile;
    return {};
}

[[nodiscard]] WebpDecodeResult decode_impl(const NativeSource &source,
                                           const WebpDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(WebpDecodeErrorCode::invalid_limits, "invalid WebP decode limits");
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(WebpDecodeErrorCode::source_limit_exceeded, "WebP source exceeds limit");
    }
    if (const auto probe = probe_source(source); probe.error) {
        return probe;
    }

    std::vector<std::byte> bytes;
    if (const auto read = read_source(source, bytes); read.error) {
        return read;
    }
    const WebPData data{reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
    WebPDemuxState demux_state = WEBP_DEMUX_PARSE_ERROR;
    Demux demux(WebPDemuxInternal(&data, 0, &demux_state, WEBP_DEMUX_ABI_VERSION));
    if (!demux) {
        return failure(demux_state == WEBP_DEMUX_PARSING_HEADER
                           ? WebpDecodeErrorCode::truncated_input
                           : WebpDecodeErrorCode::malformed_input,
                       "cannot parse WebP container");
    }
    if (demux_state != WEBP_DEMUX_DONE) {
        return failure(WebpDecodeErrorCode::truncated_input, "WebP container is incomplete");
    }

    const auto canvas_width = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
    const auto canvas_height = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    const auto frame_count = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
    const auto flags = WebPDemuxGetI(demux.get(), WEBP_FF_FORMAT_FLAGS);
    if (canvas_width == 0 || canvas_height == 0 || frame_count == 0) {
        return failure(WebpDecodeErrorCode::malformed_input, "WebP has empty canvas");
    }
    if (frame_count > kMaximumWebpFrames) {
        return failure(WebpDecodeErrorCode::resource_limit_exceeded,
                       "WebP frame count exceeds limit");
    }
    if (multiply_exceeds(canvas_width, canvas_height, limits.maximum_source_pixels)) {
        return failure(WebpDecodeErrorCode::dimension_limit_exceeded,
                       "WebP canvas exceeds pixel limit");
    }

    FrameIterator frame;
    frame.active = WebPDemuxGetFrame(demux.get(), 1, &frame.value) != 0;
    if (!frame.active || frame.value.complete == 0 || frame.value.width <= 0 ||
        frame.value.height <= 0 || frame.value.x_offset < 0 || frame.value.y_offset < 0) {
        return failure(WebpDecodeErrorCode::truncated_input, "WebP first frame is incomplete");
    }
    const auto frame_width = static_cast<std::uint32_t>(frame.value.width);
    const auto frame_height = static_cast<std::uint32_t>(frame.value.height);
    const auto frame_x = static_cast<std::uint32_t>(frame.value.x_offset);
    const auto frame_y = static_cast<std::uint32_t>(frame.value.y_offset);
    if (frame_x > canvas_width || frame_y > canvas_height || frame_width > canvas_width - frame_x ||
        frame_height > canvas_height - frame_y ||
        multiply_exceeds(frame_width, frame_height, limits.maximum_source_pixels)) {
        return failure(WebpDecodeErrorCode::malformed_input, "WebP frame lies outside canvas");
    }

    const auto [output_width, output_height] =
        scaled_size(canvas_width, canvas_height, limits.maximum_output_edge);
    if (multiply_exceeds(output_width, output_height, limits.maximum_output_bytes / 4U)) {
        return failure(WebpDecodeErrorCode::output_limit_exceeded,
                       "WebP output exceeds byte limit");
    }
    const auto output_bytes = static_cast<std::size_t>(output_width) * output_height * 4U;
    if (output_bytes > limits.maximum_output_bytes) {
        return failure(WebpDecodeErrorCode::output_limit_exceeded,
                       "WebP output exceeds byte limit");
    }

    const auto output_x = scaled_floor({frame_x, output_width, canvas_width});
    const auto output_y = scaled_floor({frame_y, output_height, canvas_height});
    const auto output_right = scaled_ceil({frame_x + frame_width, output_width, canvas_width});
    const auto output_bottom = scaled_ceil({frame_y + frame_height, output_height, canvas_height});
    const auto decoded_width = std::max(1U, output_right - output_x);
    const auto decoded_height = std::max(1U, output_bottom - output_y);
    if (output_x + decoded_width > output_width || output_y + decoded_height > output_height) {
        return failure(WebpDecodeErrorCode::malformed_input, "scaled WebP frame is invalid");
    }

    WebPDecoderConfig config{};
    if (WebPInitDecoderConfig(&config) == 0) {
        return failure(WebpDecodeErrorCode::decoder_unavailable, "libwebp decoder ABI mismatch");
    }
    const auto features_status =
        WebPGetFeatures(frame.value.fragment.bytes, frame.value.fragment.size, &config.input);
    if (features_status != VP8_STATUS_OK) {
        return map_decode_status(features_status);
    }
    if (config.input.width != frame.value.width || config.input.height != frame.value.height) {
        return failure(WebpDecodeErrorCode::malformed_input,
                       "WebP frame dimensions disagree with container");
    }

    std::vector<std::byte> decoded(static_cast<std::size_t>(decoded_width) * decoded_height * 4U);
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = reinterpret_cast<std::uint8_t *>(decoded.data());
    config.output.u.RGBA.stride = static_cast<int>(decoded_width * 4U);
    config.output.u.RGBA.size = decoded.size();
    config.options.use_scaling = 1;
    config.options.scaled_width = static_cast<int>(decoded_width);
    config.options.scaled_height = static_cast<int>(decoded_height);
    config.options.use_threads = 0;
    const auto decode_status =
        WebPDecode(frame.value.fragment.bytes, frame.value.fragment.size, &config);
    WebPFreeDecBuffer(&config.output);
    if (decode_status != VP8_STATUS_OK) {
        return map_decode_status(decode_status);
    }

    std::vector<std::byte> canvas(output_bytes);
    const bool animated = (flags & ANIMATION_FLAG) != 0U;
    fill_canvas(canvas, animated ? animation_background(
                                       WebPDemuxGetI(demux.get(), WEBP_FF_BACKGROUND_COLOR))
                                 : std::array<std::byte, 4>{});
    place_frame(canvas,
                {.canvas_width = output_width,
                 .x = output_x,
                 .y = output_y,
                 .width = decoded_width,
                 .height = decoded_height,
                 .blend = animated && frame.value.blend_method == WEBP_MUX_BLEND},
                decoded);

    std::vector<std::byte> icc;
    if ((flags & ICCP_FLAG) != 0U) {
        ChunkIterator chunk;
        chunk.active = WebPDemuxGetChunk(demux.get(), "ICCP", 1, &chunk.value) != 0;
        if (!chunk.active || chunk.value.chunk.bytes == nullptr || chunk.value.chunk.size == 0) {
            return failure(WebpDecodeErrorCode::malformed_input,
                           "WebP declares a missing ICC profile");
        }
        if (chunk.value.chunk.size > vove::color::kMaximumIccProfileBytes) {
            return failure(WebpDecodeErrorCode::resource_limit_exceeded,
                           "WebP ICC profile exceeds limit");
        }
        icc.assign(reinterpret_cast<const std::byte *>(chunk.value.chunk.bytes),
                   reinterpret_cast<const std::byte *>(chunk.value.chunk.bytes) +
                       chunk.value.chunk.size);
    }

    WebpMetadata metadata{.source_width = canvas_width,
                          .source_height = canvas_height,
                          .page_count = frame_count,
                          .source_color_model = {},
                          .source_color_profile = {},
                          .source_profile_fingerprint = {},
                          .used_embedded_icc = false,
                          .has_alpha = false};
    if (const auto color = transform_color(canvas, output_width, output_height, icc, metadata);
        color.error) {
        return color;
    }
    for (std::size_t offset = 3U; offset < canvas.size(); offset += 4U) {
        metadata.has_alpha =
            metadata.has_alpha || std::to_integer<unsigned>(canvas[offset]) != 255U;
    }

    if (const auto validation = validate_source(source); validation.error) {
        return validation;
    }
    WebpDecodeResult result;
    result.image = {.width = output_width,
                    .height = output_height,
                    .rgba8 = std::move(canvas),
                    .metadata = std::move(metadata)};
    return result;
}

} // namespace

WebpDecodeResult decode_webp(const NativeSource &source, const WebpDecodeLimits &limits) {
    try {
        return decode_impl(source, limits);
    } catch (const std::bad_alloc &) {
        return failure(WebpDecodeErrorCode::resource_limit_exceeded,
                       "WebP decoder allocation failed");
    } catch (const std::length_error &) {
        return failure(WebpDecodeErrorCode::resource_limit_exceeded,
                       "WebP decoder allocation exceeds limit");
    }
}

} // namespace vove::handlers::raster
