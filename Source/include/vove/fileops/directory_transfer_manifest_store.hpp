#pragma once

#include "vove/fileops/current_operation_lease.hpp"
#include "vove/fileops/directory_transfer_manifest.hpp"
#include "vove/fileops/durable_journal.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace vove::fileops {

enum class DirectoryTransferManifestStoreStatus : std::uint8_t {
    success,
    not_found,
    invalid_manifest,
    incompatible_version,
    corrupt,
    payload_mismatch,
    payload_too_large,
    io_error,
};

struct DirectoryTransferManifestStoreResult {
    DirectoryTransferManifestStoreStatus status{DirectoryTransferManifestStoreStatus::io_error};
    std::error_code error;
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectoryTransferManifestStoreStatus::success;
    }
};

struct DirectoryTransferManifestStoreReadResult : DirectoryTransferManifestStoreResult {
    DurableJournalReadSource source{DurableJournalReadSource::none};
    DirectoryTransferManifest manifest;
    DirectoryTransferManifestDigest digest{};
    std::size_t encoded_size{};
};

class DirectoryTransferManifestStore final {
  public:
    explicit DirectoryTransferManifestStore(std::filesystem::path path);

    [[nodiscard]] DirectoryTransferManifestStoreResult
    persist_immutable(const DirectoryTransferManifest &manifest) const;
    [[nodiscard]] DirectoryTransferManifestStoreResult
    persist_immutable_locked(const DirectoryTransferManifest &manifest,
                             const CurrentOperationLease &lease) const;
    [[nodiscard]] DirectoryTransferManifestStoreReadResult read() const;
    [[nodiscard]] DirectoryTransferManifestStoreReadResult
    read_locked(const CurrentOperationLease &lease) const;
    [[nodiscard]] DirectoryTransferManifestStoreResult
    remove_if_matches(const DirectoryTransferManifest &manifest) const;
    [[nodiscard]] DirectoryTransferManifestStoreResult
    remove_if_matches_locked(const DirectoryTransferManifest &manifest,
                             const CurrentOperationLease &lease) const;

    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] std::size_t maximum_payload_bytes() const noexcept;

  private:
    DurableJournalStore durable_;
};

} // namespace vove::fileops
