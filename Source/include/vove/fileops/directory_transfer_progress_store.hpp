#pragma once

#include "vove/fileops/directory_transfer_progress.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace vove::fileops {

enum class DirectoryTransferProgressStoreStatus : std::uint8_t {
    success,
    not_found,
    invalid_argument,
    incompatible_version,
    corrupt,
    payload_mismatch,
    payload_too_large,
    busy,
    unsupported_filesystem,
    recovery_required,
    io_error,
};

struct DirectoryTransferProgressStoreResult {
    DirectoryTransferProgressStoreStatus status{DirectoryTransferProgressStoreStatus::io_error};
    std::error_code error;
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectoryTransferProgressStoreStatus::success;
    }
};

struct DirectoryTransferProgressAppendResult : DirectoryTransferProgressStoreResult {
    DirectoryTransferProgressRecord record;
};

class DirectoryTransferProgressSession;

struct DirectoryTransferProgressOpenResult : DirectoryTransferProgressStoreResult {
    std::unique_ptr<DirectoryTransferProgressSession> session;
    bool created{};
    bool repaired_tail{};
};

class DirectoryTransferProgressSession final {
  public:
    ~DirectoryTransferProgressSession();

    DirectoryTransferProgressSession(const DirectoryTransferProgressSession &) = delete;
    DirectoryTransferProgressSession &operator=(const DirectoryTransferProgressSession &) = delete;
    DirectoryTransferProgressSession(DirectoryTransferProgressSession &&) noexcept;
    DirectoryTransferProgressSession &operator=(DirectoryTransferProgressSession &&) noexcept;

    [[nodiscard]] static DirectoryTransferProgressOpenResult
    open_or_create(const std::filesystem::path &current_journal_path);

    [[nodiscard]] DirectoryTransferProgressAppendResult
    append(const DirectoryTransferRootStagingEvidence &evidence);
    [[nodiscard]] DirectoryTransferProgressAppendResult
    append(const DirectoryTransferEntryEvidence &evidence);
    [[nodiscard]] DirectoryTransferProgressAppendResult
    append(const DirectoryTransferRootPublicationEvidence &evidence);

    [[nodiscard]] const DirectoryTransferProgressPlan &plan() const;
    [[nodiscard]] const DirectoryTransferProgressState &state() const;
    [[nodiscard]] const std::filesystem::path &ledger_path() const;
    [[nodiscard]] bool poisoned() const noexcept;

    void close() noexcept;

  private:
    struct Impl;
    explicit DirectoryTransferProgressSession(std::unique_ptr<Impl> impl) noexcept;

    template <typename Evidence>
    [[nodiscard]] DirectoryTransferProgressAppendResult append_evidence(const Evidence &evidence);

    std::unique_ptr<Impl> impl_;
};

} // namespace vove::fileops
