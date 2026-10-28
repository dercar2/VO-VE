#pragma once

#include "status_presenter.hpp"
#include "trash_dialog.hpp"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <optional>

class QListView;
class QWidget;

namespace vove::ui {
class CatalogController;
class FileCommandController;
class FavoritesView;

struct TrashViews {
    QWidget *dialog_owner{};
    QListView *grid{};
    FavoritesView *favorites{};
    FavoritesView *trash_favorite{};
};
struct TrashCallbacks {
    std::function<void(const QString &)> message;
    std::function<void()> changed;
};

class TrashController final : public QObject {
  public:
    TrashController(CatalogController &catalog, FileCommandController &commands,
                    TrashViews views, TrashCallbacks callbacks, QObject *parent = nullptr);
    ~TrashController() override;
    void show();
    void leave();
    void load(bool discover_recovery_roots = true);
    void poll();
    void show_context_menu(const QPoint &position);
    void cleanup();
    void set_maximum_bytes(std::uint64_t maximum);
    [[nodiscard]] bool can_cleanup() const;
    [[nodiscard]] std::size_t operation_items(const std::filesystem::path &manifest) const;
    [[nodiscard]] std::optional<TrashStatusSummary> status_summary() const;

  private:
    void apply(fileops::TrashCatalogResult result);
    void update_favorite();
    void message(const QString &text);
    CatalogController &catalog_;
    FileCommandController &commands_;
    TrashViews views_;
    TrashCallbacks callbacks_;
    TrashCatalogLoader loader_;
    QTimer timer_;
    std::optional<fileops::TrashCatalogResult> result_;
    QHash<qulonglong, QString> manifests_;
    std::uint64_t maximumBytes_{50ULL * 1024ULL * 1024ULL * 1024ULL};
    bool pendingCleanup_{};
};
} // namespace vove::ui
