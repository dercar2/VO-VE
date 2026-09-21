#include "application_icon.hpp"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageBox>
#include <QPixmap>
#include <QStyleHints>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QAbstractNativeEventFilter>
#include <QTimer>
#include <QFile>
#include <QSaveFile>
#include <QProcess>
#include <QScopeGuard>

#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace vove::ui {
namespace {

bool shell_icon_sync_supported() {
    const auto executable = QDir::fromNativeSeparators(QCoreApplication::applicationFilePath());
#ifdef Q_OS_WIN
    return executable.endsWith(QStringLiteral("/vove-ui-qt.exe"), Qt::CaseInsensitive);
#elif defined(Q_OS_LINUX)
    return executable == QStringLiteral("/usr/lib/vo-ve/vove-ui-qt");
#else
    return false;
#endif
}

class ShellIconSynchronizer final : public QObject {
  public:
    explicit ShellIconSynchronizer(QGuiApplication &application) : QObject(&application) {
        delay_.setSingleShot(true);
        budget_.setSingleShot(true);
        connect(&delay_, &QTimer::timeout, this, [this] { start(); });
        connect(&budget_, &QTimer::timeout, this, [this] {
            if (process_ != nullptr)
                process_->kill();
        });
    }

    ~ShellIconSynchronizer() override {
        delay_.stop();
        budget_.stop();
        if (process_ == nullptr)
            return;
        process_->disconnect(this);
        process_->kill();
        // Never let QProcess's destructor impose its default wait on the UI at shutdown.
        if (process_->state() != QProcess::NotRunning && !process_->waitForFinished(100)) {
            process_->setParent(nullptr);
            connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process_,
                    &QObject::deleteLater);
        }
    }

    void request(const Qt::ColorScheme scheme) {
        const auto normalized =
            scheme == Qt::ColorScheme::Light ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark;
        if (requested_ == normalized)
            return;
        requested_ = normalized;
        if (process_ == nullptr)
            delay_.start(100);
    }

  private:
    void start() {
        if (process_ != nullptr || !requested_ || attempted_ == requested_)
            return;
        attempted_ = requested_;
        auto *process = new QProcess(this);
        process_ = process;
        process->setObjectName(QStringLiteral("shellIconSyncProcess"));
        process->setProgram(QCoreApplication::applicationFilePath());
        process->setArguments({*attempted_ == Qt::ColorScheme::Light
                                   ? QStringLiteral("--sync-shell-icon=light")
                                   : QStringLiteral("--sync-shell-icon=dark")});
        process->setStandardOutputFile(QProcess::nullDevice());
        process->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
        const auto finished = [this, process] {
            if (process_ != process)
                return;
            budget_.stop();
            process_ = nullptr;
            process->deleteLater();
            if (requested_ != attempted_)
                delay_.start(100);
        };
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, finished);
        connect(process, &QProcess::errorOccurred, this,
                [finished](const QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        finished();
                });
        budget_.start(2'000);
        process->start(QIODevice::ReadOnly);
        process->closeWriteChannel();
    }

    QTimer delay_;
    QTimer budget_;
    QProcess *process_{};
    std::optional<Qt::ColorScheme> requested_;
    std::optional<Qt::ColorScheme> attempted_;
};

#ifdef Q_OS_WIN
void synchronize_shortcut_icons(const Qt::ColorScheme scheme) {
    const auto icon =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(scheme == Qt::ColorScheme::Light ? QStringLiteral("vo-ve-dark.ico")
                                                       : QStringLiteral("vo-ve-light.ico"));
    if (!QFileInfo::exists(icon))
        return;
    const auto roaming = qEnvironmentVariable("APPDATA");
    const QStringList links{
        QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
            .filePath(QStringLiteral("VO-VE.lnk")),
        roaming + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/VO-VE/VO-VE.lnk"),
        roaming + QStringLiteral(
                      "/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar/VO-VE.lnk"),
        roaming +
            QStringLiteral(
                "/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar/vove-ui-qt.lnk")};
    const auto executable =
        QDir::cleanPath(QDir::fromNativeSeparators(QCoreApplication::applicationFilePath()));
    for (const auto &link_path : links) {
        const auto normalized = QDir::fromNativeSeparators(link_path);
        if (normalized.startsWith(QStringLiteral("//")) || normalized.size() < 3 ||
            normalized.at(1) != ':' ||
            GetDriveTypeW(reinterpret_cast<LPCWSTR>(
                QDir::toNativeSeparators(normalized.left(3)).utf16())) == DRIVE_REMOTE)
            continue;
        if (synchronize_application_shortcut_icon(link_path, executable, icon)) {
            const auto filename = QDir::toNativeSeparators(link_path).toStdWString();
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, filename.c_str(), nullptr);
        }
    }
}

class ShellThemeListener final : public QObject, public QAbstractNativeEventFilter {
  public:
    explicit ShellThemeListener(QGuiApplication &application, ShellIconSynchronizer *synchronizer)
        : QObject(&application), synchronizer_(synchronizer) {
        application.installNativeEventFilter(this);
    }
    ~ShellThemeListener() override {
        if (auto *application = QCoreApplication::instance())
            application->removeNativeEventFilter(this);
    }
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
        const auto *event = static_cast<MSG *>(message);
        if (event && (event->message == WM_SETTINGCHANGE || event->message == WM_THEMECHANGED) &&
            !pending_) {
            pending_ = true;
            QTimer::singleShot(0, this, [this] {
                pending_ = false;
                const auto scheme =
                    application_shell_color_scheme(QGuiApplication::styleHints()->colorScheme());
                QGuiApplication::setWindowIcon(QIcon(application_icon_resource(scheme)));
                if (synchronizer_ != nullptr)
                    synchronizer_->request(scheme);
            });
        }
        return false;
    }

  private:
    bool pending_{};
    ShellIconSynchronizer *synchronizer_{};
};
#endif

#ifdef Q_OS_LINUX
class DialogIconSuppressor final : public QObject {
  public:
    using QObject::QObject;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event == nullptr ||
            (event->type() != QEvent::Polish && event->type() != QEvent::Show)) {
            return false;
        }
        auto *dialog = qobject_cast<QDialog *>(watched);
        if (dialog == nullptr) {
            return false;
        }
        dialog->setProperty("voveDialogIconsSuppressed", true);
        QPixmap transparent(1, 1);
        transparent.fill(Qt::transparent);
        dialog->setWindowIcon(QIcon(transparent));
        if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
            message->setIcon(QMessageBox::NoIcon);
        }
        return false;
    }
};
#endif

} // namespace

#ifdef Q_OS_WIN
bool synchronize_application_shortcut_icon(const QString &shortcut, const QString &executable,
                                           const QString &icon) {
    const QFileInfo link(shortcut);
    if (!link.isShortcut() || link.isSymbolicLink())
        return false;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return false;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    IShellLinkW *shell_link{};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void **>(&shell_link))))
        return false;
    const auto release_link = qScopeGuard([shell_link] { shell_link->Release(); });
    IPersistFile *persist{};
    if (FAILED(shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist))))
        return false;
    const auto release_persist = qScopeGuard([persist] { persist->Release(); });
    const auto filename = QDir::toNativeSeparators(link.absoluteFilePath()).toStdWString();
    if (FAILED(persist->Load(filename.c_str(), STGM_READWRITE)))
        return false;
    wchar_t target[32768]{}, previous[32768]{};
    int previous_index{};
    if (FAILED(shell_link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)) ||
        FAILED(shell_link->GetIconLocation(previous, 32768, &previous_index)))
        return false;
    const auto normalized = [](const QString &path) {
        return QDir::cleanPath(QDir::fromNativeSeparators(path));
    };
    if (normalized(QString::fromWCharArray(target))
                .compare(normalized(executable), Qt::CaseInsensitive) != 0 ||
        (normalized(QString::fromWCharArray(previous))
                 .compare(normalized(icon), Qt::CaseInsensitive) == 0 &&
         previous_index == 0))
        return false;
    const auto icon_path = QDir::toNativeSeparators(icon).toStdWString();
    return SUCCEEDED(shell_link->SetIconLocation(icon_path.c_str(), 0)) &&
           SUCCEEDED(persist->Save(filename.c_str(), TRUE));
}
#endif

QString application_icon_resource(const Qt::ColorScheme system_scheme) {
    return system_scheme == Qt::ColorScheme::Light
               ? QStringLiteral(":/artwork/application-icon-dark.png")
               : QStringLiteral(":/artwork/application-icon-light.png");
}

Qt::ColorScheme application_shell_color_scheme(const Qt::ColorScheme fallback) {
#ifdef Q_OS_WIN
    QSettings settings(
        QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    const auto value = settings.value(QStringLiteral("SystemUsesLightTheme"));
    if (value.isValid())
        return value.toInt() == 0 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
#endif
    return fallback;
}

bool synchronize_application_icon_file(const QString &path, const Qt::ColorScheme scheme) {
    QFile source(application_icon_resource(scheme));
    if (!source.open(QIODevice::ReadOnly))
        return false;
    const auto bytes = source.readAll();
    const QFileInfo existing(path);
    if (existing.isSymLink())
        return false;
    if (existing.exists()) {
        QFile current(path);
        if (current.size() > 512 * 1024 || !current.open(QIODevice::ReadOnly))
            return false;
        const auto previous = current.readAll();
        if (previous == bytes)
            return true;
        QFile alternate(application_icon_resource(
            scheme == Qt::ColorScheme::Light ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light));
        if (!alternate.open(QIODevice::ReadOnly) || previous != alternate.readAll())
            return false;
    }
    if (!QDir().mkpath(existing.absolutePath()))
        return false;
    QSaveFile destination(path);
    return destination.open(QIODevice::WriteOnly) && destination.write(bytes) == bytes.size() &&
           destination.commit();
}

bool synchronize_application_shell_icon(const Qt::ColorScheme scheme) {
    if (!shell_icon_sync_supported())
        return false;
#ifdef Q_OS_WIN
    synchronize_shortcut_icons(scheme);
#elif defined(Q_OS_LINUX)
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return !data.isEmpty() &&
           synchronize_application_icon_file(
               data + QStringLiteral("/icons/hicolor/512x512/apps/vo-ve.png"), scheme);
#endif
    return true;
}

void follow_system_application_icon(QGuiApplication &application, const bool synchronize_shell) {
    auto *synchronizer = synchronize_shell && shell_icon_sync_supported()
                             ? new ShellIconSynchronizer(application)
                             : nullptr;
    const auto update_icon = [synchronizer](const Qt::ColorScheme scheme) {
        const auto shell_scheme = application_shell_color_scheme(scheme);
        QGuiApplication::setWindowIcon(QIcon(application_icon_resource(shell_scheme)));
        if (synchronizer != nullptr)
            synchronizer->request(shell_scheme);
    };
    auto *hints = application.styleHints();
    if (!hints) {
        update_icon(Qt::ColorScheme::Unknown);
        return;
    }

    // The system hint is independent of VO-VE's selected widget palette.
    QObject::connect(hints, &QStyleHints::colorSchemeChanged, &application, update_icon);
    update_icon(hints->colorScheme());
#ifdef Q_OS_WIN
    new ShellThemeListener(application, synchronizer);
#endif
}

void suppress_linux_dialog_icons(QApplication &application) {
#ifdef Q_OS_LINUX
    if (application.property("voveDialogIconSuppressorInstalled").toBool()) {
        return;
    }
    application.setProperty("voveDialogIconSuppressorInstalled", true);
    application.installEventFilter(new DialogIconSuppressor(&application));
#else
    static_cast<void>(application);
#endif
}

} // namespace vove::ui
