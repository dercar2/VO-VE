#pragma once

#include "navigation_history.hpp"

#include <QObject>
#include <QModelIndex>
#include <QStringList>

#include <functional>

class QAbstractButton;
class QLineEdit;
class QListView;
class QMenu;
class QWidget;

namespace vove::ui {
class DirectoryListModel;

struct NavigationViews {
    QListView *grid{};
    QLineEdit *address{};
    QMenu *history{};
    QAbstractButton *back{};
    QAbstractButton *forward{};
    QAbstractButton *up{};
    QAbstractButton *history_button{};
    QWidget *dialog_owner{};
};

struct NavigationActions {
    std::function<bool()> ordinary_view;
    std::function<bool()> restore_allowed;
    std::function<bool()> trash_view;
    std::function<void()> prepare_directory;
    std::function<void(const QString &)> open_directory;
    std::function<void()> settings_changed;
    std::function<void()> selection_changed;
    std::function<void()> commands_changed;
    std::function<void()> previews_changed;
    std::function<void(const QString &)> message;
};

class NavigationController final : public QObject {
  public:
    NavigationController(DirectoryListModel &model, NavigationViews views,
                         NavigationActions actions, QObject *parent = nullptr);
    void open(const QString &path, bool add_to_history = true, const QString &reveal_child = {});
    void back();
    void forward();
    void up();
    void choose_directory(const QString &title);
    void show_location_caption(const QString &caption);
    void save();
    void catalog_started();
    void catalog_ready();
    void cancel_restore();
    void remember_refresh_position();
    void update_actions();
    void rebuild_history_menu();
    void reveal_after_refresh(const QStringList &paths, bool from_search = false);
    void restore_reveal();
    [[nodiscard]] QModelIndex index_for_path(const QString &path) const;
    [[nodiscard]] GridViewState capture_grid() const;
    void restore_grid(const GridViewState &state);
    [[nodiscard]] const QString &path() const noexcept { return path_; }
    [[nodiscard]] bool restore_pending() const noexcept { return pending_.has_value(); }
    [[nodiscard]] bool reveal_pending() const noexcept { return !reveal_paths_.isEmpty(); }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void open_history(int index);
    void restore_navigation();
    DirectoryListModel &model_;
    NavigationViews views_;
    NavigationActions actions_;
    NavigationHistory history_;
    std::optional<PendingNavigationRestore> pending_;
    QString path_;
    QStringList reveal_paths_;
    bool ready_{};
    bool reveal_from_search_{};
};
} // namespace vove::ui
