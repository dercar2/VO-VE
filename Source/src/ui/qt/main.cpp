#include "application_icon.hpp"
#include "diagnostic_sink.hpp"
#include "launch_request.hpp"
#include "main_window.hpp"
#include "scrollbar_style.hpp"
#include "release_configuration.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#include <cstring>

namespace {

constexpr auto kMaximumLaunchClients = 8;
#ifdef Q_OS_LINUX
constexpr auto kPackageUpdateMarker = "/run/lock/vo-ve-package-update";
#endif

QString server_name() {
    const auto home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation).toUtf8();
    const auto digest = QCryptographicHash::hash(home, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("vove-%1").arg(QString::fromLatin1(digest.first(16)));
}

} // namespace

int main(int argc, char *argv[]) {
#ifdef Q_OS_LINUX
    if (QFileInfo::exists(QString::fromLatin1(kPackageUpdateMarker))) {
        return 75;
    }
#endif
    if (argc >= 2 && std::strncmp(argv[1], "--sync-shell-icon=", 18) == 0) {
        if (argc != 2)
            return 2;
        // The argument is the OS scheme; this child never constructs a window or forwards a launch.
        const auto light = std::strcmp(argv[1], "--sync-shell-icon=light") == 0;
        if (!light && std::strcmp(argv[1], "--sync-shell-icon=dark") != 0)
            return 2;
        QCoreApplication application(argc, argv);
        return vove::ui::synchronize_application_shell_icon(light ? Qt::ColorScheme::Light
                                                                  : Qt::ColorScheme::Dark)
                   ? 0
                   : 1;
    }
    auto &diagnostic = vove::ui::DiagnosticSink::instance();
    static_cast<void>(diagnostic.activate_from_environment());
    diagnostic.record("main.enter");
    QApplication application(argc, argv);
    diagnostic.record("qt.application_ready");
    QApplication::setStyle(vove::ui::create_application_style());
    QCoreApplication::setOrganizationName(QStringLiteral("VO-VE"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("vo-ve.local"));
    QCoreApplication::setApplicationName(QStringLiteral("VO-VE"));
    QCoreApplication::setApplicationVersion(vove::ui::updates::compiled_application_version());
    vove::ui::suppress_linux_dialog_icons(application);
    const auto request = vove::ui::launch_request_from_arguments(QCoreApplication::arguments());
    if (!request) {
        diagnostic.record("launch.invalid_request");
        return 5;
    }
    const auto name = server_name();
    const auto lock_path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(name + QStringLiteral(".lock"));
    QLockFile startup_lock(lock_path);
    startup_lock.setStaleLockTime(5'000);
    if (!startup_lock.tryLock(2'000)) {
        diagnostic.record("launch.lock_failed");
        return 4;
    }
    switch (vove::ui::deliver_launch_request(name, *request)) {
    case vove::ui::LaunchDeliveryStatus::delivered:
        diagnostic.record("secondary_instance");
        return 0;
    case vove::ui::LaunchDeliveryStatus::failed:
        diagnostic.record("launch.delivery_failed");
        return 6;
    case vove::ui::LaunchDeliveryStatus::server_unavailable:
        break;
    }

    QLocalServer::removeServer(name);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.setMaxPendingConnections(kMaximumLaunchClients);
    if (!server.listen(name)) {
        diagnostic.record("launch.server_failed");
        return 2;
    }
    diagnostic.record("launch.server_ready");

    startup_lock.unlock();
    vove::ui::follow_system_application_icon(application, true);
    vove::ui::MainWindow window(request->initial_directory);
    diagnostic.record("window.constructed");
    if (!request->paths.isEmpty()) {
        window.open_external_paths(request->paths);
    }
    int active_connections{};
    QObject::connect(
        &server, &QLocalServer::newConnection, &window, [&server, &window, &active_connections] {
            while (auto *socket = server.nextPendingConnection()) {
                if (active_connections >= kMaximumLaunchClients) {
                    socket->disconnectFromServer();
                    socket->deleteLater();
                    continue;
                }
                ++active_connections;
                QObject::connect(socket, &QObject::destroyed, &window,
                                 [&active_connections] { --active_connections; });
                socket->setReadBufferSize(vove::ui::kMaximumLaunchFrameBytes + 1);
                auto decoder = std::make_shared<vove::ui::LaunchFrameDecoder>();
                auto processed = std::make_shared<bool>(false);
                const auto reject = [socket, processed] {
                    *processed = true;
                    socket->disconnectFromServer();
                };
                const auto receive = [socket, decoder, processed, reject, &window] {
                    if (*processed) {
                        return;
                    }
                    const auto remaining = decoder->remaining_capacity();
                    if (remaining <= 0) {
                        reject();
                        return;
                    }
                    const auto status = decoder->append(socket->read(remaining));
                    if (status == vove::ui::LaunchFrameStatus::pending) {
                        return;
                    }
                    if (status == vove::ui::LaunchFrameStatus::rejected || !decoder->request()) {
                        reject();
                        return;
                    }
                    *processed = true;
                    window.open_external_paths(decoder->request()->paths);
                    socket->write(&vove::ui::kLaunchAckByte, 1);
                    socket->flush();
                    socket->disconnectFromServer();
                };
                QObject::connect(socket, &QLocalSocket::readyRead, &window, receive);
                QObject::connect(socket, &QLocalSocket::disconnected, socket,
                                 &QLocalSocket::deleteLater);
                QTimer::singleShot(2'000, socket, [socket, processed] {
                    if (!*processed) {
                        *processed = true;
                        socket->disconnectFromServer();
                    }
                });
                receive();
            }
        });
    window.show();
    diagnostic.record("window.shown");
    std::unique_ptr<QTimer> heartbeat;
    if (diagnostic.active()) {
        heartbeat = std::make_unique<QTimer>();
        heartbeat->setInterval(2'000);
        QObject::connect(heartbeat.get(), &QTimer::timeout, &window,
                         [&diagnostic] { diagnostic.heartbeat(); });
        heartbeat->start();
    }
    const auto exit_code = application.exec();
    const auto exit_text = std::to_string(exit_code);
    diagnostic.record("application.exit", {{"code", exit_text}});
    diagnostic.shutdown();
    return exit_code;
}
