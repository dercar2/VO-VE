#pragma once

#include "vove/fileops/file_operation_service.hpp"
#include "vove/fileops/permanent_delete_transaction.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vove::fileops {

enum class PermanentDeleteRunStatus : std::uint8_t {
    success,
    rolled_back,
    invalid_request,
    unsupported,
    failed,
    recovery_required,
    journal_error,
    stopped,
};

struct PermanentDeleteProgress {
    PermanentDeletePhase phase{PermanentDeletePhase::prepared};
    std::size_t completed{};
    std::size_t total{};
    std::filesystem::path source;
};

struct PermanentDeleteResult {
    PermanentDeleteRunStatus status{PermanentDeleteRunStatus::recovery_required};
    OperationStatus operation_status{OperationStatus::io_error};
    std::size_t completed{};
    std::size_t total{};
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == PermanentDeleteRunStatus::success;
    }
};

struct PermanentDeleteCoordinatorOptions {
    FileOperationServiceOptions file_operations;
    std::filesystem::path journal_path;
};

class PermanentDeleteCoordinator final {
  public:
    using Progress = std::function<void(PermanentDeleteProgress)>;
    using Completion = std::function<void(PermanentDeleteResult)>;
    struct State;

    explicit PermanentDeleteCoordinator(PermanentDeleteCoordinatorOptions options);
    ~PermanentDeleteCoordinator();

    PermanentDeleteCoordinator(const PermanentDeleteCoordinator &) = delete;
    PermanentDeleteCoordinator &operator=(const PermanentDeleteCoordinator &) = delete;

    [[nodiscard]] bool start(std::vector<PermanentDeleteSource> sources, Progress progress,
                             Completion completion);
    [[nodiscard]] bool resume(Progress progress, Completion completion);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] bool recovery_pending() const noexcept;
    [[nodiscard]] bool owns_recovery() const noexcept;
    void stop() noexcept;

  private:
    std::shared_ptr<State> state_;
};

} // namespace vove::fileops
