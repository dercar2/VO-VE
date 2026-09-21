#include "vove/handlers/dxf/embedded_preview.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vove::handlers::dxf {
namespace {

constexpr std::size_t kReadBufferBytes = 64U * 1024U;
constexpr std::string_view kBinaryDxfSignature = "AutoCAD Binary DXF\r\n\x1a";

[[nodiscard]] DxfPreviewResult failure(const DxfPreviewStatus status, std::string diagnostic) {
    return {.status = status, .candidate = {}, .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
    const auto whitespace = [](const char character) {
        return character == ' ' || character == '\t' || character == '\r';
    };
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && whitespace(*begin)) {
        ++begin;
    }
    while (begin != end && whitespace(*(end - 1))) {
        --end;
    }
    return {begin, end};
}

[[nodiscard]] bool parse_i32(const std::string_view value, std::int32_t &number) noexcept {
    const auto normalized = trim(value);
    if (normalized.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(normalized.data(), normalized.data() + normalized.size(), number);
    return parsed.ec == std::errc{} && parsed.ptr == normalized.data() + normalized.size();
}

[[nodiscard]] bool parse_u64(const std::string_view value, std::uint64_t &number) noexcept {
    const auto normalized = trim(value);
    if (normalized.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(normalized.data(), normalized.data() + normalized.size(), number);
    return parsed.ec == std::errc{} && parsed.ptr == normalized.data() + normalized.size();
}

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1U])) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

void append_u32(std::vector<std::byte> &bytes, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

[[nodiscard]] std::optional<DxfEmbeddedCandidate> make_bmp(std::vector<std::byte> dib,
                                                           std::string &diagnostic) {
    if (dib.size() < 12U) {
        diagnostic = "DXF thumbnail DIB header is truncated";
        return std::nullopt;
    }
    const auto header_bytes = read_u32(dib, 0);
    std::uint64_t pixel_offset{};
    if (header_bytes == 12U) {
        const auto bits_per_pixel = read_u16(dib, 10);
        const auto palette_entries = bits_per_pixel <= 8U ? (1ULL << bits_per_pixel) : 0ULL;
        pixel_offset = header_bytes + palette_entries * 3ULL;
    } else if ((header_bytes == 40U || header_bytes == 52U || header_bytes == 56U ||
                header_bytes == 108U || header_bytes == 124U) &&
               header_bytes <= dib.size()) {
        if (header_bytes == 124U && read_u32(dib, 116) != 0U) {
            diagnostic = "DXF BITMAPV5 thumbnail with embedded profile is unsupported";
            return std::nullopt;
        }
        const auto bits_per_pixel = read_u16(dib, 14);
        const auto compression = read_u32(dib, 16);
        const auto colors_used = read_u32(dib, 32);
        const auto palette_entries = colors_used != 0U ? static_cast<std::uint64_t>(colors_used)
                                     : bits_per_pixel <= 8U ? (1ULL << bits_per_pixel)
                                                            : 0ULL;
        const auto external_masks = header_bytes == 40U && (compression == 3U || compression == 6U)
                                        ? (compression == 6U ? 16ULL : 12ULL)
                                        : 0ULL;
        pixel_offset =
            static_cast<std::uint64_t>(header_bytes) + external_masks + palette_entries * 4ULL;
    } else {
        diagnostic = "DXF thumbnail DIB header is unsupported";
        return std::nullopt;
    }
    const auto output_bytes = 14ULL + dib.size();
    if (pixel_offset > dib.size() || output_bytes > std::numeric_limits<std::uint32_t>::max()) {
        diagnostic = "DXF thumbnail DIB layout exceeds its limit";
        return std::nullopt;
    }
    std::vector<std::byte> bmp;
    bmp.reserve(static_cast<std::size_t>(output_bytes));
    bmp.push_back(std::byte{'B'});
    bmp.push_back(std::byte{'M'});
    append_u32(bmp, static_cast<std::uint32_t>(output_bytes));
    append_u32(bmp, 0U);
    append_u32(bmp, static_cast<std::uint32_t>(14ULL + pixel_offset));
    bmp.insert(bmp.end(), dib.begin(), dib.end());
    return DxfEmbeddedCandidate{.format = raster::RasterFormat::Bmp,
                                .normalized_name = "dxf/thumbnail.bmp",
                                .encoded_image = std::move(bmp)};
}

class LineReader final {
  public:
    enum class Status { line, end, malformed, resource_limit, io_error };

    LineReader(const raster::NativeSource &source, const DxfPreviewLimits &limits) noexcept
        : source_(source), limits_(limits) {}

    [[nodiscard]] Status next(std::string &line) {
        line.clear();
        for (;;) {
            if (begin_ == end_) {
                const auto status = refill();
                if (status != Status::line) {
                    if (status == Status::end && !line.empty()) {
                        return line.find('\0') == std::string::npos ? Status::line
                                                                    : Status::malformed;
                    }
                    return status;
                }
            }
            const auto newline =
                std::find(buffer_.begin() + static_cast<std::ptrdiff_t>(begin_),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(end_), std::byte{'\n'});
            const auto chunk_end = static_cast<std::size_t>(newline - buffer_.begin());
            const auto chunk_bytes = chunk_end - begin_;
            if (line.size() + chunk_bytes > limits_.maximum_line_bytes) {
                return Status::resource_limit;
            }
            line.append(reinterpret_cast<const char *>(buffer_.data() + begin_), chunk_bytes);
            begin_ = chunk_end;
            if (newline != buffer_.begin() + static_cast<std::ptrdiff_t>(end_)) {
                ++begin_;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line.find('\0') == std::string::npos ? Status::line : Status::malformed;
            }
        }
    }

  private:
    [[nodiscard]] Status refill() {
        if (offset_ >= source_.size()) {
            return Status::end;
        }
        if (offset_ >= limits_.maximum_scan_bytes) {
            return Status::resource_limit;
        }
        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            buffer_.size(), std::min(source_.size(), limits_.maximum_scan_bytes) - offset_));
        const auto read = source_.read_at(offset_, std::span(buffer_).first(bytes));
        if (!read.ok() || read.bytes_read != bytes) {
            return Status::io_error;
        }
        offset_ += bytes;
        begin_ = 0;
        end_ = bytes;
        return Status::line;
    }

    const raster::NativeSource &source_;
    const DxfPreviewLimits &limits_;
    std::array<std::byte, kReadBufferBytes> buffer_{};
    std::uint64_t offset_{};
    std::size_t begin_{};
    std::size_t end_{};
};

[[nodiscard]] int hex_nibble(const char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

[[nodiscard]] bool append_hex(const std::string_view text, std::vector<std::byte> &bytes,
                              const std::uint64_t maximum) {
    const auto value = trim(text);
    if (value.size() > 256U || (value.size() & 1U) != 0U ||
        value.size() / 2U > maximum - bytes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); index += 2U) {
        const auto high = hex_nibble(value[index]);
        const auto low = hex_nibble(value[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes.push_back(static_cast<std::byte>((high << 4U) | low));
    }
    return true;
}

} // namespace

DxfPreviewResult extract_dxf_embedded_preview(const raster::NativeSource &source,
                                              const DxfPreviewLimits &limits) {
    if (limits.maximum_scan_bytes == 0 || limits.maximum_scan_bytes > kMaximumDxfScanBytes ||
        limits.maximum_preview_bytes < 12U ||
        limits.maximum_preview_bytes > kMaximumDxfPreviewBytes || limits.maximum_line_bytes == 0 ||
        limits.maximum_line_bytes > kMaximumDxfLineBytes || limits.maximum_pairs == 0 ||
        limits.maximum_pairs > kMaximumDxfPairs) {
        return failure(DxfPreviewStatus::resource_limit, "DXF preview limits are invalid");
    }
    std::array<std::byte, kBinaryDxfSignature.size()> prefix{};
    if (source.size() >= prefix.size()) {
        const auto read = source.read_at(0, prefix);
        if (!read.ok() || read.bytes_read != prefix.size()) {
            return failure(DxfPreviewStatus::io_error, "DXF signature could not be read");
        }
        if (std::equal(prefix.begin(), prefix.end(),
                       reinterpret_cast<const std::byte *>(kBinaryDxfSignature.data()))) {
            return failure(DxfPreviewStatus::unsupported_container,
                           "Binary DXF embedded previews are not supported");
        }
    }

    LineReader reader(source, limits);
    std::string code_line;
    std::string value_line;
    bool expecting_section_name{};
    bool inside_section{};
    bool thumbnail_section{};
    bool saw_dxf_section{};
    bool saw_eof{};
    bool saw_count{};
    std::uint64_t expected_bytes{};
    std::vector<std::byte> preview;

    std::uint64_t pair{};
    for (; pair < limits.maximum_pairs; ++pair) {
        const auto code_status = reader.next(code_line);
        if (code_status == LineReader::Status::end)
            break;
        if (code_status == LineReader::Status::resource_limit)
            return failure(DxfPreviewStatus::resource_limit, "DXF scan exceeds its limit");
        if (code_status == LineReader::Status::io_error)
            return failure(DxfPreviewStatus::io_error, "DXF source could not be read");
        if (code_status != LineReader::Status::line)
            return failure(DxfPreviewStatus::malformed, "DXF contains an invalid text line");
        const auto value_status = reader.next(value_line);
        if (value_status == LineReader::Status::resource_limit)
            return failure(DxfPreviewStatus::resource_limit, "DXF scan exceeds its limit");
        if (value_status == LineReader::Status::io_error)
            return failure(DxfPreviewStatus::io_error, "DXF source could not be read");
        if (value_status != LineReader::Status::line)
            return failure(saw_dxf_section ? DxfPreviewStatus::malformed
                                           : DxfPreviewStatus::unsupported_container,
                           saw_dxf_section ? "DXF has an incomplete code/value pair"
                                           : "Source is not an ASCII DXF document");

        std::int32_t code{};
        if (!parse_i32(code_line, code)) {
            return failure(saw_dxf_section ? DxfPreviewStatus::malformed
                                           : DxfPreviewStatus::unsupported_container,
                           saw_dxf_section ? "DXF group code is invalid"
                                           : "Source is not an ASCII DXF document");
        }
        const auto value = trim(value_line);
        if (expecting_section_name) {
            if (code != 2) {
                return failure(DxfPreviewStatus::malformed, "DXF SECTION has no name");
            }
            thumbnail_section = value == "THUMBNAILIMAGE";
            saw_dxf_section = saw_dxf_section || value == "HEADER" || value == "CLASSES" ||
                              value == "TABLES" || value == "BLOCKS" || value == "ENTITIES" ||
                              value == "OBJECTS" || thumbnail_section;
            expecting_section_name = false;
            continue;
        }
        if (code == 0 && value == "SECTION") {
            if (inside_section) {
                return failure(DxfPreviewStatus::malformed,
                               "DXF starts a section before closing the previous section");
            }
            inside_section = true;
            expecting_section_name = true;
            thumbnail_section = false;
            continue;
        }
        if (code == 0 && value == "ENDSEC") {
            if (!inside_section) {
                return failure(DxfPreviewStatus::malformed,
                               "DXF closes a section that was not opened");
            }
            inside_section = false;
            if (!thumbnail_section) {
                continue;
            }
            if (!saw_count || preview.size() != expected_bytes) {
                return failure(DxfPreviewStatus::malformed,
                               "DXF thumbnail byte count does not match its payload");
            }
            std::string diagnostic;
            auto candidate = make_bmp(std::move(preview), diagnostic);
            if (!candidate) {
                return failure(DxfPreviewStatus::malformed, std::move(diagnostic));
            }
            return {.status = DxfPreviewStatus::success,
                    .candidate = std::move(*candidate),
                    .diagnostic = {}};
        }
        if (code == 0 && value == "EOF") {
            if (inside_section) {
                return failure(DxfPreviewStatus::malformed,
                               "DXF reaches EOF before closing its section");
            }
            saw_eof = true;
            break;
        }
        if (!thumbnail_section) {
            continue;
        }
        if (code == 90) {
            if (saw_count || !parse_u64(value, expected_bytes) || expected_bytes == 0U ||
                expected_bytes > limits.maximum_preview_bytes) {
                return failure(expected_bytes > limits.maximum_preview_bytes
                                   ? DxfPreviewStatus::resource_limit
                                   : DxfPreviewStatus::malformed,
                               "DXF thumbnail byte count is invalid");
            }
            saw_count = true;
        } else if (code == 310) {
            if (!saw_count || !append_hex(value, preview, limits.maximum_preview_bytes)) {
                return failure(DxfPreviewStatus::malformed, "DXF thumbnail hex payload is invalid");
            }
        }
    }
    if (pair == limits.maximum_pairs) {
        return failure(DxfPreviewStatus::resource_limit, "DXF pair count exceeds its limit");
    }
    if (!saw_dxf_section) {
        return failure(DxfPreviewStatus::unsupported_container,
                       "Source is not an ASCII DXF document");
    }
    if (inside_section || thumbnail_section || expecting_section_name) {
        return failure(DxfPreviewStatus::malformed, "DXF section is truncated");
    }
    return failure(saw_eof ? DxfPreviewStatus::no_preview : DxfPreviewStatus::malformed,
                   saw_eof ? "DXF has no saved thumbnail" : "DXF end marker is missing");
}

} // namespace vove::handlers::dxf
