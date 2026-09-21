#include "vove/handlers/archive/embedded_preview.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vove::handlers::archive {
namespace {

using Status = ArchivePreviewStatus;
constexpr std::uint64_t kMaximumIdentityBytes = 1024ULL * 1024ULL;
constexpr std::array kPngSignature{
    std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
    std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};

[[nodiscard]] ArchivePreviewResult failure(const ArchivePreviewKind kind, const Status status,
                                           std::string diagnostic) {
    return {.status = status, .kind = kind, .normalized_name = {}, .encoded_image = {},
            .is_fallback = false, .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] Status convert_status(const ZipStatus status) noexcept {
    switch (status) {
    case ZipStatus::success: return Status::success;
    case ZipStatus::malformed: return Status::malformed;
    case ZipStatus::resource_limit: return Status::resource_limit;
    case ZipStatus::io_error: return Status::io_error;
    case ZipStatus::unsupported: return Status::unsupported_container;
    }
    return Status::malformed;
}

[[nodiscard]] bool valid_limits(const ArchivePreviewLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 &&
           limits.maximum_source_bytes <= kMaximumArchiveSourceBytes &&
           limits.maximum_central_entries != 0 && limits.maximum_central_entries <= kMaximumZipEntries &&
           limits.maximum_central_directory_bytes != 0 &&
           limits.maximum_central_directory_bytes <= kMaximumZipDirectoryBytes &&
           limits.maximum_compressed_preview_bytes != 0 &&
           limits.maximum_compressed_preview_bytes <= kMaximumArchivePreviewBytes &&
           limits.maximum_preview_bytes != 0 &&
           limits.maximum_preview_bytes <= kMaximumArchivePreviewBytes;
}

[[nodiscard]] ArchivePreviewResult extract_impl(const raster::NativeSource &source,
                                                const ArchivePreviewLimits &limits,
                                                const ArchivePreviewSelection selection) {
    auto kind = ArchivePreviewKind::unknown;
    if (!valid_limits(limits)) {
        return failure(kind, Status::resource_limit, "archive preview limits are invalid");
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(kind, Status::resource_limit, "archive source exceeds its limit");
    }
    std::array<std::byte, 4> prefix{};
    const auto read = source.read_at(0, prefix);
    if (!read.ok()) {
        return failure(kind, Status::io_error, "archive signature could not be read");
    }
    if (read.bytes_read != prefix.size()) {
        return failure(kind, Status::malformed, "archive signature is truncated");
    }
    constexpr std::array local{std::byte{'P'}, std::byte{'K'}, std::byte{3}, std::byte{4}};
    constexpr std::array empty{std::byte{'P'}, std::byte{'K'}, std::byte{5}, std::byte{6}};
    if (prefix != local && prefix != empty) {
        return failure(kind, Status::unsupported_container, "document is not a ZIP archive");
    }
    ZipDirectory directory;
    std::string diagnostic;
    const auto parsed = read_zip_directory(source,
        {limits.maximum_central_entries, limits.maximum_central_directory_bytes},
        directory, diagnostic);
    if (parsed != ZipStatus::success) {
        return failure(kind, convert_status(parsed), std::move(diagnostic));
    }

    bool duplicate{};
    const auto find = [&](const std::string_view name) -> const ZipEntry * {
        const ZipEntry *found{};
        for (const auto &entry : directory.entries) {
            if (entry.name != name) continue;
            if (found) duplicate = true;
            found = &entry;
        }
        return found;
    };
    const auto extract = [&](const ZipEntry &entry, const ZipEntryLimits &entry_limits,
                              std::vector<std::byte> &output) {
        bool payload_failure{};
        ZipEntryRange range;
        return extract_zip_entry(source, directory, entry, entry_limits, output,
                                  diagnostic, payload_failure, range);
    };
    const auto *mime = find("mimetype");
    const ZipEntry *marker{}, *document_info{};
    if (duplicate) {
        return failure(kind, Status::malformed, "archive has duplicate mimetype entries");
    }
    if (mime) {
        std::vector<std::byte> bytes;
        const auto status = extract(*mime, {256, 128}, bytes);
        if (status != ZipStatus::success) {
            return failure(kind, convert_status(status), std::move(diagnostic));
        }
        const auto matches = [&](const std::string_view expected) {
            return bytes.size() == expected.size() &&
                   std::equal(bytes.begin(), bytes.end(),
                              reinterpret_cast<const std::byte *>(expected.data()));
        };
        if (matches("application/x-krita")) kind = ArchivePreviewKind::kra;
        else if (matches("image/openraster")) kind = ArchivePreviewKind::ora;
        else return failure(kind, Status::unsupported_container, "archive mimetype is not KRA or ORA");
    } else {
        const auto *kra_marker = find("maindoc.xml");
        const auto *kra_info = find("documentinfo.xml");
        const auto *ora_marker = find("stack.xml");
        const bool is_kra = kra_marker && kra_info;
        const bool is_ora = ora_marker != nullptr;
        if (is_kra == is_ora) {
            return failure(kind, Status::unsupported_container,
                           "archive document identity is absent or ambiguous");
        }
        kind = is_kra ? ArchivePreviewKind::kra : ArchivePreviewKind::ora;
        marker = is_kra ? kra_marker : ora_marker;
        document_info = is_kra ? kra_info : nullptr;
    }
    const auto *merged = find("mergedimage.png");
    const auto *fallback = find(kind == ArchivePreviewKind::kra
                                    ? "preview.png" : "Thumbnails/thumbnail.png");
    if (duplicate) {
        return failure(kind, Status::malformed, "archive has duplicate identity or preview entries");
    }

    // Validate only identity/preview records, never decompress unrelated layers/resources.
    std::vector<ZipEntryRange> ranges;
    const auto validate = [&](const ZipEntry *entry) -> ZipStatus {
        if (!entry) return ZipStatus::success;
        std::uint64_t data_offset{};
        ZipEntryRange range;
        const auto status = validate_zip_entry_layout(source, directory, *entry, data_offset,
                                                      range, diagnostic);
        if (status != ZipStatus::success) return status;
        if (entry->method == 0 && entry->compressed_bytes != entry->uncompressed_bytes) {
            diagnostic = "stored archive entry has inconsistent sizes";
            return ZipStatus::malformed;
        }
        if (std::any_of(ranges.begin(), ranges.end(), [&](const auto &existing) {
                return range.begin < existing.end && existing.begin < range.end;
            })) {
            diagnostic = "archive selected records overlap";
            return ZipStatus::malformed;
        }
        ranges.push_back(range);
        return ZipStatus::success;
    };
    const std::array selected{mime, mime ? nullptr : marker, mime ? nullptr : document_info,
                              merged, fallback};
    for (const auto *entry : selected) {
        const auto status = validate(entry);
        if (status != ZipStatus::success) {
            return failure(kind, convert_status(status), std::move(diagnostic));
        }
    }
    if (!mime) {
        // Canonical marker names are a fallback identity, not an XML/layer interpretation.
        for (const auto *entry : {marker, document_info}) {
            if (!entry) continue;
            std::vector<std::byte> bytes;
            const auto status = extract(*entry, {kMaximumIdentityBytes, kMaximumIdentityBytes}, bytes);
            if (status != ZipStatus::success) {
                return failure(kind, convert_status(status), std::move(diagnostic));
            }
            if (bytes.empty()) {
                return failure(kind, Status::unsupported_container, "archive identity marker is empty");
            }
        }
    }
    const bool merged_over_budget = merged &&
        (merged->compressed_bytes > limits.maximum_compressed_preview_bytes ||
         merged->uncompressed_bytes > limits.maximum_preview_bytes);
    const auto *preview = selection == ArchivePreviewSelection::thumbnail_only
                              ? fallback
                              : merged && (!merged_over_budget || !fallback) ? merged : fallback;
    if (!preview) {
        return failure(kind, Status::no_preview, "document contains no merged image or recognized thumbnail");
    }
    std::vector<std::byte> png;
    const auto status = extract(*preview,
        {limits.maximum_compressed_preview_bytes, limits.maximum_preview_bytes}, png);
    if (status != ZipStatus::success) {
        return failure(kind, convert_status(status), std::move(diagnostic));
    }
    if (png.size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), png.begin())) {
        return failure(kind, Status::malformed, "archive preview is not a PNG image");
    }
    const auto unchanged = source.validate_unchanged();
    if (!unchanged.unchanged || unchanged.error) {
        return failure(kind, unchanged.error.code == raster::NativeSourceErrorCode::source_changed
                                 ? Status::source_changed : Status::io_error,
                       "archive source changed or became unavailable");
    }
    return {.status = Status::success, .kind = kind, .normalized_name = preview->name,
            .encoded_image = std::move(png), .is_fallback = preview == fallback, .diagnostic = {}};
}

} // namespace

ArchivePreviewResult extract_archive_preview(const raster::NativeSource &source,
                                              const ArchivePreviewLimits &limits,
                                              const ArchivePreviewSelection selection) {
    try {
        return extract_impl(source, limits, selection);
    } catch (const std::bad_alloc &) {
        return failure(ArchivePreviewKind::unknown, Status::resource_limit, "archive preview allocation failed");
    } catch (const std::length_error &) {
        return failure(ArchivePreviewKind::unknown, Status::resource_limit, "archive preview allocation overflow");
    }
}

} // namespace vove::handlers::archive
