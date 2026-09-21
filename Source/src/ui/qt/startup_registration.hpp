#pragma once

#include <QByteArray>
#include <QString>

namespace vove::ui {

[[nodiscard]] bool startup_registration_enabled();
[[nodiscard]] bool set_startup_registration_enabled(bool enabled, QString *error = nullptr);

namespace startup_detail {

[[nodiscard]] QString quoted_startup_command(const QString &application_path);
[[nodiscard]] QByteArray desktop_entry(const QString &application_path);
[[nodiscard]] bool file_registration_enabled(const QString &path);
[[nodiscard]] bool set_file_registration_enabled(const QString &path,
                                                 const QString &application_path, bool enabled,
                                                 QString *error = nullptr);

} // namespace startup_detail
} // namespace vove::ui
