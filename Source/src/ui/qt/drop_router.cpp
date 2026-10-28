#include "drop_router.hpp"

#include "places_controller.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSet>
#include <QStyle>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace vove::ui {

bool trusted_internal_drop_source(const QObject *source, const QObject *view,
                                  const QObject *viewport) noexcept {
    return source != nullptr && (source == view || source == viewport);
}

namespace {
bool same_snapshot(const core::DirectoryEntry &left, const core::DirectoryEntry &right) {
    return left.kind == right.kind && left.path_utf8 == right.path_utf8 &&
           left.size_bytes == right.size_bytes && left.modified_unix_ns == right.modified_unix_ns &&
           left.source_revision_utf8 == right.source_revision_utf8;
}
bool directories_only(const QList<core::DirectoryEntry> &entries) {
    return !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::directory;
    });
}
bool objects_only(const QList<core::DirectoryEntry> &entries) {
    return !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
    });
}
}

std::optional<QStringList> local_file_paths(const QMimeData *mime) {
    constexpr qsizetype maximum_paths = 1024;
    constexpr qsizetype maximum_path_units = 32767;
    constexpr qsizetype maximum_total_units = qsizetype{1024} * 1024;
    constexpr qsizetype maximum_mime_bytes = qsizetype{2} * 1024 * 1024;
    if (!mime || !mime->hasUrls()) return std::nullopt;
    const auto encoded = mime->data(QStringLiteral("text/uri-list"));
    if (encoded.isEmpty() || encoded.size() > maximum_mime_bytes || encoded.contains('\0')) return std::nullopt;
    const auto urls = mime->urls();
    if (urls.isEmpty() || urls.size() > maximum_paths) return std::nullopt;
    QStringList paths;
    QSet<QString> seen;
    qsizetype total{};
    for (const auto &url : urls) {
        if (!url.isValid() || !url.isLocalFile() ||
            url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) != 0 ||
            !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return std::nullopt;
        auto path = QDir::cleanPath(QDir::fromNativeSeparators(url.toLocalFile()));
        if (path.isEmpty() || path.contains(QChar::Null) || path.size() > maximum_path_units ||
            !QDir::isAbsolutePath(path)) return std::nullopt;
        total += path.size();
        if (total > maximum_total_units) return std::nullopt;
        auto key = path;
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        if (!seen.contains(key)) { seen.insert(key); paths.push_back(std::move(path)); }
    }
    return paths.isEmpty() ? std::nullopt : std::optional<QStringList>(std::move(paths));
}

DropRouter::DropRouter(DropViewPort views, DirectoryListModel &model, DropCallbacks callbacks, QObject *parent)
    : QObject(parent), views_(std::move(views)), model_(model), callbacks_(std::move(callbacks)) {
    const auto watch = [this](QObject *object) {
        if (!object || watched_.contains(object)) return;
        watched_.push_back(object);
        object->installEventFilter(this);
    };
    for (auto *view : {static_cast<QAbstractItemView *>(views_.catalog.data()),
                      static_cast<QAbstractItemView *>(views_.tree.data()),
                      static_cast<QAbstractItemView *>(views_.favorites.data()),
                      static_cast<QAbstractItemView *>(views_.trash_favorite.data())})
        if (view) { view->setAcceptDrops(true); view->viewport()->setAcceptDrops(true); watch(view->viewport()); }
    for (const auto &widget : {views_.favorites_header, views_.preview_panel, views_.preview_canvas})
        if (widget) { widget->setAcceptDrops(true); watch(widget); }
    for (const auto &view : {views_.favorites, views_.trash_favorite}) if (view) {
        view->setDragEnabled(false);
        view->setDragDropMode(QAbstractItemView::DropOnly);
    }
}
DropRouter::~DropRouter() {
    for (const auto &object : watched_) if (object) object->removeEventFilter(this);
    callbacks_.drag_status_changed = {};
    clear_feedback();
}
QString DropRouter::tr(const char *text) { return QCoreApplication::translate("MainWindow", text); }
void DropRouter::set_pin_highlight(bool active) {
    auto *header = views_.favorites_header.data();
    if (!header || header->property("pinDropActive").toBool() == active) return;
    header->setProperty("pinDropActive", active);
    header->style()->unpolish(header);
    header->style()->polish(header);
    header->update();
}
void DropRouter::set_favorite_target(FavoritesView *view, QListWidgetItem *item) {
    if (views_.favorites) views_.favorites->set_drop_target_item(view == views_.favorites ? item : nullptr);
    if (views_.trash_favorite) views_.trash_favorite->set_drop_target_item(view == views_.trash_favorite ? item : nullptr);
}
void DropRouter::set_move_cursor(bool move) {
    if (set_local_drag_move_feedback(move)) {
        if (cursorOverride_) { QApplication::restoreOverrideCursor(); cursorOverride_ = false; }
        return;
    }
    if (move && !cursorOverride_) {
        QApplication::setOverrideCursor(QCursor(Qt::DragMoveCursor));
        cursorOverride_ = true;
    } else if (!move && cursorOverride_) {
        QApplication::restoreOverrideCursor();
        cursorOverride_ = false;
    }
}
void DropRouter::set_status(bool transfer, bool move, int count, const QString &message) {
    feedbackActive_ = true;
    if (callbacks_.drag_status_changed)
        callbacks_.drag_status_changed(DragStatusMessage{transfer, move, count, message});
}
void DropRouter::clear_feedback() {
    set_local_drag_move_feedback(false);
    if (cursorOverride_) { QApplication::restoreOverrideCursor(); cursorOverride_ = false; }
    set_pin_highlight(false);
    set_favorite_target(nullptr, nullptr);
    externalMime_.clear();
    externalPaths_.clear();
    if (std::exchange(feedbackActive_, false) && callbacks_.drag_status_changed)
        callbacks_.drag_status_changed(std::nullopt);
}

bool DropRouter::eventFilter(QObject *watched, QEvent *event) {
    if (!event || !watched_.contains(watched)) return false;
    if (event->type() == QEvent::DragLeave) {
        clear_feedback();
        event->accept();
        return true;
    }
    const auto is_drop = event->type() == QEvent::Drop;
    const auto is_enter = event->type() == QEvent::DragEnter;
    if (!is_drop && !is_enter && event->type() != QEvent::DragMove) return false;
    auto *drop = static_cast<QDropEvent *>(event);
    const auto state = callbacks_.state ? callbacks_.state() : DropState{};
    const auto catalog = views_.catalog && watched == views_.catalog->viewport();
    const auto tree = views_.tree && watched == views_.tree->viewport();
    const auto favorites = views_.favorites && watched == views_.favorites->viewport();
    const auto trash_favorite = views_.trash_favorite && watched == views_.trash_favorite->viewport();
    const auto header = views_.favorites_header && watched == views_.favorites_header;
    const auto preview = (views_.preview_panel && watched == views_.preview_panel) ||
                         (views_.preview_canvas && watched == views_.preview_canvas);
    auto *favorite_view = favorites ? views_.favorites.data() : trash_favorite ? views_.trash_favorite.data() : nullptr;
    const auto reject = [this, drop] {
        clear_feedback();
        drop->ignore();
        return true;
    };
    if ((state.trash_view && (catalog || preview)) ||
        (catalog && (state.global_search || state.recursive_view))) return reject();
    const auto *mime = drop->mimeData();
    if (!mime || !(drop->possibleActions() & Qt::CopyAction)) return reject();
    const auto private_type = model_.mimeTypes().value(1);
    const auto has_private = !private_type.isEmpty() && mime->hasFormat(private_type);
    if (preview) {
        if (has_private || !callbacks_.open_external_paths) return reject();
        auto paths = local_file_paths(mime);
        if (!paths) return reject();
        clear_feedback();
        if (is_drop) {
            callbacks_.open_external_paths(std::move(*paths));
        } else {
            const auto count = static_cast<int>(paths->size());
            set_status(false, false, count,
                QCoreApplication::translate("MainWindow", "Open %n item(s)", nullptr, count));
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    const auto from_view = [drop](const QAbstractItemView *view) {
        return trusted_internal_drop_source(drop->source(), view, view ? view->viewport() : nullptr);
    };
    if (from_view(views_.favorites) || from_view(views_.trash_favorite)) return reject();
    const auto has_tree = mime->hasFormat(QString::fromLatin1(directoryTreeDragMime));
    auto catalog_source = from_view(views_.catalog);
    auto tree_source = from_view(views_.tree);
#ifdef VOVE_TEST_ALLOW_SYNTHETIC_INTERNAL_DRAG
    catalog_source = catalog_source || drop->source() == nullptr;
    tree_source = tree_source || drop->source() == nullptr;
#endif
    QList<core::DirectoryEntry> entries;
    const auto internal = has_private && catalog_source && model_.decode_internal_drag(mime, entries);
    const auto tree_drag = !has_private && has_tree && tree_source;
    if ((has_private && !internal) || (has_tree && !tree_drag)) return reject();
    if (internal && state.global_search && (!views_.catalog || !views_.catalog->dragEnabled() ||
        !std::ranges::all_of(entries, [this](const auto &entry) {
            const auto *current = model_.entry_for_id(entry.id);
            return current && same_snapshot(*current, entry);
        }))) return reject();
    const auto position = drop->position().toPoint();
    auto *favorite_target = favorite_view ? favorite_view->itemAt(position) : nullptr;
    const auto trash_target = PlacesController::is_trash_favorite(favorite_target);
    const auto pin = header || (favorites && (!favorite_target || tree_drag));
    QString destination;
    bool transfer{};
    if (!pin) {
        if (favorite_target) {
            transfer = true;
            if (!trash_target) destination = favorite_target->data(Qt::UserRole).toString();
        } else if (tree && !tree_drag) {
            destination = views_.tree->indexAt(position).data(DirectoryTreeModel::PathRole).toString();
            transfer = !destination.isEmpty();
        } else if (catalog) {
            const auto index = views_.catalog->indexAt(position);
            const auto *entry = model_.entry_at(index);
            if (entry && entry->kind == core::EntryKind::directory) destination = QString::fromUtf8(entry->path_utf8);
            else if (!internal || !index.isValid()) destination = state.current_path;
            transfer = !destination.isEmpty();
        }
    }
    QStringList paths;
    if (internal) for (const auto &entry : entries) paths.push_back(QString::fromUtf8(entry.path_utf8));
    else {
        if (is_enter || externalMime_ != mime || externalPaths_.isEmpty()) {
            externalMime_.clear();
            externalPaths_.clear();
            if (auto parsed = local_file_paths(mime)) {
                externalMime_ = const_cast<QMimeData *>(mime);
                externalPaths_ = std::move(*parsed);
            }
        }
        paths = externalPaths_;
    }
    const auto validate_available = callbacks_.can_validate_external && callbacks_.can_validate_external();
    const auto transfer_available = callbacks_.can_transfer && callbacks_.can_transfer();
    const auto recovery = callbacks_.recovery_pending && callbacks_.recovery_pending();
    const auto needs_validation = !internal || state.global_search;
    const auto available = !paths.isEmpty() &&
        ((pin && (!internal || directories_only(entries)) &&
          (tree_drag || (!needs_validation || validate_available))) ||
         (transfer && transfer_available && (!needs_validation || validate_available) &&
          (!internal || objects_only(entries)) && !(internal && state.global_search && trash_target)));
    // Enter admits an authenticated catalog payload even over a gutter; later moves
    // still enforce the exact destination. Global results remain valid drag sources.
    if (is_enter && internal && !header && !recovery && !entries.isEmpty()) {
        clear_feedback();
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    if (!available) {
        reject();
        if (transfer && recovery) set_status(false, false, 0,
            tr("An incomplete file operation must be resolved first. Press F5"));
        return true;
    }
    const auto move = trash_target || (transfer && (drop->modifiers() & Qt::ShiftModifier));
    const auto kind = (drop->modifiers() & Qt::ShiftModifier) ? fileops::FileTransferKind::move : fileops::FileTransferKind::copy;
    if (!is_drop) {
        set_pin_highlight(pin);
        set_favorite_target(!pin && favorite_target ? favorite_view : nullptr, !pin ? favorite_target : nullptr);
        set_move_cursor(move);
        const auto count = static_cast<int>(paths.size());
        if (trash_target) set_status(false, true, count, tr("Move to VO-VE Trash: %1").arg(paths.size()));
        else if (pin) set_status(false, false, count, internal || tree_drag
            ? tr("Add folders to Favorites: %1").arg(paths.size()) : tr("Checking folders for Favorites\xE2\x80\xA6"));
        else set_status(true, move, count);
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    // Capture all values before clearing feedback: validation/execution may navigate.
    clear_feedback();
    bool accepted{};
    if (pin) {
        if (tree_drag || (internal && !state.global_search)) {
            if (callbacks_.pin_requested) { callbacks_.pin_requested(paths); accepted = true; }
        } else if (callbacks_.begin_external_directory_validation)
            accepted = callbacks_.begin_external_directory_validation(paths);
    } else if (internal && !state.global_search) {
        if (trash_target) {
            if (callbacks_.trash_requested) accepted = callbacks_.trash_requested(entries);
        } else if (callbacks_.transfer_requested) accepted = callbacks_.transfer_requested(entries, destination, kind);
    } else if (callbacks_.begin_external_transfer_validation)
        accepted = callbacks_.begin_external_transfer_validation(paths, destination, kind, trash_target,
            internal && state.global_search ? entries : QList<core::DirectoryEntry>{});
    if (accepted) { drop->setDropAction(Qt::CopyAction); drop->accept(); }
    else drop->ignore();
    return true;
}

} // namespace vove::ui
