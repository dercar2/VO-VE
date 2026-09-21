#pragma once

#include <QListView>
#include <QListWidget>
#include <QColor>
#include <QPixmap>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>

#include <functional>

class QAction;
class QFrame;

namespace vove::ui {

inline constexpr auto directoryTreeDragMime = "application/x-vove-directory-tree-drag";

QPixmap translucent_drag_pixmap(const QPixmap &source);
bool set_local_drag_move_feedback(bool move);

class CatalogView final : public QListView {
  public:
    explicit CatalogView(QWidget *parent = nullptr);
    [[nodiscard]] QModelIndex indexAt(const QPoint &point) const override;
    void set_item_hit_test(std::function<bool(const QModelIndex &, const QPoint &)> item_hit_test);
    void set_recursive_mode(bool enabled);
    void set_rename_action(QAction *action,
                           std::function<bool(const QModelIndex &, const QPoint &)> name_hit_test);

  protected:
    bool edit(const QModelIndex &index, EditTrigger trigger, QEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void startDrag(Qt::DropActions supported_actions) override;

  private:
    void update_recursive_outline();
    void cancel_rename();

    QTimer renameTimer_;
    QFrame *recursiveOutline_{};
    QPointer<QAction> renameAction_;
    std::function<bool(const QModelIndex &, const QPoint &)> itemHitTest_;
    std::function<bool(const QModelIndex &, const QPoint &)> nameHitTest_;
    QPersistentModelIndex renameCandidate_;
    bool singleSelectionOnPress_{};
};

class DirectoryTreeView final : public QTreeView {
  public:
    using QTreeView::QTreeView;

  protected:
    void startDrag(Qt::DropActions supported_actions) override;
    void keyPressEvent(QKeyEvent *event) override;
};

class DirectoryTreeModel : public QStandardItemModel {
  public:
    using QStandardItemModel::QStandardItemModel;
    static constexpr int PathRole = Qt::UserRole + 1;

    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override;
};

class FavoritesView final : public QListWidget {
  public:
    explicit FavoritesView(QWidget *parent = nullptr);
    void set_drop_target_item(QListWidgetItem *item);
    void clear_drop_target_item();
    void set_drop_target_theme(const QColor &background, const QColor &border);

  protected:
    void keyPressEvent(QKeyEvent *event) override;

  private:
    friend class FavoritesDropDelegate;

    QPersistentModelIndex dropTarget_;
    QColor dropTargetBackground_;
    QColor dropTargetBorder_;
};

} // namespace vove::ui
