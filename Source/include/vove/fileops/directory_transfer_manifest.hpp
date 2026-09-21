#pragma once

#include "vove/fileops/file_transfer_transaction.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kDirectoryTransferManifestVersion = 2U;
inline constexpr std::size_t kMaximumDirectoryTransferRoots = 1'024U;
inline constexpr std::size_t kMaximumDirectoryTransferEntries = 100'000U;
inline constexpr std::uint16_t kMaximumDirectoryTransferDepth = 128U;
inline constexpr std::size_t kMaximumDirectoryTransferManifestBytes =
    std::size_t{64} * 1'024U * 1'024U;
inline constexpr std::size_t kDirectoryTransferManifestDigestBytes = 32U;
inline constexpr std::size_t kDirectoryTransferOwnershipTokenBytes = 16U;
inline constexpr std::uint32_t kDirectoryTransferRootParent = 0xffffffffU;

enum class DirectoryManifestEntryKind : std::uint8_t {
    directory,
    regular_file,
};

enum class DirectoryPathSemantics : std::uint8_t {
    windows_ordinal_nfc,
    posix_exact,
};

enum class DirectoryTransferManifestDecodeStatus : std::uint8_t {
    success,
    incompatible_version,
    corrupt,
};

struct DirectoryTransferManifestRoot {
    std::filesystem::path source;
    std::filesystem::path staged_source;
    std::filesystem::path staging_destination;
    std::filesystem::path destination;
    SourceSnapshot source_snapshot;
    std::string source_parent_identity_utf8;
    std::string destination_parent_identity_utf8;
    std::string source_parent_revision_utf8;
    std::string destination_parent_revision_utf8;
    std::string source_namespace_key_utf8;
    std::string staged_source_namespace_key_utf8;
    std::string staging_destination_namespace_key_utf8;
    std::string destination_namespace_key_utf8;
    std::array<std::uint8_t, kDirectoryTransferOwnershipTokenBytes> ownership_token{};
    std::uint32_t link_count{};
    std::uint32_t unsupported_shape_flags{};
    std::uint32_t first_entry{};
    std::uint32_t entry_count{};
};

struct DirectoryTransferManifestEntry {
    std::filesystem::path name;
    std::string namespace_key_utf8;
    SourceSnapshot source_snapshot;
    std::uint32_t link_count{1U};
    std::uint32_t unsupported_shape_flags{};
    std::uint32_t root_index{};
    std::uint32_t parent_index{kDirectoryTransferRootParent};
    std::uint16_t depth{1U};
    DirectoryManifestEntryKind kind{DirectoryManifestEntryKind::regular_file};
};

struct DirectoryTransferManifest {
    std::uint32_t version{kDirectoryTransferManifestVersion};
    std::uint64_t operation_id{};
    FileTransferKind kind{FileTransferKind::copy};
    DirectoryPathSemantics path_semantics{DirectoryPathSemantics::windows_ordinal_nfc};
    std::uint64_t total_bytes{};
    std::vector<DirectoryTransferManifestRoot> roots;
    std::vector<DirectoryTransferManifestEntry> entries;
};

using DirectoryTransferManifestDigest =
    std::array<std::uint8_t, kDirectoryTransferManifestDigestBytes>;
using DirectoryTransferOwnershipToken =
    std::array<std::uint8_t, kDirectoryTransferOwnershipTokenBytes>;

[[nodiscard]] bool
generate_directory_transfer_ownership_token(DirectoryTransferOwnershipToken &token,
                                            std::error_code &error) noexcept;

[[nodiscard]] std::string directory_transfer_namespace_key(const std::filesystem::path &path,
                                                           DirectoryPathSemantics semantics);
[[nodiscard]] std::string
directory_transfer_canonical_object_identity(std::string_view source_revision_utf8,
                                             DirectoryPathSemantics semantics);

[[nodiscard]] std::filesystem::path
directory_transfer_staged_source_path(const std::filesystem::path &source,
                                      std::uint64_t operation_id, std::size_t root_index,
                                      const DirectoryTransferOwnershipToken &ownership_token);
[[nodiscard]] std::filesystem::path
directory_transfer_staging_destination_path(const std::filesystem::path &destination,
                                            std::uint64_t operation_id, std::size_t root_index,
                                            const DirectoryTransferOwnershipToken &ownership_token);
[[nodiscard]] bool valid_directory_transfer_manifest(const DirectoryTransferManifest &manifest,
                                                     std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_directory_transfer_manifest(const DirectoryTransferManifest &manifest);
[[nodiscard]] bool decode_directory_transfer_manifest(std::span<const std::byte> payload,
                                                      DirectoryTransferManifest &manifest,
                                                      std::string &detail_utf8);
[[nodiscard]] DirectoryTransferManifestDecodeStatus
decode_directory_transfer_manifest_status(std::span<const std::byte> payload,
                                          DirectoryTransferManifest &manifest,
                                          std::string &detail_utf8);

} // namespace vove::fileops
