#include "startup_registration.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace vove::ui {
namespace {

#ifdef Q_OS_WIN
constexpr auto registration_name = "VO-VE";
#endif

QString test_registration_path() {
#ifdef VOVE_UI_TEST_HOOKS
    return qEnvironmentVariable("VOVE_TEST_AUTOSTART_PATH");
#else
    return {};
#endif
}

#ifdef Q_OS_LINUX
QString desktop_registration_path() {
    const auto config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return config.isEmpty() ? QString{}
                            : QDir(config).filePath(QStringLiteral("autostart/vo-ve.desktop"));
}
#endif

} // namespace

namespace startup_detail {

QString quoted_startup_command(const QString &application_path) {
    auto escaped = QDir::toNativeSeparators(application_path);
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QByteArray desktop_entry(const QString &application_path) {
    auto escaped = application_path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    escaped.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    escaped.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    const auto command = QStringLiteral("\"%1\"").arg(escaped);
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=VO-VE\n"
                          "Exec=%1\n"
                          "Terminal=false\n"
                          "X-GNOME-Autostart-enabled=true\n")
        .arg(command)
        .toUtf8();
}

bool file_registration_enabled(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto lines = file.readAll().split('\n');
    return lines.contains(QByteArrayLiteral("X-GNOME-Autostart-enabled=true"));
}

bool set_file_registration_enabled(const QString &path, const QString &application_path,
                                   const bool enabled, QString *error) {
    if (!enabled) {
        if (!QFileInfo::exists(path)) {
            return true;
        }
        QFile file(path);
        if (file.remove()) {
            return true;
        }
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    if (path.isEmpty() || application_path.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Startup path is unavailable.");
        }
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Startup directory could not be created.");
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    const auto contents = desktop_entry(application_path);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

} // namespace startup_detail

bool startup_registration_enabled() {
    const auto test_path = test_registration_path();
    if (!test_path.isEmpty()) {
        return startup_detail::file_registration_enabled(test_path);
    }
#ifdef Q_OS_WIN
    QSettings run(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    return !run.value(QString::fromLatin1(registration_name)).toString().trimmed().isEmpty();
#elif defined(Q_OS_LINUX)
    return startup_detail::file_registration_enabled(desktop_registration_path());
#else
    return false;
#endif
}

bool set_startup_registration_enabled(const bool enabled, QString *error) {
    const auto application_path = QCoreApplication::applicationFilePath();
    const auto test_path = test_registration_path();
    if (!test_path.isEmpty()) {
        return startup_detail::set_file_registration_enabled(test_path, application_path, enabled,
                                                             error);
    }
#ifdef Q_OS_WIN
    QSettings run(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    if (enabled) {
        run.setValue(QString::fromLatin1(registration_name),
                     startup_detail::quoted_startup_command(application_path));
    } else {
        run.remove(QString::fromLatin1(registration_name));
    }
    run.sync();
    if (run.status() == QSettings::NoError) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Registry status %1").arg(static_cast<int>(run.status()));
    }
    return false;
#elif defined(Q_OS_LINUX)
    return startup_detail::set_file_registration_enabled(desktop_registration_path(),
                                                         application_path, enabled, error);
#else
    if (!enabled) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Startup registration is unavailable on this platform.");
    }
    return false;
#endif
}

} // namespace vove::ui
