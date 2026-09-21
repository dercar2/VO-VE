#include "directory_watch_helper.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <cstdlib>
#include <cstring>

int main(int argc, char *argv[]) {
    if (argc == 3 && std::strcmp(argv[1], "watch-directory") == 0) {
        return vove::ui::run_directory_watch_helper(argc, argv);
    }
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() != 3) {
        return EXIT_FAILURE;
    }
    const auto &action = arguments.at(1);
    const auto path = QDir::cleanPath(arguments.at(2));
    if (!QDir::isAbsolutePath(path)) {
        return EXIT_FAILURE;
    }
    if (action == QStringLiteral("open")) {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(path)) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (action != QStringLiteral("reveal")) {
        return EXIT_FAILURE;
    }
#ifdef _WIN32
    const auto explorer = QStandardPaths::findExecutable(QStringLiteral("explorer.exe"));
    return !explorer.isEmpty() &&
                   QProcess::startDetached(
                       explorer, {QStringLiteral("/select,") + QDir::toNativeSeparators(path)})
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
#else
    const auto parent = QFileInfo(path).absolutePath();
    return QDesktopServices::openUrl(QUrl::fromLocalFile(parent)) ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}
