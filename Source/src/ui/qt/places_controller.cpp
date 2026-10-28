#include "places_controller.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace vove::ui {
namespace {
constexpr int favoriteKindRole = Qt::UserRole + 1;
constexpr int regularFavoriteKind = 0;
constexpr int trashFavoriteKind = 1;
}

PlacesController::PlacesController(PlacesViewPort views, PlacesCallbacks callbacks, QObject *parent)
    : QObject(parent), views_(std::move(views)), callbacks_(std::move(callbacks)),
      treeModel_(std::make_unique<LazyDirectoryTreeModel>(treeSource_)) {
    treeModel_->add_roots(LazyDirectoryTreeModel::detected_roots());
    if (views_.tree) {
        views_.tree->setModel(treeModel_.get());
        const auto open = [this](const QModelIndex &index) {
            const auto path = index.data(DirectoryTreeModel::PathRole).toString();
            if (!path.isEmpty() && (trashView_ || aggregateView_ || !same_directory_path(path, currentPath_))) {
                if (callbacks_.open_requested_path) callbacks_.open_requested_path(path);
            } else update_location_selection();
        };
        connect(views_.tree, &QTreeView::clicked, this, open);
        connect(views_.tree, &QTreeView::activated, this, open);
        connect(views_.tree, &QTreeView::expanded, this, [this](const QModelIndex &index) { treeModel_->load_children(index); });
        connect(views_.tree, &QTreeView::collapsed, this, [this](const QModelIndex &index) { treeModel_->cancel(index); });
        connect(treeModel_.get(), &QAbstractItemModel::rowsAboutToBeInserted, this, [this] {
            if (!views_.tree || std::exchange(insertBatchPending_, true)) return;
            const QPersistentModelIndex current(views_.tree->currentIndex());
            const auto row = views_.tree->visualRect(current);
            revealCurrentAfterInsert_ = !row.isEmpty() && views_.tree->viewport()->rect().intersects(row);
            QTimer::singleShot(0, this, [this, current] {
                insertBatchPending_ = false;
                if (!views_.tree || !std::exchange(revealCurrentAfterInsert_, false) ||
                    current != views_.tree->currentIndex()) return;
                for (auto parent = current.parent(); parent.isValid(); parent = parent.parent())
                    if (!views_.tree->isExpanded(parent)) return;
                views_.tree->scrollTo(current);
            });
        });
        connect(views_.tree->verticalScrollBar(), &QScrollBar::actionTriggered, this,
                [this] { revealCurrentAfterInsert_ = false; });
    }
    for (const auto &view : {views_.favorites, views_.trash_favorite}) {
        if (!view) continue;
        // Favorites are reorderable only via explicit commands, never an outgoing file drag.
        view->setDragEnabled(false);
        view->setDragDropMode(QAbstractItemView::DropOnly);
        connect(view, &QListWidget::itemClicked, this, &PlacesController::open_favorite);
        connect(view, &QListWidget::itemActivated, this, &PlacesController::open_favorite);
        connect(view, &QListWidget::customContextMenuRequested, this,
                [this, view](const QPoint &position) { if (view) show_context_menu(*view, position); });
    }
    if (views_.favorites && views_.favorites_scrollbar) {
        auto *internal = views_.favorites->verticalScrollBar();
        const auto sync = [this, internal](int minimum, int maximum) {
            if (!views_.favorites_scrollbar) return;
            views_.favorites_scrollbar->setRange(minimum, maximum);
            views_.favorites_scrollbar->setSingleStep(internal->singleStep());
            views_.favorites_scrollbar->setPageStep(internal->pageStep());
            views_.favorites_scrollbar->setValue(internal->value());
        };
        connect(internal, &QScrollBar::rangeChanged, this, sync);
        connect(internal, &QScrollBar::valueChanged, views_.favorites_scrollbar, &QScrollBar::setValue);
        connect(views_.favorites_scrollbar, &QScrollBar::valueChanged, internal, &QScrollBar::setValue);
        sync(internal->minimum(), internal->maximum());
    }
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) treeModel_->add_roots(LazyDirectoryTreeModel::detected_roots());
    });
    add_trash_favorite();
}

PlacesController::~PlacesController() {
    if (views_.tree && views_.tree->model() == treeModel_.get()) views_.tree->setModel(nullptr);
}
QString PlacesController::tr(const char *text) { return QCoreApplication::translate("MainWindow", text); }
bool PlacesController::same_directory_path(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(left)).compare(
        QDir::cleanPath(QDir::fromNativeSeparators(right)), sensitivity) == 0;
}
bool PlacesController::is_trash_favorite(const QListWidgetItem *item) {
    return item && item->data(favoriteKindRole).toInt() == trashFavoriteKind;
}
QList<FavoritePlace> PlacesController::favorite_places() const {
    QList<FavoritePlace> result;
    if (!views_.favorites) return result;
    for (int row = 0; row < views_.favorites->count(); ++row) {
        const auto *item = views_.favorites->item(row);
        if (!is_trash_favorite(item)) result.push_back({item->text(), item->data(Qt::UserRole).toString()});
    }
    return result;
}
void PlacesController::restore_favorites(const QList<FavoritePlace> &places) {
    if (!views_.favorites) return;
    restoring_ = true;
    views_.favorites->clear();
    for (const auto &place : places) {
        const auto before = views_.favorites->count();
        add_favorite(place.path);
        if (views_.favorites->count() > before && !place.name.trimmed().isEmpty())
            views_.favorites->item(before)->setText(place.name);
    }
    if (views_.favorites->count() == 0)
        add_favorite(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    add_trash_favorite();
    restoring_ = false;
    update_location_selection();
}
void PlacesController::add_favorite(const QString &path) {
    if (!views_.favorites || path.isEmpty()) return;
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (int row = 0; row < views_.favorites->count(); ++row)
        if (!is_trash_favorite(views_.favorites->item(row)) && same_directory_path(
                views_.favorites->item(row)->data(Qt::UserRole).toString(), normalized)) return;
    auto *item = new QListWidgetItem(QFileInfo(normalized).fileName());
    if (item->text().isEmpty()) item->setText(QDir::toNativeSeparators(normalized));
    item->setData(Qt::UserRole, normalized);
    item->setData(favoriteKindRole, regularFavoriteKind);
    item->setToolTip(QDir::toNativeSeparators(normalized));
    auto insertion = views_.favorites->count();
    for (int row = 0; row < insertion; ++row)
        if (is_trash_favorite(views_.favorites->item(row))) { insertion = row; break; }
    views_.favorites->insertItem(insertion, item);
    update_location_selection();
    if (!restoring_ && callbacks_.favorites_changed) callbacks_.favorites_changed();
}
void PlacesController::add_current_favorite() { if (!trashView_) add_favorite(currentPath_); }
void PlacesController::add_trash_favorite() {
    if (views_.favorites) {
        bool found{};
        for (int row = 0; row < views_.favorites->count(); ++row)
            found = found || is_trash_favorite(views_.favorites->item(row));
        if (!found) {
            auto *item = new QListWidgetItem(tr("Trash"));
            item->setData(favoriteKindRole, trashFavoriteKind);
            item->setToolTip(tr("VO-VE Trash"));
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            views_.favorites->addItem(item);
            views_.favorites->setRowHidden(views_.favorites->row(item), true);
        }
    }
    if (views_.trash_favorite && views_.trash_favorite->count() == 0) {
        auto *item = new QListWidgetItem(tr("Trash"));
        item->setData(favoriteKindRole, trashFavoriteKind);
        item->setToolTip(tr("VO-VE Trash"));
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        views_.trash_favorite->addItem(item);
        views_.trash_favorite->setFixedHeight(std::max(24, views_.trash_favorite->sizeHintForRow(0)));
    }
}
void PlacesController::rename_selected_favorite() {
    if (!views_.favorites) return;
    auto *item = views_.favorites->currentItem();
    if (!item || is_trash_favorite(item)) return;
    bool accepted{};
    const auto name = QInputDialog::getText(views_.favorites, tr("Rename favorite"), tr("Name"),
                                          QLineEdit::Normal, item->text(), &accepted);
    if (accepted) rename_selected_favorite(name);
}
bool PlacesController::rename_selected_favorite(const QString &name) {
    if (!views_.favorites) return false;
    auto *item = views_.favorites->currentItem();
    const auto trimmed = name.trimmed();
    if (!item || is_trash_favorite(item) || trimmed.isEmpty() || trimmed == item->text()) return false;
    item->setText(trimmed);
    if (callbacks_.favorites_changed) callbacks_.favorites_changed();
    return true;
}
void PlacesController::move_selected_favorite(int offset) {
    if (!views_.favorites) return;
    const auto row = views_.favorites->currentRow();
    const qint64 target = qint64(row) + offset;
    if (row < 0 || target < 0 || target >= views_.favorites->count() ||
        is_trash_favorite(views_.favorites->item(row)) ||
        is_trash_favorite(views_.favorites->item(static_cast<int>(target)))) return;
    auto *item = views_.favorites->takeItem(row);
    views_.favorites->insertItem(static_cast<int>(target), item);
    update_location_selection();
    views_.favorites->setCurrentItem(item);
    if (callbacks_.favorites_changed) callbacks_.favorites_changed();
}
void PlacesController::remove_selected_favorite() {
    if (!views_.favorites || !views_.favorites->currentItem() ||
        is_trash_favorite(views_.favorites->currentItem())) return;
    delete views_.favorites->takeItem(views_.favorites->currentRow());
    update_location_selection();
    if (callbacks_.favorites_changed) callbacks_.favorites_changed();
}
void PlacesController::set_location(QString path, bool trash, bool aggregate_view) {
    currentPath_ = std::move(path);
    trashView_ = trash;
    aggregateView_ = aggregate_view;
    update_directory_tree();
}
void PlacesController::update_directory_tree() {
    if (!views_.tree || currentPath_.isEmpty()) { update_location_selection(); return; }
    if (same_directory_path(views_.tree->property("locationPath").toString(), currentPath_)) {
        update_location_selection();
        return;
    }
    views_.tree->setProperty("locationPath", currentPath_);
    const auto current = treeModel_->ensure_path(currentPath_);
    treeModel_->refresh(current);
    for (auto parent = current.parent(); parent.isValid(); parent = parent.parent()) views_.tree->expand(parent);
    views_.tree->expand(current);
    update_location_selection();
    views_.tree->scrollTo(current);
}
void PlacesController::update_location_selection() {
    if (!views_.favorites || currentPath_.isEmpty()) return;
    const QSignalBlocker favorites_block(views_.favorites);
    QListWidgetItem *active{};
    if (!trashView_) for (int row = 0; row < views_.favorites->count(); ++row) {
        auto *item = views_.favorites->item(row);
        if (!is_trash_favorite(item) && same_directory_path(item->data(Qt::UserRole).toString(), currentPath_)) {
            active = item;
            break;
        }
    }
    views_.favorites->setCurrentItem(active);
    views_.favorites->clearSelection();
    if (views_.trash_favorite) {
        const QSignalBlocker trash_block(views_.trash_favorite);
        views_.trash_favorite->clearSelection();
        if (trashView_) {
            auto *item = views_.trash_favorite->item(0);
            views_.trash_favorite->setCurrentItem(item);
            if (item) item->setSelected(true);
        }
    }
    if (active) active->setSelected(true);
    if (!views_.tree || !views_.tree->selectionModel()) return;
    const QSignalBlocker tree_block(views_.tree->selectionModel());
    if (trashView_) views_.tree->selectionModel()->setCurrentIndex({}, QItemSelectionModel::Clear);
    else views_.tree->selectionModel()->setCurrentIndex(treeModel_->ensure_path(currentPath_),
        active ? QItemSelectionModel::Clear : QItemSelectionModel::ClearAndSelect);
}
void PlacesController::refresh_current_directory() {
    if (!currentPath_.isEmpty()) treeModel_->refresh(treeModel_->ensure_path(currentPath_));
}
void PlacesController::reconcile_current_directory(const QList<QPair<QString, QString>> &directories, bool complete) {
    if (!currentPath_.isEmpty() && !trashView_ && !aggregateView_)
        treeModel_->reconcile_children(treeModel_->ensure_path(currentPath_), directories, complete);
}
void PlacesController::retranslate() {
    for (const auto &view : {views_.favorites, views_.trash_favorite}) {
        if (!view) continue;
        for (int row = 0; row < view->count(); ++row) if (is_trash_favorite(view->item(row))) {
            view->item(row)->setText(tr("Trash"));
            view->item(row)->setToolTip(tr("VO-VE Trash"));
        }
    }
}
void PlacesController::open_favorite(QListWidgetItem *item) {
    if (!item) return;
    if (is_trash_favorite(item)) {
        if (callbacks_.file_command_intent) callbacks_.file_command_intent(PlacesCommand::show_trash);
        return;
    }
    const auto path = item->data(Qt::UserRole).toString();
    if (trashView_ || aggregateView_ || !same_directory_path(path, currentPath_)) {
        if (callbacks_.open_requested_path) callbacks_.open_requested_path(path);
    } else update_location_selection();
}
void PlacesController::show_context_menu(FavoritesView &view, const QPoint &position) {
    auto *item = view.itemAt(position);
    QMenu menu(&view);
    if (item) view.setCurrentItem(item);
    if (is_trash_favorite(item)) {
        auto *clear = menu.addAction(tr("Empty Trash\xE2\x80\xA6"));
        clear->setObjectName(QStringLiteral("emptyTrashAction"));
        clear->setEnabled(callbacks_.can_empty_trash && callbacks_.can_empty_trash());
        if (menu.exec(view.viewport()->mapToGlobal(position)) == clear && callbacks_.file_command_intent)
            callbacks_.file_command_intent(PlacesCommand::empty_trash);
        return;
    }
    if (&view != views_.favorites) return;
    auto *add = menu.addAction(tr("Add current folder"));
    auto *rename = menu.addAction(tr("Rename favorite"));
    rename->setObjectName(QStringLiteral("renameFavoriteAction"));
    menu.addSeparator();
    auto *up = menu.addAction(tr("Move up"));
    up->setObjectName(QStringLiteral("moveFavoriteUpAction"));
    auto *down = menu.addAction(tr("Move down"));
    down->setObjectName(QStringLiteral("moveFavoriteDownAction"));
    menu.addSeparator();
    auto *remove = menu.addAction(tr("Remove from Favorites"));
    remove->setObjectName(QStringLiteral("removeFavoriteAction"));
    const auto row = item ? view.row(item) : -1;
    rename->setEnabled(item != nullptr);
    up->setEnabled(row > 0 && !is_trash_favorite(view.item(row - 1)));
    down->setEnabled(row >= 0 && row + 1 < view.count() && !is_trash_favorite(view.item(row + 1)));
    remove->setEnabled(item != nullptr);
    const auto *chosen = menu.exec(view.viewport()->mapToGlobal(position));
    if (chosen == add) add_current_favorite();
    else if (chosen == rename) rename_selected_favorite();
    else if (chosen == up) move_selected_favorite(-1);
    else if (chosen == down) move_selected_favorite(1);
    else if (chosen == remove) remove_selected_favorite();
}

} // namespace vove::ui
