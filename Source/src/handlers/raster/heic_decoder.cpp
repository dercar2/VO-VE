#include "vove/handlers/raster/heic_decoder.hpp"

#include "vove/color/color_transform.h"
#include "vove/handlers/raster/profile_fingerprint.hpp"
#include "vove/handlers/raster/signature_probe.hpp"

#include <libheif/heif.h>
#include <lcms2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vove::handlers::raster {
namespace {

constexpr std::uint64_t kMaximumDecoderMemory = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumDecoderBlock = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaximumTopLevelImages = 4096;
constexpr std::size_t kMaximumDiagnosticBytes = 256;

struct ContextDeleter {
    void operator()(heif_context *context) const noexcept {
        heif_context_free(context);
    }
};

struct HandleDeleter {
    void operator()(const heif_image_handle *handle) const noexcept {
        heif_image_handle_release(handle);
    }
};

struct ImageDeleter {
    void operator()(const heif_image *image) const noexcept {
        heif_image_release(image);
    }
};

struct OptionsDeleter {
    void operator()(heif_decoding_options *options) const noexcept {
        heif_decoding_options_free(options);
    }
};

struct NclxDeleter {
    void operator()(heif_color_profile_nclx *profile) const noexcept {
        heif_nclx_color_profile_free(profile);
    }
};

using Context = std::unique_ptr<heif_context, ContextDeleter>;
using Handle = std::unique_ptr<const heif_image_handle, HandleDeleter>;
using Image = std::unique_ptr<const heif_image, ImageDeleter>;
using Options = std::unique_ptr<heif_decoding_options, OptionsDeleter>;
using Nclx = std::unique_ptr<heif_color_profile_nclx, NclxDeleter>;

struct ReaderState {
    const NativeSource *source{};
    std::uint64_t position{};
    NativeSourceError source_error;
    bool short_read{};
};

[[nodiscard]] std::string bounded_detail(const std::string_view detail) {
    return std::string(detail.substr(0, kMaximumDiagnosticBytes));
}

[[nodiscard]] HeicDecodeResult failure(const HeicDecodeErrorCode code,
                                       const std::string_view detail = {}) {
    HeicDecodeResult result;
    result.error = {.code = code, .detail = bounded_detail(detail)};
    return result;
}

[[nodiscard]] bool valid_limits(const HeicDecodeLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 &&
           limits.maximum_source_bytes <= kMaximumHeicSourceBytes &&
           limits.maximum_source_pixels != 0 &&
           limits.maximum_source_pixels <= kMaximumHeicSourcePixels &&
           limits.maximum_output_edge != 0 &&
           limits.maximum_output_edge <= kMaximumHeicOutputEdge &&
           limits.maximum_output_bytes != 0 &&
           limits.maximum_output_bytes <= kMaximumHeicOutputBytes;
}

[[nodiscard]] bool multiply_exceeds(const std::uint64_t left, const std::uint64_t right,
                                    const std::uint64_t limit) noexcept {
    return left != 0 && right > limit / left;
}

template <typename Value>
[[nodiscard]] constexpr Value bounded_limit(const Value current, const Value hard_limit) noexcept {
    return current == 0 ? hard_limit : std::min(current, hard_limit);
}

[[nodiscard]] int64_t reader_get_position(void *userdata) noexcept {
    const auto *state = static_cast<const ReaderState *>(userdata);
    if (state == nullptr || state->position > static_cast<std::uint64_t>(INT64_MAX)) {
        return -1;
    }
    return static_cast<int64_t>(state->position);
}

[[nodiscard]] int reader_read(void *data, const std::size_t size, void *userdata) noexcept {
    auto *state = static_cast<ReaderState *>(userdata);
    if (state == nullptr || state->source == nullptr || (data == nullptr && size != 0) ||
        size > std::numeric_limits<std::uint64_t>::max() - state->position) {
        return 1;
    }
    if (size == 0) {
        return 0;
    }
    auto destination = std::span(static_cast<std::byte *>(data), size);
    const auto read_result = state->source->read_at(state->position, destination);
    if (!read_result.ok()) {
        state->source_error = read_result.error;
        return 1;
    }
    if (read_result.bytes_read != size) {
        state->short_read = true;
        return 1;
    }
    state->position += size;
    return 0;
}

[[nodiscard]] int reader_seek(const int64_t position, void *userdata) noexcept {
    auto *state = static_cast<ReaderState *>(userdata);
    if (state == nullptr || state->source == nullptr || position < 0 ||
        static_cast<std::uint64_t>(position) > state->source->size()) {
        return 1;
    }
    state->position = static_cast<std::uint64_t>(position);
    return 0;
}

[[nodiscard]] heif_reader_grow_status reader_wait_for_size(const int64_t target_size,
                                                           void *userdata) noexcept {
    const auto *state = static_cast<const ReaderState *>(userdata);
    if (state == nullptr || state->source == nullptr || target_size < 0) {
        return heif_reader_grow_status_error;
    }
    return static_cast<std::uint64_t>(target_size) <= state->source->size()
               ? heif_reader_grow_status_size_reached
               : heif_reader_grow_status_size_beyond_eof;
}

constexpr heif_reader kNativeReader{
    1,
    reader_get_position,
    reader_read,
    reader_seek,
    reader_wait_for_size,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

[[nodiscard]] HeicDecodeResult map_heif_error(const heif_error error, const ReaderState &reader,
                                              const NativeSource &source) {
    if (reader.source_error) {
        return failure(reader.source_error.code == NativeSourceErrorCode::disconnected
                           ? HeicDecodeErrorCode::source_disconnected
                           : HeicDecodeErrorCode::source_io_error,
                       "source read failed");
    }
    const auto unchanged = source.validate_unchanged();
    if (!unchanged.unchanged || unchanged.error) {
        return failure(unchanged.error.code == NativeSourceErrorCode::disconnected
                           ? HeicDecodeErrorCode::source_disconnected
                           : HeicDecodeErrorCode::source_io_error,
                       "HEIC source changed during decode");
    }
    if (reader.short_read || error.subcode == heif_suberror_End_of_data) {
        return failure(HeicDecodeErrorCode::truncated_input, "HEIC input is truncated");
    }
    if (error.subcode == heif_suberror_Unsupported_color_conversion) {
        return failure(HeicDecodeErrorCode::unsupported_color,
                       "HEIF color conversion or HDR tone mapping is unsupported");
    }
    switch (error.code) {
    case heif_error_Invalid_input:
        return failure(HeicDecodeErrorCode::malformed_input, "invalid HEIC structure");
    case heif_error_Unsupported_filetype:
        return failure(HeicDecodeErrorCode::unsupported_format, "unsupported HEIF file type");
    case heif_error_Unsupported_feature:
        return failure(HeicDecodeErrorCode::unsupported_format,
                       "HEIF payload is not supported by the selected decoder");
    case heif_error_Decoder_plugin_error:
        return failure(HeicDecodeErrorCode::malformed_input, "invalid HEIF image payload");
    case heif_error_Plugin_loading_error:
        return failure(HeicDecodeErrorCode::decoder_unavailable, "HEIF decoder is unavailable");
    case heif_error_Memory_allocation_error:
        return failure(HeicDecodeErrorCode::resource_limit_exceeded,
                       "HEIC decoder resource limit exceeded");
    case heif_error_Color_profile_does_not_exist:
        return failure(HeicDecodeErrorCode::unsupported_color, "unsupported HEIC color profile");
    case heif_error_Usage_error:
    case heif_error_Canceled:
    case heif_error_End_of_sequence:
    case heif_error_Encoding_error:
    case heif_error_Encoder_plugin_error:
    case heif_error_Input_does_not_exist:
        return failure(HeicDecodeErrorCode::decode_failed, "HEIC decode failed");
    case heif_error_Ok:
        break;
    }
    return failure(HeicDecodeErrorCode::decode_failed, "HEIC decode failed");
}

[[nodiscard]] HeicDecodeResult probe_source(const NativeSource &source, bool &avif) {
    const auto prefix_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), kMaxSignatureProbeBytes));
    if (prefix_size == 0) {
        return failure(HeicDecodeErrorCode::truncated_input, "empty HEIC input");
    }
    std::array<std::byte, kMaxSignatureProbeBytes> prefix{};
    const auto read_result = source.read_at(0, std::span(prefix).first(prefix_size));
    if (!read_result.ok() || read_result.bytes_read != prefix_size) {
        return failure(read_result.error.code == NativeSourceErrorCode::disconnected
                           ? HeicDecodeErrorCode::source_disconnected
                           : HeicDecodeErrorCode::source_io_error,
                       "cannot read HEIC signature");
    }
    const auto probe = probe_raster_signature(std::span(prefix).first(prefix_size));
    if (probe.status == ProbeStatus::Truncated) {
        return failure(HeicDecodeErrorCode::truncated_input, "HEIF signature is truncated");
    }
    if (probe.status == ProbeStatus::Malformed) {
        return failure(HeicDecodeErrorCode::malformed_input, "invalid HEIF signature");
    }
    avif = probe.format == RasterFormat::Avif;
    if (avif && !probe.matched()) {
        return failure(HeicDecodeErrorCode::unsupported_sequence,
                       "AVIF sequence without a still image is not supported");
    }
    if (!probe.matched() || (!avif && probe.format != RasterFormat::Heif)) {
        return failure(HeicDecodeErrorCode::unsupported_format, "input is not HEVC HEIF or AVIF");
    }
    return {};
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

[[nodiscard]] bool is_hdr_transfer(const heif_transfer_characteristics transfer) noexcept {
    return transfer == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ ||
           transfer == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG;
}

[[nodiscard]] std::vector<std::byte> read_icc(const heif_image_handle *handle,
                                              HeicDecodeError &error) {
    const auto profile_size = heif_image_handle_get_raw_color_profile_size(handle);
    if (profile_size == 0) {
        return {};
    }
    if (profile_size > vove::color::kMaximumIccProfileBytes) {
        error = {.code = HeicDecodeErrorCode::resource_limit_exceeded,
                 .detail = "embedded ICC profile exceeds limit"};
        return {};
    }
    std::vector<std::byte> profile(profile_size);
    const auto read_result = heif_image_handle_get_raw_color_profile(handle, profile.data());
    if (read_result.code != heif_error_Ok) {
        error = {.code = HeicDecodeErrorCode::unsupported_color,
                 .detail = "cannot read embedded ICC profile"};
        return {};
    }
    return profile;
}

[[nodiscard]] Nclx read_nclx(const heif_image_handle *handle) noexcept {
    heif_color_profile_nclx *raw_profile = nullptr;
    const auto result = heif_image_handle_get_nclx_color_profile(handle, &raw_profile);
    return result.code == heif_error_Ok ? Nclx(raw_profile) : Nclx{};
}

[[nodiscard]] std::vector<std::byte> nclx_icc(heif_color_profile_nclx profile) {
    if (profile.color_primaries == heif_color_primaries_unspecified) {
        profile.color_primary_red_x = 0.64F;
        profile.color_primary_red_y = 0.33F;
        profile.color_primary_green_x = 0.30F;
        profile.color_primary_green_y = 0.60F;
        profile.color_primary_blue_x = 0.15F;
        profile.color_primary_blue_y = 0.06F;
        profile.color_primary_white_x = 0.3127F;
        profile.color_primary_white_y = 0.3290F;
    }
    const cmsCIExyY white{profile.color_primary_white_x, profile.color_primary_white_y, 1};
    const cmsCIExyYTRIPLE primaries{
        {profile.color_primary_red_x, profile.color_primary_red_y, 1},
        {profile.color_primary_green_x, profile.color_primary_green_y, 1},
        {profile.color_primary_blue_x, profile.color_primary_blue_y, 1}};
    for (const auto &point : {white, primaries.Red, primaries.Green, primaries.Blue}) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x <= 0 ||
            point.y <= 0 || point.x + point.y > 1.0001) {
            return {};
        }
    }
    cmsToneCurve *raw_curve{};
    std::array<double, 5> parameters{};
    switch (profile.transfer_characteristics) {
    case heif_transfer_characteristic_linear:
        raw_curve = cmsBuildGamma(nullptr, 1);
        break;
    case heif_transfer_characteristic_ITU_R_BT_470_6_System_M:
        raw_curve = cmsBuildGamma(nullptr, 2.2);
        break;
    case heif_transfer_characteristic_ITU_R_BT_470_6_System_B_G:
        raw_curve = cmsBuildGamma(nullptr, 2.8);
        break;
    case heif_transfer_characteristic_ITU_R_BT_709_5:
    case heif_transfer_characteristic_ITU_R_BT_601_6:
    case heif_transfer_characteristic_ITU_R_BT_2020_2_10bit:
    case heif_transfer_characteristic_ITU_R_BT_2020_2_12bit:
        parameters = {1 / 0.45, 1 / 1.09929682680944, 0.09929682680944 / 1.09929682680944,
                      1 / 4.5, 4.5 * 0.018053968510807};
        break;
    case heif_transfer_characteristic_SMPTE_240M:
        parameters = {1 / 0.45, 1 / 1.1115, 0.1115 / 1.1115, 1 / 4.0, 0.0912};
        break;
    case heif_transfer_characteristic_unspecified:
    case heif_transfer_characteristic_IEC_61966_2_1:
        parameters = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
        break;
    default:
        return {};
    }
    if (!raw_curve && parameters[0] != 0) {
        raw_curve = cmsBuildParametricToneCurve(nullptr, 4, parameters.data());
    }
    const std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)> curve(raw_curve,
                                                                         cmsFreeToneCurve);
    if (!curve) {
        return {};
    }
    const std::array curves{curve.get(), curve.get(), curve.get()};
    const std::unique_ptr<void, decltype(&cmsCloseProfile)> icc(
        cmsCreateRGBProfile(&white, &primaries, curves.data()), cmsCloseProfile);
    cmsUInt32Number size{};
    if (!icc || !cmsSaveProfileToMem(icc.get(), nullptr, &size) || size == 0 || size > 65536) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!cmsSaveProfileToMem(icc.get(), bytes.data(), &size)) {
        return {};
    }
    return bytes;
}

[[nodiscard]] HeicDecodeResult decode_impl(const NativeSource &source,
                                           const HeicDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(HeicDecodeErrorCode::invalid_limits, "invalid HEIC decode limits");
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(HeicDecodeErrorCode::source_limit_exceeded, "HEIC source exceeds limit");
    }
    bool avif{};
    if (const auto probe = probe_source(source, avif); probe.error) {
        return probe;
    }

    static const heif_error library_status = heif_init(nullptr);
    if (library_status.code != heif_error_Ok ||
        heif_have_decoder_for_format(avif ? heif_compression_AV1 : heif_compression_HEVC) == 0) {
        return failure(HeicDecodeErrorCode::decoder_unavailable,
                       avif ? "AV1 decoder is unavailable" : "HEVC decoder is unavailable");
    }

    Context context(heif_context_alloc());
    if (!context) {
        return failure(HeicDecodeErrorCode::resource_limit_exceeded,
                       "cannot allocate HEIC context");
    }
    auto *security = heif_context_get_security_limits(context.get());
    if (security == nullptr) {
        return failure(HeicDecodeErrorCode::resource_limit_exceeded,
                       "HEIC security limits unavailable");
    }
    security->max_image_size_pixels =
        bounded_limit(security->max_image_size_pixels, limits.maximum_source_pixels);
    security->max_items = bounded_limit(security->max_items, kMaximumTopLevelImages);
    security->max_color_profile_size = bounded_limit(
        security->max_color_profile_size,
        static_cast<std::uint32_t>(std::min<std::size_t>(
            vove::color::kMaximumIccProfileBytes, std::numeric_limits<std::uint32_t>::max())));
    security->max_memory_block_size =
        bounded_limit(security->max_memory_block_size, kMaximumDecoderBlock);
    security->max_total_memory = bounded_limit(security->max_total_memory, kMaximumDecoderMemory);
    heif_context_set_max_decoding_threads(context.get(), 0);

    ReaderState reader{
        .source = &source,
        .position = 0,
        .source_error = {},
        .short_read = false,
    };
    const auto read_result =
        heif_context_read_from_reader(context.get(), &kNativeReader, &reader, nullptr);
    if (read_result.code != heif_error_Ok) {
        return map_heif_error(read_result, reader, source);
    }

    const auto top_level_images = heif_context_get_number_of_top_level_images(context.get());
    if (top_level_images <= 0) {
        return failure(HeicDecodeErrorCode::malformed_input, "HEIC has no top-level image");
    }
    if (top_level_images > static_cast<int>(kMaximumTopLevelImages)) {
        return failure(HeicDecodeErrorCode::resource_limit_exceeded,
                       "too many HEIC top-level images");
    }

    heif_image_handle *raw_handle = nullptr;
    const auto primary_result = heif_context_get_primary_image_handle(context.get(), &raw_handle);
    if (primary_result.code != heif_error_Ok || raw_handle == nullptr) {
        return map_heif_error(primary_result, reader, source);
    }
    Handle handle(raw_handle);

    const auto width = heif_image_handle_get_width(handle.get());
    const auto height = heif_image_handle_get_height(handle.get());
    if (width <= 0 || height <= 0) {
        return failure(HeicDecodeErrorCode::malformed_input, "invalid HEIC dimensions");
    }
    const auto source_width = static_cast<std::uint32_t>(width);
    const auto source_height = static_cast<std::uint32_t>(height);
    if (multiply_exceeds(source_width, source_height, limits.maximum_source_pixels)) {
        return failure(HeicDecodeErrorCode::dimension_limit_exceeded,
                       "HEIC dimensions exceed pixel limit");
    }
    const auto [planned_width, planned_height] =
        scaled_size(source_width, source_height, limits.maximum_output_edge);
    if (multiply_exceeds(planned_width, planned_height, limits.maximum_output_bytes / 4ULL)) {
        return failure(HeicDecodeErrorCode::output_limit_exceeded,
                       "HEIC thumbnail exceeds output limit");
    }

    heif_colorspace preferred_colorspace = heif_colorspace_undefined;
    heif_chroma preferred_chroma = heif_chroma_undefined;
    const auto preferred_result = heif_image_handle_get_preferred_decoding_colorspace(
        handle.get(), &preferred_colorspace, &preferred_chroma);
    static_cast<void>(preferred_chroma);
    if (preferred_result.code != heif_error_Ok) {
        return map_heif_error(preferred_result, reader, source);
    }

    const auto luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle.get());
    const auto chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle.get());
    if (luma_bits <= 0 ||
        (preferred_colorspace != heif_colorspace_monochrome && chroma_bits <= 0)) {
        return failure(HeicDecodeErrorCode::unsupported_color,
                       "HEIC bit depth cannot be determined safely");
    }
    const auto high_bit_depth = luma_bits > 8 || chroma_bits > 8;

    HeicDecodeError color_error;
    auto embedded_icc = read_icc(handle.get(), color_error);
    if (color_error) {
        HeicDecodeResult result;
        result.error = std::move(color_error);
        return result;
    }
    auto source_nclx = read_nclx(handle.get());
    if (source_nclx && is_hdr_transfer(source_nclx->transfer_characteristics)) {
        return failure(HeicDecodeErrorCode::unsupported_color, "PQ/HLG HEIC requires tone mapping");
    }

    Options options(heif_decoding_options_alloc());
    if (!options) {
        return failure(HeicDecodeErrorCode::resource_limit_exceeded,
                       "cannot allocate HEIC decode options");
    }
    options->ignore_transformations = 0;
    // The canonical preview is RGBA8. Higher-depth SDR is quantized by libheif before
    // the LCMS transform; PQ/HLG requires a separate, explicit tone-mapping policy.
    options->convert_hdr_to_8bit = high_bit_depth ? 1 : 0;
    options->strict_decoding = 1;
    options->num_library_threads = 0;
    options->num_codec_threads = 1;

    // libheif converts the YCbCr matrix, not RGB primaries/transfer. Keep native RGB
    // samples, then use either the embedded ICC or an NCLX-derived profile in LCMS.
    options->output_image_nclx_profile = nullptr;
    options->output_image_nclx_profile_passthrough = 1;

    heif_image *raw_image = nullptr;
    const auto decode_result = heif_decode_image(handle.get(), &raw_image, heif_colorspace_RGB,
                                                 heif_chroma_interleaved_RGBA, options.get());
    if (decode_result.code != heif_error_Ok || raw_image == nullptr) {
        return map_heif_error(decode_result, reader, source);
    }
    Image decoded(raw_image);

    if (!source_nclx) {
        heif_color_profile_nclx *decoded_nclx{};
        if (heif_image_get_nclx_color_profile(decoded.get(), &decoded_nclx).code == heif_error_Ok) {
            source_nclx.reset(decoded_nclx);
        }
    }
    if (source_nclx && is_hdr_transfer(source_nclx->transfer_characteristics)) {
        return failure(HeicDecodeErrorCode::unsupported_color, "PQ/HLG HEIF requires tone mapping");
    }

    auto decoded_width = heif_image_get_width(decoded.get(), heif_channel_interleaved);
    auto decoded_height = heif_image_get_height(decoded.get(), heif_channel_interleaved);
    if (decoded_width <= 0 || decoded_height <= 0) {
        return failure(HeicDecodeErrorCode::decode_failed, "decoded HEIC has no RGBA plane");
    }

    const auto [target_width, target_height] =
        scaled_size(static_cast<std::uint32_t>(decoded_width),
                    static_cast<std::uint32_t>(decoded_height), limits.maximum_output_edge);
    if (multiply_exceeds(target_width, target_height, limits.maximum_output_bytes / 4ULL)) {
        return failure(HeicDecodeErrorCode::output_limit_exceeded,
                       "HEIC thumbnail exceeds output limit");
    }

    Image scaled;
    const heif_image *canonical_image = decoded.get();
    if (target_width != static_cast<std::uint32_t>(decoded_width) ||
        target_height != static_cast<std::uint32_t>(decoded_height)) {
        heif_image *raw_scaled = nullptr;
        const auto scale_result =
            heif_image_scale_image(decoded.get(), &raw_scaled, static_cast<int>(target_width),
                                   static_cast<int>(target_height), nullptr);
        if (scale_result.code != heif_error_Ok || raw_scaled == nullptr) {
            return map_heif_error(scale_result, reader, source);
        }
        scaled.reset(raw_scaled);
        canonical_image = scaled.get();
        decoded_width = heif_image_get_width(canonical_image, heif_channel_interleaved);
        decoded_height = heif_image_get_height(canonical_image, heif_channel_interleaved);
        if (decoded_width != static_cast<int>(target_width) ||
            decoded_height != static_cast<int>(target_height)) {
            return failure(HeicDecodeErrorCode::decode_failed,
                           "HEIC scaler returned unexpected dimensions");
        }
    }

    std::size_t stride{};
    const auto *plane =
        heif_image_get_plane_readonly2(canonical_image, heif_channel_interleaved, &stride);
    const auto row_bytes = static_cast<std::size_t>(decoded_width) * 4U;
    if (plane == nullptr || stride < row_bytes ||
        static_cast<std::uint64_t>(row_bytes) * static_cast<std::uint64_t>(decoded_height) >
            limits.maximum_output_bytes) {
        return failure(HeicDecodeErrorCode::output_limit_exceeded,
                       "invalid or oversized HEIC RGBA plane");
    }

    std::vector<std::byte> rgba(row_bytes * static_cast<std::size_t>(decoded_height));
    for (int row = 0; row < decoded_height; ++row) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(row) * row_bytes,
                    plane + static_cast<std::size_t>(row) * stride, row_bytes);
    }

    const auto source_model = preferred_colorspace == heif_colorspace_monochrome
                                  ? HeicColorModel::grayscale
                                  : HeicColorModel::rgb;
    std::string source_profile = source_nclx ? "NCLX" : "sRGB (assumed)";
    auto source_profile_fingerprint = profile_sha256(embedded_icc);
    auto conversion_icc = embedded_icc;
    if (conversion_icc.empty() && source_nclx) {
        conversion_icc = nclx_icc(*source_nclx);
        if (conversion_icc.empty()) {
            return failure(HeicDecodeErrorCode::unsupported_color,
                           "unsupported HEIF SDR color primaries or transfer");
        }
        source_profile = "NCLX " + std::to_string(source_nclx->color_primaries) + "/" +
                         std::to_string(source_nclx->transfer_characteristics);
    }
    if (!conversion_icc.empty()) {
        const auto transformed = vove::color::to_srgb_rgba8({
            .width = static_cast<std::uint32_t>(decoded_width),
            .height = static_cast<std::uint32_t>(decoded_height),
            .source_stride = row_bytes,
            .source_format = vove::color::PixelFormat::rgba8,
            .source_pixels = rgba,
            .embedded_icc = conversion_icc,
            .fallback_cmyk_icc = {},
        });
        if (!transformed.ok()) {
            return failure(transformed.error == vove::color::TransformError::resource_limit
                               ? HeicDecodeErrorCode::resource_limit_exceeded
                               : HeicDecodeErrorCode::unsupported_color,
                           transformed.detail);
        }
        rgba = transformed.rgba_pixels;
        if (!embedded_icc.empty()) {
            source_profile = transformed.metadata.source_profile;
        }
    }

    const auto unchanged = source.validate_unchanged();
    if (!unchanged.unchanged || unchanged.error) {
        return failure(unchanged.error.code == NativeSourceErrorCode::disconnected
                           ? HeicDecodeErrorCode::source_disconnected
                           : HeicDecodeErrorCode::source_io_error,
                       "HEIC source changed during decode");
    }

    HeicDecodeResult result;
    result.image = {
        .width = static_cast<std::uint32_t>(decoded_width),
        .height = static_cast<std::uint32_t>(decoded_height),
        .rgba8 = std::move(rgba),
        .metadata =
            {
                .source_width = source_width,
                .source_height = source_height,
                .page_count = static_cast<std::uint32_t>(top_level_images),
                .source_color_model = source_model,
                .source_color_profile = std::move(source_profile),
                .source_profile_fingerprint = std::move(source_profile_fingerprint),
                .used_embedded_icc = !embedded_icc.empty(),
                .has_alpha = heif_image_handle_has_alpha_channel(handle.get()) != 0,
            },
    };
    return result;
}

} // namespace

HeicDecodeResult decode_heic(const NativeSource &source, const HeicDecodeLimits &limits) {
    try {
        return decode_impl(source, limits);
    } catch (const std::bad_alloc &) {
        return failure(HeicDecodeErrorCode::resource_limit_exceeded, "HEIC allocation failed");
    } catch (const std::length_error &) {
        return failure(HeicDecodeErrorCode::resource_limit_exceeded, "HEIC allocation overflow");
    }
}

} // namespace vove::handlers::raster
