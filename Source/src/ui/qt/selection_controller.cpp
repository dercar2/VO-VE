#include "selection_controller.hpp"

#include "default_application_name.hpp"
#include "drop_router.hpp"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace vove::ui {
namespace {
QString external_open_helper_override(QString configured) {
#ifdef VOVE_UI_TEST_HOOKS
    if (configured.isEmpty()) return QString::fromUtf8(qgetenv("VOVE_TEST_OPEN_HELPER_OVERRIDE"));
#endif
    return configured;
}
bool ordinary_objects(const QList<core::DirectoryEntry> &entries) {
    return !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
    });
}
QByteArray application_icon_png(const QString &path) {
    QFileIconProvider provider;
    const auto pixmap = provider.icon(QFileInfo(path)).pixmap(QSize(32, 32));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!pixmap.isNull() && buffer.open(QIODevice::WriteOnly)) pixmap.save(&buffer, "PNG");
    return encoded;
}
QIcon stored_application_icon(const QByteArray &encoded) {
    QPixmap pixmap;
    return !encoded.isEmpty() && pixmap.loadFromData(encoded, "PNG") ? QIcon(pixmap) : QIcon{};
}
} // namespace

SelectionController::SelectionController(QListView &view, DirectoryListModel &model,
                                         SelectionActions actions, SelectionCallbacks callbacks,
                                         QObject *parent, QString open_helper_override)
    : QObject(parent), view_(&view), model_(model), actions_(actions),
      callbacks_(std::move(callbacks)),
      externalOpenCoordinator_(external_open_helper_override(std::move(open_helper_override))) {
#ifdef VOVE_UI_TEST_HOOKS
    setObjectName(QStringLiteral("selectionController"));
#endif
    const auto bind = [this](QAction *action, auto method) {
        if (action) connect(action, &QAction::triggered, this, method);
    };
    bind(actions_.copy_names, &SelectionController::copy_selected_names);
    bind(actions_.copy_paths, &SelectionController::copy_selected_paths);
    bind(actions_.copy_objects, &SelectionController::copy_selected_objects);
    if (actions_.paste_objects) connect(actions_.paste_objects, &QAction::triggered, this, [this] {
        update_clipboard_actions();
        if (actions_.paste_objects->isEnabled() && callbacks_.paste_requested)
            callbacks_.paste_requested();
    });
    // Restrict Ctrl+C to the catalog so text editors keep their native text-copy semantics.
    for (auto *action : {actions_.copy_objects, actions_.paste_objects}) {
        if (action) {
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            view.addAction(action);
        }
    }
    connect(&view, &QListView::customContextMenuRequested, this,
            &SelectionController::show_catalog_context_menu);
    connect(&view, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!availability().trash_view)
            if (const auto *entry = model_.entry_at(index)) open_entry(*entry);
    });
    if (view.selectionModel()) connect(view.selectionModel(), &QItemSelectionModel::selectionChanged,
                                      this, [this] {
        if (callbacks_.refresh_command_actions) callbacks_.refresh_command_actions();
        update_clipboard_actions();
    });
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &SelectionController::update_clipboard_actions);
    update_clipboard_actions();
}

SelectionController::~SelectionController() = default;
QString SelectionController::tr(const char *text) { return QCoreApplication::translate("MainWindow", text); }
OperationAvailability SelectionController::availability() const {
    if (callbacks_.availability) return callbacks_.availability();
    OperationAvailability blocked;
    blocked.mutation_busy = true;
    blocked.external_drop_pending = true;
    return blocked;
}

QList<core::DirectoryEntry> SelectionController::selected_entries_in_view_order() const {
    QList<core::DirectoryEntry> entries;
    if (!view_ || !view_->selectionModel()) return entries;
    auto selected = view_->selectionModel()->selectedRows();
    if (selected.isEmpty() && view_->currentIndex().isValid()) selected.push_back(view_->currentIndex());
    std::ranges::sort(selected, {}, [](const QModelIndex &index) { return index.row(); });
    entries.reserve(selected.size());
    for (const auto &index : selected)
        if (const auto *entry = model_.entry_at(index)) entries.push_back(*entry);
    return entries;
}

SelectionSnapshot SelectionController::selection_snapshot() const {
    if (availability().trash_view) return SelectionSnapshot{};
    const auto entries = selected_entries_in_view_order();
    return SelectionSnapshot(std::span<const core::DirectoryEntry>(entries.data(), static_cast<std::size_t>(entries.size())));
}
bool SelectionController::invoke_selection_command(const SelectionCommand &command) const {
    return dispatch_selection_command(selection_snapshot(), command);
}
QString SelectionController::selection_status_text() const {
    if (!view_ || !view_->selectionModel()) return {};
    const auto rows = view_->selectionModel()->selectedRows();
    if (rows.size() < 2) return {};
    qulonglong directories{}, files{};
    for (const auto &row : rows) {
        if (const auto *entry = model_.entry_at(row)) {
            if (entry->kind == core::EntryKind::directory) ++directories;
            else ++files;
        }
    }
    return tr("Selected: %1 of %2 folders \xC2\xB7 %3 of %4 files")
        .arg(directories).arg(static_cast<qulonglong>(model_.visible_directories()))
        .arg(files).arg(static_cast<qulonglong>(model_.visible_files()));
}
void SelectionController::copy_selected_names() {
    QStringList values;
    for (const auto &entry : selected_entries_in_view_order()) values.push_back(QString::fromUtf8(entry.name_utf8));
    if (values.isEmpty()) return;
    QApplication::clipboard()->setText(values.join(QLatin1Char('\n')));
    if (callbacks_.status) callbacks_.status(tr("Names copied: %1").arg(values.size()));
}
void SelectionController::copy_selected_paths() {
    QStringList values;
    for (const auto &entry : selected_entries_in_view_order())
        values.push_back(QDir::toNativeSeparators(QString::fromUtf8(entry.path_utf8)));
    if (values.isEmpty()) return;
    QApplication::clipboard()->setText(values.join(QLatin1Char('\n')));
    if (callbacks_.status) callbacks_.status(tr("Paths copied: %1").arg(values.size()));
}
std::unique_ptr<QMimeData> SelectionController::file_mime_data() const {
    if (availability().trash_view || !view_ || !view_->selectionModel() ||
        view_->selectionModel()->selectedRows().isEmpty()) return {};
    const auto entries = selected_entries_in_view_order();
    if (entries.size() > 1024 || !ordinary_objects(entries)) return {};
    auto mime = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    QStringList names;
    for (const auto &entry : entries) {
        urls.push_back(QUrl::fromLocalFile(QString::fromUtf8(entry.path_utf8)));
        names.push_back(QString::fromUtf8(entry.name_utf8));
    }
    mime->setUrls(urls);
    mime->setText(names.join(QLatin1Char('\n')));
    return local_file_paths(mime.get()) ? std::move(mime) : nullptr;
}
void SelectionController::copy_selected_objects() {
    if (auto mime = file_mime_data()) QApplication::clipboard()->setMimeData(mime.release());
}
void SelectionController::update_clipboard_actions() {
    const auto state = availability();
    const auto entries = selected_entries_in_view_order();
    const auto selected = view_ && view_->selectionModel() &&
                          !view_->selectionModel()->selectedRows().isEmpty();
    if (actions_.copy_objects) actions_.copy_objects->setEnabled(!state.trash_view && selected &&
        entries.size() <= 1024 && ordinary_objects(entries));
    if (actions_.copy_paths) actions_.copy_paths->setEnabled(!state.trash_view && selected && !entries.isEmpty());
    const auto *mime = QApplication::clipboard()->mimeData();
    if (actions_.paste_objects) actions_.paste_objects->setEnabled(
        callbacks_.can_paste_objects && callbacks_.can_paste_objects() && mime && mime->hasUrls());
    if (callbacks_.selection_summary_changed) callbacks_.selection_summary_changed(selection_status_text());
}

std::unique_ptr<QMenu> SelectionController::create_context_menu(const QPoint &position) {
    if (!view_ || !view_->selectionModel() || availability().trash_view) return {};
    const auto state = availability();
    auto menu = std::make_unique<QMenu>(view_);
    const auto clicked = view_->indexAt(position);
    const auto add = [&](QAction *action) { if (action) menu->addAction(action); };
    if (!clicked.isValid()) {
        update_clipboard_actions();
        add(actions_.paste_objects);
        if (!state.global_search && !state.recursive_view) add(actions_.create_folder);
        return menu;
    }
    if (!view_->selectionModel()->isSelected(clicked))
        view_->selectionModel()->setCurrentIndex(clicked, QItemSelectionModel::ClearAndSelect);
    const auto entries = selected_entries_in_view_order();
    if (entries.isEmpty()) return {};
    if (callbacks_.refresh_command_actions) callbacks_.refresh_command_actions();
    update_clipboard_actions();
    if (actions_.copy_names) actions_.copy_names->setText(entries.size() == 1 ? tr("Copy name") : tr("Copy names"));
    if (actions_.copy_paths) actions_.copy_paths->setText(entries.size() == 1 ? tr("Copy full path") : tr("Copy full paths"));
    if (entries.size() == 1) {
        const auto entry = entries.front();
        const auto path = QString::fromUtf8(entry.path_utf8);
        if (entry.kind == core::EntryKind::file) {
            auto *force = menu->addAction(tr("Force preview"));
            force->setObjectName(QStringLiteral("forcePreviewAttemptAction"));
            force->setEnabled(callbacks_.can_force_preview && callbacks_.can_force_preview(entry));
            connect(force, &QAction::triggered, this, [this, id = entry.id] {
                if (callbacks_.force_preview_requested) callbacks_.force_preview_requested(id);
            });
            if (callbacks_.extended_preview_active && callbacks_.extended_preview_active()) add(actions_.cancel_preview);
            menu->addSeparator();
        }
        auto application = callbacks_.default_application_name ? callbacks_.default_application_name(path)
                                                              : vove::ui::default_application_name(path);
        application.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *open = menu->addAction(entry.kind == core::EntryKind::file
            ? tr("Open in %1").arg(application.isEmpty() ? tr("default application") : application) : tr("Open"));
        open->setObjectName(QStringLiteral("openDefaultAction"));
        connect(open, &QAction::triggered, this, [this, entry] { open_entry(entry); });
        if (entry.kind == core::EntryKind::file) {
            auto *with = menu->addMenu(tr("Open with"));
            with->setObjectName(QStringLiteral("openWithMenu"));
            for (const auto &app : externalApplications_.applications()) {
                auto label = app.name;
                label.replace(QLatin1Char('&'), QStringLiteral("&&"));
                auto *action = with->addAction(stored_application_icon(app.icon_png), label);
                action->setObjectName(QStringLiteral("openWithApplicationAction"));
                action->setData(app.path);
                connect(action, &QAction::triggered, this, [this, app, path] { open_file_with_application(app, path); });
            }
            if (!externalApplications_.applications().isEmpty()) with->addSeparator();
            auto *add_app = with->addAction(tr("Add Application\xE2\x80\xA6"));
            add_app->setObjectName(QStringLiteral("addExternalApplicationAction"));
            connect(add_app, &QAction::triggered, this, [this, path] { prompt_add_external_application(path); });
            menu->addSeparator();
#ifdef Q_OS_WIN
            auto *reveal = menu->addAction(tr("Show in File Explorer"));
#else
            auto *reveal = menu->addAction(tr("Show in File Manager"));
#endif
            reveal->setObjectName(QStringLiteral("revealInFileManagerAction"));
            connect(reveal, &QAction::triggered, this, [this, path] { reveal_file_in_file_manager(path); });
        }
        if (state.global_search || state.recursive_view) {
            auto *show = menu->addAction(tr("Show in Folder"));
            connect(show, &QAction::triggered, this, [this, entry] {
                if (callbacks_.show_in_folder_requested) callbacks_.show_in_folder_requested(entry);
            });
        }
        menu->addSeparator();
    }
    if (state.selection_context_visible(ordinary_objects(entries))) {
        add(actions_.rename);
        add(actions_.remove);
        for (auto kind : {fileops::FileTransferKind::copy, fileops::FileTransferKind::move}) {
            auto *action = menu->addAction(kind == fileops::FileTransferKind::copy
                ? tr("Copy to Folder\xE2\x80\xA6") : tr("Move to Folder\xE2\x80\xA6"));
            connect(action, &QAction::triggered, this, [this, entries, kind] {
                if (callbacks_.transfer_requested) callbacks_.transfer_requested(entries, kind);
            });
        }
        menu->addSeparator();
    }
    add(actions_.copy_objects);
    add(actions_.paste_objects);
    menu->addSeparator();
    add(actions_.copy_names);
    add(actions_.copy_paths);
    return menu;
}
void SelectionController::show_catalog_context_menu(const QPoint &position) {
    if (availability().trash_view) {
        if (callbacks_.trash_menu_requested) callbacks_.trash_menu_requested(position);
    } else if (auto menu = create_context_menu(position)) {
        menu->exec(view_->viewport()->mapToGlobal(position));
    }
}

const QList<ExternalApplication> &SelectionController::external_applications() const noexcept {
    return externalApplications_.applications();
}
void SelectionController::set_registry_data(ExternalApplicationRegistry applications) {
    externalApplications_ = std::move(applications);
}
void SelectionController::set_registry_data(QList<ExternalApplication> applications) {
    externalApplications_.restore_loaded_data(std::move(applications));
}
AddExternalApplicationResult SelectionController::register_external_application(const ExternalApplication &application) {
    const auto result = externalApplications_.add(application);
    if (result == AddExternalApplicationResult::added && callbacks_.external_applications_changed)
        callbacks_.external_applications_changed();
    return result;
}
bool SelectionController::remove_external_application(const QString &path) {
    if (!externalApplications_.remove(path)) return false;
    if (callbacks_.external_applications_changed) callbacks_.external_applications_changed();
    return true;
}
void SelectionController::open_entry(const core::DirectoryEntry &entry) {
    const auto path = QString::fromUtf8(entry.path_utf8);
    if (entry.kind == core::EntryKind::directory) {
        if (callbacks_.open_requested_path) callbacks_.open_requested_path(path);
    } else open_file_in_default_application(path);
}
void SelectionController::open_file_in_default_application(const QString &path) {
    if (!callbacks_.native_open_allowed || !callbacks_.native_open_allowed()) return;
#ifdef VOVE_UI_TEST_HOOKS
    if (property("captureExternalOpen").toBool()) {
        setProperty("lastExternalOpenKind", QStringLiteral("default"));
        setProperty("lastExternalOpenPath", QDir::cleanPath(path));
    }
#endif
    externalOpenCoordinator_.open(path, [this](ExternalOpenResult result) {
        QString message;
        switch (result) {
        case ExternalOpenResult::opened: break;
        case ExternalOpenResult::timed_out: message = tr("Opening the file timed out"); break;
        case ExternalOpenResult::helper_unavailable: message = tr("The file opening component is unavailable"); break;
        case ExternalOpenResult::busy: message = tr("Another file is being opened"); break;
        case ExternalOpenResult::failed: message = tr("No application could open the selected file"); break;
        }
        if (callbacks_.status) callbacks_.status(message);
    });
}
void SelectionController::open_file_with_application(const ExternalApplication &application, const QString &path) {
    if (!callbacks_.native_open_allowed || !callbacks_.native_open_allowed()) return;
#ifdef VOVE_UI_TEST_HOOKS
    if (property("captureExternalOpen").toBool()) {
        setProperty("lastExternalOpenKind", QStringLiteral("saved"));
        setProperty("lastExternalOpenApplication", application.path);
        setProperty("lastExternalOpenPath", QDir::cleanPath(path));
        return;
    }
#endif
    QString error;
    if (!externalApplications_.launch(application, path, &error) && callbacks_.status)
        callbacks_.status(error == QStringLiteral("application_unavailable")
            ? tr("The selected application is no longer available") : tr("The selected application could not be started"));
}
void SelectionController::reveal_file_in_file_manager(const QString &path) {
    if (!callbacks_.native_open_allowed || !callbacks_.native_open_allowed()) return;
    externalOpenCoordinator_.reveal(path, [this](ExternalOpenResult result) {
        QString message;
        switch (result) {
        case ExternalOpenResult::opened: break;
        case ExternalOpenResult::timed_out: message = tr("The file manager did not respond"); break;
        case ExternalOpenResult::helper_unavailable: message = tr("The desktop integration component is unavailable"); break;
        case ExternalOpenResult::busy: message = tr("Another external action is still running"); break;
        case ExternalOpenResult::failed: message = tr("Could not show the selected file in the file manager"); break;
        }
        if (callbacks_.status) callbacks_.status(message);
    });
}
void SelectionController::prompt_add_external_application(const QString &file_path) {
    if (!callbacks_.native_open_allowed || !callbacks_.native_open_allowed() || !view_) return;
    const auto path = QFileDialog::getOpenFileName(view_, tr("Add Application"),
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation));
    if (path.isEmpty()) return;
    const auto result = register_external_application({.name = {}, .path = path, .icon_png = application_icon_png(path)});
    if (result != AddExternalApplicationResult::added) {
        if (callbacks_.status) callbacks_.status(result == AddExternalApplicationResult::duplicate
            ? tr("This application is already in the list") : tr("The selected application cannot be used"));
        return;
    }
    open_file_with_application(externalApplications_.applications().back(), file_path);
}

} // namespace vove::ui
