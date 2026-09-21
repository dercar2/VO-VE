#pragma once

#include "vove/fileops/directory_transfer_progress.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kDirectoryRootStagingVersion = 1U;
inline constexpr std::uint32_t kDirectoryRootOwnershipMarkerVersion = 1U;
inline constexpr std::size_t kDirectoryRootOwnershipMarkerBytes = 112U;

// Durable authority comes from the immutable manifest. The helper may create only this tokenized
// hidden root and may never choose or substitute its own token or path.
struct DirectoryRootStagingBinding {
    std::uint32_t version{kDirectoryRootStagingVersion};
    std::uint64_t parent_operation_id{};
    DirectoryTransferManifestDigest parent_manifest_digest{};
    std::uint32_t root_index{};
    DirectoryPathSemantics path_semantics{DirectoryPathSemantics::windows_ordinal_nfc};
    std::filesystem::path staging_root;
    std::string destination_parent_identity_utf8;
    DirectoryTransferOwnershipToken ownership_token{};
};

// The mutable revision is only fresh precondition evidence. It is deliberately not a persistent
// ownership anchor; the stable parent identity in the binding is authoritative across recovery.
struct DirectoryRootStagingRequest {
    DirectoryRootStagingBinding binding;
    std::string destination_parent_revision_utf8;
};

struct DirectoryRootOwnershipMarker {
    std::uint32_t version{kDirectoryRootOwnershipMarkerVersion};
    std::uint64_t parent_operation_id{};
    DirectoryTransferManifestDigest parent_manifest_digest{};
    std::uint32_t root_index{};
    DirectoryTransferOwnershipToken ownership_token{};
};

[[nodiscard]] bool valid_directory_root_staging_binding(const DirectoryRootStagingBinding &binding,
                                                        std::string &detail_utf8);
[[nodiscard]] DirectoryRootStagingRequest
prepare_directory_root_staging_request(const DirectoryTransferProgressPlan &plan,
                                       const DirectoryTransferProgressState &state);
[[nodiscard]] bool directory_root_staging_request_matches(
    const DirectoryTransferProgressPlan &plan, const DirectoryTransferProgressState &state,
    const DirectoryRootStagingRequest &request, std::string &detail_utf8);

[[nodiscard]] DirectoryRootOwnershipMarker
directory_root_ownership_marker_for(const DirectoryRootStagingBinding &binding);
[[nodiscard]] bool valid_directory_root_ownership_marker(const DirectoryRootOwnershipMarker &marker,
                                                         std::string &detail_utf8);
[[nodiscard]] bool
directory_root_ownership_marker_matches(const DirectoryRootOwnershipMarker &marker,
                                        const DirectoryRootStagingBinding &binding,
                                        std::string &detail_utf8);

[[nodiscard]] std::vector<std::byte>
encode_directory_root_ownership_marker(const DirectoryRootOwnershipMarker &marker);
[[nodiscard]] bool decode_directory_root_ownership_marker(std::span<const std::byte> payload,
                                                          DirectoryRootOwnershipMarker &marker,
                                                          std::string &detail_utf8);

} // namespace vove::fileops
