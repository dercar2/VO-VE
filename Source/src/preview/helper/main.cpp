#include "service.hpp"

#include "vove/cache/sqlite_artifact_index.hpp"
#include "vove/preview/worker_thumbnail_renderer.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMetaObject>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "vove/worker/windows_runtime_access.hpp"
#else
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

#ifdef _WIN32
inline constexpr char kRasterWorkerName[] = "vove-raster-worker.exe";
inline constexpr UINT kForcedInstallerShutdownExitCode = 197;
#ifdef VOVE_HAS_MUPDF_DOCUMENT_WORKER
inline constexpr char kDocumentWorkerName[] = "vove-document-worker.exe";
#endif
inline constexpr char kCdrWorkerName[] = "vove-cdr-worker.exe";
inline constexpr char kXcfWorkerName[] = "vove-xcf-worker.exe";
#ifdef VOVE_HAS_RESVG_SVG_WORKER
inline constexpr char kSvgWorkerName[] = "vove-svg-worker.exe";
#endif

[[nodiscard]] std::wstring installation_shutdown_event_name() {
    const auto directory = QDir::cleanPath(QCoreApplication::applicationDirPath()).toCaseFolded();
    const auto digest =
        QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return (QStringLiteral("Global\\VO-VE.PreviewShutdown.") + QString::fromLatin1(digest))
        .toStdWString();
}

[[nodiscard]] int request_installation_shutdown() {
    const auto name = installation_shutdown_event_name();
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!event) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : 11;
    }
    const auto signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    if (!signaled) {
        return 12;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE remaining = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
        if (!remaining) {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : 13;
        }
        CloseHandle(remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 14;
}

class InstallationShutdownGuard final {
  public:
    InstallationShutdownGuard() {
        const auto name = installation_shutdown_event_name();
        event_ = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (!event_) {
            return;
        }
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) {
            CloseHandle(event_);
            event_ = nullptr;
            return;
        }
        watcher_ = std::thread([this] { watch(); });
    }

    ~InstallationShutdownGuard() {
        if (stop_) {
            SetEvent(stop_);
        }
        if (watcher_.joinable()) {
            watcher_.join();
        }
        if (event_) {
            CloseHandle(event_);
        }
        if (stop_) {
            CloseHandle(stop_);
        }
    }

    InstallationShutdownGuard(const InstallationShutdownGuard &) = delete;
    InstallationShutdownGuard &operator=(const InstallationShutdownGuard &) = delete;

  private:
    void watch() const {
        const HANDLE handles[]{event_, stop_};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);

        // A decoder request is deliberately synchronous in this small helper. Give an idle helper
        // time to leave normally, then make an explicit uninstall deterministic even while that
        // request is blocked. Preview cache writes are transactional and disposable.
        if (WaitForSingleObject(stop_, 2'000) == WAIT_TIMEOUT) {
            TerminateProcess(GetCurrentProcess(), kForcedInstallerShutdownExitCode);
        }
    }

    HANDLE event_{};
    HANDLE stop_{};
    std::thread watcher_;
};
#else
inline constexpr char kRasterWorkerName[] = "vove-raster-worker";
#ifdef VOVE_HAS_MUPDF_DOCUMENT_WORKER
inline constexpr char kDocumentWorkerName[] = "vove-document-worker";
#endif
inline constexpr char kCdrWorkerName[] = "vove-cdr-worker";
inline constexpr char kXcfWorkerName[] = "vove-xcf-worker";
#ifdef VOVE_HAS_RESVG_SVG_WORKER
inline constexpr char kSvgWorkerName[] = "vove-svg-worker";
#endif
#endif

class ParentProcessGuard final {
  public:
    ParentProcessGuard(const qulonglong process_id, const qulonglong process_created) noexcept {
#ifdef _WIN32
        if (process_id == 0 || process_id > std::numeric_limits<DWORD>::max() ||
            process_created == 0) {
            return;
        }
        process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(process_id));
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        if (!process_ || GetProcessTimes(process_, &created, &exited, &kernel, &user) == FALSE ||
            WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
            if (process_) {
                CloseHandle(process_);
            }
            process_ = nullptr;
            return;
        }
        ULARGE_INTEGER native_created{};
        native_created.LowPart = created.dwLowDateTime;
        native_created.HighPart = created.dwHighDateTime;
        if (native_created.QuadPart != process_created) {
            CloseHandle(process_);
            process_ = nullptr;
            return;
        }
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) {
            CloseHandle(process_);
            process_ = nullptr;
            return;
        }
        try {
            watcher_ = std::thread([this] {
                const HANDLE handles[]{process_, stop_};
                const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) {
                    TerminateProcess(GetCurrentProcess(), 8);
                }
            });
            valid_ = true;
        } catch (...) {
            CloseHandle(stop_);
            CloseHandle(process_);
            stop_ = nullptr;
            process_ = nullptr;
        }
#else
        static_cast<void>(process_created);
        if (process_id == 0 ||
            process_id > static_cast<qulonglong>(std::numeric_limits<pid_t>::max())) {
            return;
        }
        process_id_ = static_cast<pid_t>(process_id);
        valid_ = process_id_ > 1 && ::getppid() == process_id_ &&
                 ::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && ::getppid() == process_id_;
#endif
    }

    ~ParentProcessGuard() {
#ifdef _WIN32
        if (stop_) {
            SetEvent(stop_);
        }
        if (watcher_.joinable()) {
            watcher_.join();
        }
        if (stop_) {
            CloseHandle(stop_);
        }
        if (process_) {
            CloseHandle(process_);
        }
#endif
    }

    ParentProcessGuard(const ParentProcessGuard &) = delete;
    ParentProcessGuard &operator=(const ParentProcessGuard &) = delete;

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return valid_;
#else
        return valid_;
#endif
    }

  private:
#ifdef _WIN32
    HANDLE process_{};
    HANDLE stop_{};
    std::thread watcher_;
    bool valid_{};
#else
    pid_t process_id_{};
    bool valid_{};
#endif
};

[[nodiscard]] std::filesystem::path native_path(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}

int run(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("vove-preview-helper"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("VO-VE persistent preview helper"));
    parser.addHelpOption();
    const QCommandLineOption server_option(
        QStringLiteral("server"), QStringLiteral("Local server name."), QStringLiteral("name"));
    const QCommandLineOption cache_option(
        QStringLiteral("cache"), QStringLiteral("Persistent cache root."), QStringLiteral("path"));
    const QCommandLineOption cache_limit_option(QStringLiteral("cache-limit-bytes"),
                                                QStringLiteral("Maximum preview cache bytes."),
                                                QStringLiteral("bytes"));
    const QCommandLineOption build_option(QStringLiteral("build"),
                                          QStringLiteral("Protocol build identifier."),
                                          QStringLiteral("id"));
    const QCommandLineOption parent_option(QStringLiteral("parent-pid"),
                                           QStringLiteral("Owning UI process identifier."),
                                           QStringLiteral("pid"));
    const QCommandLineOption parent_created_option(
        QStringLiteral("parent-created"), QStringLiteral("Owning process creation timestamp."),
        QStringLiteral("timestamp"));
    const QCommandLineOption cmyk_profile_option(QStringLiteral("cmyk-profile"),
                                                 QStringLiteral("Fallback CMYK ICC profile."),
                                                 QStringLiteral("path"));
    const QCommandLineOption ghostscript_option(QStringLiteral("ghostscript"),
                                                QStringLiteral("Ghostscript executable."),
                                                QStringLiteral("path"));
    parser.addOptions({server_option, cache_option, cache_limit_option, build_option, parent_option,
                       parent_created_option, cmyk_profile_option, ghostscript_option});
#ifdef _WIN32
    const QCommandLineOption prepare_runtime_option(
        QStringLiteral("prepare-worker-runtime"),
        QStringLiteral("Prepare read/execute access to this installation's decoder files."));
    const QCommandLineOption request_shutdown_option(
        QStringLiteral("request-installation-shutdown"),
        QStringLiteral("Ask preview helpers from this installation to exit."));
    parser.addOptions({prepare_runtime_option, request_shutdown_option});
#endif
    parser.process(application);

#ifdef _WIN32
    if (parser.isSet(prepare_runtime_option)) {
        const auto prepared = vove::worker::prepare_windows_worker_runtime(
            native_path(QCoreApplication::applicationDirPath()));
        if (!prepared.ok()) {
            std::cerr << "runtime access: " << prepared.item << " error=" << prepared.system_error
                      << '\n';
            return 10;
        }
        return 0;
    }
    if (parser.isSet(request_shutdown_option)) {
        return request_installation_shutdown();
    }
#endif

    const auto auth_token = qEnvironmentVariable("VOVE_PREVIEW_AUTH_TOKEN");
    qunsetenv("VOVE_PREVIEW_AUTH_TOKEN");
    if (!parser.isSet(server_option) || auth_token.isEmpty() || !parser.isSet(cache_option) ||
        !parser.isSet(build_option) || !parser.isSet(parent_option)) {
        std::cerr << "required helper arguments are missing\n";
        return 2;
    }

    bool parent_id_ok{};
    const auto parent_id = parser.value(parent_option).toULongLong(&parent_id_ok);
    bool parent_created_ok = true;
    qulonglong parent_created{};
#ifdef _WIN32
    parent_created = parser.value(parent_created_option).toULongLong(&parent_created_ok);
#endif
    ParentProcessGuard parent_guard(parent_id, parent_created);
    if (!parent_id_ok || !parent_created_ok || !parent_guard.valid()) {
        std::cerr << "preview helper owner is unavailable\n";
        return 8;
    }

    const auto cache_root = native_path(parser.value(cache_option));
    std::error_code filesystem_error;
    std::filesystem::create_directories(cache_root, filesystem_error);
    if (filesystem_error) {
        std::cerr << "preview cache directory is unavailable\n";
        return 3;
    }

    auto index = std::make_unique<vove::cache::SqliteArtifactIndex>(cache_root / "index.sqlite3");
    if (!index->ready()) {
        std::cerr << "preview cache index is unavailable\n";
        return 4;
    }

    vove::cache::DiskCacheLimits disk_cache_limits;
    if (parser.isSet(cache_limit_option)) {
        bool cache_limit_ok{};
        const auto cache_limit = parser.value(cache_limit_option).toULongLong(&cache_limit_ok);
        if (!cache_limit_ok || cache_limit == 0) {
            std::cerr << "preview cache limit is invalid\n";
            return 9;
        }
        disk_cache_limits.maximum_bytes = cache_limit;
    }

    const auto worker_name = QString::fromLatin1(kRasterWorkerName);
    vove::preview::WorkerThumbnailRendererOptions renderer_options{
        .worker_executable =
            native_path(QCoreApplication::applicationDirPath() + QLatin1Char('/') + worker_name),
        .expected_build_id = "vove-raster-jpeg-3",
        .document_worker_executable = {},
        .document_expected_build_id = {},
        .cdr_worker_executable =
            native_path(QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                        QString::fromLatin1(kCdrWorkerName)),
        .cdr_expected_build_id = "vove-embedded-documents-2",
        .svg_worker_executable = {},
        .svg_expected_build_id = {},
        .svg_font_environment_fingerprint = {},
        .xcf_worker_executable =
            native_path(QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                        QString::fromLatin1(kXcfWorkerName)),
        .xcf_expected_build_id = "vove-kimageformats-xcf-2",
        .ghostscript_executable = parser.isSet(ghostscript_option)
                                      ? native_path(parser.value(ghostscript_option))
                                      : std::filesystem::path{},
        .fallback_cmyk_profile = parser.isSet(cmyk_profile_option)
                                     ? native_path(parser.value(cmyk_profile_option))
                                     : std::filesystem::path{},
        .timeout = std::chrono::seconds(10)};
#ifdef VOVE_HAS_MUPDF_DOCUMENT_WORKER
    renderer_options.document_worker_executable =
        native_path(QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                    QString::fromLatin1(kDocumentWorkerName));
    renderer_options.document_expected_build_id = "vove-stage5-mupdf-1";
#endif
#ifdef VOVE_HAS_RESVG_SVG_WORKER
    renderer_options.svg_worker_executable =
        native_path(QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                    QString::fromLatin1(kSvgWorkerName));
    renderer_options.svg_expected_build_id = "vove-stage10d-resvg-1";
#endif
#ifdef _WIN32
    InstallationShutdownGuard installation_shutdown_guard;
    // Portable copies and updates may replace file ACLs. Check once per helper, never per image.
    const auto prepared = vove::worker::prepare_windows_worker_runtime(
        native_path(QCoreApplication::applicationDirPath()));
    if (!prepared.ok()) {
        // A read-only volume may already permit loading. Keep normal restrictions and let the
        // supervisor return the actual launch error instead of blocking cached previews.
        std::cerr << "runtime access: " << prepared.item << " error=" << prepared.system_error
                  << '\n';
    }
#endif
    auto backend = vove::preview::make_worker_thumbnail_renderer(std::move(renderer_options));
    vove::preview::helper::Service service(
        {.server_name_utf8 = parser.value(server_option).toUtf8().toStdString(),
         .auth_token_utf8 = auth_token.toUtf8().toStdString(),
         .build_id_utf8 = parser.value(build_option).toUtf8().toStdString(),
         .cache_root = cache_root,
         .disk_cache_limits = disk_cache_limits,
         .client_disconnected = [] { QCoreApplication::quit(); },
#ifdef __linux__
         .abstract_namespace = true,
         .expected_peer_process_id = parent_id},
#else
         .abstract_namespace = false,
         .expected_peer_process_id = 0},
#endif
        std::move(index), std::move(backend.render), std::move(backend.resolve_source),
        std::move(backend.resolve_color_policy));
    if (!service.listen()) {
        std::cerr << "preview helper could not listen\n";
        return 5;
    }
    return application.exec();
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception &) {
        std::cerr << "preview helper configuration is invalid\n";
        return 6;
    } catch (...) {
        std::cerr << "preview helper failed\n";
        return 7;
    }
}
