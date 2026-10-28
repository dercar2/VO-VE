#pragma once

#include "lazy_directory_tree_model.hpp"
#include "vove/platform/directory_service.hpp"

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QScrollBar;

namespace vove::ui {

struct PlacesViewPort {
    QPointer<FavoritesView> favorites{};
    QPointer<FavoritesView> trash_favorite{};
    QPointer<QTreeView> tree{};
    QPointer<QScrollBar> favorites_scrollbar{};
};

enum class PlacesCommand { show_trash, empty_trash };
struct PlacesCallbacks {
    std::function<void(QString)> open_requested_path{};
    std::function<void(PlacesCommand)> file_command_intent{};
    std::function<bool()> can_empty_trash{};
    std::function<void()> favorites_changed{};
};

struct FavoritePlace { QString name; QString path; };

// Wiring: remove treeSource_, treeModel_ allocation/deletion, insertion flags and
// favorites/tree interaction connections from MainWindow. Pass existing styled views.
// Persist favorite_places(), restore via restore_favorites(); tree_model() is borrowed.
// set_location(path, trash, aggregate) receives Catalog mode, not root-window copies.
// Catalog listing completion calls reconcile_current_directory; F5 calls refresh_current_directory.
// Remove MainWindow add/rename/move/remove/update-location implementations and the external
// favorites scrollbar connections, which this owner installs. DropRouter alone owns highlights.
class PlacesController final : public QObject {
  public:
    PlacesController(PlacesViewPort views, PlacesCallbacks callbacks,
                     QObject *parent = nullptr);
    ~PlacesController() override;
    [[nodiscard]] LazyDirectoryTreeModel &tree_model() noexcept { return *treeModel_; }
    [[nodiscard]] QList<FavoritePlace> favorite_places() const;
    void restore_favorites(const QList<FavoritePlace> &places);
    void add_favorite(const QString &path);
    void add_current_favorite();
    void add_trash_favorite();
    void rename_selected_favorite();
    bool rename_selected_favorite(const QString &name);
    void move_selected_favorite(int offset);
    void remove_selected_favorite();
    void set_location(QString path, bool trash = false, bool aggregate_view = false);
    void update_directory_tree();
    void update_location_selection();
    void refresh_current_directory();
    void reconcile_current_directory(const QList<QPair<QString, QString>> &directories,
                                     bool complete);
    void retranslate();
    [[nodiscard]] static bool is_trash_favorite(const QListWidgetItem *item);
    [[nodiscard]] static bool same_directory_path(const QString &left, const QString &right);

  private:
    [[nodiscard]] static QString tr(const char *text);
    void show_context_menu(FavoritesView &view, const QPoint &position);
    void open_favorite(QListWidgetItem *item);
    PlacesViewPort views_;
    PlacesCallbacks callbacks_;
    platform::DirectoryService treeSource_;
    std::unique_ptr<LazyDirectoryTreeModel> treeModel_;
    QString currentPath_;
    bool trashView_{};
    bool aggregateView_{};
    bool insertBatchPending_{};
    bool revealCurrentAfterInsert_{};
    bool restoring_{};
};

} // namespace vove::ui
