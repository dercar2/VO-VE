#pragma once

#include "selection_command.hpp"
#include "window_settings.hpp"
#include "vove/fileops/file_operation_service.hpp"
#include "vove/fileops/file_transfer_service.hpp"

#include <QMainWindow>
#include <QStringList>

#include <memory>

class QCloseEvent;

namespace vove::ui {
class CatalogController;
class DirectoryItemDelegate;
class DropRouter;
class FileCommandController;
class NavigationController;
class PlacesController;
class PreviewController;
class SelectionController;
class StatusPresenter;
class TrashController;
class WindowShell;
struct FileCommandActionState;
struct ViewerStyle;

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(QString initial_path = {}, QWidget *parent = nullptr,
                        QString preview_helper_override = {},
                        fileops::FileOperationServiceOptions file_operation_options = {},
                        const QString &batch_journal_override = {},
                        fileops::FileTransferServiceOptions file_transfer_options = {},
                        QString global_search_helper_override = {});
    ~MainWindow() override;

    void open_external_path(const QString &path, bool select_file = false);
    void open_external_paths(const QStringList &paths);

    // UI-thread boundary for future read-only commands, including print composition.
    [[nodiscard]] SelectionSnapshot selection_snapshot() const;
    [[nodiscard]] bool invoke_selection_command(const SelectionCommand &command) const;

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void connect_commands();
    void apply_style(const ViewerStyle &style);
    void retranslate_modules();
    void publish_catalog_status();
    void publish_operation_activity();
    void apply_command_actions(const FileCommandActionState &state);
    void schedule_settings_save();
    [[nodiscard]] WindowSettingsSnapshot capture_settings() const;
    void restore_settings();
    void show_settings_dialog();
    void show_update_dialog();
    void show_components_dialog();
    void open_external_items_in_parent(const QStringList &paths);
    void refresh_after_rename(const QStringList &paths);

    WindowSettingsSnapshot preferences_;
    std::unique_ptr<WindowSettings> settings_;
    std::unique_ptr<WindowShell> shell_;
    std::unique_ptr<CatalogController> catalog_;
    std::unique_ptr<DirectoryItemDelegate> delegate_;
    std::unique_ptr<StatusPresenter> status_;
    std::unique_ptr<PreviewController> preview_;
    std::unique_ptr<NavigationController> navigation_;
    std::unique_ptr<FileCommandController> commands_;
    std::unique_ptr<SelectionController> selection_;
    std::unique_ptr<PlacesController> places_;
    std::unique_ptr<TrashController> trash_;
    std::unique_ptr<DropRouter> drops_;
    bool restoringSettings_{};
    bool shuttingDown_{};
};

} // namespace vove::ui
