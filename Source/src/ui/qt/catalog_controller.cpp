#include "catalog_controller.hpp"

#include "catalog_tool_button.hpp"
#include "directory_item_delegate.hpp"
#include "drag_views.hpp"
#include "file_command_controller.hpp"
#include "navigation_controller.hpp"
#include "places_controller.hpp"
#include "selection_controller.hpp"
#include "toolbar_fields.hpp"
#include "ui_text.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyleOptionViewItem>
#include <QToolButton>

#include <algorithm>
#include <utility>

namespace vove::ui {
namespace {
std::filesystem::path native_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
QString path_key(const std::filesystem::path &path) {
#ifdef Q_OS_WIN
    auto key = QString::fromStdWString(path.native());
#else
    const auto bytes = path.u8string();
    auto key = QString::fromUtf8(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size()));
#endif
    key = QDir::cleanPath(QDir::fromNativeSeparators(key));
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}
QString unavailable_message() {
#ifdef Q_OS_WIN
    return QCoreApplication::translate("MainWindow", "<b>Everything is unavailable.</b><br>"
        "Make sure it is installed, running, and available to VO-VE with the same permissions. "
        "<a href=\"https://www.voidtools.com/downloads/\">Install Everything</a>");
#else
    return QCoreApplication::translate("MainWindow", "<b>plocate is unavailable.</b><br>"
        "Install plocate and update its filename index. It searches only mounted resources "
        "present in that index. <a href=\"https://plocate.sesse.net/\">About plocate</a>");
#endif
}
QString scope_message(const QString &root) {
#ifdef Q_OS_WIN
    return QCoreApplication::translate("MainWindow", "<b>The work folder is not included in the Everything index.</b><br>"
        "Add it to the folders indexed by Everything and retry the search.%1").arg(root);
#else
    return QCoreApplication::translate("MainWindow", "<b>The work folder is absent from the plocate index.</b><br>"
        "Make sure the resource is mounted and included by updatedb, then refresh the index.%1").arg(root);
#endif
}
} // namespace

CatalogController::CatalogController(QString search_helper_override, QObject *parent)
    : QObject(parent), session_(source_, model_), search_(this, std::move(search_helper_override)),
      monitor_(this) {
    pollTimer_.setInterval(16);
    searchTimer_.setInterval(250);
    searchTimer_.setSingleShot(true);
    connect(&pollTimer_, &QTimer::timeout, this, [this] { poll(); });
    connect(&searchTimer_, &QTimer::timeout, this, [this] { start_search(); });
    search_.set_reply_handler([this](search::SearchBatch batch) { handle_search_batch(std::move(batch)); });
    monitor_.refresh_requested = [this] { return refresh_automatically(); };
}
CatalogController::~CatalogController() {
    stop();
    if (views_.grid) {
        auto *grid = static_cast<CatalogView *>(views_.grid);
        grid->set_item_hit_test({});
        grid->set_rename_action(nullptr, {});
        grid->setModel(nullptr);
    }
}

void CatalogController::attachViews(CatalogViews views, PreviewController &preview,
    NavigationController &navigation, FileCommandController &commands, PlacesController &places,
    SelectionController &selection, CatalogCallbacks callbacks) {
    views_ = views;
    callbacks_ = std::move(callbacks);
    preview_ = &preview;
    navigation_ = &navigation;
    commands_ = &commands;
    places_ = &places;
    selection_ = &selection;
    auto *grid = static_cast<CatalogView *>(views_.grid);
    grid->set_item_hit_test([this](const QModelIndex &index, const QPoint &point) {
        QStyleOptionViewItem option;
        option.initFrom(views_.grid);
        option.rect = views_.grid->visualRect(index);
        return views_.delegate->item_hit_region(option, index).contains(point);
    });
    grid->set_rename_action(views_.rename, [this](const QModelIndex &index, const QPoint &point) {
        QStyleOptionViewItem option;
        option.initFrom(views_.grid);
        option.rect = views_.grid->visualRect(index);
        return views_.delegate->name_rect(option, index).contains(point);
    });
    connect(views_.filter, &QLineEdit::textChanged, this, [this] { apply_filter(); });
    connect(views_.global_search, &QLineEdit::textChanged, this, [this] { schedule_search(); });
    connect(views_.global_search, &QLineEdit::returnPressed, this, [this] {
        searchTimer_.stop();
        start_search();
    });
    connect(views_.sort, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { apply_sort(); });
    connect(views_.sort_direction, &QToolButton::toggled, this, [this] { apply_sort(); });
    connect(views_.thumbnail_scale, &QSlider::valueChanged, this, [this](int value) { apply_thumbnail_extent(value); });
    connect(views_.recursive, &QToolButton::clicked, this, [this](bool enabled) { set_recursive(enabled); });
    connect(views_.refresh, &QToolButton::clicked, this, [this] { refresh(); });
    views_.grid->viewport()->installEventFilter(this);
    views_.window->installEventFilter(this);
    sync_preview_context();
}
void CatalogController::stop() {
    pollTimer_.stop();
    searchTimer_.stop();
    monitor_.stop();
    search_.cancel();
    session_.suspend();
}
void CatalogController::notify() {
    update_tools();
    if (callbacks_.status_changed) callbacks_.status_changed();
}
void CatalogController::sync_preview_context() {
    if (!preview_) return;
    const auto mode = search_active() ? PreviewCatalogMode::global_search
        : recursive_active() ? PreviewCatalogMode::recursive
        : trash_active() ? PreviewCatalogMode::trash : PreviewCatalogMode::directory;
    preview_->updatePreviewContext({mode, navigation_ && navigation_->restore_pending()});
}
bool CatalogController::eventFilter(QObject *watched, QEvent *event) {
    if (event && watched == views_.grid->viewport() && event->type() == QEvent::Resize)
        update_search_message_geometry();
    if (event && watched == views_.window && (event->type() == QEvent::Show ||
        event->type() == QEvent::Hide || event->type() == QEvent::WindowStateChange))
        QTimer::singleShot(0, this, [this] { update_monitor_visibility(); });
    return QObject::eventFilter(watched, event);
}
void CatalogController::prepare_navigation() {
    leave_recursive(false);
    if (trash_active()) {
        if (callbacks_.leave_trash) callbacks_.leave_trash();
        leave_trash();
    }
    if (search_active() || !views_.global_search->text().isEmpty()) {
        const QSignalBlocker blocker(views_.global_search);
        views_.global_search->clear();
        leave_search(false);
    }
}
void CatalogController::open_directory(const QString &path) {
    prepare_navigation();
    path_ = path;
    automatic_ = false;
    monitor_.set_directory(path_);
    monitor_.scan_started();
    update_monitor_visibility();
    sync_preview_context();
    preview_->catalogOpened();
    session_.open(native_path(path_));
    places_->set_location(path_);
    pollTimer_.setInterval(16);
    pollTimer_.start();
    update_ = {};
    update_.state = catalog::CatalogSessionState::loading;
    notify();
}
void CatalogController::refresh() {
    if (trash_active()) {
        const auto state = commands_->refresh_current_operation_journal_state();
        if (state == CurrentOperationJournalState::none && !commands_->busy()) {
            if (callbacks_.refresh_trash) callbacks_.refresh_trash();
            return;
        }
        if (callbacks_.leave_trash) callbacks_.leave_trash();
        leave_trash();
        navigation_->open(path_, false);
    }
    if (commands_->refresh_or_recover()) { notify(); return; }
    if (search_active()) { start_search(); return; }
    preview_->catalogRefreshed();
    refresh_directory_sources();
    resume_polling(16);
    update_ = {};
    update_.state = catalog::CatalogSessionState::loading;
    notify();
}
void CatalogController::refresh_directory_sources() {
    if (search_active()) { places_->refresh_current_directory(); return; }
    automatic_ = false;
    if (recursive_active()) navigation_->remember_refresh_position();
    if (!recursive_active()) monitor_.scan_started();
    session_.refresh();
    update_.state = catalog::CatalogSessionState::loading;
    selection_->update_clipboard_actions();
    if (recursive_active()) {
        update_.truncated = false;
        update_.error = {};
        commands_->update_actions();
        update_tools();
    } else places_->refresh_current_directory();
}
void CatalogController::resume_polling(const int interval_ms) {
    if (interval_ms >= 0) pollTimer_.setInterval(interval_ms);
    pollTimer_.start();
}
void CatalogController::request_recheck() { monitor_.request_recheck(); }
bool CatalogController::automatic_refresh_blocked() const {
    return !ordinary_view() || !views_.window->isVisible() || views_.window->isMinimized() ||
        QApplication::activeModalWidget() || QApplication::activePopupWidget() ||
        QApplication::mouseButtons() != Qt::NoButton ||
        (callbacks_.drag_active && callbacks_.drag_active()) || commands_->automatic_refresh_blocked();
}
void CatalogController::update_monitor_visibility() {
    monitor_.set_visible(views_.window->isVisible() && !views_.window->isMinimized() && ordinary_view());
}
bool CatalogController::refresh_automatically() {
    if (automatic_refresh_blocked() || !source_.idle() ||
        update_.state == catalog::CatalogSessionState::loading ||
        update_.state == catalog::CatalogSessionState::waiting_retry) return false;
    automatic_ = true;
    beforeAutomatic_ = update_;
    monitor_.scan_started();
    session_.refresh();
    update_.state = catalog::CatalogSessionState::loading;
    selection_->update_clipboard_actions();
    resume_polling(16);
    return true;
}
void CatalogController::poll() {
    if (search_active() || trash_active()) return;
    if (automatic_ && automatic_refresh_blocked()) {
        session_.suspend();
        pollTimer_.stop();
        update_ = beforeAutomatic_;
        automatic_ = false;
        monitor_.scan_finished(true);
        monitor_.request_recheck();
        selection_->update_clipboard_actions();
        return;
    }
    const auto automatic = automatic_;
    std::optional<QSignalBlocker> blocker;
    if (automatic) blocker.emplace(views_.grid->selectionModel());
    const auto previous = update_.state;
    const auto grid = navigation_->capture_grid();
    catalog::CatalogSessionUpdate next;
    std::size_t received{};
    bool changed{};
    for (int batch = 0; batch < 8; ++batch) {
        auto result = session_.poll();
        received += result.entries_received;
        changed = changed || result.model_changed;
        const bool progressed = result.entries_received != 0 || result.model_changed;
        next = std::move(result);
        if (!progressed) break;
    }
    next.entries_received = received;
    next.model_changed = changed;
    if (automatic && next.state == catalog::CatalogSessionState::ready && !model_.last_replacement_changed())
        next.model_changed = false;
    if (next.model_changed) {
        navigation_->restore_grid(grid);
        if (automatic) commands_->update_actions();
        if (next.state == catalog::CatalogSessionState::ready) {
            if (!removedPaths_.isEmpty()) {
                for (int row = 0; row < model_.rowCount(); ++row) {
                    const auto *entry = model_.entry_at(model_.index(row, 0));
                    if (entry && !entry->source_revision_utf8.empty())
                        observe_search_path(QString::fromUtf8(entry->path_utf8));
                }
            }
            if (automatic && ordinary_view())
                places_->reconcile_current_directory(model_.directory_entries(), !next.truncated);
            if (ordinary_view()) places_->update_directory_tree();
            preview_->schedule();
        }
        if (recursive_active()) preview_->scheduleIfIdle();
    }
    if (next.state == catalog::CatalogSessionState::ready) {
        navigation_->catalog_ready();
    }
    update_ = next;
    if (recursive_active()) commands_->update_actions();
    sync_preview_context();
    preview_->catalogProgress(update_, previous, automatic, next.model_changed);
    if (!automatic || update_.state != catalog::CatalogSessionState::loading) notify();
    switch (update_.state) {
    case catalog::CatalogSessionState::ready:
    case catalog::CatalogSessionState::network_disconnected:
    case catalog::CatalogSessionState::timed_out:
    case catalog::CatalogSessionState::authentication_required:
    case catalog::CatalogSessionState::permission_denied:
    case catalog::CatalogSessionState::unavailable:
        pollTimer_.stop();
        monitor_.scan_finished(update_.state == catalog::CatalogSessionState::ready);
        automatic_ = false;
        break;
    case catalog::CatalogSessionState::waiting_retry: pollTimer_.setInterval(100); break;
    default: pollTimer_.setInterval(16); break;
    }
    selection_->update_clipboard_actions();
}
void CatalogController::apply_filter() {
    navigation_->cancel_restore();
    views_.global_search->setEnabled(!trash_active() && (search_active() || views_.filter->text().isEmpty()));
    if (search_active()) { notify(); return; }
    model_.set_filter(views_.filter->text());
    notify();
    if (callbacks_.settings_changed) callbacks_.settings_changed();
}
void CatalogController::apply_sort() {
    const auto field = static_cast<core::SortField>(views_.sort->currentData().toInt());
    const auto direction = views_.sort_direction->isChecked() ? core::SortDirection::descending : core::SortDirection::ascending;
    const auto text = ui_text(UiTextId::sort_direction) + QStringLiteral(": ") +
        ui_text(views_.sort_direction->isChecked() ? UiTextId::descending : UiTextId::ascending);
    views_.sort_direction->setToolTip(text);
    views_.sort_direction->setAccessibleName(text);
    const auto grid = navigation_->capture_grid();
    {
        const QSignalBlocker blocker(views_.grid->selectionModel());
        model_.set_sort(field, direction);
        navigation_->restore_grid(grid);
    }
    preview_->refreshSelectedMetadata();
    commands_->update_actions();
    notify();
    if (callbacks_.settings_changed) callbacks_.settings_changed();
}
void CatalogController::apply_thumbnail_extent(const int extent) {
    const bool changed = views_.delegate->thumbnail_extent() != std::clamp(extent, 64, 320);
    views_.delegate->set_thumbnail_extent(extent);
    views_.thumbnail_scale->setToolTip(QCoreApplication::translate("MainWindow", "Thumbnail size: %1").arg(views_.delegate->thumbnail_extent()));
    views_.grid->setGridSize(views_.delegate->sizeHint({}, {}));
    views_.grid->doItemsLayout();
    preview_->setThumbnailExtent(views_.delegate->thumbnail_extent());
    if (changed) { preview_->startGeneration(); preview_->schedule(); }
    if (callbacks_.settings_changed) callbacks_.settings_changed();
}

void CatalogController::schedule_search() {
    views_.filter->setEnabled(!search_active() && views_.global_search->text().trimmed().isEmpty());
    if (search_active()) update_tools();
    if (views_.global_search->text().trimmed().isEmpty()) {
        searchTimer_.stop();
        if (search_active()) leave_search(true);
        return;
    }
    searchTimer_.start();
}
void CatalogController::start_search() {
    if (!views_.global_search->isEnabled()) { searchTimer_.stop(); return; }
    if (commands_->mutation_in_flight()) {
        if (!views_.global_search->text().trimmed().isEmpty()) searchTimer_.start();
        return;
    }
    commands_->cancel_search_drop();
    const auto query = views_.global_search->text().trimmed();
    if (query.isEmpty()) { if (search_active()) leave_search(true); return; }
    if (recursive_active()) leave_recursive(true);
    search_.cancel();
    searchGeneration_ = 0;
    query_ = query;
    searchReady_ = false;
    commands_->clear_completed_status();
    if (!search_active()) {
        monitor_.set_visible(false);
        monitor_.set_directory({});
        automatic_ = false;
        session_.suspend();
        pollTimer_.stop();
        beforeSearch_ = take_view();
        savedSearchDirty_ = false;
        mode_ = CatalogViewMode::global_search;
        places_->set_location(path_, false, true);
        views_.window->setProperty("globalSearchActive", true);
        views_.filter->setEnabled(false);
        views_.grid->setDragEnabled(false);
        views_.grid->setAcceptDrops(false);
        views_.grid->viewport()->setAcceptDrops(false);
        views_.grid->setDragDropMode(QAbstractItemView::DragOnly);
        model_.set_filter({});
    }
    sync_preview_context();
    preview_->leaveProtectedState();
    preview_->startGeneration();
    model_.begin_catalog();
    preview_->clearSelection();
    searchStatus_ = QCoreApplication::translate("MainWindow", "Global search…");
    searchCount_ = 0;
    searchTruncated_ = false;
    show_search_message(QCoreApplication::translate("MainWindow", "Searching the system filename index…"));
    notify();
    commands_->update_actions();
    if (searchRoots_.isEmpty()) {
        if (model_.loading()) model_.finish_catalog();
        searchStatus_ = QCoreApplication::translate("MainWindow", "Global search · scope not configured");
        show_search_message(QCoreApplication::translate("MainWindow", "Set work folders in <b>VO-VE → Settings…</b>."));
        notify();
        return;
    }
    searchGeneration_ = search_.start(query, searchRoots_);
}
void CatalogController::handle_search_batch(search::SearchBatch batch) {
    if (!search_active() || searchReady_ || batch.generation != searchGeneration_) return;
    if (batch.status != search::SearchStatus::success) {
        if (model_.loading()) model_.abort_catalog();
        preview_->schedule();
        searchStatus_ = batch.status == search::SearchStatus::timed_out
            ? QCoreApplication::translate("MainWindow", "Global search timed out")
            : QCoreApplication::translate("MainWindow", "Global search is unavailable");
        switch (batch.status) {
        case search::SearchStatus::provider_unavailable: show_search_message(unavailable_message()); break;
        case search::SearchStatus::scope_not_indexed: {
            const auto root = QString::fromUtf8(batch.message_utf8).toHtmlEscaped();
            show_search_message(scope_message(root.isEmpty() ? QString{} : QStringLiteral("<br><code>%1</code>").arg(root)));
            break;
        }
        case search::SearchStatus::timed_out: show_search_message(QCoreApplication::translate("MainWindow", "The filename index did not respond in time. Retry the search.")); break;
        case search::SearchStatus::invalid_request: show_search_message(QCoreApplication::translate("MainWindow", "Enter search text and select an available search scope.")); break;
        case search::SearchStatus::io_error: show_search_message(QCoreApplication::translate("MainWindow", "Failed to get results from the filename index.")); break;
        case search::SearchStatus::cancelled: show_search_message(QCoreApplication::translate("MainWindow", "Search cancelled.")); break;
        default: break;
        }
        notify();
        return;
    }
    std::erase_if(batch.entries, [this](const auto &entry) { return removed_search_path(entry.path_utf8); });
    searchCount_ += static_cast<std::uint64_t>(batch.entries.size());
    if (!batch.entries.empty()) { model_.append_catalog(std::move(batch.entries)); views_.search_message->hide(); }
    searchTruncated_ = searchTruncated_ || batch.truncated;
    if (batch.is_final) {
        if (model_.loading()) model_.finish_catalog();
        searchReady_ = true;
        searchCount_ = model_.total_size();
        searchStatus_ = QCoreApplication::translate("MainWindow", "Global search · %1 found").arg(static_cast<qulonglong>(searchCount_));
        if (batch.provider_index_modified_unix_ns > 0) {
            const auto date = QDateTime::fromMSecsSinceEpoch(batch.provider_index_modified_unix_ns / 1'000'000);
            searchStatus_ += QCoreApplication::translate("MainWindow", " · index %1").arg(QLocale().toString(date.toLocalTime(), QLocale::ShortFormat));
        }
        if (searchTruncated_) searchStatus_ += QCoreApplication::translate("MainWindow", " · results limited");
        if (searchCount_ == 0) show_search_message(QCoreApplication::translate("MainWindow", "Nothing found"));
    } else {
        searchStatus_ = QCoreApplication::translate("MainWindow", "Global search… · %1 found").arg(static_cast<qulonglong>(searchCount_));
    }
    preview_->schedule();
    notify();
}
bool CatalogController::removed_search_path(const std::string &path) const {
    if (removedPaths_.isEmpty()) return false;
    auto current = native_path(QString::fromUtf8(path)).lexically_normal();
    if (observedPaths_.contains(path_key(current))) return false;
    while (!current.empty()) {
        if (removedPaths_.contains(path_key(current))) return true;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return false;
}
void CatalogController::observe_search_path(const QString &path) {
    if (!removed_search_path(path.toUtf8().toStdString())) return;
    auto current = native_path(path).lexically_normal();
    while (!current.empty()) {
        if (observedPaths_.size() == static_cast<qsizetype>(search::kMaximumSearchResults)) break;
        observedPaths_.insert(path_key(current));
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}
void CatalogController::reconcile_search_move(const QStringList &paths) {
    if (paths.isEmpty()) return;
    QSet<QString> moved;
    for (const auto &path : paths) {
        if (removedPaths_.size() == static_cast<qsizetype>(search::kMaximumSearchResults)) break;
        const auto key = path_key(native_path(path));
        removedPaths_.insert(key);
        moved.insert(key);
    }
    for (auto it = observedPaths_.begin(); it != observedPaths_.end();) {
        auto candidate = *it;
        bool removed{};
        while (!candidate.isEmpty()) {
            if (moved.contains(candidate)) { removed = true; break; }
            const auto slash = candidate.lastIndexOf(QLatin1Char('/'));
            if (slash < 0) break;
            candidate.truncate(slash);
        }
        if (removed) it = observedPaths_.erase(it); else ++it;
    }
    if (!search_active()) return;
    const auto grid = navigation_->capture_grid();
    std::vector<core::DirectoryEntry> remaining;
    for (int row = 0; row < model_.rowCount(); ++row) {
        const auto *entry = model_.entry_at(model_.index(row, 0));
        if (entry && !removed_search_path(entry->path_utf8)) remaining.push_back(*entry);
    }
    model_.replace_catalog(std::move(remaining));
    navigation_->restore_grid(grid);
    if (!model_.entry_for_id(preview_->selectedEntryId())) preview_->resetSelectedPreview();
    searchCount_ = model_.total_size();
    searchStatus_ = QCoreApplication::translate("MainWindow", "Global search · %1 found").arg(static_cast<qulonglong>(searchCount_));
    if (searchTruncated_) searchStatus_ += QCoreApplication::translate("MainWindow", " · results limited");
    if (searchCount_ == 0) show_search_message(QCoreApplication::translate("MainWindow", "Nothing found"));
}
void CatalogController::leave_search(const bool restore_catalog) {
    commands_->cancel_search_drop();
    searchTimer_.stop();
    search_.cancel();
    searchGeneration_ = 0;
    searchCount_ = 0;
    searchTruncated_ = false;
    searchStatus_.clear();
    if (model_.loading()) model_.abort_catalog();
    mode_ = CatalogViewMode::directory;
    searchReady_ = false;
    query_.clear();
    monitor_.set_directory(path_);
    update_monitor_visibility();
    views_.window->setProperty("globalSearchActive", false);
    views_.search_message->hide();
    views_.filter->setEnabled(true);
    views_.grid->setDragEnabled(true);
    views_.grid->setAcceptDrops(true);
    views_.grid->viewport()->setAcceptDrops(true);
    views_.grid->setDragDropMode(QAbstractItemView::DragDrop);
    model_.set_filter(views_.filter->text());
    commands_->update_actions();
    auto saved = std::exchange(beforeSearch_, std::nullopt);
    const bool dirty = std::exchange(savedSearchDirty_, false);
    sync_preview_context();
    if (!restore_catalog || path_.isEmpty() || !saved) return;
    restore_view(std::move(*saved));
    if (dirty || update_.state == catalog::CatalogSessionState::loading ||
        update_.state == catalog::CatalogSessionState::waiting_retry) {
        refresh_directory_sources();
        update_.state = catalog::CatalogSessionState::loading;
        update_.truncated = false;
        update_.error = {};
        update_.retry_in.reset();
        resume_polling(16);
    }
    preview_->schedule();
    notify();
}
CatalogController::SavedView CatalogController::take_view() {
    SavedView saved;
    saved.grid = navigation_->capture_grid();
    saved.update = update_;
    preview_->startGeneration(false);
    saved.preview = preview_->takeCatalogState();
    saved.model = model_.take_snapshot();
    return saved;
}
void CatalogController::restore_view(SavedView saved) {
    preview_->startGeneration(false);
    model_.restore_snapshot(std::move(saved.model));
    model_.set_filter(views_.filter->text());
    model_.set_sort(static_cast<core::SortField>(views_.sort->currentData().toInt()),
        views_.sort_direction->isChecked() ? core::SortDirection::descending : core::SortDirection::ascending);
    sync_preview_context();
    preview_->restoreCatalogState(std::move(saved.preview));
    navigation_->restore_grid(saved.grid);
    places_->set_location(path_);
    update_ = saved.update;
    update_.entries_received = 0;
    update_.model_changed = false;
    preview_->schedule();
}
void CatalogController::set_recursive(const bool enabled) {
    if (enabled == recursive_active()) return;
    if (!enabled) { leave_recursive(true); return; }
    if (!ordinary_view() || path_.isEmpty() || update_.state != catalog::CatalogSessionState::ready ||
        !commands_->availability().create_directory_allowed()) { update_tools(); return; }
    navigation_->save();
    navigation_->cancel_restore();
    monitor_.set_visible(false);
    monitor_.set_directory({});
    automatic_ = false;
    session_.suspend();
    pollTimer_.stop();
    beforeRecursive_ = take_view();
    mode_ = CatalogViewMode::recursive;
    places_->set_location(path_, false, true);
    navigation_->catalog_started();
    views_.window->setProperty("recursiveViewActive", true);
    static_cast<CatalogView *>(views_.grid)->set_recursive_mode(true);
    commands_->clear_completed_status();
    sync_preview_context();
    preview_->leaveProtectedState();
    preview_->startGeneration();
    session_.open(native_path(path_), catalog::kRecursiveCatalogMaximumEntries, true);
    preview_->selectedEntryChanged({});
    update_ = {};
    resume_polling(16);
    commands_->update_actions();
    notify();
}
void CatalogController::leave_recursive(const bool restore_catalog) {
    if (!recursive_active()) return;
    session_.suspend();
    pollTimer_.stop();
    mode_ = CatalogViewMode::directory;
    views_.window->setProperty("recursiveViewActive", false);
    static_cast<CatalogView *>(views_.grid)->set_recursive_mode(false);
    auto saved = std::exchange(beforeRecursive_, std::nullopt);
    navigation_->cancel_restore();
    sync_preview_context();
    if (restore_catalog && saved) {
        restore_view(std::move(*saved));
        navigation_->catalog_ready();
        monitor_.set_directory(path_);
        update_monitor_visibility();
        session_.open(native_path(path_), 100'000, false, true);
        monitor_.scan_started();
        update_ = {};
        resume_polling(16);
        commands_->update_actions();
        notify();
    }
    update_tools();
}
void CatalogController::update_tools() {
    if (!commands_ || !views_.recursive) return;
    const bool busy = commands_->catalog_tools_busy();
    views_.recursive->setChecked(recursive_active());
    views_.recursive->setEnabled(!busy && !trash_active() && !search_active() &&
        (recursive_active() || (update_.state == catalog::CatalogSessionState::ready &&
                               commands_->availability().create_directory_allowed())));
    const auto eye = recursive_active() ? QCoreApplication::translate("MainWindow", "Return to ordinary folder view")
        : QCoreApplication::translate("MainWindow", "Show files from all subfolders");
    views_.recursive->setToolTip(eye);
    views_.recursive->setAccessibleName(eye);
    const bool recovery = commands_->recovery_pending();
    const auto refresh = recovery ? QCoreApplication::translate("MainWindow", "Resume interrupted operation (F5)")
        : QCoreApplication::translate("MainWindow", "Refresh folder and previews (F5)");
    views_.refresh->setToolTip(refresh);
    views_.refresh->setAccessibleName(refresh);
    if (search_active()) {
        const bool ready = searchReady_ && !busy && !recovery && search_query_current() &&
            !commands_->journal_blocked() && removedPaths_.size() < static_cast<qsizetype>(search::kMaximumSearchResults);
        views_.grid->setDragEnabled(ready);
        model_.set_drag_and_edit_enabled(ready, false);
    } else if (recursive_active()) {
        const bool ready = update_.state == catalog::CatalogSessionState::ready;
        views_.grid->setDragEnabled(ready);
        model_.set_drag_and_edit_enabled(ready);
    } else if (ordinary_view()) {
        views_.grid->setDragEnabled(true);
        model_.set_drag_and_edit_enabled(true);
    }
}
void CatalogController::enter_trash() {
    navigation_->save();
    navigation_->cancel_restore();
    leave_recursive(false);
    if (search_active() || !views_.global_search->text().isEmpty()) {
        const QSignalBlocker blocker(views_.global_search);
        views_.global_search->clear();
        leave_search(false);
    }
    monitor_.set_visible(false);
    monitor_.set_directory({});
    session_.suspend();
    pollTimer_.stop();
    automatic_ = false;
    mode_ = CatalogViewMode::trash;
    views_.window->setProperty("trashViewActive", true);
    views_.global_search->setEnabled(false);
    views_.grid->setDragEnabled(false);
    views_.grid->setAcceptDrops(false);
    views_.grid->viewport()->setAcceptDrops(false);
    views_.grid->setDragDropMode(QAbstractItemView::NoDragDrop);
    model_.set_tooltip_paths({});
    model_.replace_catalog({});
    model_.set_drag_and_edit_enabled(false);
    sync_preview_context();
    preview_->startGeneration();
    preview_->clearSelection();
    navigation_->show_location_caption(QCoreApplication::translate("MainWindow", "VO-VE Trash"));
    places_->set_location(path_, true);
    navigation_->update_actions();
    commands_->update_actions();
    notify();
}
void CatalogController::leave_trash() {
    if (!trash_active()) return;
    mode_ = CatalogViewMode::directory;
    views_.window->setProperty("trashViewActive", false);
    model_.set_tooltip_paths({});
    model_.replace_catalog({});
    model_.set_drag_and_edit_enabled(true);
    views_.global_search->setEnabled(views_.filter->text().isEmpty());
    views_.grid->setDragEnabled(true);
    views_.grid->setAcceptDrops(true);
    views_.grid->viewport()->setAcceptDrops(true);
    views_.grid->setDragDropMode(QAbstractItemView::DragDrop);
    views_.filter->setEnabled(true);
    sync_preview_context();
}
void CatalogController::replace_trash_entries(std::vector<core::DirectoryEntry> entries,
                                              QHash<qulonglong, QString> tooltips) {
    preview_->startGeneration();
    model_.replace_catalog(std::move(entries));
    model_.set_tooltip_paths(std::move(tooltips));
    model_.set_filter(views_.filter->text());
    model_.set_sort(static_cast<core::SortField>(views_.sort->currentData().toInt()),
        views_.sort_direction->isChecked() ? core::SortDirection::descending : core::SortDirection::ascending);
    preview_->schedule(0);
    commands_->update_actions();
    notify();
}
void CatalogController::set_search_roots(QStringList roots) { searchRoots_ = std::move(roots); }
void CatalogController::mark_saved_search_dirty() { savedSearchDirty_ = true; }
bool CatalogController::search_query_current() const {
    return !search_active() || (views_.global_search && query_ == views_.global_search->text().trimmed());
}
void CatalogController::show_entry_in_folder(const core::DirectoryEntry &entry) {
    const auto path = QString::fromUtf8(entry.path_utf8);
    const auto parent = QFileInfo(path).absolutePath();
    if (parent.isEmpty()) return;
    { const QSignalBlocker blocker(views_.global_search); views_.global_search->clear(); }
    if (search_active()) leave_search(false);
    views_.filter->clear();
    navigation_->reveal_after_refresh({path}, true);
    navigation_->open(parent);
}
void CatalogController::show_search_message(const QString &text) {
    views_.search_message->setText(text);
    update_search_message_geometry();
    views_.search_message->show();
    views_.search_message->raise();
}
void CatalogController::update_search_message_geometry() {
    const auto bounds = views_.grid->viewport()->rect();
    views_.search_message->setGeometry(bounds.adjusted(std::max(16, bounds.width() / 8),
        std::max(16, bounds.height() / 5), -std::max(16, bounds.width() / 8), -std::max(16, bounds.height() / 5)));
}
} // namespace vove::ui
