#include "vove/handlers/cdr/embedded_preview.hpp"
#include "vove/handlers/archive/zip_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vove::handlers::cdr {
namespace {

constexpr std::uint32_t kZipLocalSignature = 0x04034b50U;
constexpr std::uint32_t kRiffSignature = 0x46464952U;
constexpr std::size_t kMaximumRiffDepth = 16;

[[nodiscard]] CdrPreviewResult failure(const CdrPreviewStatus status, const CdrContainer container,
                                       std::string diagnostic) {
    return {.status = status,
            .container = container,
            .candidates = {},
            .diagnostic = std::move(diagnostic)};
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

void append_u32(std::vector<std::byte> &bytes, const std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
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

[[nodiscard]] bool is_cdr_marker(const std::string_view name) noexcept {
    return name == "content/root.dat" || name == "content/riffdata.cdr";
}

[[nodiscard]] raster::RasterFormat candidate_format(const std::string_view name) noexcept {
    if (name == "previews/page1.png" || name == "previews/thumbnail.png") {
        return raster::RasterFormat::Png;
    }
    if (name == "metadata/thumbnails/page1.bmp" || name == "metadata/thumbnails/thumbnail.bmp") {
        return raster::RasterFormat::Bmp;
    }
    return raster::RasterFormat::Unknown;
}

[[nodiscard]] bool is_page_one(const std::string_view name) noexcept {
    return name.ends_with("/page1.png") || name.ends_with("/page1.bmp");
}

[[nodiscard]] CdrPreviewStatus zip_status(const archive::ZipStatus status) noexcept {
    switch (status) {
    case archive::ZipStatus::success: return CdrPreviewStatus::success;
    case archive::ZipStatus::malformed: return CdrPreviewStatus::malformed;
    case archive::ZipStatus::io_error: return CdrPreviewStatus::io_error;
    case archive::ZipStatus::unsupported:
    case archive::ZipStatus::resource_limit: return CdrPreviewStatus::resource_limit;
    }
    return CdrPreviewStatus::malformed;
}

using archive::ZipEntry;
using archive::ZipEntryRange;
struct ZipDirectory : archive::ZipDirectory {
    std::vector<ZipEntry> markers;
    std::vector<ZipEntry> candidates;
};

[[nodiscard]] CdrPreviewStatus parse_zip_directory(const raster::NativeSource &source,
                                                   const CdrPreviewLimits &limits,
                                                   ZipDirectory &directory,
                                                   std::string &diagnostic) {
    const auto parsed = archive::read_zip_directory(source,
        {limits.maximum_central_entries, limits.maximum_central_directory_bytes},
        directory, diagnostic, archive::ZipNameMode::cdr_ascii_folded);
    if (parsed != archive::ZipStatus::success) {
        return zip_status(parsed);
    }
    for (const auto &entry : directory.entries) {
        const bool marker = is_cdr_marker(entry.name);
        if (!marker && candidate_format(entry.name) == raster::RasterFormat::Unknown) {
            continue;
        }
        const auto duplicate = [&](const auto &entries) {
            return std::any_of(entries.begin(), entries.end(),
                               [&](const auto &existing) { return existing.name == entry.name; });
        };
        if (duplicate(directory.markers) || duplicate(directory.candidates)) {
            diagnostic = "CDR ZIP contains a duplicate selected entry";
            return CdrPreviewStatus::malformed;
        }
        const auto encoding = archive::validate_zip_entry_encoding(entry, diagnostic);
        if (encoding != archive::ZipStatus::success) {
            return zip_status(encoding);
        }
        if (!marker && (entry.compressed_bytes == 0 || entry.uncompressed_bytes == 0 ||
                        entry.compressed_bytes > limits.maximum_compressed_preview_bytes ||
                        entry.uncompressed_bytes > limits.maximum_total_preview_bytes)) {
            diagnostic = "CDR embedded preview exceeds its size limit";
            return CdrPreviewStatus::resource_limit;
        }
        (marker ? directory.markers : directory.candidates).push_back(entry);
    }
    return CdrPreviewStatus::success;
}

[[nodiscard]] CdrPreviewStatus validate_cdr_entry_layout(
    const raster::NativeSource &source, const ZipDirectory &directory, const ZipEntry &entry,
    std::uint64_t &data_offset, ZipEntryRange &range, std::string &diagnostic) {
    return zip_status(archive::validate_zip_entry_layout(source, directory, entry, data_offset,
                                                        range, diagnostic));
}

[[nodiscard]] CdrPreviewResult extract_zip(const raster::NativeSource &source,
                                           const CdrPreviewLimits &limits) {
    ZipDirectory directory;
    std::string diagnostic;
    const auto directory_status = parse_zip_directory(source, limits, directory, diagnostic);
    if (directory_status != CdrPreviewStatus::success) {
        return failure(directory_status, CdrContainer::zip, std::move(diagnostic));
    }
    if (directory.markers.empty()) {
        return failure(CdrPreviewStatus::unsupported_container, CdrContainer::zip,
                       "ZIP container has no CorelDRAW content marker");
    }

    std::vector<ZipEntryRange> entry_ranges;
    entry_ranges.reserve(directory.markers.size() + directory.candidates.size());
    for (const auto &marker : directory.markers) {
        std::uint64_t marker_data_offset{};
        ZipEntryRange marker_range;
        const auto marker_status = validate_cdr_entry_layout(
            source, directory, marker, marker_data_offset, marker_range, diagnostic);
        if (marker_status != CdrPreviewStatus::success) {
            return failure(marker_status, CdrContainer::zip, std::move(diagnostic));
        }
        const auto overlaps = std::any_of(
            entry_ranges.cbegin(), entry_ranges.cend(), [&marker_range](const auto &existing) {
                return marker_range.begin < existing.end && existing.begin < marker_range.end;
            });
        if (overlaps) {
            return failure(CdrPreviewStatus::malformed, CdrContainer::zip,
                           "CDR ZIP selected records overlap");
        }
        entry_ranges.push_back(marker_range);
    }
    if (directory.candidates.empty()) {
        return failure(CdrPreviewStatus::no_preview, CdrContainer::zip,
                       "CDR has no recognized embedded preview");
    }

    std::uint64_t total_bytes{};
    std::vector<CdrEmbeddedCandidate> candidates;
    candidates.reserve(directory.candidates.size());
    CdrPreviewStatus last_candidate_status{CdrPreviewStatus::malformed};
    std::string last_candidate_diagnostic;
    for (const auto &entry : directory.candidates) {
        if (!checked_add(total_bytes, entry.uncompressed_bytes, total_bytes) ||
            total_bytes > limits.maximum_total_preview_bytes) {
            return failure(CdrPreviewStatus::resource_limit, CdrContainer::zip,
                           "total CDR preview payload exceeds its limit");
        }
        std::vector<std::byte> image;
        bool recoverable_candidate_failure{};
        ZipEntryRange entry_range;
        const auto status = zip_status(archive::extract_zip_entry(
            source, directory, entry,
            {limits.maximum_compressed_preview_bytes, limits.maximum_total_preview_bytes},
            image, diagnostic, recoverable_candidate_failure, entry_range));
        if (entry_range.end != 0) {
            const auto overlaps = std::any_of(
                entry_ranges.cbegin(), entry_ranges.cend(), [&entry_range](const auto &existing) {
                    return entry_range.begin < existing.end && existing.begin < entry_range.end;
                });
            if (overlaps) {
                return failure(CdrPreviewStatus::malformed, CdrContainer::zip,
                               "CDR ZIP selected records overlap");
            }
            entry_ranges.push_back(entry_range);
        }
        if (status != CdrPreviewStatus::success) {
            if (!recoverable_candidate_failure) {
                return failure(status, CdrContainer::zip, std::move(diagnostic));
            }
            last_candidate_status = status;
            last_candidate_diagnostic = std::move(diagnostic);
            continue;
        }
        candidates.push_back({.format = candidate_format(entry.name),
                              .normalized_name = entry.name,
                              .encoded_image = std::move(image),
                              .page_one = is_page_one(entry.name)});
    }
    if (candidates.empty()) {
        return failure(last_candidate_status, CdrContainer::zip,
                       last_candidate_diagnostic.empty()
                           ? "CDR contains no valid recognized embedded preview"
                           : std::move(last_candidate_diagnostic));
    }
    return {.status = CdrPreviewStatus::success,
            .container = CdrContainer::zip,
            .candidates = std::move(candidates),
            .diagnostic = {}};
}

struct RiffWalkState {
    const raster::NativeSource &source;
    const CdrPreviewLimits &limits;
    std::uint32_t chunks{};
    std::optional<CdrEmbeddedCandidate> preview;
    CdrPreviewStatus status{CdrPreviewStatus::success};
    std::string diagnostic;
};

[[nodiscard]] std::optional<CdrEmbeddedCandidate>
make_disp_preview(const std::span<const std::byte> payload, const CdrPreviewLimits &limits,
                  std::string &diagnostic) {
    if (payload.size() < 4U + 40U || payload.size() > limits.maximum_total_preview_bytes) {
        diagnostic = "CDR RIFF DISP payload has invalid size";
        return std::nullopt;
    }
    const auto dib = payload.subspan(4);
    const auto dib_header_bytes = read_u32(dib, 0);
    if (dib_header_bytes < 40U || dib_header_bytes > dib.size()) {
        diagnostic = "CDR RIFF DISP DIB header is invalid";
        return std::nullopt;
    }
    const auto output_bytes = 14ULL + dib.size();
    if (output_bytes > limits.maximum_total_preview_bytes ||
        output_bytes > std::numeric_limits<std::uint32_t>::max()) {
        diagnostic = "CDR RIFF DISP preview exceeds its limit";
        return std::nullopt;
    }
    const auto image_bytes = read_u32(dib, 20);
    std::uint64_t pixel_offset = image_bytes != 0 && image_bytes <= dib.size()
                                     ? output_bytes - image_bytes
                                     : 14ULL + dib_header_bytes;
    if (pixel_offset < 14ULL || pixel_offset > output_bytes ||
        pixel_offset > std::numeric_limits<std::uint32_t>::max()) {
        diagnostic = "CDR RIFF DISP pixel offset is invalid";
        return std::nullopt;
    }

    std::vector<std::byte> bmp;
    bmp.reserve(static_cast<std::size_t>(output_bytes));
    bmp.push_back(static_cast<std::byte>(static_cast<unsigned char>('B')));
    bmp.push_back(static_cast<std::byte>(static_cast<unsigned char>('M')));
    append_u32(bmp, static_cast<std::uint32_t>(output_bytes));
    append_u32(bmp, 0);
    append_u32(bmp, static_cast<std::uint32_t>(pixel_offset));
    bmp.insert(bmp.end(), dib.begin(), dib.end());
    return CdrEmbeddedCandidate{.format = raster::RasterFormat::Bmp,
                                .normalized_name = "riff/disp.bmp",
                                .encoded_image = std::move(bmp),
                                .page_one = true};
}

void walk_riff(RiffWalkState &state, const std::uint64_t begin, const std::uint64_t end,
               const std::size_t depth) {
    if (state.status != CdrPreviewStatus::success || state.preview || depth > kMaximumRiffDepth) {
        if (depth > kMaximumRiffDepth) {
            state.status = CdrPreviewStatus::resource_limit;
            state.diagnostic = "CDR RIFF nesting exceeds its limit";
        }
        return;
    }
    std::uint64_t cursor = begin;
    while (cursor + 8U <= end && !state.preview) {
        if (++state.chunks > state.limits.maximum_central_entries) {
            state.status = CdrPreviewStatus::resource_limit;
            state.diagnostic = "CDR RIFF chunk count exceeds its limit";
            return;
        }
        std::array<std::byte, 8> header{};
        raster::NativeSourceError source_error;
        if (!read_exact(state.source, cursor, header, source_error)) {
            state.status = CdrPreviewStatus::io_error;
            state.diagnostic = "CDR RIFF chunk header could not be read";
            return;
        }
        const std::string id(reinterpret_cast<const char *>(header.data()), 4);
        const auto payload_bytes = read_u32(header, 4);
        std::uint64_t payload_begin{};
        std::uint64_t payload_end{};
        if (!checked_add(cursor, 8U, payload_begin) ||
            !checked_add(payload_begin, payload_bytes, payload_end) || payload_end > end) {
            state.status = CdrPreviewStatus::malformed;
            state.diagnostic = "CDR RIFF chunk range is invalid";
            return;
        }
        if (id == "DISP") {
            if (payload_bytes > state.limits.maximum_total_preview_bytes) {
                state.status = CdrPreviewStatus::resource_limit;
                state.diagnostic = "CDR RIFF DISP payload exceeds its limit";
                return;
            }
            std::vector<std::byte> payload(payload_bytes);
            if (!read_exact(state.source, payload_begin, payload, source_error)) {
                state.status = CdrPreviewStatus::io_error;
                state.diagnostic = "CDR RIFF DISP payload could not be read";
                return;
            }
            state.preview = make_disp_preview(payload, state.limits, state.diagnostic);
            if (!state.preview) {
                state.status = CdrPreviewStatus::malformed;
            }
            return;
        }
        if ((id == "LIST" || id == "RIFF") && payload_bytes >= 4U) {
            walk_riff(state, payload_begin + 4U, payload_end, depth + 1U);
            if (state.status != CdrPreviewStatus::success || state.preview) {
                return;
            }
        }
        const auto padded = static_cast<std::uint64_t>(payload_bytes) + (payload_bytes & 1U);
        if (!checked_add(payload_begin, padded, cursor) || cursor > end) {
            state.status = CdrPreviewStatus::malformed;
            state.diagnostic = "CDR RIFF padded chunk range is invalid";
            return;
        }
    }
    if (cursor != end && cursor + 1U != end) {
        state.status = CdrPreviewStatus::malformed;
        state.diagnostic = "CDR RIFF has trailing partial chunk data";
    }
}

[[nodiscard]] CdrPreviewResult extract_riff(const raster::NativeSource &source,
                                            const CdrPreviewLimits &limits,
                                            const std::span<const std::byte> prefix) {
    const auto declared = static_cast<std::uint64_t>(read_u32(prefix, 4)) + 8U;
    if (declared > source.size() || declared < 12U) {
        return failure(CdrPreviewStatus::malformed, CdrContainer::riff,
                       "CDR RIFF declared size is invalid");
    }
    RiffWalkState state{.source = source,
                        .limits = limits,
                        .chunks = 0,
                        .preview = std::nullopt,
                        .status = CdrPreviewStatus::success,
                        .diagnostic = {}};
    walk_riff(state, 12U, declared, 0);
    if (state.status != CdrPreviewStatus::success) {
        return failure(state.status, CdrContainer::riff, std::move(state.diagnostic));
    }
    if (!state.preview) {
        return failure(CdrPreviewStatus::no_preview, CdrContainer::riff,
                       "CDR has no recognized RIFF DISP preview");
    }
    std::vector<CdrEmbeddedCandidate> candidates;
    candidates.push_back(std::move(*state.preview));
    return {.status = CdrPreviewStatus::success,
            .container = CdrContainer::riff,
            .candidates = std::move(candidates),
            .diagnostic = {}};
}

[[nodiscard]] bool valid_limits(const CdrPreviewLimits &limits) noexcept {
    return limits.maximum_central_entries > 0 &&
           limits.maximum_central_entries <= kMaximumCdrCentralEntries &&
           limits.maximum_central_directory_bytes > 0 &&
           limits.maximum_central_directory_bytes <= kMaximumCdrCentralDirectoryBytes &&
           limits.maximum_compressed_preview_bytes > 0 &&
           limits.maximum_compressed_preview_bytes <= kMaximumCdrCompressedPreviewBytes &&
           limits.maximum_total_preview_bytes > 0 &&
           limits.maximum_total_preview_bytes <= kMaximumCdrPreviewBytes;
}

} // namespace

CdrPreviewResult extract_cdr_embedded_previews(const raster::NativeSource &source,
                                               const CdrPreviewLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(CdrPreviewStatus::resource_limit, CdrContainer::unknown,
                       "CDR preview limits are invalid");
    }
    std::array<std::byte, 12> prefix{};
    raster::NativeSourceError source_error;
    if (source.size() < 4U ||
        !read_exact(source, 0,
                    std::span(prefix).first(static_cast<std::size_t>(
                        std::min<std::uint64_t>(source.size(), prefix.size()))),
                    source_error)) {
        return failure(source_error ? CdrPreviewStatus::io_error : CdrPreviewStatus::malformed,
                       CdrContainer::unknown, "CDR signature could not be read");
    }
    const auto prefix_span = std::span<const std::byte>(prefix);
    if (read_u32(prefix_span, 0) == kZipLocalSignature) {
        return extract_zip(source, limits);
    }
    if (source.size() >= prefix.size() && read_u32(prefix_span, 0) == kRiffSignature &&
        std::to_integer<unsigned char>(prefix[8]) == static_cast<unsigned char>('C') &&
        std::to_integer<unsigned char>(prefix[9]) == static_cast<unsigned char>('D') &&
        std::to_integer<unsigned char>(prefix[10]) == static_cast<unsigned char>('R')) {
        return extract_riff(source, limits, prefix_span);
    }
    return failure(CdrPreviewStatus::unsupported_container, CdrContainer::unknown,
                   "source is not a recognized CDR container");
}

} // namespace vove::handlers::cdr
