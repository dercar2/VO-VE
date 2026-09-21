#pragma once

#include "vove/fileops/file_operation_service.hpp"
#include "vove/fileops/trash_transaction.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace vove::fileops {

inline constexpr std::uintmax_t kTrashCriticalFreeSpaceBytes = std::uintmax_t{64} * 1024U * 1024U;
inline constexpr std::uintmax_t kDefaultTrashMaximumBytes =
    std::uintmax_t{50} * 1024U * 1024U * 1024U;

namespace detail {
enum class TrashFreeSpaceState : std::uint8_t {
    available,
    insufficient,
    unavailable,
};

[[nodiscard]] TrashFreeSpaceState classify_trash_free_space(std::uintmax_t available,
                                                            const std::error_code &error) noexcept;
} // namespace detail

enum class TrashRunStatus : std::uint8_t {
    success,
    rolled_back,
    invalid_request,
    unsupported,
    storage_unavailable,
    insufficient_space,
    recovery_required,
    journal_error,
    stopped,
};

struct TrashProgress {
    TrashPhase phase{TrashPhase::prepared};
    std::size_t completed{};
    std::size_t total{};
    std::filesystem::path source;
};

struct TrashResult {
    TrashRunStatus status{TrashRunStatus::recovery_required};
    OperationStatus operation_status{OperationStatus::io_error};
    std::size_t completed{};
    std::size_t total{};
    std::filesystem::path manifest_path;
    std::vector<std::filesystem::path> stored_paths;
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == TrashRunStatus::success;
    }
};

struct TrashCoordinatorOptions {
    FileOperationServiceOptions file_operations;
    std::filesystem::path journal_path;
    std::filesystem::path manifest_directory;
    std::uintmax_t maximum_bytes{kDefaultTrashMaximumBytes};
};

[[nodiscard]] std::filesystem::path
trash_recovery_root_for_source(const std::filesystem::path &source,
                               const std::filesystem::path &manifest_directory,
                               std::string &detail_utf8);

struct TrashRecoveryRootDiscovery {
    std::vector<std::filesystem::path> roots;
    std::size_t discovery_failures{};
};

// Returns every local fixed-volume recovery root owned by the current user, including volumes
// mounted only into directories. Network paths are deliberately excluded.
[[nodiscard]] TrashRecoveryRootDiscovery
trash_known_recovery_roots(const std::filesystem::path &manifest_directory);

class TrashCoordinator final {
  public:
    using Progress = std::function<void(TrashProgress)>;
    using Completion = std::function<void(TrashResult)>;
    struct State;

    explicit TrashCoordinator(TrashCoordinatorOptions options);
    ~TrashCoordinator();

    TrashCoordinator(const TrashCoordinator &) = delete;
    TrashCoordinator &operator=(const TrashCoordinator &) = delete;

    [[nodiscard]] bool start(std::vector<TrashSource> sources, Progress progress,
                             Completion completion);
    [[nodiscard]] bool restore(std::filesystem::path manifest_path, Progress progress,
                               Completion completion);
    [[nodiscard]] bool restore_to(std::filesystem::path manifest_path,
                                  std::filesystem::path destination_directory, Progress progress,
                                  Completion completion);
    [[nodiscard]] bool purge(std::filesystem::path manifest_path, Progress progress,
                             Completion completion);
    [[nodiscard]] bool resume(Progress progress, Completion completion);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] bool recovery_pending() const noexcept;
    [[nodiscard]] bool owns_recovery() const noexcept;
    void set_maximum_bytes(std::uintmax_t maximum_bytes) noexcept;
    void stop() noexcept;

  private:
    std::shared_ptr<State> state_;
};

} // namespace vove::fileops
