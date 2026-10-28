#pragma once

#include "directory_list_model.hpp"
#include "drag_views.hpp"
#include "status_presenter.hpp"
#include "vove/fileops/file_transfer_coordinator.hpp"

#include <QObject>
#include <QPointer>
#include <functional>
#include <optional>

class QMimeData;

namespace vove::ui {

[[nodiscard]] bool trusted_internal_drop_source(const QObject *source, const QObject *view,
                                                const QObject *viewport) noexcept;

// Lexical parsing only: never stats a file or visits an SMB source.
[[nodiscard]] std::optional<QStringList> local_file_paths(const QMimeData *mime);

struct DropViewPort {
    QPointer<QListView> catalog{};
    QPointer<QTreeView> tree{};
    QPointer<FavoritesView> favorites{};
    QPointer<FavoritesView> trash_favorite{};
    QPointer<QWidget> favorites_header{};
    QPointer<QWidget> preview_panel{};
    QPointer<QWidget> preview_canvas{};
};
struct DropState {
    QString current_path;
    bool trash_view{};
    bool global_search{};
    bool recursive_view{};
};
struct DropCallbacks {
    std::function<DropState()> state{};
    std::function<bool()> can_transfer{};
    std::function<bool()> can_validate_external{};
    std::function<bool()> recovery_pending{};
    std::function<void(QStringList)> pin_requested{};
    std::function<bool(QStringList)> begin_external_directory_validation{};
    // Final argument carries authenticated global-result revisions for FileCommands.
    std::function<bool(QStringList, QString, fileops::FileTransferKind, bool,
                       QList<core::DirectoryEntry>)> begin_external_transfer_validation{};
    std::function<bool(QList<core::DirectoryEntry>, QString, fileops::FileTransferKind)> transfer_requested{};
    std::function<bool(QList<core::DirectoryEntry>)> trash_requested{};
    std::function<void(std::optional<DragStatusMessage>)> drag_status_changed{};
    std::function<void(QStringList)> open_external_paths{};
};

// Wiring: remove only DnD clauses of MainWindow::eventFilter and its cursor/status,
// externalDragMime_/Paths_ state. Leave resize/navigation/preview/font filters there.
// PendingExternal*Drop, dropProbeSource_, generations/timers belong to FileCommands.
// Install after view construction; all successful transport actions remain CopyAction.
// state reads Catalog; can_transfer/can_validate_external/recovery_pending read FileCommands.
// begin_external_directory_validation maps to begin_external_directory_drop(pin, paths).
// begin_external_transfer_validation maps to begin_external_transfer_drop(paths, destination,
// kind, trash, false, search_entries). Right preview drops call open_external_paths instead:
// queue/open their parent through the existing external-open workflow, even while operations run.
// drag_status_changed maps directly to StatusPresenter::setDragStatus; Catalog drag_active
// reads active(). Remove root dragStatus_/dragStatusActive authority and formatting callbacks.
class DropRouter final : public QObject {
  public:
    DropRouter(DropViewPort views, DirectoryListModel &model, DropCallbacks callbacks,
               QObject *parent = nullptr);
    ~DropRouter() override;
    void clear_feedback();
    [[nodiscard]] bool active() const noexcept { return feedbackActive_; }
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    [[nodiscard]] static QString tr(const char *text);
    void set_pin_highlight(bool active);
    void set_favorite_target(FavoritesView *view, QListWidgetItem *item);
    void set_move_cursor(bool move);
    void set_status(bool transfer, bool move, int count, const QString &message = {});
    DropViewPort views_;
    DirectoryListModel &model_;
    DropCallbacks callbacks_;
    QList<QPointer<QObject>> watched_;
    QPointer<QMimeData> externalMime_;
    QStringList externalPaths_;
    bool feedbackActive_{};
    bool cursorOverride_{};
};

} // namespace vove::ui
