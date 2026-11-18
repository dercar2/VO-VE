#include "navigation_history.hpp"

#include <QDir>

#include <utility>

namespace vove::ui {
namespace {
bool same_path(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(left))
               .compare(QDir::cleanPath(QDir::fromNativeSeparators(right)), sensitivity) == 0;
}
} // namespace

void NavigationHistory::visit(const QString &path, const bool returning_to_parent) {
    NavigationEntry next{.path = path};
    if (returning_to_parent) {
        const auto use_parent = [&](const NavigationEntry &entry) {
            if (entry.saved && same_path(entry.path, path)) {
                next.grid = entry.grid;
                next.saved = true;
            }
        };
        for (int i = index_; i >= 0 && !next.saved; --i) use_parent(entries_[i]);
        // The parent can also have been visited only on the forward branch.
        for (auto i = index_ + 1; i < entries_.size() && !next.saved; ++i) use_parent(entries_[i]);
    }
    while (entries_.size() - 1 > index_) entries_.removeLast();
    if (entries_.isEmpty() || !same_path(entries_.back().path, path)) {
        entries_.push_back(std::move(next));
    } else {
        entries_.back() = std::move(next);
    }
    if (entries_.size() > 128) entries_.removeFirst();
    index_ = static_cast<int>(entries_.size()) - 1;
}

void NavigationHistory::save(const QString &path, GridViewState state) {
    const auto *entry = current();
    if (!entry || !same_path(entry->path, path)) return;
    if (state.selected_ids.size() > 512) {
        state.selected_ids.clear();
        if (state.current_id != 0) state.selected_ids.push_back(state.current_id);
    }
    entries_[index_].grid = std::move(state);
    entries_[index_].saved = true;
}

bool NavigationHistory::select(const int index) {
    if (index < 0 || index >= entries_.size()) return false;
    index_ = index;
    return true;
}

const NavigationEntry *NavigationHistory::current() const noexcept {
    return index_ >= 0 && index_ < entries_.size() ? &entries_[index_] : nullptr;
}

bool NavigationHistory::can_go_forward() const noexcept {
    return index_ >= 0 && index_ + 1 < entries_.size();
}

std::optional<PendingNavigationRestore>
NavigationHistory::restore_for(const QString &path, const QString &reveal_child) const {
    const auto *entry = current();
    if (entry && entry->saved && same_path(entry->path, path)) {
        return PendingNavigationRestore{path, entry->grid, reveal_child};
    }
    if (!reveal_child.isEmpty()) return PendingNavigationRestore{path, {}, reveal_child};
    return std::nullopt;
}

} // namespace vove::ui
