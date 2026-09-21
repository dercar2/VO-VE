#include "vove/handlers/raster/raw_preview_decoder.hpp"

#include "vove/handlers/raster/signature_probe.hpp"

#include "src/piex.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace vove::handlers::raster {
namespace {

[[nodiscard]] RawPreviewResult failure(const RawPreviewErrorCode code, std::string detail) {
    return {.previews = {}, .error = {.code = code, .detail = std::move(detail)}};
}

[[nodiscard]] RawPreviewErrorCode source_error_code(const NativeSourceErrorCode code) noexcept {
    switch (code) {
    case NativeSourceErrorCode::io_error:
        return RawPreviewErrorCode::source_io_error;
    case NativeSourceErrorCode::disconnected:
        return RawPreviewErrorCode::source_disconnected;
    case NativeSourceErrorCode::source_changed:
        return RawPreviewErrorCode::source_changed;
    case NativeSourceErrorCode::size_limit_exceeded:
        return RawPreviewErrorCode::resource_limit_exceeded;
    case NativeSourceErrorCode::offset_out_of_range:
    case NativeSourceErrorCode::offset_overflow:
    case NativeSourceErrorCode::invalid_handle:
    case NativeSourceErrorCode::not_regular_file:
        return RawPreviewErrorCode::malformed_input;
    case NativeSourceErrorCode::none:
        return RawPreviewErrorCode::none;
    }
    return RawPreviewErrorCode::source_io_error;
}

class PiexNativeStream final : public piex::StreamInterface {
  public:
    PiexNativeStream(const NativeSource &source, const RawPreviewLimits &limits) noexcept
        : source_(source), limits_(limits) {}

    piex::Error GetData(const std::size_t offset, const std::size_t length,
                        std::uint8_t *data) override {
        if (length == 0) {
            return piex::kOk;
        }
        if (data == nullptr || offset > source_.size() || length > source_.size() - offset) {
            set_error(RawPreviewErrorCode::malformed_input,
                      "RAW metadata points outside the source");
            return piex::kFail;
        }
        if (reads_ >= limits_.maximum_metadata_reads || length > limits_.maximum_metadata_bytes ||
            metadata_bytes_ > limits_.maximum_metadata_bytes - length) {
            set_error(RawPreviewErrorCode::resource_limit_exceeded,
                      "RAW metadata exceeds the extraction budget");
            return piex::kFail;
        }
        ++reads_;
        metadata_bytes_ += length;
        auto destination = std::span<std::byte>{reinterpret_cast<std::byte *>(data), length};
        const auto read = source_.read_at(offset, destination);
        if (!read.ok() || read.bytes_read != length) {
            const auto code = read.ok() ? RawPreviewErrorCode::source_io_error
                                        : source_error_code(read.error.code);
            set_error(code, "RAW metadata could not be read");
            return piex::kFail;
        }
        return piex::kOk;
    }

    [[nodiscard]] const RawPreviewError &error() const noexcept {
        return error_;
    }

    [[nodiscard]] std::uint64_t metadata_bytes() const noexcept {
        return metadata_bytes_;
    }

    [[nodiscard]] std::uint32_t reads() const noexcept {
        return reads_;
    }

  private:
    void set_error(const RawPreviewErrorCode code, const char *detail) {
        if (!error_) {
            error_ = {.code = code, .detail = detail};
        }
    }

    const NativeSource &source_;
    RawPreviewLimits limits_;
    std::uint64_t metadata_bytes_{};
    std::uint32_t reads_{};
    RawPreviewError error_;
};

[[nodiscard]] bool valid_limits(const RawPreviewLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 &&
           limits.maximum_source_bytes <= kMaximumRawSourceBytes &&
           limits.maximum_metadata_bytes != 0 &&
           limits.maximum_metadata_bytes <= kMaximumRawMetadataBytes &&
           limits.maximum_preview_bytes != 0 &&
           limits.maximum_preview_bytes <= kMaximumRawPreviewBytes &&
           limits.maximum_metadata_reads != 0 &&
           limits.maximum_metadata_reads <= kMaximumRawMetadataReads;
}

[[nodiscard]] bool is_jpeg_candidate(const piex::Image &image) noexcept {
    return image.length != 0 && image.format == piex::Image::kJpegCompressed;
}

[[nodiscard]] std::uint64_t candidate_area(const piex::Image &image) noexcept {
    return static_cast<std::uint64_t>(image.width) * image.height;
}

[[nodiscard]] std::array<const piex::Image *, 2>
ordered_jpegs(const piex::PreviewImageData &data) noexcept {
    std::array<const piex::Image *, 2> candidates{};
    std::size_t count{};
    for (const auto *candidate : {&data.preview, &data.thumbnail}) {
        if (!is_jpeg_candidate(*candidate)) {
            continue;
        }
        candidates[count++] = candidate;
    }
    if (count == 2 && (candidate_area(*candidates[1]) > candidate_area(*candidates[0]) ||
                       (candidate_area(*candidates[1]) == candidate_area(*candidates[0]) &&
                        candidates[1]->length > candidates[0]->length))) {
        std::swap(candidates[0], candidates[1]);
    }
    return candidates;
}

struct MetadataBudget {
    std::uint64_t bytes{};
    std::uint32_t reads{};
};

[[nodiscard]] bool read_metadata(const NativeSource &source, const std::uint64_t offset,
                                 const std::span<std::byte> destination,
                                 const RawPreviewLimits &limits, MetadataBudget &budget,
                                 RawPreviewError &error) {
    if (offset > source.size() || destination.size() > source.size() - offset) {
        error = {.code = RawPreviewErrorCode::malformed_input,
                 .detail =
                     "RAW metadata points outside the source (offset=" + std::to_string(offset) +
                     ", length=" + std::to_string(destination.size()) +
                     ", source=" + std::to_string(source.size()) + ")"};
        return false;
    }
    if (budget.reads >= limits.maximum_metadata_reads ||
        destination.size() > limits.maximum_metadata_bytes ||
        budget.bytes > limits.maximum_metadata_bytes - destination.size()) {
        error = {.code = RawPreviewErrorCode::resource_limit_exceeded,
                 .detail = "RAW metadata exceeds the extraction budget"};
        return false;
    }
    ++budget.reads;
    budget.bytes += destination.size();
    const auto read = source.read_at(offset, destination);
    if (!read.ok() || read.bytes_read != destination.size()) {
        error = {.code = read.ok() ? RawPreviewErrorCode::source_io_error
                                   : source_error_code(read.error.code),
                 .detail = "RAW metadata could not be read"};
        return false;
    }
    return true;
}

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::byte> bytes,
                                     const std::size_t offset, const bool big_endian) noexcept {
    const auto first = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto second = std::to_integer<std::uint16_t>(bytes[offset + 1U]);
    return big_endian ? static_cast<std::uint16_t>((first << 8U) | second)
                      : static_cast<std::uint16_t>(first | (second << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset, const bool big_endian) noexcept {
    std::uint32_t value{};
    if (big_endian) {
        for (std::size_t index{}; index < 4U; ++index) {
            value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[offset + index]);
        }
    } else {
        for (std::size_t index{}; index < 4U; ++index) {
            value |=
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                << (index * 8U);
        }
    }
    return value;
}

struct EmbeddedJpegCandidate {
    std::uint64_t offset{};
    std::uint64_t length{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t orientation{};
    bool adobe_rgb{};
};

[[nodiscard]] RawPreviewResult copy_jpeg(const NativeSource &source, const RawPreviewLimits &limits,
                                         const EmbeddedJpegCandidate &candidate) {
    if (candidate.length == 0) {
        return failure(RawPreviewErrorCode::embedded_preview_unavailable,
                       "camera RAW contains no usable JPEG preview");
    }
    if (candidate.length > limits.maximum_preview_bytes) {
        return failure(RawPreviewErrorCode::resource_limit_exceeded,
                       "embedded RAW preview exceeds the preview limit");
    }
    if (candidate.offset > source.size() || candidate.length > source.size() - candidate.offset ||
        candidate.length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return failure(RawPreviewErrorCode::malformed_input,
                       "embedded RAW preview points outside the source (offset=" +
                           std::to_string(candidate.offset) +
                           ", length=" + std::to_string(candidate.length) +
                           ", source=" + std::to_string(source.size()) + ")");
    }

    std::vector<std::byte> jpeg(static_cast<std::size_t>(candidate.length));
    const auto read = source.read_at(candidate.offset, jpeg);
    if (!read.ok() || read.bytes_read != jpeg.size()) {
        const auto code =
            read.ok() ? RawPreviewErrorCode::source_io_error : source_error_code(read.error.code);
        return failure(code, "embedded RAW preview could not be read");
    }
    const auto signature = probe_raster_signature(std::span<const std::byte>{jpeg}.first(
        std::min<std::size_t>(jpeg.size(), kMaxSignatureProbeBytes)));
    if (!signature.matched() || signature.format != RasterFormat::Jpeg) {
        return failure(RawPreviewErrorCode::malformed_input,
                       "embedded RAW preview is not a valid JPEG source");
    }
    const auto validation = source.validate_unchanged();
    if (!validation.unchanged) {
        return failure(source_error_code(validation.error.code),
                       "RAW source changed while extracting its preview");
    }
    std::vector<RawEmbeddedPreview> previews;
    previews.push_back({.jpeg = std::move(jpeg),
                        .width = candidate.width,
                        .height = candidate.height,
                        .orientation = candidate.orientation,
                        .adobe_rgb = candidate.adobe_rgb});
    return {.previews = std::move(previews), .error = {}};
}

[[nodiscard]] std::optional<RawPreviewResult>
fallback_container_preview(const NativeSource &source, const RawPreviewLimits &limits,
                           MetadataBudget budget,
                           const piex::image_type_recognition::RawImageTypes type) {
    if (type != piex::image_type_recognition::kRafImage &&
        type != piex::image_type_recognition::kRw2Image) {
        return std::nullopt;
    }
    constexpr std::size_t header_size = 96;
    std::array<std::byte, header_size> header{};
    RawPreviewError error;
    if (!read_metadata(source, 0, header, limits, budget, error)) {
        return RawPreviewResult{.previews = {}, .error = std::move(error)};
    }
    if (type == piex::image_type_recognition::kRafImage) {
        constexpr std::size_t preview_offset_field = 84;
        const auto offset = read_u32(header, preview_offset_field, true);
        const auto length = read_u32(header, preview_offset_field + 4U, true);
        return copy_jpeg(source, limits, {.offset = offset, .length = length, .orientation = 0});
    }

    const bool little_endian = header[0] == std::byte{'I'} && header[1] == std::byte{'I'};
    const bool big_endian = header[0] == std::byte{'M'} && header[1] == std::byte{'M'};
    if ((!little_endian && !big_endian) || read_u16(header, 2, big_endian) != 0x55U) {
        return std::nullopt;
    }
    const auto ifd_offset = static_cast<std::uint64_t>(read_u32(header, 4, big_endian));
    std::array<std::byte, 2> count_bytes{};
    if (!read_metadata(source, ifd_offset, count_bytes, limits, budget, error)) {
        return RawPreviewResult{.previews = {}, .error = std::move(error)};
    }
    const auto entry_count = read_u16(count_bytes, 0, big_endian);
    constexpr std::uint16_t maximum_entries = 4'096;
    if (entry_count == 0 || entry_count > maximum_entries) {
        return failure(RawPreviewErrorCode::malformed_input,
                       "RW2 directory entry count is invalid");
    }
    const auto entry_bytes = static_cast<std::size_t>(entry_count) * 12U;
    std::vector<std::byte> entries(entry_bytes);
    if (!read_metadata(source, ifd_offset + 2U, entries, limits, budget, error)) {
        return RawPreviewResult{.previews = {}, .error = std::move(error)};
    }
    constexpr std::uint16_t pana_jpeg_tag = 0x002e;
    constexpr std::uint16_t undefined_type = 7;
    for (std::size_t offset{}; offset < entries.size(); offset += 12U) {
        if (read_u16(entries, offset, big_endian) != pana_jpeg_tag) {
            continue;
        }
        const auto type = read_u16(entries, offset + 2U, big_endian);
        const auto length = read_u32(entries, offset + 4U, big_endian);
        const auto jpeg_offset = read_u32(entries, offset + 8U, big_endian);
        if (type != undefined_type || length <= 4U) {
            return failure(RawPreviewErrorCode::malformed_input,
                           "RW2 JPEG directory entry is invalid");
        }
        return copy_jpeg(source, limits,
                         {.offset = jpeg_offset, .length = length, .orientation = 0});
    }
    return failure(RawPreviewErrorCode::embedded_preview_unavailable,
                   "camera RAW contains no usable JPEG preview");
}

} // namespace

RawPreviewResult extract_raw_embedded_preview(const NativeSource &source,
                                              const RawPreviewLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(RawPreviewErrorCode::invalid_limits, "RAW preview limits are invalid");
    }
    if (source.size() == 0) {
        return failure(RawPreviewErrorCode::malformed_input, "RAW source is empty");
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(RawPreviewErrorCode::resource_limit_exceeded,
                       "RAW source exceeds the input limit");
    }

    PiexNativeStream stream(source, limits);
    if (!piex::IsRaw(&stream)) {
        if (stream.error()) {
            return {.previews = {}, .error = stream.error()};
        }
        return failure(RawPreviewErrorCode::not_raw, "source is not a recognized camera RAW");
    }

    piex::PreviewImageData data;
    piex::image_type_recognition::RawImageTypes type = piex::image_type_recognition::kNonRawImage;
    const auto extracted = piex::GetPreviewImageData(&stream, &data, &type);
    std::uint32_t explicit_orientation{};
    const auto has_orientation = piex::GetOrientation(&stream, &explicit_orientation) &&
                                 explicit_orientation >= 1 && explicit_orientation <= 8;
    const auto candidates = ordered_jpegs(data);
    const auto stream_failure_is_local =
        !stream.error() || stream.error().code == RawPreviewErrorCode::malformed_input ||
        stream.error().code == RawPreviewErrorCode::resource_limit_exceeded;
    if (candidates[0] != nullptr && stream_failure_is_local) {
        std::vector<RawEmbeddedPreview> previews;
        previews.reserve(2);
        std::optional<RawPreviewResult> first_local_failure;
        for (const auto *candidate : candidates) {
            if (candidate == nullptr) {
                continue;
            }
            auto copied = copy_jpeg(
                source, limits,
                {.offset = candidate->offset,
                 .length = candidate->length,
                 .width = candidate->width,
                 .height = candidate->height,
                 .orientation = has_orientation ? static_cast<std::uint16_t>(explicit_orientation)
                                                : std::uint16_t{},
                 .adobe_rgb = data.color_space == piex::PreviewImageData::kAdobeRgb});
            if (copied.ok()) {
                previews.push_back(std::move(copied.previews.front()));
                continue;
            }
            if (copied.error.code != RawPreviewErrorCode::malformed_input &&
                copied.error.code != RawPreviewErrorCode::resource_limit_exceeded) {
                if (previews.empty()) {
                    return copied;
                }
                break;
            }
            if (!first_local_failure.has_value()) {
                first_local_failure = std::move(copied);
            }
        }
        if (!previews.empty()) {
            return {.previews = std::move(previews), .error = {}};
        }
        if (first_local_failure.has_value()) {
            return std::move(*first_local_failure);
        }
    }

    if (const auto fallback = fallback_container_preview(
            source, limits, {.bytes = stream.metadata_bytes(), .reads = stream.reads()}, type)) {
        return *fallback;
    }
    if (stream.error()) {
        return {.previews = {}, .error = stream.error()};
    }
    if (extracted == piex::kUnsupported) {
        return failure(RawPreviewErrorCode::unsupported_format,
                       "camera RAW format is not supported");
    }
    if (extracted != piex::kOk) {
        return failure(RawPreviewErrorCode::malformed_input, "camera RAW metadata is malformed");
    }
    return failure(RawPreviewErrorCode::embedded_preview_unavailable,
                   "camera RAW contains no usable JPEG preview");
}

} // namespace vove::handlers::raster
