#pragma once

#include "vove/handlers/archive/zip_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::archive {

inline constexpr std::uint64_t kMaximumArchiveSourceBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumArchivePreviewBytes = kMaximumZipEntryBytes;

enum class ArchivePreviewKind : std::uint8_t { unknown, kra, ora };
enum class ArchivePreviewSelection : std::uint8_t { best_available, thumbnail_only };
enum class ArchivePreviewStatus : std::uint8_t {
    success, unsupported_container, no_preview, malformed, resource_limit, io_error, source_changed,
};

struct ArchivePreviewLimits {
    std::uint64_t maximum_source_bytes{kMaximumArchiveSourceBytes};
    std::uint32_t maximum_central_entries{kMaximumZipEntries};
    std::uint64_t maximum_central_directory_bytes{kMaximumZipDirectoryBytes};
    std::uint64_t maximum_compressed_preview_bytes{kMaximumArchivePreviewBytes};
    std::uint64_t maximum_preview_bytes{kMaximumArchivePreviewBytes};
};

struct ArchivePreviewResult {
    ArchivePreviewStatus status{ArchivePreviewStatus::malformed};
    ArchivePreviewKind kind{ArchivePreviewKind::unknown};
    std::string normalized_name;
    std::vector<std::byte> encoded_image;
    bool is_fallback{};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == ArchivePreviewStatus::success && !encoded_image.empty();
    }
};

// Identity comes only from archive records, not a path or extension. PNG bytes are
// returned unchanged, including their ICC/color chunks. No layer rendering.
[[nodiscard]] ArchivePreviewResult extract_archive_preview(
    const raster::NativeSource &source, const ArchivePreviewLimits &limits = {},
    ArchivePreviewSelection selection = ArchivePreviewSelection::best_available);

} // namespace vove::handlers::archive
