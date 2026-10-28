#pragma once

#include "external_application_registry.hpp"
#include "ui_text.hpp"

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <functional>

class QSettings;

namespace vove::ui {

enum class AppTheme : int { white, vanilla, blue, dark_gray };

struct WindowFavorite {
    QString path;
    QString label;
};

// Saved preferences only. Operation journals and live controller state are not part of this value.
struct WindowSettingsSnapshot {
    QByteArray geometry;
    double main_share{0.60};
    double favorites_share{0.30};
    int thumbnail_extent{160};
    QString filter;
    int sort_field{};
    int sort_direction{};
    QString last_path;
    AppTheme theme{AppTheme::blue};
    AppLanguage language{AppLanguage::english};
    QList<WindowFavorite> favorites;
    QStringList search_roots;
    std::uint64_t cache_maximum_bytes{10ULL * 1024 * 1024 * 1024};
    std::uint64_t trash_maximum_bytes{50ULL * 1024 * 1024 * 1024};
    QList<ExternalApplication> external_applications;
};

class WindowSettings final : public QObject {
  public:
    using SnapshotProvider = std::function<WindowSettingsSnapshot()>;

    explicit WindowSettings(QObject *parent = nullptr);
    [[nodiscard]] static WindowSettingsSnapshot load(QSettings &settings);
    static void save(QSettings &settings, const WindowSettingsSnapshot &snapshot);

    void set_save_provider(SnapshotProvider provider);
    void schedule_save();
    void save_now();
    [[nodiscard]] bool save_pending() const noexcept;

  private:
    QTimer timer_;
    SnapshotProvider provider_;
};

} // namespace vove::ui
