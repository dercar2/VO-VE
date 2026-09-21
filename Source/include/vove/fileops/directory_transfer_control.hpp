#pragma once

#include "vove/fileops/directory_transfer_manifest_store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kDirectoryTransferControlVersion = 1U;
inline constexpr std::size_t kDirectoryTransferControlEncodedBytes = 64U;

// Version 1 is intentionally a prepared-only binding. Progress fields are introduced only with
// the bounded ledger format and its monotonic transition contract.
struct DirectoryTransferControlRecord {
    std::uint32_t version{kDirectoryTransferControlVersion};
    std::uint64_t operation_id{};
    FileTransferKind kind{FileTransferKind::copy};
    std::uint32_t root_count{};
    std::uint64_t manifest_encoded_size{};
    DirectoryTransferManifestDigest manifest_digest{};
};

enum class DirectoryTransferBootstrapStatus : std::uint8_t {
    success,
    not_found,
    recovery_required,
    invalid_argument,
    incompatible_version,
    corrupt,
    payload_mismatch,
    payload_too_large,
    busy,
    io_error,
};

struct DirectoryTransferBootstrapResult {
    DirectoryTransferBootstrapStatus status{DirectoryTransferBootstrapStatus::io_error};
    std::error_code error;
    std::string detail_utf8;
    DirectoryTransferControlRecord control;
    DirectoryTransferManifestStoreReadResult stored_manifest;
    std::filesystem::path manifest_path;

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectoryTransferBootstrapStatus::success;
    }
};

[[nodiscard]] std::filesystem::path
directory_transfer_manifest_sidecar_path(const std::filesystem::path &current_journal_path,
                                         std::uint64_t operation_id);
[[nodiscard]] std::filesystem::path
directory_transfer_progress_ledger_path(const std::filesystem::path &current_journal_path,
                                        std::uint64_t operation_id);

[[nodiscard]] bool valid_directory_transfer_control(const DirectoryTransferControlRecord &record,
                                                    std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_directory_transfer_control(const DirectoryTransferControlRecord &record);
[[nodiscard]] bool decode_directory_transfer_control(std::span<const std::byte> payload,
                                                     DirectoryTransferControlRecord &record,
                                                     std::string &detail_utf8);

[[nodiscard]] DirectoryTransferBootstrapResult
publish_prepared_directory_transfer(const std::filesystem::path &current_journal_path,
                                    const DirectoryTransferManifest &manifest);
[[nodiscard]] DirectoryTransferBootstrapResult
load_prepared_directory_transfer(const std::filesystem::path &current_journal_path);

} // namespace vove::fileops
