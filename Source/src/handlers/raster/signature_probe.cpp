#include "vove/handlers/raster/signature_probe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vove::handlers::raster {
namespace {

using Bytes = std::span<const std::byte>;

constexpr std::array kJpegMagic{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}};
constexpr std::array kPngMagic{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
                               std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
constexpr std::array kGif87aMagic{std::byte{0x47}, std::byte{0x49}, std::byte{0x46},
                                  std::byte{0x38}, std::byte{0x37}, std::byte{0x61}};
constexpr std::array kGif89aMagic{std::byte{0x47}, std::byte{0x49}, std::byte{0x46},
                                  std::byte{0x38}, std::byte{0x39}, std::byte{0x61}};
constexpr std::array kIcoMagic{std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}};
constexpr std::array kTiffLittleClassic{std::byte{0x49}, std::byte{0x49}, std::byte{0x2A},
                                        std::byte{0x00}};
constexpr std::array kTiffBigClassic{std::byte{0x4D}, std::byte{0x4D}, std::byte{0x00},
                                     std::byte{0x2A}};
constexpr std::array kTiffLittleBig{std::byte{0x49}, std::byte{0x49}, std::byte{0x2B},
                                    std::byte{0x00}};
constexpr std::array kTiffBigBig{std::byte{0x4D}, std::byte{0x4D}, std::byte{0x00},
                                 std::byte{0x2B}};
constexpr std::array kRiffMagic{std::byte{0x52}, std::byte{0x49}, std::byte{0x46}, std::byte{0x46}};
constexpr std::array kWebpMagic{std::byte{0x57}, std::byte{0x45}, std::byte{0x42}, std::byte{0x50}};
constexpr std::array kFtypMagic{std::byte{0x66}, std::byte{0x74}, std::byte{0x79}, std::byte{0x70}};
constexpr std::array kPsdMagic{std::byte{0x38}, std::byte{0x42}, std::byte{0x50}, std::byte{0x53}};
constexpr std::array kJpegxlBoxMagic{
    std::byte{0}, std::byte{0}, std::byte{0}, std::byte{12}, std::byte{0x4A}, std::byte{0x58},
    std::byte{0x4C}, std::byte{0x20}, std::byte{0x0D}, std::byte{0x0A}, std::byte{0x87}, std::byte{0x0A}};

[[nodiscard]] SignatureProbeResult matched(const RasterFormat format) noexcept {
    return {format, ProbeStatus::Matched, 0};
}

[[nodiscard]] SignatureProbeResult
unsupported(const RasterFormat format = RasterFormat::Unknown) noexcept {
    return {format, ProbeStatus::Unsupported, 0};
}

[[nodiscard]] SignatureProbeResult truncated(const RasterFormat format,
                                             const std::size_t required_bytes) noexcept {
    return {format, ProbeStatus::Truncated, required_bytes};
}

[[nodiscard]] SignatureProbeResult malformed(const RasterFormat format) noexcept {
    return {format, ProbeStatus::Malformed, 0};
}

template <std::size_t Size>
[[nodiscard]] bool matches_available_prefix(const Bytes bytes,
                                            const std::array<std::byte, Size> &magic) noexcept {
    const auto compared = std::min(bytes.size(), magic.size());
    return std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(compared),
                      magic.begin());
}

template <std::size_t Size>
[[nodiscard]] bool matches_available_at(const Bytes bytes, const std::size_t offset,
                                        const std::array<std::byte, Size> &magic) noexcept {
    if (bytes.size() <= offset) {
        return true;
    }
    const auto compared = std::min(bytes.size() - offset, magic.size());
    return std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + compared),
                      magic.begin());
}

template <std::size_t Size>
[[nodiscard]] bool matches_at(const Bytes bytes, const std::size_t offset,
                              const std::array<std::byte, Size> &magic) noexcept {
    return bytes.size() >= offset + magic.size() &&
           std::equal(magic.begin(), magic.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] std::uint16_t read_le16(const Bytes bytes, const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read_le32(const Bytes bytes, const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t read_be32(const Bytes bytes, const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_be64(const Bytes bytes, const std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

constexpr std::uint32_t kHeicBrand = 0x68656963U;
constexpr std::uint32_t kHeixBrand = 0x68656978U;
constexpr std::uint32_t kHevcBrand = 0x68657663U;
constexpr std::uint32_t kHevxBrand = 0x68657678U;
constexpr std::uint32_t kHeimBrand = 0x6865696DU;
constexpr std::uint32_t kHeisBrand = 0x68656973U;
constexpr std::uint32_t kHevmBrand = 0x6865766DU;
constexpr std::uint32_t kHevsBrand = 0x68657673U;
constexpr std::uint32_t kAvifBrand = 0x61766966U;
constexpr std::uint32_t kAvisBrand = 0x61766973U;

[[nodiscard]] bool is_hevc_brand(const std::uint32_t brand) noexcept {
    return brand == kHeicBrand || brand == kHeixBrand || brand == kHevcBrand ||
           brand == kHevxBrand || brand == kHeimBrand || brand == kHeisBrand ||
           brand == kHevmBrand || brand == kHevsBrand;
}

[[nodiscard]] bool is_avif_brand(const std::uint32_t brand) noexcept {
    return brand == kAvifBrand || brand == kAvisBrand;
}

[[nodiscard]] SignatureProbeResult probe_ftyp(const Bytes bytes) noexcept {
    const auto size32 = read_be32(bytes, 0);
    std::uint64_t box_size = size32;
    std::size_t header_size = 8;

    if (size32 == 1U) {
        if (bytes.size() < 16) {
            return truncated(RasterFormat::Unknown, 16);
        }
        box_size = read_be64(bytes, 8);
        header_size = 16;
    } else if (size32 == 0U) {
        return malformed(RasterFormat::Unknown);
    }

    const auto minimum_size = header_size + 8U;
    if (box_size < minimum_size || box_size > kMaxSignatureProbeBytes ||
        (box_size - minimum_size) % 4U != 0U) {
        return malformed(RasterFormat::Unknown);
    }

    const auto bounded_box_size = static_cast<std::size_t>(box_size);
    if (bytes.size() < bounded_box_size) {
        return truncated(RasterFormat::Unknown, bounded_box_size);
    }

    const auto major_brand = read_be32(bytes, header_size);
    bool has_hevc_compatible_brand = false;
    bool has_avif_compatible_brand = false;
    bool has_avif_still_brand = major_brand == kAvifBrand;
    for (std::size_t offset = header_size + 8U; offset < bounded_box_size; offset += 4U) {
        const auto brand = read_be32(bytes, offset);
        has_hevc_compatible_brand = has_hevc_compatible_brand || is_hevc_brand(brand);
        has_avif_compatible_brand = has_avif_compatible_brand || is_avif_brand(brand);
        has_avif_still_brand = has_avif_still_brand || brand == kAvifBrand;
    }

    if (is_avif_brand(major_brand) || has_avif_compatible_brand) {
        return has_avif_still_brand ? matched(RasterFormat::Avif)
                                   : unsupported(RasterFormat::Avif);
    }
    if (is_hevc_brand(major_brand) || has_hevc_compatible_brand) {
        return matched(RasterFormat::Heif);
    }
    return unsupported();
}

[[nodiscard]] SignatureProbeResult probe_bmp(const Bytes bytes) noexcept {
    constexpr std::size_t kBmpFileHeaderBytes = 14;
    if (bytes.size() < kBmpFileHeaderBytes) {
        return truncated(RasterFormat::Bmp, kBmpFileHeaderBytes);
    }

    const auto file_size = read_le32(bytes, 2);
    const auto pixel_offset = read_le32(bytes, 10);
    const bool reserved_is_zero = read_le32(bytes, 6) == 0U;
    if (!reserved_is_zero || pixel_offset < kBmpFileHeaderBytes ||
        (file_size != 0U && file_size < pixel_offset)) {
        return malformed(RasterFormat::Bmp);
    }
    return matched(RasterFormat::Bmp);
}

[[nodiscard]] SignatureProbeResult probe_webp(const Bytes bytes) noexcept {
    constexpr std::size_t kWebpHeaderBytes = 12;
    if (bytes.size() < kWebpHeaderBytes) {
        if (!matches_available_at(bytes, 8, kWebpMagic)) {
            return unsupported();
        }
        return truncated(RasterFormat::Webp, kWebpHeaderBytes);
    }
    if (!matches_at(bytes, 8, kWebpMagic)) {
        return unsupported();
    }

    const auto riff_size = read_le32(bytes, 4);
    if (riff_size < 4U || riff_size % 2U != 0U) {
        return malformed(RasterFormat::Webp);
    }
    return matched(RasterFormat::Webp);
}

} // namespace

SignatureProbeResult probe_raster_signature(const std::span<const std::byte> prefix) noexcept {
    const auto bytes = prefix.first(std::min(prefix.size(), kMaxSignatureProbeBytes));

    if (bytes.size() >= 2 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0x0A}) {
        return matched(RasterFormat::Jpegxl);
    }
    if (bytes.size() >= 4 && matches_available_prefix(bytes, kJpegxlBoxMagic)) {
        return bytes.size() < kJpegxlBoxMagic.size()
                   ? truncated(RasterFormat::Jpegxl, kJpegxlBoxMagic.size())
                   : matched(RasterFormat::Jpegxl);
    }

    if (!bytes.empty() && matches_available_prefix(bytes, kPsdMagic)) {
        if (bytes.size() < kPsdMagic.size()) {
            return truncated(RasterFormat::Psd, kPsdMagic.size());
        }
        return matched(RasterFormat::Psd);
    }

    if (matches_available_prefix(bytes, kJpegMagic)) {
        if (bytes.size() < kJpegMagic.size()) {
            return truncated(bytes.size() >= 2 ? RasterFormat::Jpeg : RasterFormat::Unknown,
                             kJpegMagic.size());
        }
        return matched(RasterFormat::Jpeg);
    }
    if (bytes.size() >= 2 && bytes[0] == kJpegMagic[0] && bytes[1] == kJpegMagic[1]) {
        return malformed(RasterFormat::Jpeg);
    }

    if (matches_available_prefix(bytes, kPngMagic)) {
        if (bytes.size() < kPngMagic.size()) {
            return truncated(RasterFormat::Png, kPngMagic.size());
        }
        return matched(RasterFormat::Png);
    }
    if (bytes.size() >= 4 && std::equal(kPngMagic.begin(), kPngMagic.begin() + 4, bytes.begin())) {
        return malformed(RasterFormat::Png);
    }

    if (matches_available_prefix(bytes, kGif87aMagic) ||
        matches_available_prefix(bytes, kGif89aMagic)) {
        if (bytes.size() < kGif87aMagic.size()) {
            return truncated(RasterFormat::Gif, kGif87aMagic.size());
        }
        return matched(RasterFormat::Gif);
    }
    if (bytes.size() >= 4 &&
        std::equal(kGif87aMagic.begin(), kGif87aMagic.begin() + 4, bytes.begin())) {
        return malformed(RasterFormat::Gif);
    }

    if (!bytes.empty() && bytes[0] == std::byte{0x42}) {
        if (bytes.size() == 1) {
            return truncated(RasterFormat::Unknown, 2);
        }
        if (bytes[1] == std::byte{0x4D}) {
            return probe_bmp(bytes);
        }
    }

    if (matches_available_prefix(bytes, kIcoMagic)) {
        if (bytes.size() < kIcoMagic.size()) {
            return truncated(RasterFormat::Unknown, kIcoMagic.size());
        }
        if (bytes.size() < 6) {
            return truncated(RasterFormat::Ico, 6);
        }
        if (read_le16(bytes, 4) == 0U) {
            return malformed(RasterFormat::Ico);
        }
        return matched(RasterFormat::Ico);
    }

    const bool little_classic = matches_available_prefix(bytes, kTiffLittleClassic);
    const bool big_classic = matches_available_prefix(bytes, kTiffBigClassic);
    const bool little_bigtiff = matches_available_prefix(bytes, kTiffLittleBig);
    const bool big_bigtiff = matches_available_prefix(bytes, kTiffBigBig);
    if (little_classic || big_classic || little_bigtiff || big_bigtiff) {
        if (bytes.size() < 4) {
            return truncated(RasterFormat::Unknown, 4);
        }
        if (little_classic || big_classic) {
            return matched(RasterFormat::Tiff);
        }
        if (bytes.size() < 8) {
            return truncated(RasterFormat::Tiff, 8);
        }
        const bool valid_little_bigtiff =
            little_bigtiff && bytes[4] == std::byte{0x08} && bytes[5] == std::byte{0x00} &&
            bytes[6] == std::byte{0x00} && bytes[7] == std::byte{0x00};
        const bool valid_big_bigtiff = big_bigtiff && bytes[4] == std::byte{0x00} &&
                                       bytes[5] == std::byte{0x08} && bytes[6] == std::byte{0x00} &&
                                       bytes[7] == std::byte{0x00};
        return valid_little_bigtiff || valid_big_bigtiff ? matched(RasterFormat::Tiff)
                                                         : malformed(RasterFormat::Tiff);
    }

    if (matches_available_prefix(bytes, kRiffMagic)) {
        if (bytes.size() < kRiffMagic.size()) {
            return truncated(RasterFormat::Unknown, kRiffMagic.size());
        }
        return probe_webp(bytes);
    }

    if (bytes.size() < 8) {
        if (bytes.size() > 4 && matches_available_at(bytes, 4, kFtypMagic)) {
            return truncated(RasterFormat::Unknown, 8);
        }
        return unsupported();
    }
    if (matches_at(bytes, 4, kFtypMagic)) {
        return probe_ftyp(bytes);
    }

    return unsupported();
}

} // namespace vove::handlers::raster
