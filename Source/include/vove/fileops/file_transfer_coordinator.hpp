#pragma once

#include "vove/fileops/file_operation_service.hpp"
#include "vove/fileops/directory_leaf_transfer.hpp"
#include "vove/fileops/file_transfer_service.hpp"
#include "vove/fileops/file_transfer_transaction.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vove::fileops {

enum class FileTransferRunStatus : std::uint8_t {
    success,
    partial_failure,
    invalid_request,
    unsupported,
    recovery_required,
    journal_error,
    stopped,
    cancelled,
};

struct FileTransferConflictItem {
    std::filesystem::path source;
    std::filesystem::path destination;
};

struct FileTransferConflict {
    // The first item remains available directly for source compatibility with single-item clients.
    std::filesystem::path source;
    std::filesystem::path destination;
    std::vector<FileTransferConflictItem> items;
};

enum class FileTransferConflictDecision : std::uint8_t {
    create_copy,
    overwrite,
    cancel,
};

struct FileTransferProgressUpdate {
    FileTransferPhase phase{FileTransferPhase::prepared};
    FileTransferStep step{FileTransferStep::none};
    std::size_t completed{};
    std::size_t total{};
    std::size_t item_index{};
    std::uint64_t bytes_written{};
    std::uint64_t bytes_total{};
    std::filesystem::path source;
    std::filesystem::path destination;
};

struct DirectoryLeafCompletionProof {
    // Point-in-time evidence only. The parent must re-open the bound staging root and verify this
    // digest immediately before appending its durable entry_materialized acknowledgement.
    DirectoryLeafTransferBinding binding;
    std::filesystem::path destination;
    SourceSnapshot source_snapshot;
    SourceSnapshot destination_snapshot;
    std::array<std::uint8_t, kFileTransferDigestBytes> content_sha256{};
    std::uint64_t content_bytes{};
};

struct FileTransferResult {
    FileTransferRunStatus status{FileTransferRunStatus::recovery_required};
    OperationStatus operation_status{OperationStatus::io_error};
    OperationEvidence evidence{OperationEvidence::none};
    std::size_t completed{};
    std::size_t total{};
    std::optional<std::size_t> failed_index;
    std::vector<std::filesystem::path> destinations;
    std::optional<DirectoryLeafCompletionProof> directory_leaf_completion_proof;
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == FileTransferRunStatus::success;
    }
};

struct FileTransferCoordinatorOptions {
    FileOperationServiceOptions file_operations;
    FileTransferServiceOptions file_transfers;
    std::filesystem::path journal_path;
    // A parent queue commits the terminal child outcome before its journal can disappear.
    std::function<bool(const FileTransferTransaction &, std::string &)> before_journal_retirement{};
};

class FileTransferCoordinator final {
  public:
    using Progress = std::function<void(FileTransferProgressUpdate)>;
    using Completion = std::function<void(FileTransferResult)>;
    using Conflict = std::function<void(FileTransferConflict)>;
    using DirectoryLeafRecoveryAdmission =
        std::function<bool(const DirectoryLeafTransferRequest &, std::string &)>;
    struct State;

    explicit FileTransferCoordinator(FileTransferCoordinatorOptions options);
    ~FileTransferCoordinator();

    FileTransferCoordinator(const FileTransferCoordinator &) = delete;
    FileTransferCoordinator &operator=(const FileTransferCoordinator &) = delete;

    [[nodiscard]] bool start(FileTransferKind kind, std::vector<FileTransferSource> sources,
                             Progress progress, Completion completion, Conflict conflict = {});
    [[nodiscard]] bool resume(Progress progress, Completion completion, Conflict conflict = {});
    // Conflict runs on the worker thread. Resolve from any thread; stop interrupts the wait.
    void resolve_conflict(FileTransferConflictDecision decision) noexcept;
    void resolve_conflict(bool overwrite) noexcept;
    [[nodiscard]] bool start_directory_leaf_copy(DirectoryLeafTransferRequest request,
                                                 Progress progress, Completion completion);
    [[nodiscard]] bool resume_directory_leaf_copy(DirectoryLeafTransferBinding expected_binding,
                                                  DirectoryLeafRecoveryAdmission admit_recovery,
                                                  Progress progress, Completion completion);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] bool recovery_pending() const noexcept;
    [[nodiscard]] bool owns_recovery() const noexcept;
    [[nodiscard]] bool owns_directory_leaf_recovery() const noexcept;
    void stop() noexcept;

  private:
    std::shared_ptr<State> state_;
};

} // namespace vove::fileops
