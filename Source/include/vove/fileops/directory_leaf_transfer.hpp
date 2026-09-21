#pragma once

#include "vove/fileops/directory_transfer_manifest.hpp"
#include "vove/fileops/directory_transfer_progress.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kDirectoryLeafTransferVersion = 2U;

struct DirectoryLeafTransferBinding {
    std::uint32_t version{kDirectoryLeafTransferVersion};
    std::uint64_t parent_operation_id{};
    DirectoryTransferManifestDigest parent_manifest_digest{};
    std::uint32_t root_index{};
    std::uint32_t entry_index{};
    DirectoryPathSemantics path_semantics{DirectoryPathSemantics::windows_ordinal_nfc};
    std::filesystem::path staging_root;
    DirectoryTransferProgressOwnershipToken staging_ownership_token{};
    DirectoryTransferProgressDigest staging_root_identity_sha256{};
    std::string staging_root_identity_utf8;
};

struct DirectoryLeafTransferRequest {
    DirectoryLeafTransferBinding binding;
    FileTransferSource file;
};

struct DirectoryLeafTransferJournal {
    DirectoryLeafTransferBinding binding;
    FileTransferSource original_file;
    FileTransferTransaction transfer;
};

[[nodiscard]] bool
valid_directory_leaf_transfer_request(const DirectoryLeafTransferRequest &request,
                                      std::string &detail_utf8);
[[nodiscard]] bool
valid_directory_leaf_transfer_binding(const DirectoryLeafTransferBinding &binding,
                                      std::string &detail_utf8);
[[nodiscard]] bool
valid_directory_leaf_transfer_journal(const DirectoryLeafTransferJournal &journal,
                                      std::string &detail_utf8);
[[nodiscard]] DirectoryLeafTransferJournal
prepare_directory_leaf_transfer(const DirectoryLeafTransferRequest &request,
                                std::uint64_t child_operation_id);
[[nodiscard]] bool directory_leaf_transfer_matches(const DirectoryLeafTransferJournal &journal,
                                                   const DirectoryLeafTransferRequest &expected,
                                                   std::string &detail_utf8);
[[nodiscard]] bool
directory_leaf_transfer_binding_matches(const DirectoryLeafTransferBinding &actual,
                                        const DirectoryLeafTransferBinding &expected,
                                        std::string &detail_utf8);
[[nodiscard]] bool
directory_leaf_transfer_request_matches(const DirectoryLeafTransferRequest &actual,
                                        const DirectoryLeafTransferRequest &expected,
                                        std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_directory_leaf_transfer(const DirectoryLeafTransferJournal &journal);
[[nodiscard]] bool decode_directory_leaf_transfer(std::span<const std::byte> payload,
                                                  DirectoryLeafTransferJournal &journal,
                                                  std::string &detail_utf8);

} // namespace vove::fileops
