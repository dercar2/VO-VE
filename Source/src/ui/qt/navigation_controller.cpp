#include "navigation_controller.hpp"

#include "directory_list_model.hpp"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>

#include <algorithm>
#include <utility>

namespace vove::ui {
namespace {
bool same_path(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(left)).compare(
               QDir::cleanPath(QDir::fromNativeSeparators(right)), sensitivity) == 0;
}
void call(const std::function<void()> &action) { if (action) action(); }
} // namespace

NavigationController::NavigationController(DirectoryListModel &model, NavigationViews views,
                                           NavigationActions actions, QObject *parent)
    : QObject(parent), model_(model), views_(views), actions_(std::move(actions)) {
    if (views_.back) connect(views_.back, &QAbstractButton::clicked, this, [this] { back(); });
    if (views_.forward) connect(views_.forward, &QAbstractButton::clicked, this, [this] { forward(); });
    if (views_.up) connect(views_.up, &QAbstractButton::clicked, this, [this] { up(); });
    if (views_.history) connect(views_.history, &QMenu::aboutToShow, this,
                                [this] { rebuild_history_menu(); });
    if (views_.grid) {
        views_.grid->installEventFilter(this);
        views_.grid->viewport()->installEventFilter(this);
        views_.grid->verticalScrollBar()->installEventFilter(this);
    }
}

bool NavigationController::eventFilter(QObject *watched, QEvent *event) {
    if (pending_ && event && views_.grid &&
        (watched == views_.grid || watched == views_.grid->viewport() ||
         watched == views_.grid->verticalScrollBar()) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel ||
         event->type() == QEvent::KeyPress || event->type() == QEvent::TouchBegin)) cancel_restore();
    return QObject::eventFilter(watched, event);
}

void NavigationController::open(const QString &path, const bool add_to_history,
                                 const QString &reveal_child) {
    auto normalized = path.trimmed();
    if (normalized.size() >= 2 && normalized.front() == QLatin1Char('"') &&
        normalized.back() == QLatin1Char('"')) normalized = normalized.mid(1, normalized.size() - 2);
    if (normalized.isEmpty()) return;
    if (add_to_history) save();
    call(actions_.prepare_directory);
    pending_.reset();
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
    if (QDir::isRelativePath(normalized)) {
        normalized = QDir::cleanPath(QDir(path_.isEmpty() ? QDir::currentPath() : path_)
                                        .absoluteFilePath(normalized));
    }
    if (add_to_history) history_.visit(normalized);
    pending_ = history_.restore_for(normalized, reveal_child);
    path_ = normalized;
    ready_ = false;
    if (views_.address) views_.address->setText(QDir::toNativeSeparators(path_));
    call(actions_.settings_changed);
    if (actions_.open_directory) actions_.open_directory(path_);
    update_actions();
}

void NavigationController::back() {
    if (actions_.trash_view && actions_.trash_view()) { open(path_, false); return; }
    if (history_.can_go_back()) open_history(history_.index() - 1);
}
void NavigationController::forward() {
    if (history_.can_go_forward()) open_history(history_.index() + 1);
}
void NavigationController::up() {
    const auto parent = QFileInfo(path_).dir().absolutePath();
    if (!parent.isEmpty() && parent != path_) open(parent, true, path_);
}
void NavigationController::choose_directory(const QString &title) {
    auto options = QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks;
#ifdef VOVE_UI_TEST_HOOKS
    options |= QFileDialog::DontUseNativeDialog;
#endif
    const auto chosen = QFileDialog::getExistingDirectory(views_.dialog_owner, title, path_, options);
    if (!chosen.isEmpty()) open(chosen);
}
void NavigationController::show_location_caption(const QString &caption) {
    if (views_.address) views_.address->setText(caption);
}
void NavigationController::save() {
    if (ready_ && (!actions_.ordinary_view || actions_.ordinary_view())) history_.save(path_, capture_grid());
}
void NavigationController::catalog_started() { ready_ = false; }
void NavigationController::catalog_ready() {
    ready_ = !actions_.ordinary_view || actions_.ordinary_view();
    restore_navigation();
    restore_reveal();
    update_actions();
}
void NavigationController::remember_refresh_position() {
    if (pending_) return;
    auto grid = capture_grid();
    if (grid.selected_ids.size() > 512) grid.selected_ids.resize(512);
    pending_ = PendingNavigationRestore{path_, std::move(grid), {}};
}
void NavigationController::cancel_restore() {
    const bool had_pending = pending_.has_value();
    pending_.reset();
    if (had_pending) call(actions_.previews_changed);
}
void NavigationController::open_history(const int index) {
    if (index < 0 || index >= history_.entries().size()) return;
    save();
    if (history_.select(index)) open(history_.current()->path, false);
}
void NavigationController::rebuild_history_menu() {
    if (!views_.history) return;
    views_.history->clear();
    const auto &entries = history_.entries();
    for (int index = static_cast<int>(entries.size()) - 1; index >= 0; --index) {
        auto *action = views_.history->addAction(QDir::toNativeSeparators(entries[index].path));
        action->setCheckable(true);
        action->setChecked(index == history_.index());
        action->setData(index);
        connect(action, &QAction::triggered, this, [this, index] { open_history(index); });
    }
}
void NavigationController::update_actions() {
    const bool trash = actions_.trash_view && actions_.trash_view();
    if (views_.back) views_.back->setEnabled(trash || history_.can_go_back());
    if (views_.forward) views_.forward->setEnabled(!trash && history_.can_go_forward());
    if (views_.up) views_.up->setEnabled(!trash);
    if (views_.history_button) views_.history_button->setEnabled(!history_.entries().isEmpty());
}

void NavigationController::restore_navigation() {
    if (!pending_) return;
    auto pending = std::move(*pending_);
    pending_.reset();
    if (!same_path(pending.path, path_) || (actions_.restore_allowed && !actions_.restore_allowed())) return;
    QModelIndex child;
    if (!pending.reveal_child.isEmpty()) {
        for (int row = 0; row < model_.rowCount(); ++row) {
            const auto index = model_.index(row, 0);
            const auto *entry = model_.entry_at(index);
            if (entry && entry->kind == core::EntryKind::directory &&
                same_path(QString::fromUtf8(entry->path_utf8), pending.reveal_child)) {
                child = index;
                pending.grid.current_id = entry->id;
                if (std::ranges::find(pending.grid.selected_ids, entry->id) == pending.grid.selected_ids.end()) {
                    if (pending.grid.selected_ids.size() >= 512) pending.grid.selected_ids.pop_back();
                    pending.grid.selected_ids.push_back(entry->id);
                }
                break;
            }
        }
    }
    {
        const QSignalBlocker blocker(views_.grid->selectionModel());
        restore_grid(pending.grid);
        if (child.isValid() && !views_.grid->viewport()->rect().intersects(views_.grid->visualRect(child)))
            views_.grid->scrollTo(child, QAbstractItemView::EnsureVisible);
    }
    call(actions_.selection_changed);
    call(actions_.commands_changed);
    call(actions_.previews_changed);
}

GridViewState NavigationController::capture_grid() const {
    GridViewState state;
    state.scroll_value = views_.grid->verticalScrollBar()->value();
    state.current_id = views_.grid->currentIndex().data(DirectoryListModel::EntryIdRole).toULongLong();
    const auto selected = views_.grid->selectionModel()->selectedIndexes();
    state.selected_ids.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto &index : selected) state.selected_ids.push_back(index.data(DirectoryListModel::EntryIdRole).toULongLong());
    int first = 0;
    int last = model_.rowCount();
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (views_.grid->visualRect(model_.index(middle, 0)).bottom() < 0) first = middle + 1;
        else last = middle;
    }
    const auto top = model_.index(first, 0);
    if (top.isValid()) {
        state.top_id = top.data(DirectoryListModel::EntryIdRole).toULongLong();
        state.top_offset = views_.grid->visualRect(top).top();
        state.has_top = true;
    }
    return state;
}
void NavigationController::restore_grid(const GridViewState &state) {
    auto *selection = views_.grid->selectionModel();
    QSet<qulonglong> ids;
    ids.reserve(static_cast<qsizetype>(state.selected_ids.size()));
    for (const auto id : state.selected_ids) ids.insert(static_cast<qulonglong>(id));
    const auto ranges = model_.selection_for_ids(ids);
    selection->select(ranges, QItemSelectionModel::ClearAndSelect);
    const auto row = model_.row_for_id(state.current_id);
    if (row >= 0) selection->setCurrentIndex(model_.index(row, 0), QItemSelectionModel::NoUpdate);
    else if (!ranges.isEmpty()) selection->setCurrentIndex(ranges.front().topLeft(), QItemSelectionModel::NoUpdate);
    else selection->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
    const auto top = state.has_top ? model_.row_for_id(state.top_id) : -1;
    if (top < 0) { views_.grid->verticalScrollBar()->setValue(state.scroll_value); return; }
    views_.grid->doItemsLayout();
    const auto index = model_.index(top, 0);
    views_.grid->scrollTo(index, QAbstractItemView::PositionAtTop);
    views_.grid->verticalScrollBar()->setValue(views_.grid->verticalScrollBar()->value() -
                                               state.top_offset + views_.grid->visualRect(index).top());
}
QModelIndex NavigationController::index_for_path(const QString &path) const {
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (int row = 0; row < model_.rowCount(); ++row) {
        const auto index = model_.index(row, 0);
        if (QDir::cleanPath(QDir::fromNativeSeparators(index.data(DirectoryListModel::EntryPathRole).toString())) == normalized)
            return index;
    }
    return {};
}
void NavigationController::reveal_after_refresh(const QStringList &paths, const bool from_search) {
    reveal_paths_.clear();
    for (const auto &path : paths) reveal_paths_.push_back(QDir::cleanPath(QDir::fromNativeSeparators(path)));
    reveal_from_search_ = from_search;
}
void NavigationController::restore_reveal() {
    if (reveal_paths_.isEmpty()) return;
    QItemSelection selection;
    QModelIndex first;
    for (const auto &path : std::as_const(reveal_paths_)) {
        const auto index = index_for_path(path);
        if (!index.isValid()) continue;
        selection.select(index, index);
        if (!first.isValid()) first = index;
    }
    reveal_paths_.clear();
    if (!first.isValid()) {
        if (std::exchange(reveal_from_search_, false) && actions_.message)
            actions_.message(QCoreApplication::translate("MainWindow", "The file can no longer be found. It may have been moved or deleted after the search."));
        return;
    }
    views_.grid->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    views_.grid->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    views_.grid->scrollTo(first, QAbstractItemView::EnsureVisible);
    reveal_from_search_ = false;
}
} // namespace vove::ui
