#pragma once

#include <QList>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace vove::ui {

struct GridViewState {
    std::vector<std::uint64_t> selected_ids;
    std::uint64_t current_id{};
    std::uint64_t top_id{};
    int top_offset{};
    int scroll_value{};
    bool has_top{};
};

struct NavigationEntry {
    QString path;
    GridViewState grid{};
    bool saved{};
};

struct PendingNavigationRestore {
    QString path;
    GridViewState grid;
    QString reveal_child;
};

// Pure history state: paths are lexical, so visiting an offline share never probes it.
class NavigationHistory final {
  public:
    void visit(const QString &path);
    void save(const QString &path, GridViewState state);
    [[nodiscard]] bool select(int index);
    [[nodiscard]] const NavigationEntry *current() const noexcept;
    [[nodiscard]] const QList<NavigationEntry> &entries() const noexcept { return entries_; }
    [[nodiscard]] int index() const noexcept { return index_; }
    [[nodiscard]] bool can_go_back() const noexcept { return index_ > 0; }
    [[nodiscard]] bool can_go_forward() const noexcept;
    [[nodiscard]] std::optional<PendingNavigationRestore>
    restore_for(const QString &path, const QString &reveal_child = {}) const;

  private:
    QList<NavigationEntry> entries_;
    int index_{-1};
};

} // namespace vove::ui
