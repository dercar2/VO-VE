#include "window_settings.hpp"

#include "vove/search/global_search.hpp"

#include <QLocale>
#include <QSettings>

#include <algorithm>
#include <utility>
#include <vector>

namespace vove::ui {

WindowSettings::WindowSettings(QObject *parent) : QObject(parent) {
    timer_.setSingleShot(true);
    timer_.setInterval(250);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { save_now(); });
}

WindowSettingsSnapshot WindowSettings::load(QSettings &settings) {
    WindowSettingsSnapshot result;
    result.language = settings.contains(QStringLiteral("appearance/language"))
        ? app_language_from_code(settings.value(QStringLiteral("appearance/language")).toString())
        : app_language_from_locale(QLocale());
    result.geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    result.main_share = settings.value(QStringLiteral("window/mainShare"), 0.60).toDouble();
    result.favorites_share = settings.value(QStringLiteral("window/favoritesShare"), 0.30).toDouble();
    result.thumbnail_extent = settings.value(QStringLiteral("catalog/thumbnailExtent"), 160).toInt();
    result.filter = settings.value(QStringLiteral("catalog/filter")).toString();
    result.sort_field = settings.value(QStringLiteral("catalog/sortField"), 0).toInt();
    result.sort_direction = settings.value(QStringLiteral("catalog/sortDirection"), 0).toInt();
    result.theme = static_cast<AppTheme>(std::clamp(
        settings.value(QStringLiteral("appearance/theme"), static_cast<int>(AppTheme::blue)).toInt(),
        static_cast<int>(AppTheme::white), static_cast<int>(AppTheme::dark_gray)));
    result.last_path = settings.value(QStringLiteral("catalog/lastPath")).toString();
    const auto favorites = settings.value(QStringLiteral("catalog/favorites")).toStringList();
    const auto labels = settings.value(QStringLiteral("catalog/favoriteLabels")).toStringList();
    result.favorites.reserve(favorites.size());
    for (qsizetype index{}; index < favorites.size(); ++index) {
        result.favorites.push_back({favorites.at(index), index < labels.size() ? labels.at(index) : QString{}});
    }
    const auto roots = settings.value(QStringLiteral("search/allowedRoots")).toStringList();
    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(roots.size()));
    for (const auto &root : roots) encoded.push_back(root.toUtf8().toStdString());
    for (const auto &root : search::normalize_allowed_roots(encoded)) {
        result.search_roots.push_back(QString::fromUtf8(root));
    }
    // Preserve the existing one-time preference migration, not operation-recovery state.
    settings.remove(QStringLiteral("color/fallbackCmykProfile"));
    result.cache_maximum_bytes = settings.value(QStringLiteral("cache/maximumBytes"),
        QVariant::fromValue<qulonglong>(result.cache_maximum_bytes)).toULongLong();
    if (result.cache_maximum_bytes == 0) result.cache_maximum_bytes = 10ULL * 1024 * 1024 * 1024;
    result.trash_maximum_bytes = settings.value(QStringLiteral("trash/maximumBytes"),
        QVariant::fromValue<qulonglong>(result.trash_maximum_bytes)).toULongLong();
    if (result.trash_maximum_bytes == 0) result.trash_maximum_bytes = 50ULL * 1024 * 1024 * 1024;
    // Reuse the registry's structural read validation without taking ownership of its runtime.
    ExternalApplicationRegistry applications;
    applications.load(settings);
    result.external_applications = applications.applications();
    return result;
}

void WindowSettings::save(QSettings &settings, const WindowSettingsSnapshot &snapshot) {
    settings.setValue(QStringLiteral("window/geometry"), snapshot.geometry);
    settings.setValue(QStringLiteral("window/mainShare"), snapshot.main_share);
    settings.setValue(QStringLiteral("window/favoritesShare"), snapshot.favorites_share);
    settings.setValue(QStringLiteral("catalog/thumbnailExtent"), snapshot.thumbnail_extent);
    settings.setValue(QStringLiteral("catalog/filter"), snapshot.filter);
    settings.setValue(QStringLiteral("catalog/sortField"), snapshot.sort_field);
    settings.setValue(QStringLiteral("catalog/sortDirection"), snapshot.sort_direction);
    settings.setValue(QStringLiteral("catalog/lastPath"), snapshot.last_path);
    settings.setValue(QStringLiteral("appearance/theme"), static_cast<int>(snapshot.theme));
    settings.setValue(QStringLiteral("appearance/language"), language_code(snapshot.language));
    QStringList favorites;
    QStringList labels;
    for (const auto &favorite : snapshot.favorites) {
        favorites.push_back(favorite.path);
        labels.push_back(favorite.label);
    }
    settings.setValue(QStringLiteral("catalog/favorites"), favorites);
    settings.setValue(QStringLiteral("catalog/favoriteLabels"), labels);
    settings.setValue(QStringLiteral("search/allowedRoots"), snapshot.search_roots);
    settings.setValue(QStringLiteral("cache/maximumBytes"),
                      QVariant::fromValue<qulonglong>(snapshot.cache_maximum_bytes));
    settings.setValue(QStringLiteral("trash/maximumBytes"),
                      QVariant::fromValue<qulonglong>(snapshot.trash_maximum_bytes));
    settings.beginWriteArray(QStringLiteral("desktop/openWith"),
                             static_cast<int>(snapshot.external_applications.size()));
    for (qsizetype index{}; index < snapshot.external_applications.size(); ++index) {
        settings.setArrayIndex(static_cast<int>(index));
        const auto &application = snapshot.external_applications.at(index);
        settings.setValue(QStringLiteral("kind"),
                          application.kind == ExternalApplicationKind::desktop_entry
                              ? QStringLiteral("desktop") : QStringLiteral("executable"));
        settings.setValue(QStringLiteral("name"), application.name);
        settings.setValue(QStringLiteral("path"), application.path);
        settings.setValue(QStringLiteral("iconPng"), application.icon_png);
    }
    settings.endArray();
    settings.sync();
}

void WindowSettings::set_save_provider(SnapshotProvider provider) {
    provider_ = std::move(provider);
}

void WindowSettings::schedule_save() { timer_.start(); }

void WindowSettings::save_now() {
    timer_.stop();
    if (!provider_) return;
    const auto snapshot = provider_();
    QSettings settings;
    save(settings, snapshot);
}

bool WindowSettings::save_pending() const noexcept { return timer_.isActive(); }

} // namespace vove::ui
