#pragma once

#include "vove/fileops/batch_rename.hpp"
#include "vove/fileops/batch_rename_transaction.hpp"
#include "vove/fileops/file_operation_service.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vove::fileops {

enum class BatchRunStatus : std::uint8_t {
    success,
    rolled_back,
    invalid_request,
    busy,
    unsupported,
    journal_error,
    recovery_required,
    stopped,
};

struct BatchRenameProgress {
    BatchTransactionPhase phase{BatchTransactionPhase::prepared};
    std::size_t completed{};
    std::size_t total{};
    std::filesystem::path source;
    std::filesystem::path destination;
};

struct BatchRenameResult {
    BatchRunStatus status{BatchRunStatus::invalid_request};
    OperationStatus operation_status{OperationStatus::success};
    std::size_t completed{};
    std::size_t total{};
    std::vector<std::filesystem::path> destinations;
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == BatchRunStatus::success;
    }
};

struct BatchRenameCoordinatorOptions {
    FileOperationServiceOptions file_operations;
    std::filesystem::path journal_path;
};

class BatchRenameCoordinator final {
  public:
    using Progress = std::function<void(BatchRenameProgress)>;
    using Completion = std::function<void(BatchRenameResult)>;

    explicit BatchRenameCoordinator(BatchRenameCoordinatorOptions options);
    ~BatchRenameCoordinator();

    BatchRenameCoordinator(const BatchRenameCoordinator &) = delete;
    BatchRenameCoordinator &operator=(const BatchRenameCoordinator &) = delete;

    [[nodiscard]] bool start(BatchRenamePlan plan, std::vector<BatchRenameSource> sources,
                             Progress progress, Completion completion);
    [[nodiscard]] bool resume(Progress progress, Completion completion);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] bool recovery_pending() const noexcept;
    [[nodiscard]] bool owns_recovery() const noexcept;
    void stop() noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace vove::fileops
