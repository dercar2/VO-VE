#pragma once

#include <QString>
#include <Qt>

class QGuiApplication;
class QApplication;

namespace vove::ui {

QString application_icon_resource(Qt::ColorScheme system_scheme);
Qt::ColorScheme application_shell_color_scheme(Qt::ColorScheme fallback);
bool synchronize_application_icon_file(const QString &path, Qt::ColorScheme scheme);
#ifdef Q_OS_WIN
// Returns true only when the matching shortcut was changed; does not notify the shell.
bool synchronize_application_shortcut_icon(const QString &shortcut, const QString &executable,
                                           const QString &icon);
#endif
bool synchronize_application_shell_icon(Qt::ColorScheme scheme);
void follow_system_application_icon(QGuiApplication &application, bool synchronize_shell = false);
void suppress_linux_dialog_icons(QApplication &application);

} // namespace vove::ui
