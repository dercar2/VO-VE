#pragma once

#include "directory_list_model.hpp"
#include "directory_monitor.hpp"
#include "global_search_client.hpp"
#include "navigation_history.hpp"
#include "preview_controller.hpp"
#include "vove/catalog/catalog_session.hpp"
#include "vove/platform/directory_service.hpp"

#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>
#include <optional>

class QComboBox;
class QAction;
class QLabel;
class QListView;
class QSlider;
class QToolButton;
class QWidget;

namespace vove::ui {
class CatalogToolButton;
class DirectoryItemDelegate;
class FileCommandController;
class NavigationController;
class PlacesController;
class SelectionController;
class SearchField;

enum class CatalogViewMode : std::uint8_t { directory, recursive, global_search, trash };

struct CatalogViews {
    QWidget *window{};
    QListView *grid{};
    SearchField *filter{};
    SearchField *global_search{};
    QComboBox *sort{};
    QToolButton *sort_direction{};
    CatalogToolButton *recursive{};
    CatalogToolButton *refresh{};
    QLabel *search_message{};
    QSlider *thumbnail_scale{};
    DirectoryItemDelegate *delegate{};
    QAction *rename{};
};

struct CatalogCallbacks {
    std::function<void()> settings_changed;
    std::function<void()> status_changed;
    std::function<bool()> drag_active;
    std::function<void()> leave_trash;
    std::function<void()> refresh_trash;
};

class CatalogController final : public QObject {
  public:
    explicit CatalogController(QString search_helper_override = {}, QObject *parent = nullptr);
    ~CatalogController() override;
    void attachViews(CatalogViews views, PreviewController &preview, NavigationController &navigation,
                     FileCommandController &commands, PlacesController &places,
                     SelectionController &selection, CatalogCallbacks callbacks);
    void open_directory(const QString &path);
    void prepare_navigation();
    void refresh();
    void refresh_directory_sources();
    void resume_polling(int interval_ms = -1);
    void poll();
    void request_recheck();
    void stop();
    void apply_filter();
    void apply_sort();
    void apply_thumbnail_extent(int extent);
    void schedule_search();
    void start_search();
    void leave_search(bool restore_catalog);
    void set_recursive(bool enabled);
    void leave_recursive(bool restore_catalog);
    void update_tools();
    void enter_trash();
    void leave_trash();
    void replace_trash_entries(std::vector<core::DirectoryEntry> entries,
                               QHash<qulonglong, QString> tooltip_paths);
    void set_search_roots(QStringList roots);
    void mark_saved_search_dirty();
    void observe_search_path(const QString &path);
    void reconcile_search_move(const QStringList &paths);
    void show_entry_in_folder(const core::DirectoryEntry &entry);
    [[nodiscard]] DirectoryListModel &model() noexcept { return model_; }
    [[nodiscard]] const DirectoryListModel &model() const noexcept { return model_; }
    [[nodiscard]] const QString &path() const noexcept { return path_; }
    [[nodiscard]] const catalog::CatalogSessionUpdate &update() const noexcept { return update_; }
    [[nodiscard]] CatalogViewMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool search_active() const noexcept { return mode_ == CatalogViewMode::global_search; }
    [[nodiscard]] bool recursive_active() const noexcept { return mode_ == CatalogViewMode::recursive; }
    [[nodiscard]] bool trash_active() const noexcept { return mode_ == CatalogViewMode::trash; }
    [[nodiscard]] bool ordinary_view() const noexcept { return mode_ == CatalogViewMode::directory; }
    [[nodiscard]] bool search_ready() const noexcept { return searchReady_; }
    [[nodiscard]] bool search_query_current() const;
    [[nodiscard]] std::uint64_t search_generation() const noexcept { return searchGeneration_; }
    [[nodiscard]] qsizetype removed_search_paths() const noexcept { return removedPaths_.size(); }
    [[nodiscard]] const QString &search_status() const noexcept { return searchStatus_; }
    [[nodiscard]] const QStringList &search_roots() const noexcept { return searchRoots_; }
    [[nodiscard]] bool automatic_refresh_blocked() const;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct SavedView {
        DirectoryListModel::Snapshot model;
        GridViewState grid;
        catalog::CatalogSessionUpdate update;
        PreviewCatalogState preview;
    };
    [[nodiscard]] SavedView take_view();
    void restore_view(SavedView saved);
    void handle_search_batch(search::SearchBatch batch);
    [[nodiscard]] bool removed_search_path(const std::string &path) const;
    void show_search_message(const QString &message);
    void update_search_message_geometry();
    void update_monitor_visibility();
    [[nodiscard]] bool refresh_automatically();
    void notify();
    void sync_preview_context();

    DirectoryListModel model_;
    platform::DirectoryService source_;
    catalog::CatalogSession session_;
    GlobalSearchClient search_;
    DirectoryMonitor monitor_;
    QTimer pollTimer_;
    QTimer searchTimer_;
    CatalogViews views_;
    CatalogCallbacks callbacks_;
    PreviewController *preview_{};
    NavigationController *navigation_{};
    FileCommandController *commands_{};
    PlacesController *places_{};
    SelectionController *selection_{};
    QString path_;
    QStringList searchRoots_;
    QString searchStatus_;
    QString query_;
    CatalogViewMode mode_{CatalogViewMode::directory};
    catalog::CatalogSessionUpdate update_;
    catalog::CatalogSessionUpdate beforeAutomatic_;
    std::optional<SavedView> beforeSearch_;
    std::optional<SavedView> beforeRecursive_;
    QSet<QString> removedPaths_;
    QSet<QString> observedPaths_;
    std::uint64_t searchGeneration_{};
    std::uint64_t searchCount_{};
    bool searchReady_{};
    bool searchTruncated_{};
    bool savedSearchDirty_{};
    bool automatic_{};
};
} // namespace vove::ui
