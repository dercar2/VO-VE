#pragma once

#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QWidget;

namespace vove::ui {

struct SettingsApplication {
    QString name;
    QString path;
};

struct SettingsDialogData {
    QStringList search_roots;
    std::vector<SettingsApplication> applications;
    bool startup_enabled{};
    std::uint64_t cache_maximum_bytes{};
    std::uint64_t trash_maximum_bytes{};
};

struct SettingsDialogResult {
    QStringList search_roots;
    QStringList retained_application_paths;
    std::uint64_t cache_maximum_bytes{};
    std::uint64_t trash_maximum_bytes{};
};

struct SettingsCacheUsage {
    bool success{};
    std::uint64_t bytes{};
};

using SettingsCacheCompletion = std::function<void(SettingsCacheUsage)>;
struct SettingsDialogActions {
    std::function<QString()> choose_search_root;
    std::function<bool(bool, QString *)> set_startup_enabled;
    std::function<bool(SettingsCacheCompletion)> inspect_cache;
    std::function<bool(SettingsCacheCompletion)> clear_cache;
    std::function<void()> cache_cleared;
    std::function<void()> empty_trash;
    std::function<void()> check_updates;
};

[[nodiscard]] std::optional<SettingsDialogResult>
show_settings_dialog(QWidget *owner, const SettingsDialogData &data,
                     const SettingsDialogActions &actions);

} // namespace vove::ui
