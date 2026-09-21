#include "vove/handlers/raster/psd_decoder.hpp"

#include "miniz_tinfl.h"
#include "vove/color/color_transform.h"
#include "vove/handlers/raster/profile_fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vove::handlers::raster {
namespace {

constexpr std::array kPsdSignature{std::byte{'8'}, std::byte{'B'}, std::byte{'P'}, std::byte{'S'}};
constexpr std::array kResourceSignature{std::byte{'8'}, std::byte{'B'}, std::byte{'I'},
                                        std::byte{'M'}};
constexpr std::array kLargeResourceSignature{std::byte{'8'}, std::byte{'B'}, std::byte{'6'},
                                             std::byte{'4'}};
constexpr std::array kMergedTransparencyKey{std::byte{'M'}, std::byte{'t'}, std::byte{'r'},
                                            std::byte{'n'}};
constexpr std::array kMergedTransparency16Key{std::byte{'M'}, std::byte{'t'}, std::byte{'1'},
                                              std::byte{'6'}};
constexpr std::array kMergedTransparency32Key{std::byte{'M'}, std::byte{'t'}, std::byte{'3'},
                                              std::byte{'2'}};
constexpr std::uint16_t kPsdVersion = 1;
constexpr std::uint16_t kPsbVersion = 2;
constexpr std::uint16_t kIccResourceId = 0x040f;
constexpr std::uint16_t kCompressionRaw = 0;
constexpr std::uint16_t kCompressionRle = 1;
constexpr std::uint16_t kCompressionZip = 2;
constexpr std::uint16_t kCompressionZipPrediction = 3;
constexpr std::uint16_t kModeGrayscale = 1;
constexpr std::uint16_t kModeRgb = 3;
constexpr std::uint16_t kModeCmyk = 4;
constexpr std::uint16_t kModeLab = 9;
constexpr std::uint16_t kMaximumChannels = 56;

struct Header {
    bool large_document{};
    std::uint16_t channels{};
    std::uint32_t height{};
    std::uint32_t width{};
    std::uint16_t depth{};
    std::uint16_t color_mode{};
};

struct Layout {
    Header header;
    std::uint16_t base_channels{};
    color::PixelFormat pixel_format{color::PixelFormat::rgb8};
    PsdColorModel color_model{PsdColorModel::rgb};
    std::uint64_t image_data_offset{};
    std::uint16_t compression{};
    std::size_t row_bytes{};
    std::vector<std::byte> embedded_icc;
    bool merged_transparency{};
};

struct ReadFailure {
    NativeSourceErrorCode code{NativeSourceErrorCode::none};
};

[[nodiscard]] PsdDecodeResult failure(const PsdDecodeErrorCode code, std::string detail) {
    return {.image = {}, .error = {.code = code, .detail = std::move(detail)}};
}

[[nodiscard]] PsdDecodeErrorCode source_error(const NativeSourceErrorCode code) noexcept {
    switch (code) {
    case NativeSourceErrorCode::disconnected:
        return PsdDecodeErrorCode::source_disconnected;
    case NativeSourceErrorCode::source_changed:
        return PsdDecodeErrorCode::source_changed;
    case NativeSourceErrorCode::size_limit_exceeded:
    case NativeSourceErrorCode::offset_out_of_range:
    case NativeSourceErrorCode::offset_overflow:
        return PsdDecodeErrorCode::source_limit_exceeded;
    case NativeSourceErrorCode::io_error:
        return PsdDecodeErrorCode::source_io_error;
    case NativeSourceErrorCode::none:
    case NativeSourceErrorCode::invalid_handle:
    case NativeSourceErrorCode::not_regular_file:
        return PsdDecodeErrorCode::malformed_input;
    }
    return PsdDecodeErrorCode::source_io_error;
}

[[nodiscard]] bool read_exact(const NativeSource &source, const std::uint64_t offset,
                              const std::span<std::byte> output, ReadFailure &read_failure) {
    const auto result = source.read_at(offset, output);
    if (!result.ok() || result.bytes_read != output.size()) {
        read_failure.code = result.error.code;
        if (read_failure.code == NativeSourceErrorCode::none) {
            const auto validation = source.validate_unchanged();
            read_failure.code = validation.unchanged
                                    ? NativeSourceErrorCode::io_error
                                    : (validation.error.code == NativeSourceErrorCode::none
                                           ? NativeSourceErrorCode::source_changed
                                           : validation.error.code);
        }
        return false;
    }
    return true;
}

[[nodiscard]] std::uint16_t be16(const std::span<const std::byte> bytes,
                                 const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
                                      std::to_integer<std::uint16_t>(bytes[offset + 1U]));
}

[[nodiscard]] std::uint32_t be32(const std::span<const std::byte> bytes,
                                 const std::size_t offset) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::uint64_t be64(const std::span<const std::byte> bytes,
                                const std::size_t offset) noexcept {
    return (static_cast<std::uint64_t>(be32(bytes, offset)) << 32U) | be32(bytes, offset + 4U);
}

[[nodiscard]] bool large_tag_length(const std::span<const std::byte> key) noexcept {
    constexpr std::array<std::string_view, 19> keys{
        "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk",
        "lnk2", "FEid", "FXid", "PxSD", "lnk3", "lnkE", "FELS", "pths", "extd", "extn"};
    return std::ranges::any_of(keys, [key](const auto value) {
        return std::equal(key.begin(), key.end(), value.begin(),
                          [](std::byte a, char b) { return a == static_cast<std::byte>(b); });
    });
}

[[nodiscard]] bool checked_add(const std::uint64_t left, const std::uint64_t right,
                               std::uint64_t &result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                                    std::uint64_t &result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool section_end(const std::uint64_t payload_offset, const std::uint64_t length,
                               const std::uint64_t source_size, std::uint64_t &end) noexcept {
    return checked_add(payload_offset, length, end) && end <= source_size;
}

[[nodiscard]] PsdDecodeResult read_u32(const NativeSource &source, const std::uint64_t offset,
                                       std::uint32_t &value) {
    std::array<std::byte, 4> bytes{};
    ReadFailure read_failure;
    if (!read_exact(source, offset, bytes, read_failure)) {
        return failure(source_error(read_failure.code), "PSD section length is unavailable");
    }
    value = be32(bytes, 0);
    return {};
}

[[nodiscard]] PsdDecodeResult read_section_length(const NativeSource &source,
                                                 const std::uint64_t offset,
                                                 const bool large, std::uint64_t &value) {
    std::array<std::byte, 8> bytes{};
    ReadFailure read_failure;
    const auto count = large ? 8U : 4U;
    if (!read_exact(source, offset, std::span(bytes).first(count), read_failure)) {
        return failure(source_error(read_failure.code), "PSD/PSB section length is unavailable");
    }
    value = large ? be64(bytes, 0) : be32(bytes, 0);
    return {};
}

[[nodiscard]] PsdDecodeResult parse_resources(const NativeSource &source, const std::uint64_t begin,
                                              const std::uint64_t end,
                                              std::vector<std::byte> &icc) {
    auto cursor = begin;
    std::size_t resource_count{};
    while (cursor < end) {
        if (++resource_count > 4096U) {
            return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                           "PSD image-resource count exceeds its limit");
        }
        if (end - cursor < 12U) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD image-resource record is truncated");
        }
        std::array<std::byte, 7> prefix{};
        ReadFailure read_failure;
        if (!read_exact(source, cursor, prefix, read_failure)) {
            return failure(source_error(read_failure.code),
                           "PSD image-resource header is unavailable");
        }
        if (!std::equal(kResourceSignature.cbegin(), kResourceSignature.cend(), prefix.cbegin())) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD image-resource signature is invalid");
        }
        const auto resource_id = be16(prefix, 4);
        const auto name_bytes =
            static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(prefix[6]));
        const auto padded_name_bytes = (1U + name_bytes + 1U) & ~std::uint64_t{1};
        std::uint64_t length_offset{};
        if (!checked_add(cursor, 6U + padded_name_bytes, length_offset) || length_offset > end ||
            end - length_offset < 4U) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD image-resource name is out of range");
        }
        std::array<std::byte, 4> length_bytes{};
        if (!read_exact(source, length_offset, length_bytes, read_failure)) {
            return failure(source_error(read_failure.code),
                           "PSD image-resource length is unavailable");
        }
        const auto payload_bytes = static_cast<std::uint64_t>(be32(length_bytes, 0));
        const auto payload_offset = length_offset + length_bytes.size();
        std::uint64_t payload_end{};
        if (!section_end(payload_offset, payload_bytes, end, payload_end)) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD image-resource payload is out of range");
        }
        if (resource_id == kIccResourceId) {
            if (!icc.empty()) {
                return failure(PsdDecodeErrorCode::malformed_input,
                               "PSD contains more than one ICC resource");
            }
            if (payload_bytes == 0 || payload_bytes > color::kMaximumIccProfileBytes) {
                return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                               "PSD ICC resource exceeds its limit");
            }
            icc.resize(static_cast<std::size_t>(payload_bytes));
            if (!read_exact(source, payload_offset, icc, read_failure)) {
                return failure(source_error(read_failure.code), "PSD ICC resource is unavailable");
            }
        }
        const auto padded_payload_bytes = payload_bytes + (payload_bytes & 1U);
        if (!checked_add(payload_offset, padded_payload_bytes, cursor) || cursor > end) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD image-resource padding is out of range");
        }
    }
    return {};
}

[[nodiscard]] PsdDecodeResult parse_merged_transparency(const NativeSource &source,
                                                        const std::uint64_t begin,
                                                        const std::uint64_t end,
                                                        const bool large,
                                                        bool &merged_transparency) {
    if (begin == end) {
        return {};
    }
    const auto length_size = large ? 8U : 4U;
    if (end - begin < length_size) {
        return failure(PsdDecodeErrorCode::malformed_input, "PSD layer-info length is truncated");
    }
    std::array<std::byte, 10> header{};
    ReadFailure read_failure;
    const auto header_bytes =
        static_cast<std::size_t>(std::min<std::uint64_t>(length_size + 2U, end - begin));
    if (!read_exact(source, begin, std::span(header).first(header_bytes), read_failure)) {
        return failure(source_error(read_failure.code), "PSD layer header is unavailable");
    }
    const auto layer_info_bytes = large ? be64(header, 0) : be32(header, 0);
    if (layer_info_bytes > end - begin - length_size) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD layer-info section exceeds its container");
    }
    if (layer_info_bytes >= 2U) {
        if (header_bytes < length_size + 2U) {
            return failure(PsdDecodeErrorCode::truncated_input, "PSD layer count is truncated");
        }
        merged_transparency = (be16(header, length_size) & 0x8000U) != 0U;
    }
    std::uint64_t unpadded_end{};
    if (!checked_add(begin + length_size, layer_info_bytes, unpadded_end) || unpadded_end > end) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD layer-info section is out of range");
    }
    if (unpadded_end == end) {
        return {};
    }
    const auto padded_layer_bytes = layer_info_bytes + (layer_info_bytes & 1U);
    std::uint64_t cursor{};
    if (!checked_add(begin + length_size, padded_layer_bytes, cursor) || cursor > end) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD padded layer-info section is out of range");
    }
    if (cursor == end) {
        return {};
    }
    if (end - cursor < 4U) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD global-layer-mask length is truncated");
    }
    std::array<std::byte, 4> length{};
    if (!read_exact(source, cursor, length, read_failure)) {
        return failure(source_error(read_failure.code),
                       "PSD global-layer-mask length is unavailable");
    }
    const auto global_mask_bytes = static_cast<std::uint64_t>(be32(length, 0));
    if (!checked_add(cursor + 4U, global_mask_bytes, cursor) || cursor > end) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD global-layer-mask section is out of range");
    }
    std::size_t tagged_count{};
    while (cursor < end) {
        if (++tagged_count > 4096U) {
            return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                           "PSD/PSB tagged-block count exceeds its limit");
        }
        if (end - cursor < 12U) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD additional-layer record is truncated");
        }
        std::array<std::byte, 12> record{};
        if (!read_exact(source, cursor, record, read_failure)) {
            return failure(source_error(read_failure.code),
                           "PSD additional-layer record is unavailable");
        }
        const auto valid_signature =
            std::equal(kResourceSignature.cbegin(), kResourceSignature.cend(), record.cbegin()) ||
            std::equal(kLargeResourceSignature.cbegin(), kLargeResourceSignature.cend(),
                       record.cbegin());
        if (!valid_signature) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD additional-layer signature is invalid");
        }
        const auto key = std::span<const std::byte>(record).subspan(4U, 4U);
        if (std::equal(kMergedTransparencyKey.cbegin(), kMergedTransparencyKey.cend(),
                       key.begin()) ||
            std::equal(kMergedTransparency16Key.cbegin(), kMergedTransparency16Key.cend(),
                       key.begin()) ||
            std::equal(kMergedTransparency32Key.cbegin(), kMergedTransparency32Key.cend(),
                       key.begin())) {
            merged_transparency = true;
        }
        const auto wide_length = large && large_tag_length(key);
        const auto record_size = wide_length ? 16U : 12U;
        if (end - cursor < record_size) {
            return failure(PsdDecodeErrorCode::truncated_input,
                           "PSB additional-layer length is truncated");
        }
        std::uint64_t payload_bytes = be32(record, 8);
        if (wide_length) {
            auto result = read_section_length(source, cursor + 8U, true, payload_bytes);
            if (result.error) { return result; }
        }
        if (payload_bytes > end - cursor - record_size) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD/PSB additional-layer payload exceeds its container");
        }
        // Photoshop writes the global tagged blocks at the end of the layer/mask
        // section on four-byte boundaries. Tagged blocks embedded in a layer record
        // use the two-byte rule from the generic block description, but this parser
        // intentionally reads only the global sequence.
        const auto padded_payload = (payload_bytes + 3U) & ~std::uint64_t{3U};
        if (!checked_add(cursor + record_size, padded_payload, cursor) || cursor > end) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD additional-layer payload is out of range");
        }
    }
    return {};
}

[[nodiscard]] PsdDecodeResult parse_layout(const NativeSource &source,
                                           const PsdDecodeLimits &limits, Layout &layout) {
    if (source.size() > limits.maximum_source_bytes) {
        return failure(PsdDecodeErrorCode::source_limit_exceeded, "PSD source exceeds its limit");
    }
    if (source.size() < 38U) {
        return failure(PsdDecodeErrorCode::truncated_input, "PSD header is truncated");
    }
    std::array<std::byte, 26> header{};
    ReadFailure read_failure;
    if (!read_exact(source, 0, header, read_failure)) {
        return failure(source_error(read_failure.code), "PSD header is unavailable");
    }
    if (!std::equal(kPsdSignature.cbegin(), kPsdSignature.cend(), header.cbegin())) {
        return failure(PsdDecodeErrorCode::malformed_input, "input is not a PSD document");
    }
    const auto version = be16(header, 4);
    if (version != kPsdVersion && version != kPsbVersion) {
        return failure(PsdDecodeErrorCode::unsupported_version,
                       "only PSD version 1 and PSB version 2 composites are supported");
    }
    if (!std::all_of(header.cbegin() + 6, header.cbegin() + 12,
                     [](const std::byte value) { return value == std::byte{}; })) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD reserved header bytes are nonzero");
    }
    const auto large = version == kPsbVersion;
    if (source.size() > (large ? kMaximumPsbSourceBytes : kMaximumPsdSourceBytes)) {
        return failure(PsdDecodeErrorCode::source_limit_exceeded,
                       "PSD/PSB source exceeds its format limit");
    }
    layout.header = {.large_document = large,
                     .channels = be16(header, 12),
                     .height = be32(header, 14),
                     .width = be32(header, 18),
                     .depth = be16(header, 22),
                     .color_mode = be16(header, 24)};
    if (layout.header.channels == 0 || layout.header.channels > kMaximumChannels) {
        return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                       "PSD channel count exceeds the supported limit");
    }
    const auto dimension_limit = std::min(limits.maximum_dimension,
                                         large ? kMaximumPsbDimension : kMaximumPsdDimension);
    if (layout.header.width == 0 || layout.header.height == 0 ||
        layout.header.width > dimension_limit || layout.header.height > dimension_limit) {
        return failure(PsdDecodeErrorCode::dimension_limit_exceeded,
                       "PSD dimensions exceed the supported limit");
    }
    std::uint64_t pixels{};
    if (!checked_multiply(layout.header.width, layout.header.height, pixels) ||
        pixels > limits.maximum_source_pixels) {
        return failure(PsdDecodeErrorCode::dimension_limit_exceeded,
                       "PSD pixel count exceeds the supported limit");
    }
    if (layout.header.depth != 8 && layout.header.depth != 16) {
        return failure(PsdDecodeErrorCode::unsupported_depth,
                       "PSD composite depth must be 8 or 16 bits");
    }
    switch (layout.header.color_mode) {
    case kModeGrayscale:
        layout.base_channels = 1;
        layout.pixel_format = color::PixelFormat::gray8;
        layout.color_model = PsdColorModel::grayscale;
        break;
    case kModeRgb:
        layout.base_channels = 3;
        layout.pixel_format = color::PixelFormat::rgb8;
        layout.color_model = PsdColorModel::rgb;
        break;
    case kModeCmyk:
        layout.base_channels = 4;
        layout.pixel_format = color::PixelFormat::cmyk8;
        layout.color_model = PsdColorModel::cmyk;
        break;
    case kModeLab:
        layout.base_channels = 3;
        layout.pixel_format = color::PixelFormat::lab8;
        layout.color_model = PsdColorModel::lab;
        break;
    default:
        return failure(PsdDecodeErrorCode::unsupported_color_mode,
                       "PSD color mode is not supported");
    }
    if (layout.header.channels < layout.base_channels) {
        return failure(PsdDecodeErrorCode::malformed_input,
                       "PSD composite has fewer channels than its color mode");
    }
    const auto bytes_per_sample = static_cast<std::uint64_t>(layout.header.depth / 8U);
    std::uint64_t row_bytes{};
    if (!checked_multiply(layout.header.width, bytes_per_sample, row_bytes) ||
        row_bytes > std::numeric_limits<std::size_t>::max()) {
        return failure(PsdDecodeErrorCode::resource_limit_exceeded, "PSD row size overflows");
    }
    layout.row_bytes = static_cast<std::size_t>(row_bytes);

    std::uint64_t cursor = 26;
    for (int section = 0; section < 2; ++section) {
        std::uint32_t section_bytes{};
        auto read_result = read_u32(source, cursor, section_bytes);
        if (read_result.error) {
            return read_result;
        }
        cursor += 4U;
        std::uint64_t end{};
        if (!section_end(cursor, section_bytes, source.size(), end)) {
            return failure(PsdDecodeErrorCode::truncated_input,
                           "PSD header section extends beyond the source");
        }
        if (section == 1) {
            auto resource_result = parse_resources(source, cursor, end, layout.embedded_icc);
            if (resource_result.error) {
                return resource_result;
            }
        }
        cursor = end;
    }
    std::uint64_t layer_bytes{};
    auto layer_result = read_section_length(source, cursor, large, layer_bytes);
    if (layer_result.error) {
        return layer_result;
    }
    cursor += large ? 8U : 4U;
    std::uint64_t image_offset{};
    if (!section_end(cursor, layer_bytes, source.size(), image_offset) ||
        source.size() - image_offset < 2U) {
        return failure(PsdDecodeErrorCode::truncated_input,
                       "PSD layer section or composite header is truncated");
    }
    auto transparency_result =
        parse_merged_transparency(source, cursor, image_offset, large, layout.merged_transparency);
    if (transparency_result.error) {
        return transparency_result;
    }
    std::array<std::byte, 2> compression{};
    if (!read_exact(source, image_offset, compression, read_failure)) {
        return failure(source_error(read_failure.code), "PSD compression field is unavailable");
    }
    layout.compression = be16(compression, 0);
    if (layout.compression > kCompressionZipPrediction) {
        return failure(PsdDecodeErrorCode::unsupported_compression,
                       "PSD composite compression is not supported");
    }
    if (layout.compression == kCompressionZipPrediction && layout.header.depth != 8) {
        return failure(PsdDecodeErrorCode::unsupported_compression,
                       "ZIP prediction is supported only for 8-bit PSD composites");
    }
    layout.image_data_offset = image_offset + 2U;
    return {};
}

[[nodiscard]] bool decode_packbits(const std::span<const std::byte> encoded,
                                   const std::span<std::byte> decoded) noexcept {
    std::size_t input{};
    std::size_t output{};
    while (input < encoded.size() && output < decoded.size()) {
        const auto control = std::to_integer<std::uint8_t>(encoded[input++]);
        if (control <= 127U) {
            const auto count = static_cast<std::size_t>(control) + 1U;
            if (count > encoded.size() - input || count > decoded.size() - output) {
                return false;
            }
            std::copy_n(encoded.data() + input, count, decoded.data() + output);
            input += count;
            output += count;
        } else if (control >= 129U) {
            const auto count = static_cast<std::size_t>(257U - control);
            if (input >= encoded.size() || count > decoded.size() - output) {
                return false;
            }
            std::fill_n(decoded.data() + output, count, encoded[input]);
            ++input;
            output += count;
        }
    }
    while (input < encoded.size() && encoded[input] == std::byte{0x80}) {
        ++input;
    }
    return output == decoded.size() && input == encoded.size();
}

class RowReader final {
  public:
    RowReader(const NativeSource &source, const Layout &layout, const PsdDecodeLimits &limits)
        : source_(source), layout_(layout), limits_(limits) {}

    [[nodiscard]] std::optional<PsdDecodeResult> prepare() {
        std::uint64_t row_count{};
        if (!checked_multiply(layout_.header.channels, layout_.header.height, row_count)) {
            return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                           "PSD row table size overflows");
        }
        if (layout_.compression == kCompressionRaw) {
            std::uint64_t bytes{};
            std::uint64_t end{};
            if (!checked_multiply(row_count, layout_.row_bytes, bytes) ||
                !checked_add(layout_.image_data_offset, bytes, end)) {
                return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                               "PSD raw composite size overflows");
            }
            if (end > source_.size()) {
                return failure(PsdDecodeErrorCode::truncated_input,
                               "PSD raw composite is truncated");
            }
            return std::nullopt;
        }
        if (layout_.compression == kCompressionRle) {
            if (row_count > (64U * 1024U * 1024U) /
                                (sizeof(std::uint64_t) + 2U * sizeof(std::uint32_t))) {
                return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                               "PSD RLE row table exceeds its memory limit");
            }
            const auto entry_size = layout_.header.large_document ? 4U : 2U;
            std::uint64_t table_bytes{};
            if (!checked_multiply(row_count, entry_size, table_bytes) ||
                table_bytes > std::numeric_limits<std::size_t>::max() ||
                table_bytes > source_.size() - layout_.image_data_offset) {
                return failure(PsdDecodeErrorCode::truncated_input,
                               "PSD RLE row table is truncated");
            }
            std::vector<std::byte> table(static_cast<std::size_t>(table_bytes));
            ReadFailure read_failure;
            if (!read_exact(source_, layout_.image_data_offset, table, read_failure)) {
                return failure(source_error(read_failure.code), "PSD RLE row table is unavailable");
            }
            row_offsets_.resize(static_cast<std::size_t>(row_count));
            row_lengths_.resize(static_cast<std::size_t>(row_count));
            auto offset = layout_.image_data_offset + table_bytes;
            for (std::size_t row = 0; row < row_offsets_.size(); ++row) {
                const auto length = layout_.header.large_document ? be32(table, row * entry_size)
                                                                  : be16(table, row * entry_size);
                if (length == 0) {
                    return failure(PsdDecodeErrorCode::malformed_input,
                                   "PSD RLE row has an empty payload");
                }
                if (length > layout_.row_bytes * 2U + 1024U) {
                    return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                                   "PSD/PSB encoded row exceeds its memory limit");
                }
                row_offsets_[row] = offset;
                row_lengths_[row] = length;
                if (!checked_add(offset, length, offset) || offset > source_.size()) {
                    return failure(PsdDecodeErrorCode::truncated_input,
                                   "PSD RLE row extends beyond the source");
                }
            }
            return std::nullopt;
        }

        std::uint64_t expected{};
        if (!checked_multiply(row_count, layout_.row_bytes, expected) ||
            expected > limits_.maximum_zip_bytes ||
            expected > std::numeric_limits<std::size_t>::max()) {
            return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                           "PSD ZIP composite exceeds its memory limit");
        }
        const auto compressed_bytes = source_.size() - layout_.image_data_offset;
        if (compressed_bytes == 0 || compressed_bytes > limits_.maximum_zip_bytes ||
            compressed_bytes > limits_.maximum_zip_bytes - expected ||
            compressed_bytes > std::numeric_limits<std::size_t>::max()) {
            return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                           "PSD ZIP payload and output exceed their combined memory limit");
        }
        std::vector<std::byte> compressed(static_cast<std::size_t>(compressed_bytes));
        ReadFailure read_failure;
        if (!read_exact(source_, layout_.image_data_offset, compressed, read_failure)) {
            return failure(source_error(read_failure.code), "PSD ZIP payload is unavailable");
        }
        zip_bytes_.resize(static_cast<std::size_t>(expected));
        tinfl_decompressor decompressor{};
        tinfl_init(&decompressor);
        auto input_bytes = compressed.size();
        auto output_bytes = zip_bytes_.size();
        const auto status = tinfl_decompress(
            &decompressor, reinterpret_cast<const mz_uint8 *>(compressed.data()), &input_bytes,
            reinterpret_cast<mz_uint8 *>(zip_bytes_.data()),
            reinterpret_cast<mz_uint8 *>(zip_bytes_.data()), &output_bytes,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (status != TINFL_STATUS_DONE || input_bytes != compressed.size() ||
            output_bytes != zip_bytes_.size()) {
            return failure(PsdDecodeErrorCode::malformed_input,
                           "PSD ZIP composite stream is invalid");
        }
        if (layout_.compression == kCompressionZipPrediction) {
            for (std::size_t offset = 0; offset < zip_bytes_.size(); offset += layout_.row_bytes) {
                for (std::size_t column = 1; column < layout_.row_bytes; ++column) {
                    zip_bytes_[offset + column] = static_cast<std::byte>(
                        std::to_integer<std::uint8_t>(zip_bytes_[offset + column]) +
                        std::to_integer<std::uint8_t>(zip_bytes_[offset + column - 1U]));
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<PsdDecodeResult> read(const std::uint16_t channel,
                                                      const std::uint32_t row,
                                                      std::vector<std::byte> &output) const {
        if (channel >= layout_.header.channels || row >= layout_.header.height) {
            return failure(PsdDecodeErrorCode::malformed_input, "PSD row index is out of range");
        }
        const auto index = static_cast<std::size_t>(channel) * layout_.header.height + row;
        output.resize(layout_.row_bytes);
        if (layout_.compression == kCompressionZip ||
            layout_.compression == kCompressionZipPrediction) {
            const auto offset = index * layout_.row_bytes;
            std::copy_n(zip_bytes_.data() + offset, output.size(), output.data());
            return std::nullopt;
        }
        ReadFailure read_failure;
        if (layout_.compression == kCompressionRaw) {
            const auto offset = layout_.image_data_offset +
                                static_cast<std::uint64_t>(index) * layout_.row_bytes;
            if (!read_exact(source_, offset, output, read_failure)) {
                return failure(source_error(read_failure.code), "PSD raw row is unavailable");
            }
            return std::nullopt;
        }
        std::vector<std::byte> encoded(row_lengths_[index]);
        if (!read_exact(source_, row_offsets_[index], encoded, read_failure)) {
            return failure(source_error(read_failure.code), "PSD RLE row is unavailable");
        }
        if (!decode_packbits(encoded, output)) {
            return failure(PsdDecodeErrorCode::malformed_input, "PSD PackBits row is malformed");
        }
        return std::nullopt;
    }

  private:
    const NativeSource &source_;
    const Layout &layout_;
    const PsdDecodeLimits &limits_;
    std::vector<std::uint64_t> row_offsets_;
    std::vector<std::uint32_t> row_lengths_;
    std::vector<std::byte> zip_bytes_;
};

struct RowSampleLocation {
    std::uint32_t column{};
    std::uint16_t depth{};
};

[[nodiscard]] std::uint8_t row_sample(const std::span<const std::byte> row,
                                      const RowSampleLocation location) noexcept {
    if (location.depth == 8) {
        return std::to_integer<std::uint8_t>(row[location.column]);
    }
    const auto offset = static_cast<std::size_t>(location.column) * 2U;
    const auto value =
        static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(row[offset]) << 8U) |
                                   std::to_integer<std::uint16_t>(row[offset + 1U]));
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) + 128U) / 257U);
}

struct SamplingPoint {
    std::uint32_t first{};
    std::uint32_t second{};
    double fraction{};
};

[[nodiscard]] std::vector<SamplingPoint> sampling_points(const std::uint32_t source,
                                                         const std::uint32_t target) {
    std::vector<SamplingPoint> points(target);
    for (std::uint32_t index = 0; index < target; ++index) {
        const auto coordinate = (static_cast<double>(index) + 0.5) * source / target - 0.5;
        const auto floor_value = std::floor(coordinate);
        const auto first = static_cast<std::uint32_t>(
            std::clamp(floor_value, 0.0, static_cast<double>(source - 1U)));
        points[index] = {.first = first,
                         .second = std::min(first + 1U, source - 1U),
                         .fraction = std::clamp(coordinate - floor_value, 0.0, 1.0)};
    }
    return points;
}

struct BilinearSample {
    std::array<std::uint8_t, 4> corners{};
    double horizontal{};
    double vertical{};
};

[[nodiscard]] std::uint8_t interpolate(const BilinearSample &sample) noexcept {
    const auto top = static_cast<double>(sample.corners[0]) * (1.0 - sample.horizontal) +
                     static_cast<double>(sample.corners[1]) * sample.horizontal;
    const auto bottom = static_cast<double>(sample.corners[2]) * (1.0 - sample.horizontal) +
                        static_cast<double>(sample.corners[3]) * sample.horizontal;
    return static_cast<std::uint8_t>(std::clamp(
        std::lround(top * (1.0 - sample.vertical) + bottom * sample.vertical), 0L, 255L));
}

struct ChannelDecodeRequest {
    std::uint16_t channel{};
    std::uint32_t target_width{};
    std::uint32_t target_height{};
    std::span<std::byte> target;
    std::size_t stride{};
    std::size_t component{};
    bool invert{};
};

[[nodiscard]] std::optional<PsdDecodeResult> decode_channel(RowReader &reader, const Layout &layout,
                                                            const ChannelDecodeRequest &request) {
    const auto xs = sampling_points(layout.header.width, request.target_width);
    const auto ys = sampling_points(layout.header.height, request.target_height);
    std::vector<std::byte> first_row;
    std::vector<std::byte> second_row;
    for (std::uint32_t y = 0; y < request.target_height; ++y) {
        if (auto error = reader.read(request.channel, ys[y].first, first_row)) {
            return error;
        }
        if (ys[y].second == ys[y].first) {
            second_row = first_row;
        } else if (auto error = reader.read(request.channel, ys[y].second, second_row)) {
            return error;
        }
        for (std::uint32_t x = 0; x < request.target_width; ++x) {
            const auto depth = layout.header.depth;
            auto value = interpolate(
                {.corners = {row_sample(first_row, {.column = xs[x].first, .depth = depth}),
                             row_sample(first_row, {.column = xs[x].second, .depth = depth}),
                             row_sample(second_row, {.column = xs[x].first, .depth = depth}),
                             row_sample(second_row, {.column = xs[x].second, .depth = depth})},
                 .horizontal = xs[x].fraction,
                 .vertical = ys[y].fraction});
            if (request.invert) {
                value = static_cast<std::uint8_t>(255U - value);
            }
            request
                .target[(static_cast<std::size_t>(y) * request.target_width + x) * request.stride +
                        request.component] = static_cast<std::byte>(value);
        }
    }
    return std::nullopt;
}

[[nodiscard]] PsdDecodeErrorCode transform_error(const color::TransformError error) noexcept {
    switch (error) {
    case color::TransformError::profile_required:
        return PsdDecodeErrorCode::color_profile_required;
    case color::TransformError::invalid_profile:
    case color::TransformError::profile_mismatch:
        return PsdDecodeErrorCode::invalid_color_profile;
    case color::TransformError::resource_limit:
        return PsdDecodeErrorCode::resource_limit_exceeded;
    case color::TransformError::invalid_dimensions:
    case color::TransformError::invalid_buffer:
        return PsdDecodeErrorCode::malformed_input;
    case color::TransformError::transform_failed:
        return PsdDecodeErrorCode::color_transform_failed;
    case color::TransformError::none:
        return PsdDecodeErrorCode::none;
    }
    return PsdDecodeErrorCode::color_transform_failed;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
target_size(const Header &header, const std::uint32_t maximum_edge) noexcept {
    const auto largest = std::max(header.width, header.height);
    if (largest <= maximum_edge) {
        return {header.width, header.height};
    }
    const auto scale = static_cast<double>(maximum_edge) / largest;
    return {std::max(1U, static_cast<std::uint32_t>(std::lround(header.width * scale))),
            std::max(1U, static_cast<std::uint32_t>(std::lround(header.height * scale)))};
}

} // namespace

PsdDecodeResult decode_psd_composite(const NativeSource &source, const PsdDecodeLimits &limits) {
    try {
        if (limits.maximum_source_bytes == 0 || limits.maximum_source_pixels == 0 ||
            limits.maximum_dimension == 0 || limits.maximum_output_edge == 0 ||
            limits.maximum_output_edge > kMaximumPsdOutputEdge ||
            limits.maximum_output_bytes == 0 || limits.maximum_zip_bytes == 0 ||
            limits.fallback_cmyk_icc.size() > color::kMaximumIccProfileBytes) {
            return failure(PsdDecodeErrorCode::invalid_limits, "invalid PSD decode limits");
        }
        Layout layout;
        auto parse_result = parse_layout(source, limits, layout);
        if (parse_result.error) {
            return parse_result;
        }
        const auto [width, height] = target_size(layout.header, limits.maximum_output_edge);
        std::uint64_t output_bytes{};
        if (!checked_multiply(width, height, output_bytes) ||
            !checked_multiply(output_bytes, 4U, output_bytes) ||
            output_bytes > limits.maximum_output_bytes) {
            return failure(PsdDecodeErrorCode::output_limit_exceeded,
                           "PSD thumbnail exceeds its output limit");
        }
        RowReader reader(source, layout, limits);
        if (auto error = reader.prepare()) {
            return *error;
        }
        const auto target_pixels = static_cast<std::size_t>(width) * height;
        std::vector<std::byte> color_pixels(target_pixels * layout.base_channels);
        for (std::uint16_t channel = 0; channel < layout.base_channels; ++channel) {
            if (auto error =
                    decode_channel(reader, layout,
                                   {.channel = channel,
                                    .target_width = width,
                                    .target_height = height,
                                    .target = std::span(color_pixels),
                                    .stride = layout.base_channels,
                                    .component = channel,
                                    .invert = layout.color_model == PsdColorModel::cmyk})) {
                return *error;
            }
        }
        const auto has_alpha =
            layout.merged_transparency && layout.header.channels > layout.base_channels;
        std::vector<std::byte> alpha(target_pixels, std::byte{255});
        if (has_alpha) {
            if (auto error = decode_channel(reader, layout,
                                            {.channel = layout.base_channels,
                                             .target_width = width,
                                             .target_height = height,
                                             .target = std::span(alpha),
                                             .stride = 1U,
                                             .component = 0U,
                                             .invert = false})) {
                return *error;
            }
        }
        const auto fallback = layout.color_model == PsdColorModel::cmyk
                                  ? limits.fallback_cmyk_icc
                                  : std::span<const std::byte>{};
        auto transformed = color::to_srgb_rgba8(
            {.width = width,
             .height = height,
             .source_stride = static_cast<std::size_t>(width) * layout.base_channels,
             .source_format = layout.pixel_format,
             .source_pixels = color_pixels,
             .embedded_icc = layout.embedded_icc,
             .fallback_cmyk_icc = fallback});
        if (!transformed.ok()) {
            auto detail = transformed.detail.empty() ? std::string{"PSD color transform failed"}
                                                     : std::move(transformed.detail);
            return failure(transform_error(transformed.error), std::move(detail));
        }
        if (transformed.rgba_pixels.size() != output_bytes) {
            return failure(PsdDecodeErrorCode::color_transform_failed,
                           "PSD color transform returned an invalid image");
        }
        if (has_alpha) {
            for (std::size_t pixel = 0; pixel < target_pixels; ++pixel) {
                transformed.rgba_pixels[pixel * 4U + 3U] = alpha[pixel];
            }
        }
        const auto validation = source.validate_unchanged();
        if (!validation.unchanged) {
            return failure(source_error(validation.error.code),
                           "PSD source changed or became unavailable while decoding");
        }
        auto profile_name = transformed.metadata.source_profile;
        const auto used_fallback = layout.embedded_icc.empty() && !fallback.empty();
        if (used_fallback) {
            profile_name += " (default)";
        }
        return {.image = {.width = width,
                          .height = height,
                          .rgba8 = std::move(transformed.rgba_pixels),
                          .metadata = {.source_width = layout.header.width,
                                       .source_height = layout.header.height,
                                       .source_depth = layout.header.depth,
                                       .source_channels = layout.header.channels,
                                       .source_color_model = layout.color_model,
                                       .source_color_profile = std::move(profile_name),
                                       .source_profile_fingerprint = profile_sha256(
                                           layout.embedded_icc.empty()
                                               ? fallback
                                               : std::span<const std::byte>(layout.embedded_icc)),
                                       .used_embedded_icc = !layout.embedded_icc.empty(),
                                       .used_fallback_icc = used_fallback,
                                       .has_alpha = has_alpha}},
                .error = {}};
    } catch (const std::bad_alloc &) {
        return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                       "PSD decoder allocation limit exceeded");
    } catch (const std::length_error &) {
        return failure(PsdDecodeErrorCode::resource_limit_exceeded,
                       "PSD decoder size limit exceeded");
    }
}

} // namespace vove::handlers::raster
