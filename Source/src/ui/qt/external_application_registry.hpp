#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

class QSettings;

namespace vove::ui {

enum class ExternalApplicationKind {
    executable,
    desktop_entry,
};

struct ExternalApplication {
    ExternalApplicationKind kind{ExternalApplicationKind::executable};
    QString name;
    QString path;
    QByteArray icon_png;
};

enum class AddExternalApplicationResult {
    added,
    duplicate,
    invalid,
};

class ExternalApplicationRegistry final {
  public:
    void load(QSettings &settings);
    void save(QSettings &settings) const;
    // Restore preference records without checking availability or reading any files/settings.
    // Retains unavailable apps; applies only structural normalization, deduplication and bounds.
    void restore_loaded_data(QList<ExternalApplication> applications);

    [[nodiscard]] AddExternalApplicationResult add(const ExternalApplication &application);
    [[nodiscard]] bool remove(const QString &path);
    [[nodiscard]] bool launch(const ExternalApplication &application, const QString &file_path,
                              QString *error = nullptr) const;
    [[nodiscard]] const QList<ExternalApplication> &applications() const noexcept;

  private:
    QList<ExternalApplication> applications_;
};

} // namespace vove::ui
