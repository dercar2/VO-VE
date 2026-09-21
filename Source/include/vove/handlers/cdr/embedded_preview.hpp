#pragma once

#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/raster/signature_probe.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::cdr {

inline constexpr std::uint32_t kMaximumCdrCentralEntries = 4'096;
inline constexpr std::uint64_t kMaximumCdrCentralDirectoryBytes = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumCdrCompressedPreviewBytes = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumCdrPreviewBytes = 64ULL * 1024ULL * 1024ULL;

enum class CdrPreviewStatus : std::uint8_t {
    success,
    unsupported_container,
    no_preview,
    malformed,
    resource_limit,
    io_error,
};

enum class CdrContainer : std::uint8_t { unknown, zip, riff };

struct CdrPreviewLimits {
    std::uint32_t maximum_central_entries{kMaximumCdrCentralEntries};
    std::uint64_t maximum_central_directory_bytes{kMaximumCdrCentralDirectoryBytes};
    std::uint64_t maximum_compressed_preview_bytes{kMaximumCdrCompressedPreviewBytes};
    std::uint64_t maximum_total_preview_bytes{kMaximumCdrPreviewBytes};
};

struct CdrEmbeddedCandidate {
    raster::RasterFormat format{raster::RasterFormat::Unknown};
    std::string normalized_name;
    std::vector<std::byte> encoded_image;
    bool page_one{};
};

struct CdrPreviewResult {
    CdrPreviewStatus status{CdrPreviewStatus::malformed};
    CdrContainer container{CdrContainer::unknown};
    std::vector<CdrEmbeddedCandidate> candidates;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == CdrPreviewStatus::success && !candidates.empty();
    }
};

[[nodiscard]] CdrPreviewResult extract_cdr_embedded_previews(const raster::NativeSource &source,
                                                             const CdrPreviewLimits &limits = {});

} // namespace vove::handlers::cdr
