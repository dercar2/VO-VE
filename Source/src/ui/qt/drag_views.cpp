#include "drag_views.hpp"
#include <QFrame>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QUrl>

#include <memory>

#ifdef Q_OS_WIN
#include "windows_file_drag.hpp"
#include <optional>
#endif

namespace vove::ui {

class FavoritesDropDelegate final : public QStyledItemDelegate {
  public:
    explicit FavoritesDropDelegate(FavoritesView *view) : QStyledItemDelegate(view), view_(view) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (view_ == nullptr || view_->dropTarget_ != index ||
            !view_->dropTargetBorder_.isValid()) {
            return;
        }
        painter->save();
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(view_->dropTargetBorder_, 1));
        painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
        painter->restore();
    }

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (view_ == nullptr || view_->dropTarget_ != index ||
            !view_->dropTargetBackground_.isValid()) {
            return;
        }
        option->state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
        option->backgroundBrush = view_->dropTargetBackground_;
    }

  private:
    FavoritesView *view_{};
};

namespace {

QPointer<QDrag> active_drag;
bool active_drag_move{};

QPixmap transfer_drag_cursor(const bool move, const qreal dpr) {
    // X11 centers the custom cursor hotspot; Windows uses the top-left corner.
#ifdef Q_OS_WIN
    constexpr int extent = 32;
    constexpr int origin = 0;
#else
    constexpr int extent = 64;
    constexpr int origin = 31;
#endif
    QPixmap image(QSize(qCeil(extent * dpr), qCeil(extent * dpr)));
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.translate(origin, origin);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, 1));
    painter.setBrush(Qt::white);
    painter.drawPolygon(
        QPolygonF{{1, 1}, {1, 20}, {6, 15}, {10, 24}, {14, 22}, {10, 13}, {18, 13}});
    if (!move) {
        painter.drawRect(QRectF(18, 18, 12, 12));
        painter.drawLine(QPointF(21, 24), QPointF(27, 24));
        painter.drawLine(QPointF(24, 21), QPointF(24, 27));
        return image;
    }
    QPainterPath arrows;
    arrows.moveTo(18, 24);
    arrows.lineTo(30, 24);
    arrows.moveTo(24, 18);
    arrows.lineTo(24, 30);
    for (const auto &points :
         {QPolygonF{{21, 21}, {24, 18}, {27, 21}}, QPolygonF{{21, 27}, {24, 30}, {27, 27}},
          QPolygonF{{21, 21}, {18, 24}, {21, 27}}, QPolygonF{{27, 21}, {30, 24}, {27, 27}}}) {
        arrows.moveTo(points.front());
        arrows.lineTo(points[1]);
        arrows.lineTo(points.back());
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::black, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(arrows);
    painter.setPen(QPen(Qt::white, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(arrows);
    return image;
}

void start_translucent_drag(QAbstractItemView &view, const Qt::DropActions supported_actions) {
    if (view.model() == nullptr || view.selectionModel() == nullptr ||
        !(supported_actions & view.model()->supportedDragActions() & Qt::CopyAction)) {
        return;
    }
    auto indexes = view.selectionModel()->selectedIndexes();
    indexes.removeIf(
        [](const QModelIndex &index) { return !(index.flags() & Qt::ItemIsDragEnabled); });
    if (indexes.isEmpty()) {
        return;
    }
    std::unique_ptr<QMimeData> mime(view.model()->mimeData(indexes));
    if (!mime || mime->formats().isEmpty()) {
        return;
    }
#ifdef Q_OS_WIN
    std::optional<detail::WindowsFileDragMetadata> native_metadata;
    if (QApplication::platformName() == QStringLiteral("windows") &&
        detail::WindowsFileDragMimeData::supports(*mime)) {
        auto files = std::make_unique<detail::WindowsFileDragMimeData>(*mime);
        native_metadata.emplace(files.get());
        if (!native_metadata->ready()) return;
        mime = std::move(files);
    }
#endif
    auto lead = view.currentIndex();
    if (!indexes.contains(lead)) {
        lead = indexes.front();
    }
    // Qt retains the drag while an X11 target may still request its MIME data.
    auto *drag = new QDrag(&view);
    drag->setMimeData(mime.release());
    const auto visible_rect = view.visualRect(lead).intersected(view.viewport()->rect());
    if (!visible_rect.isEmpty()) {
        drag->setPixmap(translucent_drag_pixmap(view.viewport()->grab(visible_rect)));
        drag->setHotSpot(QPoint(0, 0));
    }
    // Targets own copying/moving; neither source view removes rows after a drop.
    drag->setDragCursor(transfer_drag_cursor(false, view.devicePixelRatioF()), Qt::CopyAction);
    active_drag = drag;
    active_drag_move = false;
    drag->exec(Qt::CopyAction, Qt::CopyAction);
    active_drag = nullptr;
    active_drag_move = false;
}

} // namespace

bool set_local_drag_move_feedback(const bool move) {
    if (!active_drag) {
        return false;
    }
    if (active_drag_move != move) {
        active_drag_move = move;
        const auto *source = qobject_cast<QWidget *>(active_drag->source());
        // Native drag feedback owns the cursor during exec(); a QApplication override
        // is overwritten by its CopyAction feedback even when VO-VE is doing a move.
        // A null reset does not invalidate Windows' cached native move cursor.
        active_drag->setDragCursor(
            transfer_drag_cursor(move, source ? source->devicePixelRatioF() : 1), Qt::CopyAction);
    }
    return true;
}

QPixmap translucent_drag_pixmap(const QPixmap &source) {
    if (source.isNull()) {
        return {};
    }
    const auto dpr = source.devicePixelRatio();
    const QSize bounds(qRound(180 * dpr), qRound(160 * dpr));
    const auto image = source.width() > bounds.width() || source.height() > bounds.height()
                           ? source.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           : source;
    // Leave the pointer and its action indicator outside the floating thumbnail.
    QPixmap result(image.size() + QSize(qCeil(16 * dpr), qCeil(20 * dpr)));
    result.setDevicePixelRatio(dpr);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setOpacity(0.45);
    painter.drawPixmap(QPointF(16, 20), image);
    return result;
}

CatalogView::CatalogView(QWidget *parent) : QListView(parent), renameTimer_(this) {
    recursiveOutline_ = new QFrame(this);
    recursiveOutline_->setObjectName(QStringLiteral("recursiveCatalogOutline"));
    recursiveOutline_->setAttribute(Qt::WA_TransparentForMouseEvents);
    recursiveOutline_->setFocusPolicy(Qt::NoFocus);
    recursiveOutline_->setStyleSheet(QStringLiteral(
        "QFrame#recursiveCatalogOutline { border: 4px solid #FF7F00; background: transparent; }"));
    recursiveOutline_->hide();
    verticalScrollBar()->parentWidget()->installEventFilter(this);
    horizontalScrollBar()->parentWidget()->installEventFilter(this);
    // Inset only the thumbnails; stylesheet padding would shorten the scrollbar too.
    setViewportMargins(32, 26, 0, 0);
    setEditTriggers(QAbstractItemView::SelectedClicked);
    renameTimer_.setSingleShot(true);
    renameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&renameTimer_, &QTimer::timeout, this, [this] {
        const auto candidate = renameCandidate_;
        cancel_rename();
        if (candidate.isValid() && currentIndex() == candidate && selectionModel() &&
            selectionModel()->selectedIndexes() == QModelIndexList{candidate} && isVisible() &&
            hasFocus() && renameAction_ && renameAction_->isEnabled()) {
            renameAction_->trigger();
        }
    });
}

QModelIndex CatalogView::indexAt(const QPoint &point) const {
    const auto index = QListView::indexAt(point);
    return index.isValid() && itemHitTest_ && !itemHitTest_(index, point) ? QModelIndex{} : index;
}

void CatalogView::set_recursive_mode(const bool enabled) {
    update_recursive_outline();
    recursiveOutline_->setVisible(enabled);
    if (enabled) recursiveOutline_->raise();
}

void CatalogView::update_recursive_outline() {
    auto bounds = rect();
    // Keep thumbnail padding inside the frame, but leave the scroll rails outside it.
    for (auto *bar : {verticalScrollBar(), horizontalScrollBar()}) {
        auto *container = bar->parentWidget();
        if (!container->isVisibleTo(this)) continue;
        const QRect rail(container->mapTo(this, QPoint{}), container->size());
        if (bar->orientation() == Qt::Vertical) {
            if (rail.center().x() < rect().center().x()) bounds.setLeft(rail.right() + 1);
            else bounds.setRight(rail.left() - 1);
        } else {
            if (rail.center().y() < rect().center().y()) bounds.setTop(rail.bottom() + 1);
            else bounds.setBottom(rail.top() - 1);
        }
    }
    recursiveOutline_->setGeometry(bounds.adjusted(2, 2, -2, -2));
    if (!recursiveOutline_->isHidden()) recursiveOutline_->raise();
}

void CatalogView::set_item_hit_test(
    std::function<bool(const QModelIndex &, const QPoint &)> item_hit_test) {
    itemHitTest_ = std::move(item_hit_test);
}

void CatalogView::set_rename_action(
    QAction *action, std::function<bool(const QModelIndex &, const QPoint &)> name_hit_test) {
    renameAction_ = action;
    nameHitTest_ = std::move(name_hit_test);
}

bool CatalogView::edit(const QModelIndex &index, EditTrigger trigger, QEvent *event) {
    if (trigger == SelectedClicked) {
        if (!singleSelectionOnPress_ || !renameAction_ || !renameAction_->isEnabled() ||
            !nameHitTest_ || !event || event->type() != QEvent::MouseButtonRelease) {
            return false;
        }
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier ||
            !nameHitTest_(index, mouse->position().toPoint())) {
            return false;
        }
        // Own the delay so cancellation and a fresh click cannot share an old deadline.
        renameCandidate_ = index;
        renameTimer_.start(QApplication::doubleClickInterval());
        return true;
    }
    cancel_rename();
    return QListView::edit(index, trigger, event);
}

void CatalogView::cancel_rename() {
    renameTimer_.stop();
    renameCandidate_ = QPersistentModelIndex{};
}

bool CatalogView::event(QEvent *event) {
    switch (event->type()) {
    case QEvent::FocusOut:
    case QEvent::Hide:
    case QEvent::WindowDeactivate:
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
    case QEvent::Resize:
        cancel_rename();
        break;
    default:
        break;
    }
    const auto handled = QListView::event(event);
    if (recursiveOutline_ != nullptr &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
         event->type() == QEvent::LayoutRequest || event->type() == QEvent::StyleChange ||
         event->type() == QEvent::LayoutDirectionChange)) {
        update_recursive_outline();
    }
    return handled;
}

bool CatalogView::eventFilter(QObject *watched, QEvent *event) {
    const auto handled = QListView::eventFilter(watched, event);
    if (recursiveOutline_ != nullptr &&
        (watched == verticalScrollBar()->parentWidget() ||
         watched == horizontalScrollBar()->parentWidget()) &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        update_recursive_outline();
    }
    return handled;
}

void CatalogView::mousePressEvent(QMouseEvent *event) {
    cancel_rename();
    singleSelectionOnPress_ = selectionModel() && selectionModel()->selectedIndexes().size() == 1;
    QListView::mousePressEvent(event);
}

void CatalogView::mouseDoubleClickEvent(QMouseEvent *event) {
    cancel_rename();
    singleSelectionOnPress_ = false;
    QListView::mouseDoubleClickEvent(event);
}

void CatalogView::scrollContentsBy(int dx, int dy) {
    cancel_rename();
    QListView::scrollContentsBy(dx, dy);
}

void CatalogView::startDrag(const Qt::DropActions supported_actions) {
    cancel_rename();
    start_translucent_drag(*this, supported_actions);
}

void DirectoryTreeView::startDrag(const Qt::DropActions supported_actions) {
    start_translucent_drag(*this, supported_actions);
}

void DirectoryTreeView::keyPressEvent(QKeyEvent *event) {
    const auto previous = currentIndex();
    const auto selected = selectionModel()->selectedIndexes();
    QTreeView::keyPressEvent(event);
    if (currentIndex().isValid() &&
        (currentIndex() != previous || selectionModel()->selectedIndexes() != selected)) {
        emit activated(currentIndex());
    }
}

void FavoritesView::keyPressEvent(QKeyEvent *event) {
    const auto *previous = currentItem();
    const auto selected = selectedItems();
    QListWidget::keyPressEvent(event);
    if (currentItem() != nullptr && (currentItem() != previous || selectedItems() != selected)) {
        emit itemActivated(currentItem());
    }
}

QStringList DirectoryTreeModel::mimeTypes() const {
    return {QStringLiteral("text/uri-list"), QString::fromLatin1(directoryTreeDragMime)};
}

QMimeData *DirectoryTreeModel::mimeData(const QModelIndexList &indexes) const {
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    QSet<QString> paths;
    for (const auto &index : indexes) {
        if (!index.isValid() || index.model() != this) {
            continue;
        }
        const auto path = index.data(PathRole).toString();
        if (path.isEmpty() || !QDir::isAbsolutePath(path) || paths.contains(path)) {
            continue;
        }
        paths.insert(path);
        urls.push_back(QUrl::fromLocalFile(path));
    }
    if (!urls.isEmpty()) {
        mime->setUrls(urls);
        mime->setData(QString::fromLatin1(directoryTreeDragMime), QByteArrayLiteral("1"));
    }
    return mime;
}

Qt::DropActions DirectoryTreeModel::supportedDragActions() const {
    return Qt::CopyAction;
}

FavoritesView::FavoritesView(QWidget *parent) : QListWidget(parent) {
    setItemDelegate(new FavoritesDropDelegate(this));
    setProperty("dropTargetActive", false);
    setProperty("dropTargetRow", -1);
}

void FavoritesView::set_drop_target_item(QListWidgetItem *item) {
    const QPersistentModelIndex next = item != nullptr ? indexFromItem(item) : QModelIndex{};
    if (dropTarget_ == next) {
        return;
    }
    dropTarget_ = next;
    setProperty("dropTargetActive", dropTarget_.isValid());
    setProperty("dropTargetRow", dropTarget_.isValid() ? dropTarget_.row() : -1);
    viewport()->update();
}

void FavoritesView::clear_drop_target_item() {
    set_drop_target_item(nullptr);
}

void FavoritesView::set_drop_target_theme(const QColor &background, const QColor &border) {
    if (dropTargetBackground_ == background && dropTargetBorder_ == border) {
        return;
    }
    dropTargetBackground_ = background;
    dropTargetBorder_ = border;
    if (dropTarget_.isValid()) {
        viewport()->update();
    }
}

} // namespace vove::ui
