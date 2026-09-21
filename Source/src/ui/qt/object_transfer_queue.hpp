#pragma once

#include "vove/core/directory_entry.hpp"
#include "vove/fileops/file_transfer_coordinator.hpp"
#include "vove/platform/directory_service.hpp"

#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

namespace vove::ui {

struct ObjectTransferResult {
    bool success{};
    bool cancelled{};
    bool recovery{};
    fileops::OperationStatus operation_status{fileops::OperationStatus::success};
    std::size_t completed{};
    std::size_t total{};
    QString detail;
    QStringList moved_sources;
    QStringList published_paths;
};

class ObjectTransferQueue final {
  public:
    using Progress = std::function<void(fileops::FileTransferProgressUpdate)>;
    using Conflict = std::function<void(fileops::FileTransferConflict, bool overwrite_allowed,
                                        bool copy_allowed)>;
    using Completion = std::function<void(ObjectTransferResult)>;
    ObjectTransferQueue(fileops::FileTransferCoordinatorOptions options,
                        platform::DirectoryServiceOptions catalog_options = {});
    ~ObjectTransferQueue();
    // Selection binds object identity. Admission captures the current exact revision;
    // conflict handling, execution and recovery must not refresh that revision.
    bool start(fileops::FileTransferKind kind, std::vector<core::DirectoryEntry> sources,
               std::filesystem::path destination, Progress progress, Conflict conflict,
               Completion completion);
    bool resume(Progress progress, Conflict conflict, Completion completion);
    void resolve_conflict(fileops::FileTransferConflictDecision decision);
    // Drains accepted callbacks. From a queue callback, requests stop without waiting on itself.
    void stop();
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace vove::ui
