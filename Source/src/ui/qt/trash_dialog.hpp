#pragma once

#include "vove/fileops/trash_catalog.hpp"

#include <QDialog>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace vove::ui {

struct TrashCatalogLoadState;

class TrashCatalogLoader final {
  public:
    TrashCatalogLoader() = default;
    ~TrashCatalogLoader();

    TrashCatalogLoader(const TrashCatalogLoader &) = delete;
    TrashCatalogLoader &operator=(const TrashCatalogLoader &) = delete;

    [[nodiscard]] bool
    start(std::filesystem::path manifest_directory,
          std::optional<std::vector<std::filesystem::path>> recovery_roots = std::nullopt);
    [[nodiscard]] std::optional<fileops::TrashCatalogResult> poll();
    [[nodiscard]] bool loading() const noexcept;
    void cancel() noexcept;

  private:
    std::shared_ptr<TrashCatalogLoadState> state_;
};

#ifdef VOVE_TEST_TRASH_BLOCKING_LOADER
struct TrashCatalogLoaderStats {
    std::size_t workers_started{};
    std::size_t active_jobs{};
    std::size_t queued_jobs{};
    std::size_t rejected_jobs{};
};

[[nodiscard]] TrashCatalogLoaderStats trash_catalog_loader_stats_for_test();
#endif

enum class TrashAction : std::uint8_t {
    none,
    restore,
    restore_to,
    purge,
};

struct TrashCleanupPlan {
    std::vector<std::filesystem::path> manifests;
    std::size_t total_items{};
    std::uint64_t total_bytes{};
    bool totals_saturated{};
};

[[nodiscard]] TrashCleanupPlan make_trash_cleanup_plan(const fileops::TrashCatalogResult &catalog,
                                                       bool remove_all, int days,
                                                       std::int64_t now_unix_ns) noexcept;

class TrashCleanupDialog final : public QDialog {
  public:
    explicit TrashCleanupDialog(const fileops::TrashCatalogResult &catalog,
                                QWidget *parent = nullptr);

    [[nodiscard]] TrashCleanupPlan plan() const;

  private:
    void update_summary();

    const fileops::TrashCatalogResult *catalog_{};
    QRadioButton *all_{};
    QRadioButton *older_{};
    QSpinBox *days_{};
    QLabel *summary_{};
    QPushButton *clear_{};
    std::int64_t nowUnixNs_{};
};

} // namespace vove::ui
