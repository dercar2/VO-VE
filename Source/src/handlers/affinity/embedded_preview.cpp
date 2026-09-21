#include "vove/handlers/affinity/embedded_preview.hpp"

#include "vove/handlers/publishing_signature.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vove::handlers::affinity {
namespace {

using cdr::CdrPreviewLimits;
using cdr::CdrPreviewResult;
using Status = cdr::CdrPreviewStatus;

constexpr std::size_t kThumbnailHeaderBytes = 29;
constexpr std::uint32_t kMaximumDimension = 32'768;
constexpr std::array kPngSignature{
    std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
    std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};

[[nodiscard]] CdrPreviewResult failure(const Status status, std::string diagnostic) {
    return {.status = status,
            .container = cdr::CdrContainer::unknown,
            .candidates = {},
            .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] std::uint64_t little(const std::span<const std::byte> bytes,
                                 const std::size_t offset, const std::size_t count) noexcept {
    std::uint64_t value{};
    for (std::size_t i = 0; i < count; ++i) {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + i])} << (8U * i);
    }
    return value;
}

[[nodiscard]] std::uint32_t big32(const std::span<const std::byte> bytes,
                                const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + i]);
    }
    return value;
}

[[nodiscard]] bool fits(const std::uint64_t offset, const std::uint64_t length,
                        const std::uint64_t size) noexcept {
    return offset <= size && length <= size - offset;
}

[[nodiscard]] bool read_exact(const raster::NativeSource &source, const std::uint64_t offset,
                             const std::span<std::byte> bytes) noexcept {
    const auto read = source.read_at(offset, bytes);
    return read.ok() && read.bytes_read == bytes.size();
}

[[nodiscard]] bool valid_limits(const CdrPreviewLimits &limits) noexcept {
    return limits.maximum_central_entries > 0 &&
           limits.maximum_central_entries <= cdr::kMaximumCdrCentralEntries &&
           limits.maximum_central_directory_bytes > 0 &&
           limits.maximum_central_directory_bytes <= cdr::kMaximumCdrCentralDirectoryBytes &&
           limits.maximum_compressed_preview_bytes > 0 &&
           limits.maximum_compressed_preview_bytes <= cdr::kMaximumCdrCompressedPreviewBytes &&
           limits.maximum_total_preview_bytes > 0 &&
           limits.maximum_total_preview_bytes <= cdr::kMaximumCdrPreviewBytes;
}

[[nodiscard]] std::uint32_t png_crc(const std::span<const std::byte> bytes) noexcept {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < result.size(); ++i) {
            auto crc = i;
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1U) ^ ((0U - (crc & 1U)) & 0xedb88320U);
            }
            result[i] = crc;
        }
        return result;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc = (crc >> 8U) ^ table[(crc ^ std::to_integer<unsigned char>(byte)) & 0xffU];
    }
    return ~crc;
}

[[nodiscard]] Status validate_ihdr(const std::span<const std::byte> png,
                                 const CdrPreviewLimits &limits) noexcept {
    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), png.begin()) ||
        big32(png, 8) != 13 || big32(png, 12) != 0x49484452U ||
        png_crc(png.subspan(12, 17)) != big32(png, 29)) {
        return Status::malformed;
    }
    const auto width = big32(png, 16);
    const auto height = big32(png, 20);
    const auto depth = std::to_integer<unsigned char>(png[24]);
    const auto color = std::to_integer<unsigned char>(png[25]);
    const bool valid_depth =
        (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
        (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
        ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16));
    if (width == 0 || height == 0 || !valid_depth || png[26] != std::byte{} ||
        png[27] != std::byte{} || std::to_integer<unsigned char>(png[28]) > 1) {
        return Status::malformed;
    }
    if (width > kMaximumDimension || height > kMaximumDimension ||
        std::uint64_t{width} * height * (depth == 16 ? 8U : 4U) >
            limits.maximum_total_preview_bytes) {
        return Status::resource_limit;
    }
    return Status::success;
}

// This validates framing, not pixel data; the existing raster worker still decodes PNG.
[[nodiscard]] Status validate_png(const std::span<const std::byte> png,
                                const CdrPreviewLimits &limits) noexcept {
    std::size_t offset = 8;
    std::uint32_t chunks{};
    bool palette{};
    bool idat{};
    bool idat_ended{};
    std::uint64_t idat_bytes{};
    while (offset < png.size()) {
        if (++chunks > limits.maximum_central_entries) {
            return Status::resource_limit;
        }
        if (png.size() - offset < 12) {
            return Status::malformed;
        }
        const auto length = big32(png, offset);
        const auto tag = big32(png, offset + 4);
        if (length > png.size() - offset - 12) {
            return Status::malformed;
        }
        const auto chunk = png.subspan(offset + 4, std::size_t{length} + 4);
        for (std::size_t i = 0; i < 4; ++i) {
            const auto c = std::to_integer<unsigned char>(chunk[i]);
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
                return Status::malformed;
            }
        }
        if ((std::to_integer<unsigned char>(chunk[2]) & 0x20U) != 0 ||
            png_crc(chunk) != big32(png, offset + 8 + length)) {
            return Status::malformed;
        }
        if (idat && tag != 0x49444154U) {
            idat_ended = true;
        }
        switch (tag) {
        case 0x49484452U: // IHDR
            if (offset != 8 || length != 13) {
                return Status::malformed;
            }
            break;
        case 0x504c5445U: // PLTE
            if (palette || idat || length == 0 || length > 768 || length % 3 != 0 ||
                png[25] == std::byte{0} || png[25] == std::byte{4}) {
                return Status::malformed;
            }
            if (png[25] == std::byte{3} &&
                length / 3 > (1U << std::to_integer<unsigned char>(png[24]))) {
                return Status::malformed;
            }
            palette = true;
            break;
        case 0x49444154U: // IDAT
            if (idat_ended || (png[25] == std::byte{3} && !palette)) {
                return Status::malformed;
            }
            idat = true;
            idat_bytes += length;
            break;
        case 0x49454e44U: // IEND
            return length == 0 && idat_bytes > 0 && offset + 12 == png.size()
                       ? Status::success : Status::malformed;
        default:
            if ((std::to_integer<unsigned char>(chunk[0]) & 0x20U) == 0 ||
                tag == 0x6163544cU || tag == 0x6663544cU || tag == 0x66644154U) {
                return Status::unsupported_container; // Unknown critical chunk or APNG.
            }
            break;
        }
        offset += std::size_t{length} + 12;
    }
    return Status::malformed;
}

[[nodiscard]] CdrPreviewResult extract(const raster::NativeSource &source,
                                      const CdrPreviewLimits &limits) {
    std::array<std::byte, 72> header{};
    if (source.size() < 4) {
        return failure(Status::malformed, "Affinity signature is truncated");
    }
    if (limits.maximum_central_directory_bytes < 12) {
        return failure(Status::resource_limit, "Affinity header exceeds metadata read budget");
    }
    const auto prefix_size = static_cast<std::size_t>(std::min<std::uint64_t>(12, source.size()));
    if (!read_exact(source, 0, std::span(header).first(prefix_size))) {
        return failure(Status::io_error, "Affinity header could not be read");
    }
    if (publishing_container(std::span(header).first(prefix_size)) != PublishingContainer::affinity) {
        return failure(Status::unsupported_container, "Source is not an Affinity archive");
    }
    if (prefix_size < 12) {
        return failure(Status::malformed, "Affinity header is truncated");
    }
    const auto version = little(header, 4, 2);
    const auto flags = little(header, 6, 2);
    if (version < 7 || version > 12 || (flags & ~std::uint64_t{0x0c}) != 0 ||
        little(header, 8, 4) != 0x5072736eU) {
        return failure(Status::unsupported_container, "Unsupported Affinity archive version, flags or class");
    }
    const std::size_t header_size = version == 7 ? 64 : 72;
    if (source.size() < header_size) {
        return failure(Status::malformed, "Affinity information header is truncated");
    }
    if (header_size > limits.maximum_central_directory_bytes) {
        return failure(Status::resource_limit, "Affinity header exceeds metadata read budget");
    }
    if (!read_exact(source, 12, std::span(header).subspan(12, header_size - 12))) {
        return failure(Status::io_error, "Affinity information header could not be read");
    }
    if (little(header, 12, 4) != 0x666e4923U ||
        (version > 7 && little(header, 64, 4) != 0x746f7250U)) {
        return failure(Status::malformed, "Affinity #Inf or Prot header is invalid");
    }

    // afread's Archive.cpp identifies the absolute thumbnail pointer at #Inf+12.
    // Public author samples (archive versions 9, 10, 11, 12) agree on this layout:
    // +0 FFFFFFFF, +4 Thmb, +8 1, +12 u32 body bytes, +16 u64 relative PNG
    // offset (29), +24 u32 PNG bytes, +28 type (1). Only this layout is accepted;
    // no FAT traversal, historic-thumbnail search or arbitrary image scan occurs.
    // https://github.com/VMDevCpp/afread/blob/master/source/affinity/src/Archive.cpp
    const auto thumbnail = little(header, 24, 8);
    if (thumbnail == 0) {
        return failure(Status::no_preview, "Affinity document has no saved thumbnail");
    }
    if (thumbnail < header_size || !fits(thumbnail, kThumbnailHeaderBytes, source.size())) {
        return failure(Status::malformed, "Affinity thumbnail pointer is outside the file");
    }
    if (header_size + kThumbnailHeaderBytes > limits.maximum_central_directory_bytes) {
        return failure(Status::resource_limit, "Affinity thumbnail exceeds metadata read budget");
    }
    std::array<std::byte, kThumbnailHeaderBytes> record{};
    if (!read_exact(source, thumbnail, record)) {
        return failure(Status::io_error, "Affinity thumbnail record could not be read");
    }
    if (little(record, 0, 4) != 0xffffffffU || little(record, 4, 4) != 0x626d6854U) {
        return failure(Status::malformed, "Affinity thumbnail record marker is invalid");
    }
    if (little(record, 8, 4) != 1 || little(record, 16, 8) != kThumbnailHeaderBytes ||
        record[28] != std::byte{1}) {
        return failure(Status::unsupported_container, "Unsupported Affinity thumbnail descriptor layout");
    }
    const auto body_bytes = little(record, 12, 4);
    const auto png_bytes = little(record, 24, 4);
    if (body_bytes != png_bytes + 13 || png_bytes < 45 ||
        !fits(thumbnail, body_bytes + 16, source.size())) {
        return failure(Status::malformed, "Affinity thumbnail length is invalid or truncated");
    }
    if (png_bytes > limits.maximum_compressed_preview_bytes ||
        png_bytes > limits.maximum_total_preview_bytes) {
        return failure(Status::resource_limit, "Affinity PNG exceeds preview byte budget");
    }
    const auto image_offset = thumbnail + kThumbnailHeaderBytes;
    std::array<std::byte, 33> ihdr{};
    if (!read_exact(source, image_offset, ihdr)) {
        return failure(Status::io_error, "Affinity PNG header could not be read");
    }
    auto status = validate_ihdr(ihdr, limits);
    if (status != Status::success) {
        return failure(status, "Affinity PNG header is invalid or exceeds dimension budget");
    }
    std::vector<std::byte> png(static_cast<std::size_t>(png_bytes));
    std::copy(ihdr.begin(), ihdr.end(), png.begin());
    if (!read_exact(source, image_offset + ihdr.size(), std::span(png).subspan(ihdr.size()))) {
        return failure(Status::io_error, "Affinity PNG payload could not be read");
    }
    status = validate_png(png, limits);
    if (status != Status::success) {
        return failure(status, "Affinity PNG framing, checksum or chunk budget is invalid or unsupported");
    }
    CdrPreviewResult result{.status = Status::success,
                            .container = cdr::CdrContainer::unknown,
                            .candidates = {},
                            .diagnostic = {}};
    result.candidates.push_back({.format = raster::RasterFormat::Png,
                                 .normalized_name = "affinity/document-thumbnail.png",
                                 .encoded_image = std::move(png),
                                 .page_one = false});
    return result;
}

} // namespace

cdr::CdrPreviewResult extract_affinity_embedded_previews(const raster::NativeSource &source,
                                                      const cdr::CdrPreviewLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(Status::resource_limit, "Affinity preview limits are invalid");
    }
    if (!source.validate_unchanged().unchanged) {
        return failure(Status::io_error, "Affinity source changed or is unavailable");
    }
    try {
        auto result = extract(source, limits);
        if (!source.validate_unchanged().unchanged) {
            return failure(Status::io_error, "Affinity source changed during thumbnail extraction");
        }
        return result;
    } catch (const std::bad_alloc &) {
        return failure(Status::resource_limit, "Affinity thumbnail allocation failed");
    }
}

} // namespace vove::handlers::affinity
