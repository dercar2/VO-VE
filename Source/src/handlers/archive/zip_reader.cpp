// Bounded native-source ZIP mechanics extracted from the selected entry reader.
#include "vove/handlers/archive/zip_reader.hpp"

extern "C" {
#include "miniz_tinfl.h"
}

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace vove::handlers::archive {
namespace {

constexpr std::uint32_t kZipLocalSignature = 0x04034b50U;
constexpr std::uint32_t kZipCentralSignature = 0x02014b50U;
constexpr std::uint32_t kZipEndSignature = 0x06054b50U;
constexpr std::uint32_t kZipDataDescriptorSignature = 0x08074b50U;
constexpr std::size_t kZipLocalHeaderBytes = 30;
constexpr std::size_t kZipCentralHeaderBytes = 46;
constexpr std::size_t kZipEndHeaderBytes = 22;
constexpr std::size_t kMaximumZipCommentBytes = 65'535;
constexpr std::uint16_t kZipEncryptedFlag = 1U << 0U;
constexpr std::uint16_t kZipDataDescriptorFlag = 1U << 3U;
constexpr std::uint16_t kZipMethodStored = 0;
constexpr std::uint16_t kZipMethodDeflate = 8;
constexpr std::uint32_t kZip64Sentinel = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
    const auto value =
        static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
        static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1U])) << 8U;
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool checked_add(const std::uint64_t left, const std::uint64_t right,
                               std::uint64_t &result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool read_exact(const raster::NativeSource &source, const std::uint64_t offset,
                              const std::span<std::byte> output,
                              raster::NativeSourceError &error) noexcept {
    const auto result = source.read_at(offset, output);
    error = result.error;
    return result.ok() && result.bytes_read == output.size();
}

[[nodiscard]] std::string normalize_zip_name(const std::span<const std::byte> bytes,
                                             const ZipNameMode mode) {
    std::string result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        if (value == 0 || (mode == ZipNameMode::cdr_ascii_folded && value >= 0x80U)) {
            return {};
        }
        if (mode == ZipNameMode::exact) {
            result.push_back(static_cast<char>(value));
            continue;
        }
        const auto character =
            value == static_cast<unsigned char>('\\') ? '/' : static_cast<char>(value);
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

[[nodiscard]] std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<unsigned char>(byte);
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const auto mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] std::optional<std::size_t>
find_zip_end(const std::span<const std::byte> tail) noexcept {
    if (tail.size() < kZipEndHeaderBytes) {
        return std::nullopt;
    }
    for (std::size_t offset = tail.size() - kZipEndHeaderBytes + 1U; offset-- > 0;) {
        if (read_u32(tail, offset) != kZipEndSignature) {
            continue;
        }
        const auto comment_bytes = read_u16(tail, offset + 20U);
        if (offset + kZipEndHeaderBytes + comment_bytes == tail.size()) {
            return offset;
        }
    }
    return std::nullopt;
}

} // namespace

ZipStatus validate_zip_entry_encoding(const ZipEntry &entry, std::string &diagnostic) {
    if ((entry.flags & (kZipEncryptedFlag | (1U << 6U) | (1U << 13U))) != 0U ||
        (entry.method != kZipMethodStored && entry.method != kZipMethodDeflate)) {
        diagnostic = "ZIP selected entry uses encryption or an unsupported method";
        return ZipStatus::malformed;
    }
    if (entry.compressed_bytes == kZip64Sentinel || entry.uncompressed_bytes == kZip64Sentinel ||
        entry.local_header_offset == kZip64Sentinel) {
        diagnostic = "ZIP64 selected entry is not supported";
        return ZipStatus::unsupported;
    }
    return ZipStatus::success;
}

[[nodiscard]] ZipStatus read_zip_directory(const raster::NativeSource &source,
                                                   const ZipDirectoryLimits &limits,
                                                   ZipDirectory &directory,
                                                   std::string &diagnostic, const ZipNameMode names) {
    directory = {};
    directory.name_mode = names;
    if (limits.maximum_central_entries == 0 || limits.maximum_central_entries > kMaximumZipEntries ||
        limits.maximum_central_directory_bytes == 0 ||
        limits.maximum_central_directory_bytes > kMaximumZipDirectoryBytes) {
        diagnostic = "ZIP directory limits are invalid";
        return ZipStatus::resource_limit;
    }
    const auto tail_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(source.size(), kZipEndHeaderBytes + kMaximumZipCommentBytes));
    std::vector<std::byte> tail(tail_bytes);
    raster::NativeSourceError source_error;
    if (!read_exact(source, source.size() - tail_bytes, tail, source_error)) {
        diagnostic = "ZIP end record could not be read";
        return ZipStatus::io_error;
    }
    const auto end_offset = find_zip_end(tail);
    if (!end_offset) {
        diagnostic = "ZIP end record is missing";
        return ZipStatus::malformed;
    }
    const auto end = std::span<const std::byte>(tail).subspan(*end_offset, kZipEndHeaderBytes);
    const auto disk = read_u16(end, 4);
    const auto directory_disk = read_u16(end, 6);
    const auto disk_entries = read_u16(end, 8);
    const auto total_entries = read_u16(end, 10);
    const auto directory_bytes = read_u32(end, 12);
    const auto directory_offset = read_u32(end, 16);
    if (disk != 0 || directory_disk != 0 || disk_entries != total_entries) {
        diagnostic = "multi-disk ZIP is not supported";
        return ZipStatus::malformed;
    }
    if (total_entries == 0xffffU || directory_bytes == kZip64Sentinel || directory_offset == kZip64Sentinel) {
        diagnostic = "ZIP64 is not supported";
        return ZipStatus::unsupported;
    }
    if (total_entries > limits.maximum_central_entries ||
        directory_bytes > limits.maximum_central_directory_bytes) {
        diagnostic = "ZIP central directory exceeds its limit";
        return ZipStatus::resource_limit;
    }
    const auto absolute_end_offset = source.size() - tail.size() + *end_offset;
    std::uint64_t directory_end{};
    if (!checked_add(directory_offset, directory_bytes, directory_end) ||
        directory_end > absolute_end_offset) {
        diagnostic = "ZIP central directory range is invalid";
        return ZipStatus::malformed;
    }

    std::vector<std::byte> central(directory_bytes);
    if (!read_exact(source, directory_offset, central, source_error)) {
        diagnostic = "ZIP central directory could not be read";
        return ZipStatus::io_error;
    }
    std::size_t cursor{};
    for (std::uint32_t index = 0; index < total_entries; ++index) {
        if (central.size() - cursor < kZipCentralHeaderBytes ||
            read_u32(central, cursor) != kZipCentralSignature) {
            diagnostic = "ZIP central entry is truncated";
            return ZipStatus::malformed;
        }
        const auto header =
            std::span<const std::byte>(central).subspan(cursor, kZipCentralHeaderBytes);
        const auto name_bytes = read_u16(header, 28);
        const auto extra_bytes = read_u16(header, 30);
        const auto comment_bytes = read_u16(header, 32);
        const auto disk_start = read_u16(header, 34);
        const auto variable_bytes =
            static_cast<std::uint64_t>(name_bytes) + extra_bytes + comment_bytes;
        std::uint64_t entry_end{};
        if (!checked_add(cursor + kZipCentralHeaderBytes, variable_bytes, entry_end) ||
            entry_end > central.size() || disk_start != 0) {
            diagnostic = "ZIP central entry range is invalid";
            return ZipStatus::malformed;
        }
        const auto name_span = std::span<const std::byte>(central).subspan(
            cursor + kZipCentralHeaderBytes, name_bytes);
        const auto name = normalize_zip_name(name_span, names);
        if (name.empty() && !name_span.empty()) {
            diagnostic = "ZIP entry name is invalid for the selected name policy";
            return ZipStatus::malformed;
        }
        directory.entries.push_back({
            .name = name,
            .flags = read_u16(header, 8),
            .method = read_u16(header, 10),
            .crc32 = read_u32(header, 16),
            .compressed_bytes = read_u32(header, 20),
            .uncompressed_bytes = read_u32(header, 24),
            .local_header_offset = read_u32(header, 42)});
        cursor = static_cast<std::size_t>(entry_end);
    }
    if (cursor != central.size()) {
        diagnostic = "ZIP central directory has trailing records";
        return ZipStatus::malformed;
    }
    directory.offset = directory_offset;
    directory.size = directory_bytes;
    return ZipStatus::success;
}

[[nodiscard]] ZipStatus
validate_zip_entry_layout(const raster::NativeSource &source, const ZipDirectory &directory,
                          const ZipEntry &entry, std::uint64_t &data_offset,
                          ZipEntryRange &entry_range, std::string &diagnostic) {
    entry_range = {};
    const auto encoding = validate_zip_entry_encoding(entry, diagnostic);
    if (encoding != ZipStatus::success) {
        return encoding;
    }
    if (static_cast<std::uint64_t>(entry.local_header_offset) + kZipLocalHeaderBytes >
        directory.offset) {
        diagnostic = "ZIP selected local header range is invalid";
        return ZipStatus::malformed;
    }
    std::array<std::byte, kZipLocalHeaderBytes> local{};
    raster::NativeSourceError source_error;
    if (!read_exact(source, entry.local_header_offset, local, source_error)) {
        diagnostic = "ZIP local preview header could not be read";
        return ZipStatus::io_error;
    }
    if (read_u32(local, 0) != kZipLocalSignature || read_u16(local, 6) != entry.flags ||
        read_u16(local, 8) != entry.method) {
        diagnostic = "ZIP local and central preview headers disagree";
        return ZipStatus::malformed;
    }
    const auto name_bytes = read_u16(local, 26);
    const auto extra_bytes = read_u16(local, 28);
    if (!checked_add(entry.local_header_offset + kZipLocalHeaderBytes,
                     static_cast<std::uint64_t>(name_bytes) + extra_bytes, data_offset)) {
        diagnostic = "ZIP local preview offset overflows";
        return ZipStatus::malformed;
    }
    std::uint64_t data_end{};
    if (!checked_add(data_offset, entry.compressed_bytes, data_end) ||
        data_end > directory.offset) {
        diagnostic = "ZIP preview overlaps the central directory";
        return ZipStatus::malformed;
    }
    std::vector<std::byte> local_name(name_bytes);
    if (!read_exact(source, entry.local_header_offset + kZipLocalHeaderBytes, local_name,
                    source_error) ||
        normalize_zip_name(local_name, directory.name_mode) != entry.name) {
        diagnostic = "ZIP local preview name disagrees with the central directory";
        return source_error ? ZipStatus::io_error : ZipStatus::malformed;
    }
    const auto local_crc = read_u32(local, 14);
    const auto local_compressed = read_u32(local, 18);
    const auto local_uncompressed = read_u32(local, 22);
    if ((entry.flags & kZipDataDescriptorFlag) == 0U) {
        if (local_crc != entry.crc32 || local_compressed != entry.compressed_bytes ||
            local_uncompressed != entry.uncompressed_bytes) {
            diagnostic = "ZIP local entry sizes or CRC disagree";
            return ZipStatus::malformed;
        }
        entry_range = {.begin = entry.local_header_offset, .end = data_end};
        return ZipStatus::success;
    }
    if ((local_crc != 0 && local_crc != entry.crc32) ||
        (local_compressed != 0 && local_compressed != entry.compressed_bytes) ||
        (local_uncompressed != 0 && local_uncompressed != entry.uncompressed_bytes)) {
        diagnostic = "ZIP descriptor entry has conflicting local metadata";
        return ZipStatus::malformed;
    }

    const auto descriptor_available = directory.offset - data_end;
    if (descriptor_available < 12U) {
        diagnostic = "ZIP data descriptor is truncated";
        return ZipStatus::malformed;
    }
    const auto descriptor_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(descriptor_available, static_cast<std::uint64_t>(16U)));
    std::array<std::byte, 16> descriptor{};
    if (!read_exact(source, data_end, std::span<std::byte>(descriptor).first(descriptor_bytes),
                    source_error)) {
        diagnostic = "ZIP data descriptor could not be read";
        return ZipStatus::io_error;
    }
    const auto unsigned_matches = read_u32(descriptor, 0) == entry.crc32 &&
                                  read_u32(descriptor, 4) == entry.compressed_bytes &&
                                  read_u32(descriptor, 8) == entry.uncompressed_bytes;
    const auto signed_matches = descriptor_bytes >= 16U &&
                                read_u32(descriptor, 0) == kZipDataDescriptorSignature &&
                                read_u32(descriptor, 4) == entry.crc32 &&
                                read_u32(descriptor, 8) == entry.compressed_bytes &&
                                read_u32(descriptor, 12) == entry.uncompressed_bytes;
    const auto descriptor_size = signed_matches ? 16U : (unsigned_matches ? 12U : 0U);
    if (descriptor_size == 0U) {
        diagnostic = "ZIP data descriptor disagrees with the central directory";
        return ZipStatus::malformed;
    }
    entry_range = {.begin = entry.local_header_offset, .end = data_end + descriptor_size};
    return ZipStatus::success;
}

[[nodiscard]] ZipStatus
extract_zip_entry(const raster::NativeSource &source, const ZipDirectory &directory,
                  const ZipEntry &entry, const ZipEntryLimits &limits, std::vector<std::byte> &output, std::string &diagnostic,
                  bool &recoverable_candidate_failure, ZipEntryRange &entry_range) {
    recoverable_candidate_failure = false;
    entry_range = {};
    output.clear();
    if (limits.maximum_compressed_bytes == 0 || limits.maximum_compressed_bytes > kMaximumZipEntryBytes ||
        limits.maximum_uncompressed_bytes == 0 || limits.maximum_uncompressed_bytes > kMaximumZipEntryBytes ||
        entry.compressed_bytes > limits.maximum_compressed_bytes ||
        entry.uncompressed_bytes > limits.maximum_uncompressed_bytes) {
        diagnostic = "ZIP selected payload exceeds its limit";
        return ZipStatus::resource_limit;
    }
    std::uint64_t data_offset{};
    const auto layout_status =
        validate_zip_entry_layout(source, directory, entry, data_offset, entry_range, diagnostic);
    if (layout_status != ZipStatus::success) {
        return layout_status;
    }

    raster::NativeSourceError source_error;
    std::vector<std::byte> compressed(entry.compressed_bytes);
    if (!read_exact(source, data_offset, compressed, source_error)) {
        diagnostic = "ZIP preview payload could not be read";
        return ZipStatus::io_error;
    }
    output.resize(entry.uncompressed_bytes);
    if (entry.method == kZipMethodStored) {
        if (compressed.size() != output.size()) {
            diagnostic = "stored ZIP preview has inconsistent sizes";
            recoverable_candidate_failure = true;
            return ZipStatus::malformed;
        }
        output = std::move(compressed);
    } else {
        tinfl_decompressor decompressor{};
        tinfl_init(&decompressor);
        // A valid empty deflate stream still requires non-null buffer pointers.
        mz_uint8 empty_input{}, empty_output{};
        const auto *input = compressed.empty() ? &empty_input
            : reinterpret_cast<const mz_uint8 *>(compressed.data());
        auto *destination = output.empty() ? &empty_output
            : reinterpret_cast<mz_uint8 *>(output.data());
        auto input_bytes = compressed.size();
        auto output_bytes = output.size();
        const auto status =
            tinfl_decompress(&decompressor, input, &input_bytes, destination,
                             destination, &output_bytes,
                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (status != TINFL_STATUS_DONE || input_bytes != compressed.size() ||
            output_bytes != output.size()) {
            diagnostic = "ZIP preview deflate stream is invalid";
            recoverable_candidate_failure = true;
            return ZipStatus::malformed;
        }
    }
    if (crc32(output) != entry.crc32) {
        diagnostic = "ZIP preview CRC does not match";
        recoverable_candidate_failure = true;
        return ZipStatus::malformed;
    }
    return ZipStatus::success;
}

} // namespace vove::handlers::archive
