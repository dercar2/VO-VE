#pragma once

#include "vove/fileops/file_operation.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace vove::fileops {

struct FileOperationServiceOptions {
    std::filesystem::path helper_path;
    std::chrono::milliseconds timeout{std::chrono::seconds(15)};
    // Deterministic diagnostics can observe the safe slot-before-delivery boundary.
    std::function<void()> after_operation_slot_released;
};

class FileOperationService final {
  public:
    using Completion = std::function<void(OperationResult)>;
    struct State;

    explicit FileOperationService(FileOperationServiceOptions options = {});
    ~FileOperationService();

    FileOperationService(const FileOperationService &) = delete;
    FileOperationService &operator=(const FileOperationService &) = delete;

    [[nodiscard]] bool submit_rename(RenameRequest request, Completion completion);
    [[nodiscard]] bool submit_reconciliation(RenameRequest request, Completion completion);
    [[nodiscard]] bool submit_delete(DeleteRequest request, Completion completion);
    [[nodiscard]] bool submit_delete_reconciliation(DeleteRequest request, Completion completion);
    [[nodiscard]] bool submit_create_directory(CreateDirectoryRequest request,
                                               Completion completion);
    [[nodiscard]] bool submit_create_directory_reconciliation(CreateDirectoryRequest request,
                                                              Completion completion);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    void retain_accepted_completions_during_stop() noexcept;
    void stop() noexcept;

  private:
    [[nodiscard]] bool submit(RenameRequest request, Completion completion, RenameAction action);
    [[nodiscard]] bool submit(DeleteRequest request, Completion completion, DeleteAction action);
    [[nodiscard]] bool submit(CreateDirectoryRequest request, Completion completion,
                              CreateDirectoryAction action);

    std::shared_ptr<State> state_;
};

} // namespace vove::fileops
