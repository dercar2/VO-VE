#include "main_window.hpp"

#include "about_dialog.hpp"
#include "catalog_controller.hpp"
#include "catalog_tool_button.hpp"
#include "components_panel.hpp"
#include "directory_item_delegate.hpp"
#include "drag_views.hpp"
#include "drop_router.hpp"
#include "external_component_detection.hpp"
#include "file_command_controller.hpp"
#include "interface_font.hpp"
#include "main_menu_dialog.hpp"
#include "navigation_button.hpp"
#include "navigation_controller.hpp"
#include "operation_notice.hpp"
#include "page_button.hpp"
#include "places_controller.hpp"
#include "preview_controller.hpp"
#include "preview_modified_label.hpp"
#include "preview_name_label.hpp"
#include "project_dialogs.hpp"
#include "release_configuration.hpp"
#include "selection_controller.hpp"
#include "settings_dialog.hpp"
#include "startup_registration.hpp"
#include "status_presenter.hpp"
#include "toolbar_fields.hpp"
#include "trash_controller.hpp"
#include "ui_text.hpp"
#include "update_dialog.hpp"
#include "window_shell.hpp"
#include "zoom_preview.hpp"
#include "vove/search/global_search.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QLabel>
#include <QSettings>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

#include <utility>

namespace vove::ui {
namespace {
QString application_version() {
    const auto version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? updates::compiled_application_version() : version;
}
} // namespace

MainWindow::MainWindow(QString initial_path, QWidget *parent, QString preview_helper_override,
    fileops::FileOperationServiceOptions file_operation_options, const QString &batch_journal_override,
    fileops::FileTransferServiceOptions file_transfer_options, QString global_search_helper_override)
    : QMainWindow(parent), settings_(std::make_unique<WindowSettings>()) {
    install_main_menu_dialog_frames(this);
    shell_ = std::make_unique<WindowShell>(*this, WindowShellCallbacks{
        .preferences_changed = [this] { schedule_settings_save(); },
        .language_changed = [this](AppLanguage) { retranslate_modules(); },
        .style_changed = [this](const ViewerStyle &style) { apply_style(style); },
    });
    const auto &w = shell_->widgets();
    catalog_ = std::make_unique<CatalogController>(std::move(global_search_helper_override));
    delegate_ = std::make_unique<DirectoryItemDelegate>();
    w.listView->setModel(&catalog_->model());
    w.listView->setItemDelegate(delegate_.get());
    status_ = std::make_unique<StatusPresenter>(*w.status, new OperationNotice(this));
    preview_ = std::make_unique<PreviewController>(nullptr, std::move(preview_helper_override));
    preview_->attachViews({this, &catalog_->model(), w.listView, w.previewCanvas, w.previewName,
        w.previewModified, w.previewFormat, w.previewSupport, w.pageNavigation,
        w.previousPageButton, w.nextPageButton, w.passwordButton, w.pageIndicator,
        w.cancelPreviewAttemptAction}, {
        .request_source_revalidation = [this] {
            if (catalog_ && !shuttingDown_) {
                catalog_->refresh_directory_sources();
                catalog_->resume_polling(120);
            }
        },
        .status_changed = [this] {
            if (status_ && preview_) status_->setSelectedFile(preview_->selectedName(),
                                                             preview_->selectedColorSummary());
        },
    });
    navigation_ = std::make_unique<NavigationController>(catalog_->model(), NavigationViews{
        w.listView, w.pathEdit, w.historyMenu, w.backButton, w.forwardButton, w.upButton,
        w.historyButton, this}, NavigationActions{
        .ordinary_view = [this] { return catalog_->ordinary_view(); },
        .restore_allowed = [this] { return !catalog_->search_active() && !catalog_->trash_active(); },
        .trash_view = [this] { return catalog_->trash_active(); },
        .prepare_directory = [this] { catalog_->prepare_navigation(); },
        .open_directory = [this](const QString &path) { catalog_->open_directory(path); },
        .settings_changed = [this] { schedule_settings_save(); },
        .selection_changed = [this] {
            preview_->selectedEntryChanged(shell_->widgets().listView->currentIndex());
            if (selection_) selection_->update_clipboard_actions();
        },
        .commands_changed = [this] { if (commands_) commands_->update_actions(); },
        .previews_changed = [this] {
            preview_->updatePreviewContext({catalog_->search_active() ? PreviewCatalogMode::global_search :
                catalog_->recursive_active() ? PreviewCatalogMode::recursive : catalog_->trash_active() ?
                PreviewCatalogMode::trash : PreviewCatalogMode::directory, navigation_->restore_pending()});
            preview_->schedule();
        },
        .message = [this](const QString &text) { status_->setOperationMessage(text); },
    });
    FileCommandPorts ports;
    ports.dialog_owner = this;
    ports.selection = [this] { return selection_ ? selection_->selection_snapshot() : SelectionSnapshot{}; };
    ports.catalog = [this] {
        return FileCommandCatalogState{catalog_->path(),
            catalog_->update().state == catalog::CatalogSessionState::ready,
            catalog_->trash_active(), catalog_->search_active(), catalog_->recursive_active(),
            catalog_->search_ready(), shell_->widgets().listView->dragEnabled(),
            catalog_->search_query_current(), catalog_->removed_search_paths(), catalog_->search_generation()};
    };
    ports.entry_names = [this](const QString &parent_path) { return catalog_->model().all_entry_names(parent_path); };
    ports.trash_operation_items = [this](const std::filesystem::path &manifest) {
        return trash_ ? trash_->operation_items(manifest) : std::size_t{};
    };
    ports.begin_preview_generation = [this](bool clear) { preview_->startGeneration(clear); };
    ports.clear_pending_mosaics = [this] { preview_->cancelFolderPreviews(); };
    ports.stop_animation = [this] { preview_->stopSelectedAnimation(); };
    ports.stop_preview_readers = [this] { preview_->pauseReaders(); };
    ports.readers_idle = [this] { return preview_->readersIdle(); };
    ports.suspend_preview_dispatch = [this] { preview_->suspendDispatch(); };
    ports.resume_preview_dispatch = [this](int delay) { preview_->resume(delay); };
    ports.mark_preview_unverified = [this](const QString &path) { preview_->markPreviewUnverified(path); };
    ports.mark_all_previews_unverified = [this] { preview_->markAllPreviewsUnverified(); };
    ports.status_changed = [this](const QString &text) { status_->setOperationMessage(text); };
    ports.actions_changed = [this](const FileCommandActionState &state) { apply_command_actions(state); };
    ports.activity_changed = [this] { publish_operation_activity(); };
    ports.diagnostic_changed = [this](const char *name, const QVariant &value) { setProperty(name, value); };
    ports.file_in_use = [this] { status_->notifyFileInUse(); };
    ports.clear_selection = [this] { preview_->clearSelection(); };
    ports.refresh_directory_sources = [this] { catalog_->refresh_directory_sources(); };
    ports.refresh_catalog = [this] { catalog_->refresh(); };
    ports.poll_catalog = [this](int interval) { catalog_->resume_polling(interval); };
    ports.reveal_paths = [this](const QStringList &paths) { navigation_->reveal_after_refresh(paths); };
    ports.refresh_after_rename = [this](const QString &path) { refresh_after_rename({path}); };
    ports.refresh_after_batch_rename = [this](const QStringList &paths) { refresh_after_rename(paths); };
    ports.load_trash_catalog = [this](bool discover) { if (trash_) trash_->load(discover); };
    ports.mark_saved_search_dirty = [this] { catalog_->mark_saved_search_dirty(); };
    ports.observe_search_path = [this](const QString &path) { catalog_->observe_search_path(path); };
    ports.reconcile_search_move = [this](const QStringList &paths) { catalog_->reconcile_search_move(paths); };
    ports.schedule_search = [this] { catalog_->schedule_search(); };
    ports.open_path = [this](const QString &path, bool history) { navigation_->open(path, history); };
    ports.open_external_paths = [this](const QStringList &paths) { open_external_paths(paths); };
    ports.open_external_items_in_parent = [this](const QStringList &paths) { open_external_items_in_parent(paths); };
    ports.pin_path = [this](const QString &path) { if (places_) places_->add_favorite(path); };
    commands_ = std::make_unique<FileCommandController>(std::move(ports), FileCommandOptions{
        .file_operations = std::move(file_operation_options),
        .file_transfers = std::move(file_transfer_options),
        .journal_override = batch_journal_override,
        .transfer_probes = {},
        .drop_probes = {},
    }, this);
    commands_->setObjectName(QStringLiteral("fileCommandController"));
    selection_ = std::make_unique<SelectionController>(*w.listView, catalog_->model(), SelectionActions{
        w.createFolderAction, w.renameAction, w.deleteAction, w.copyNamesAction, w.copyPathsAction,
        w.copyObjectsAction, w.pasteObjectsAction, w.cancelPreviewAttemptAction}, SelectionCallbacks{
        .availability = [this] { return commands_->availability(); },
        .status = [this](QString text) { status_->setOperationMessage(std::move(text)); },
        .paste_requested = [this] { commands_->paste_objects(); },
        .can_paste_objects = [this] { return commands_->can_paste_objects(); },
        .transfer_requested = [this](QList<core::DirectoryEntry>, fileops::FileTransferKind kind) {
            commands_->prompt_transfer_selected(kind);
        },
        .open_requested_path = [this](QString path) { navigation_->open(path); },
        .show_in_folder_requested = [this](core::DirectoryEntry entry) { catalog_->show_entry_in_folder(entry); },
        .can_force_preview = [this](const core::DirectoryEntry &entry) { return preview_->canForcePreview(entry); },
        .force_preview_requested = [this](qulonglong id) { preview_->forcePreview(id); },
        .extended_preview_active = [this] { return preview_->forcedPreviewActive(); },
        .native_open_allowed = [this] { return !shuttingDown_; },
        .external_applications_changed = [this] { schedule_settings_save(); },
        .trash_menu_requested = [this](QPoint point) { trash_->show_context_menu(point); },
        .refresh_command_actions = [this] { commands_->update_actions(); },
        .selection_summary_changed = [this](QString text) { status_->setSelectionSummary(std::move(text)); },
    });
    places_ = std::make_unique<PlacesController>(PlacesViewPort{
        w.favorites, w.trashFavorite, w.directoryTree, w.favoritesScrollBar}, PlacesCallbacks{
        .open_requested_path = [this](QString path) { navigation_->open(path); },
        .file_command_intent = [this](PlacesCommand command) {
            if (command == PlacesCommand::show_trash) trash_->show(); else trash_->cleanup();
        },
        .can_empty_trash = [this] { return trash_ && trash_->can_cleanup(); },
        .favorites_changed = [this] { schedule_settings_save(); },
    });
    trash_ = std::make_unique<TrashController>(*catalog_, *commands_, TrashViews{
        this, w.listView, w.favorites, w.trashFavorite}, TrashCallbacks{
        .message = [this](const QString &text) { status_->setOperationMessage(text); },
        .changed = [this] { publish_catalog_status(); },
    });
    catalog_->attachViews({this, w.listView, w.filterEdit, w.globalSearchEdit, w.sortField,
        w.sortDirection, w.recursiveViewButton, w.refreshButton, w.globalSearchMessage,
        w.sizeSlider, delegate_.get(), w.renameAction}, *preview_, *navigation_, *commands_, *places_, *selection_, {
        .settings_changed = [this] { schedule_settings_save(); },
        .status_changed = [this] { publish_catalog_status(); },
        .drag_active = [this] { return drops_ && drops_->active(); },
        .leave_trash = [this] { trash_->leave(); },
        .refresh_trash = [this] { trash_->load(); },
    });
    drops_ = std::make_unique<DropRouter>(DropViewPort{w.listView, w.directoryTree, w.favorites,
        w.trashFavorite, w.favoritesHeader, w.previewPanel, w.previewCanvas}, catalog_->model(), DropCallbacks{
        .state = [this] { return DropState{catalog_->path(), catalog_->trash_active(),
            catalog_->search_active(), catalog_->recursive_active()}; },
        .can_transfer = [this] { return commands_->drop_transfer_allowed(); },
        .can_validate_external = [this] { return commands_->external_drop_validation_allowed(); },
        .recovery_pending = [this] { return commands_->recovery_pending() || commands_->journal_blocked(); },
        .pin_requested = [this](QStringList paths) { for (const auto &path : paths) places_->add_favorite(path); },
        .begin_external_directory_validation = [this](QStringList paths) {
            return commands_->begin_external_directory_drop(ExternalDirectoryDropIntent::pin, std::move(paths));
        },
        .begin_external_transfer_validation = [this](QStringList paths, QString destination,
            fileops::FileTransferKind kind, bool to_trash, QList<core::DirectoryEntry> search_entries) {
            return commands_->begin_external_transfer_drop(std::move(paths), destination, kind,
                to_trash, false, std::move(search_entries));
        },
        .transfer_requested = [this](QList<core::DirectoryEntry> entries, QString destination,
            fileops::FileTransferKind kind) { return commands_->start_transfer_selection(entries, destination, kind); },
        .trash_requested = [this](QList<core::DirectoryEntry> entries) { return commands_->prompt_move_entries_to_trash(entries); },
        .drag_status_changed = [this](std::optional<DragStatusMessage> message) {
            status_->setDragStatus(std::move(message));
            setProperty("dragStatusActive", status_->dragStatusActive());
        },
        .open_external_paths = [this](QStringList paths) { open_external_paths(paths); },
    });
    QObject *owners[]{catalog_.get(), preview_.get(), navigation_.get(), selection_.get(),
                      places_.get(), trash_.get(), drops_.get(), status_.get()};
    for (auto *owner : owners) owner->setParent(this);
    connect_commands();
    settings_->set_save_provider([this] { return capture_settings(); });
    restore_settings();
    static_cast<void>(commands_->refresh_current_operation_journal_state());
    if (initial_path.isEmpty()) initial_path = preferences_.last_path;
    if (initial_path.isEmpty()) initial_path = QDir::homePath();
    navigation_->open(initial_path);
    QTimer::singleShot(0, this, [this] {
        preview_->bindMonitorProfile();
        trash_->load(false);
    });
}

MainWindow::~MainWindow() {
    shuttingDown_ = true;
    settings_.reset();
    catalog_->stop();
    drops_.reset();
    trash_.reset();
    commands_.reset();
    selection_.reset();
    preview_->stop();
    preview_.reset();
    navigation_.reset();
    places_.reset();
    catalog_.reset();
    delegate_.reset();
    status_.reset();
    shell_.reset();
}

void MainWindow::connect_commands() {
    const auto &w = shell_->widgets();
    connect(w.pathEdit, &QLineEdit::returnPressed, this, [this] { navigation_->open(shell_->widgets().pathEdit->text()); });
    connect(w.chooseFolderAction, &QAction::triggered, this, [this] {
        navigation_->choose_directory(shell_->widgets().chooseFolderAction->text());
    });
    connect(w.addFavoriteButton, &QToolButton::clicked, this, [this] { places_->add_current_favorite(); });
    connect(w.createFolderAction, &QAction::triggered, this, [this] { commands_->prompt_create_directory(); });
    connect(w.renameAction, &QAction::triggered, this, [this] { commands_->prompt_rename_selected(); });
    connect(w.deleteAction, &QAction::triggered, this, [this] { commands_->prompt_delete_selected(); });
    connect(w.trashAction, &QAction::triggered, this, [this] { trash_->show(); });
    connect(w.unfinishedOperationAction, &QAction::triggered, this, [this] { commands_->show_unfinished_operation_dialog(); });
    connect(w.settingsAction, &QAction::triggered, this, [this] { show_settings_dialog(); });
    connect(w.updateAction, &QAction::triggered, this, [this] { show_update_dialog(); });
    connect(w.componentsAction, &QAction::triggered, this, [this] { show_components_dialog(); });
    connect(w.aboutAction, &QAction::triggered, this, [this] { AboutDialog(application_version(), this).exec(); });
    connect(w.donateAction, &QAction::triggered, this, [this] { show_project_contact_dialog(this, true); });
    connect(w.reportIssueAction, &QAction::triggered, this, [this] { show_project_contact_dialog(this, false); });
    connect(w.refreshShortcut, &QShortcut::activated, this, [this] { catalog_->refresh(); });
    connect(w.backShortcut, &QShortcut::activated, this, [this] { navigation_->back(); });
    connect(w.forwardShortcut, &QShortcut::activated, this, [this] { navigation_->forward(); });
    connect(w.upShortcut, &QShortcut::activated, this, [this] { navigation_->up(); });
}
void MainWindow::apply_style(const ViewerStyle &style) {
    if (!delegate_) return;
    delegate_->set_placeholder_theme(style.theme);
    delegate_->set_folder_colors(style.folder_frame, style.folder_text);
    delegate_->set_file_label_colors(style.file_text, style.page_background, style.page_text, style.page_border);
    if (status_) status_->setThemeColors(style.muted,
        QColor(style.theme == AppTheme::white || style.theme == AppTheme::vanilla ? "#0080B3" : "#F5A623"));
    if (places_) places_->update_directory_tree();
    if (shell_) shell_->widgets().listView->viewport()->update();
}
void MainWindow::retranslate_modules() {
    if (preview_) preview_->retranslate();
    if (places_) places_->retranslate();
    if (status_) { status_->setInterfaceFont(qApp->font()); status_->retranslate(); }
    if (commands_) commands_->update_actions();
    if (selection_) selection_->update_clipboard_actions();
    if (catalog_ && selection_) catalog_->update_tools();
}
void MainWindow::publish_catalog_status() {
    if (!status_ || !catalog_) return;
    const auto &model = catalog_->model();
    status_->setCatalogSnapshot({catalog_->update(), catalog_->trash_active(), catalog_->search_active(),
        catalog_->recursive_active(), static_cast<std::uint64_t>(model.rowCount()), model.total_size(),
        model.visible_directories(), catalog_->search_status(), catalog_->search_roots(),
        trash_ ? trash_->status_summary() : std::nullopt});
    if (selection_) selection_->update_clipboard_actions();
}
void MainWindow::publish_operation_activity() {
    if (!commands_ || shuttingDown_) return;
    status_->setOperationActivity({commands_->mutation_in_flight(), commands_->recovery_pending(), commands_->journal_blocked()});
    catalog_->update_tools();
    if (selection_) selection_->update_clipboard_actions();
}
void MainWindow::apply_command_actions(const FileCommandActionState &state) {
    const auto &w = shell_->widgets();
    w.renameAction->setText(state.rename_text);
    w.renameAction->setToolTip(state.rename_tooltip);
    w.renameAction->setEnabled(state.rename_enabled);
    w.deleteAction->setText(state.delete_text);
    w.deleteAction->setEnabled(state.delete_enabled);
    w.trashAction->setEnabled(state.trash_enabled);
    w.unfinishedOperationAction->setEnabled(state.unfinished_enabled);
    w.createFolderAction->setEnabled(state.create_directory_enabled);
    w.createFolderButton->setEnabled(state.create_directory_enabled);
    if (catalog_) catalog_->update_tools();
    if (selection_) selection_->update_clipboard_actions();
}
WindowSettingsSnapshot MainWindow::capture_settings() const {
    auto snapshot = shell_->capture_presentation(preferences_);
    snapshot.last_path = navigation_->path();
    snapshot.search_roots = catalog_->search_roots();
    snapshot.favorites.clear();
    for (const auto &place : places_->favorite_places()) snapshot.favorites.push_back({place.path, place.name});
    snapshot.external_applications = selection_->external_applications();
    return snapshot;
}
void MainWindow::schedule_settings_save() {
    if (settings_ && !restoringSettings_ && !shuttingDown_) settings_->schedule_save();
}
void MainWindow::restore_settings() {
    restoringSettings_ = true;
    QSettings stored;
    preferences_ = WindowSettings::load(stored);
    selection_->set_registry_data(preferences_.external_applications);
    shell_->restore_presentation(preferences_);
    QList<FavoritePlace> favorites;
    for (const auto &place : preferences_.favorites) favorites.push_back({place.label, place.path});
    places_->restore_favorites(favorites);
    catalog_->set_search_roots(preferences_.search_roots);
    preview_->setCacheMaximumBytes(preferences_.cache_maximum_bytes);
    trash_->set_maximum_bytes(preferences_.trash_maximum_bytes);
    commands_->set_trash_maximum_bytes(preferences_.trash_maximum_bytes);
    commands_->load_recovery_state();
    catalog_->apply_thumbnail_extent(shell_->widgets().sizeSlider->value());
    catalog_->apply_filter();
    catalog_->apply_sort();
    apply_style(shell_->style());
    retranslate_modules();
    restoringSettings_ = false;
}
void MainWindow::open_external_path(const QString &path, const bool select_file) {
    if (select_file) { open_external_paths({path}); return; }
    if (isMinimized()) showNormal(); else show();
    raise();
    activateWindow();
    if (!path.isEmpty()) navigation_->open(path);
}
void MainWindow::open_external_paths(const QStringList &paths) {
    if (isMinimized()) showNormal(); else show();
    raise();
    activateWindow();
    QStringList normalized;
    QSet<QString> seen;
    for (const auto &path : paths) {
        auto item = QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());
#ifdef Q_OS_WIN
        const auto key = item.toCaseFolded();
#else
        const auto &key = item;
#endif
        if (!item.isEmpty() && !seen.contains(key)) { seen.insert(key); normalized.push_back(std::move(item)); }
    }
    if (!normalized.isEmpty()) commands_->request_external_open(std::move(normalized));
}
void MainWindow::open_external_items_in_parent(const QStringList &paths) {
    if (paths.isEmpty()) return;
    const auto parent = QFileInfo(paths.front()).absolutePath();
    if (parent.isEmpty()) return;
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    QStringList siblings;
    for (const auto &path : paths)
        if (QFileInfo(path).absolutePath().compare(parent, sensitivity) == 0) siblings.push_back(path);
    if (siblings.isEmpty()) return;
    shell_->widgets().filterEdit->clear();
    navigation_->reveal_after_refresh(siblings, true);
    if (!PlacesController::same_directory_path(catalog_->path(), parent) || !catalog_->ordinary_view())
        navigation_->open(parent);
    else if (catalog_->update().state == catalog::CatalogSessionState::ready) navigation_->restore_reveal();
}
void MainWindow::refresh_after_rename(const QStringList &paths) {
    navigation_->reveal_after_refresh(paths);
    catalog_->refresh_directory_sources();
    catalog_->resume_polling(16);
}
SelectionSnapshot MainWindow::selection_snapshot() const { return selection_->selection_snapshot(); }
bool MainWindow::invoke_selection_command(const SelectionCommand &command) const {
    return selection_->invoke_selection_command(command);
}
void MainWindow::show_update_dialog() {
    auto configuration = updates::compiled_release_configuration();
#ifdef VOVE_UI_TEST_HOOKS
    if (property("disableUpdateNetwork").toBool()) {
        configuration.manifest_url = QUrl{};
        configuration.public_key.clear();
        configuration.releases_api_url = QUrl{};
    }
#endif
    updates::UpdateDialog dialog(std::move(configuration), application_version(), this);
    dialog.exec();
}
void MainWindow::show_components_dialog() {
    const auto specs = [](bool refresh) {
        std::vector<ComponentSpec> result{
            {QStringLiteral("LINE Seed JP"), ui_text(UiTextId::line_seed_purpose),
                QUrl(QStringLiteral("https://raw.githubusercontent.com/google/fonts/main/ofl/lineseedjp/LINESeedJP-Regular.ttf")),
                line_seed_jp_available(refresh), false},
            {QStringLiteral("Ghostscript"), ui_text(UiTextId::ghostscript_purpose),
                QUrl(QStringLiteral("https://ghostscript.com/releases/gsdnld.html")), ghostscript_available(false), true},
        };
#ifdef Q_OS_WIN
        result.push_back({QStringLiteral("Everything"), ui_text(UiTextId::everything_purpose),
            QUrl(QStringLiteral("https://www.voidtools.com/downloads/")), everything_available(refresh), false});
#else
        result.push_back({QStringLiteral("bubblewrap"), ui_text(UiTextId::bubblewrap_purpose),
            QUrl(QStringLiteral("https://github.com/containers/bubblewrap")), bubblewrap_available(), true});
        result.push_back({QStringLiteral("plocate"), ui_text(UiTextId::plocate_purpose),
            QUrl(QStringLiteral("https://plocate.sesse.net/")), plocate_available(refresh), false});
#endif
        return result;
    };
    show_project_components_dialog(this, specs, [this](std::function<void()> done) {
        preview_->refreshExternalComponents(std::move(done));
    });
}
void MainWindow::show_settings_dialog() {
    SettingsDialogData data{.search_roots = catalog_->search_roots(), .applications = {},
        .startup_enabled = startup_registration_enabled(),
        .cache_maximum_bytes = preferences_.cache_maximum_bytes,
        .trash_maximum_bytes = preferences_.trash_maximum_bytes};
    for (const auto &application : selection_->external_applications())
        data.applications.push_back({application.name, application.path});
    const SettingsDialogActions actions{
        .choose_search_root = [this] {
            return QFileDialog::getExistingDirectory(this, QCoreApplication::translate("MainWindow",
                "Allow Global Search in Folder"), navigation_->path(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        },
        .set_startup_enabled = set_startup_registration_enabled,
        .inspect_cache = [this](SettingsCacheCompletion complete) {
            return preview_->inspectCache([complete = std::move(complete)](const CacheReply &reply) {
                complete({reply.success, reply.bytes});
            }) != 0;
        },
        .clear_cache = [this](SettingsCacheCompletion complete) {
            return preview_->clearCache([complete = std::move(complete)](const CacheReply &reply) {
                complete({reply.success, reply.bytes});
            }) != 0;
        },
        .cache_cleared = [this] { preview_->cacheCleared(); },
        .empty_trash = [this] { trash_->cleanup(); },
        .check_updates = [this] { show_update_dialog(); },
    };
    const auto result = vove::ui::show_settings_dialog(this, data, actions);
    if (!result) return;
    std::vector<std::string> encoded;
    for (const auto &root : result->search_roots) encoded.push_back(root.toUtf8().toStdString());
    QStringList roots;
    for (const auto &root : search::normalize_allowed_roots(encoded)) roots.push_back(QString::fromUtf8(root));
    catalog_->set_search_roots(std::move(roots));
    preferences_.cache_maximum_bytes = result->cache_maximum_bytes;
    preview_->setCacheMaximumBytes(preferences_.cache_maximum_bytes);
    preferences_.trash_maximum_bytes = result->trash_maximum_bytes;
    trash_->set_maximum_bytes(preferences_.trash_maximum_bytes);
    commands_->set_trash_maximum_bytes(preferences_.trash_maximum_bytes);
    const auto applications = selection_->external_applications();
    for (const auto &application : applications)
        if (!result->retained_application_paths.contains(application.path))
            static_cast<void>(selection_->remove_external_application(application.path));
    schedule_settings_save();
    if (catalog_->search_active() && !shell_->widgets().globalSearchEdit->text().trimmed().isEmpty()) catalog_->start_search();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (!commands_->can_close()) { event->ignore(); return; }
    navigation_->save();
    settings_->save_now();
    static_cast<void>(commands_->save_recovery_state());
    QMainWindow::closeEvent(event);
}

} // namespace vove::ui
