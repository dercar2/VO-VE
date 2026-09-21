#pragma once

#include "vove/handlers/raster/native_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vove::handlers::archive {

inline constexpr std::uint32_t kMaximumZipEntries = 4'096;
inline constexpr std::uint64_t kMaximumZipDirectoryBytes = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumZipEntryBytes = 64ULL * 1024ULL * 1024ULL;

enum class ZipStatus : std::uint8_t { success, malformed, resource_limit, io_error, unsupported };
enum class ZipNameMode : std::uint8_t { exact, cdr_ascii_folded };

struct ZipDirectoryLimits {
    std::uint32_t maximum_central_entries{kMaximumZipEntries};
    std::uint64_t maximum_central_directory_bytes{kMaximumZipDirectoryBytes};
};

struct ZipEntryLimits {
    std::uint64_t maximum_compressed_bytes{kMaximumZipEntryBytes};
    std::uint64_t maximum_uncompressed_bytes{kMaximumZipEntryBytes};
};

struct ZipEntry {
    std::string name;
    std::uint16_t flags{};
    std::uint16_t method{};
    std::uint32_t crc32{};
    std::uint32_t compressed_bytes{};
    std::uint32_t uncompressed_bytes{};
    std::uint32_t local_header_offset{};
};

struct ZipDirectory {
    std::uint64_t offset{};
    std::uint64_t size{};
    ZipNameMode name_mode{ZipNameMode::exact};
    std::vector<ZipEntry> entries;
};

struct ZipEntryRange {
    std::uint64_t begin{};
    std::uint64_t end{};
};

[[nodiscard]] ZipStatus read_zip_directory(const raster::NativeSource &source,
                                            const ZipDirectoryLimits &limits,
                                            ZipDirectory &directory, std::string &diagnostic,
                                            ZipNameMode names = ZipNameMode::exact);
[[nodiscard]] ZipStatus validate_zip_entry_encoding(const ZipEntry &entry,
                                                     std::string &diagnostic);
[[nodiscard]] ZipStatus validate_zip_entry_layout(const raster::NativeSource &source,
                                                  const ZipDirectory &directory,
                                                  const ZipEntry &entry,
                                                  std::uint64_t &data_offset,
                                                  ZipEntryRange &range,
                                                  std::string &diagnostic);
// payload_failure distinguishes an intact selected layout with bad deflate/CRC.
// CDR can try its other preview; archive documents deliberately fail closed.
[[nodiscard]] ZipStatus extract_zip_entry(const raster::NativeSource &source,
                                          const ZipDirectory &directory, const ZipEntry &entry,
                                          const ZipEntryLimits &limits,
                                          std::vector<std::byte> &output,
                                          std::string &diagnostic, bool &payload_failure,
                                          ZipEntryRange &range);

} // namespace vove::handlers::archive
