#pragma once

#include "vove/fileops/basic_directory_transfer_protocol.hpp"
#include "vove/fileops/file_transfer_protocol.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace vove::fileops {

struct FileTransferServiceOptions {
    std::filesystem::path helper_path;
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(15)};
    std::function<void()> after_operation_slot_released;
};

enum class FileTransferAuditCommitOutcome : std::uint8_t {
    accepted,
    rejected_no_commit,
    recovery_required,
};

class FileTransferService final {
  public:
    using ReservationCommit =
        std::function<bool(const FileTransferReservation &, std::string &detail_utf8)>;
    using AuditCommit = std::function<FileTransferAuditCommitOutcome(
        const FileTransferStreamResult &, std::string &detail_utf8)>;
    using Progress = std::function<void(const FileTransferProgress &)>;
    using Completion = std::function<void(FileTransferStreamResult)>;
    using DirectoryProgress = std::function<void(const BasicDirectoryTransferProgress &)>;
    using DirectoryCompletion = std::function<void(BasicDirectoryTransferResult)>;
    struct State;

    explicit FileTransferService(FileTransferServiceOptions options = {});
    ~FileTransferService();

    FileTransferService(const FileTransferService &) = delete;
    FileTransferService &operator=(const FileTransferService &) = delete;

    [[nodiscard]] bool submit(FileTransferStreamRequest request,
                              ReservationCommit commit_reservation, Progress progress,
                              Completion completion);
    [[nodiscard]] bool submit_audit(FileTransferStreamRequest request, AuditCommit commit_audit,
                                    Progress progress, Completion completion);
    [[nodiscard]] bool submit_directory(BasicDirectoryTransferStreamRequest request,
                                        DirectoryProgress progress, DirectoryCompletion completion);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    void retain_accepted_completions_during_stop() noexcept;
    void stop() noexcept;

  private:
    [[nodiscard]] bool submit_impl(FileTransferStreamRequest request,
                                   ReservationCommit commit_reservation, AuditCommit commit_audit,
                                   Progress progress, Completion completion);

    std::shared_ptr<State> state_;
};

} // namespace vove::fileops
