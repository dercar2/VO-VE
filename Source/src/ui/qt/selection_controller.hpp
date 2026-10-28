#pragma once

#include "directory_list_model.hpp"
#include "external_application_registry.hpp"
#include "external_open_coordinator.hpp"
#include "operation_availability.hpp"
#include "selection_command.hpp"
#include "vove/fileops/file_transfer_coordinator.hpp"

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QAction;
class QListView;
class QMenu;
class QMimeData;

namespace vove::ui {

struct SelectionActions {
    QAction *create_folder{};
    QAction *rename{};
    QAction *remove{};
    QAction *copy_names{};
    QAction *copy_paths{};
    QAction *copy_objects{};
    QAction *paste_objects{};
    QAction *cancel_preview{};
};

struct SelectionCallbacks {
    std::function<OperationAvailability()> availability{};
    std::function<void(QString)> status{};
    std::function<void()> paste_requested{};
    std::function<bool()> can_paste_objects{};
    std::function<void(QList<core::DirectoryEntry>, fileops::FileTransferKind)> transfer_requested{};
    std::function<void(QString)> open_requested_path{};
    std::function<void(core::DirectoryEntry)> show_in_folder_requested{};
    std::function<bool(const core::DirectoryEntry &)> can_force_preview{};
    std::function<void(qulonglong)> force_preview_requested{};
    std::function<bool()> extended_preview_active{};
    std::function<QString(const QString &)> default_application_name{};
    std::function<bool()> native_open_allowed{};
    std::function<void()> external_applications_changed{};
    std::function<void(QPoint)> trash_menu_requested{};
    std::function<void()> refresh_command_actions{};
    std::function<void(QString)> selection_summary_changed{};
};

// Wiring: replace MainWindow's selected_entries/snapshot/count/copy/clipboard/menu methods
// and their connections with this owner. Existing QAction instances retain fonts/shortcuts.
// FileCommands owns paste validation and transfer execution; no MainWindow access is needed.
// Remove externalApplications_/externalOpenCoordinator_ and native-open/icon helpers from
// MainWindow. This owner connects catalog doubleClicked; folders emit open_requested_path.
// FileCommands supplies availability/can_paste_objects/native_open_allowed. Settings owns
// persistence: read external_applications(), restore its loaded snapshot list directly.
// set_registry_data is a quiet restore; register/remove emit external_applications_changed.
// selection_summary_changed wires directly to StatusPresenter::setSelectionSummary.
class SelectionController final : public QObject {
  public:
    SelectionController(QListView &view, DirectoryListModel &model, SelectionActions actions,
                        SelectionCallbacks callbacks, QObject *parent = nullptr,
                        QString open_helper_override = {});
    ~SelectionController() override;
    [[nodiscard]] const QList<ExternalApplication> &external_applications() const noexcept;
    void set_registry_data(ExternalApplicationRegistry applications);
    void set_registry_data(QList<ExternalApplication> applications);
    [[nodiscard]] AddExternalApplicationResult register_external_application(const ExternalApplication &application);
    [[nodiscard]] bool remove_external_application(const QString &path);
    void open_entry(const core::DirectoryEntry &entry);
    void open_file_in_default_application(const QString &path);
    void open_file_with_application(const ExternalApplication &application, const QString &path);
    void reveal_file_in_file_manager(const QString &path);
    void prompt_add_external_application(const QString &file_path);
    [[nodiscard]] QList<core::DirectoryEntry> selected_entries_in_view_order() const;
    [[nodiscard]] SelectionSnapshot selection_snapshot() const;
    [[nodiscard]] bool invoke_selection_command(const SelectionCommand &command) const;
    [[nodiscard]] QString selection_status_text() const;
    [[nodiscard]] std::unique_ptr<QMimeData> file_mime_data() const;
    void copy_selected_names();
    void copy_selected_paths();
    void copy_selected_objects();
    void update_clipboard_actions();
    [[nodiscard]] std::unique_ptr<QMenu> create_context_menu(const QPoint &position);
    void show_catalog_context_menu(const QPoint &position);

  private:
    [[nodiscard]] static QString tr(const char *text);
    [[nodiscard]] OperationAvailability availability() const;
    QPointer<QListView> view_;
    DirectoryListModel &model_;
    SelectionActions actions_;
    SelectionCallbacks callbacks_;
    ExternalApplicationRegistry externalApplications_;
    ExternalOpenCoordinator externalOpenCoordinator_;
};

} // namespace vove::ui
