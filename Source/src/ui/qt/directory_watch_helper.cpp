#include "directory_watch_helper.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimer>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <QWinEventNotifier>
#elif defined(Q_OS_LINUX)
#include <QSocketNotifier>
#include <sys/inotify.h>
#endif
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace vove::ui {
namespace {
class DirectoryWatch final : public QObject {
  public:
    explicit DirectoryWatch(QObject *parent) : QObject(parent) {}
    ~DirectoryWatch() override {
#ifdef Q_OS_WIN
        if (directory_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(directory_, &overlapped_);
            DWORD transferred{};
            static_cast<void>(GetOverlappedResult(directory_, &overlapped_, &transferred, TRUE));
            CloseHandle(directory_);
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
#elif defined(Q_OS_LINUX)
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
#endif
    }

    bool start(const QString &path, std::function<void(bool)> changed,
               const QString &entry_name = {}) {
        changed_ = std::move(changed);
#ifdef Q_OS_WIN
        entryName_ = entry_name;
        const auto native = QDir::toNativeSeparators(path);
        directory_ =
            CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            CloseHandle(directory_);
            directory_ = INVALID_HANDLE_VALUE;
            return false;
        }
        overlapped_.hEvent = event_;
        auto *notifier = new QWinEventNotifier(event_, this);
        connect(notifier, &QWinEventNotifier::activated, this, [this, notifier] {
            notifier->setEnabled(false);
            DWORD transferred{};
            const bool completed =
                GetOverlappedResult(directory_, &overlapped_, &transferred, FALSE);
            const auto error = completed ? ERROR_SUCCESS : GetLastError();
            if (completed || error == ERROR_NOTIFY_ENUM_DIR) {
                if (relevant_change(transferred)) {
                    changed_(false);
                }
                if (arm()) {
                    notifier->setEnabled(true);
                    return;
                }
            }
            changed_(true);
        });
        return arm();
#elif defined(Q_OS_LINUX)
        static_cast<void>(entry_name);
        descriptor_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (descriptor_ < 0 ||
            inotify_add_watch(descriptor_, path.toUtf8().constData(),
                              IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                  IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT) < 0) {
            return false;
        }
        auto *notifier = new QSocketNotifier(descriptor_, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this, notifier] {
            alignas(inotify_event) std::array<char, 8192> buffer{};
            bool lost{};
            bool changed{};
            for (int pass = 0; pass < 8; ++pass) {
                const auto bytes = read(descriptor_, buffer.data(), buffer.size());
                if (bytes < 0 && errno == EINTR) {
                    continue;
                }
                if (bytes <= 0) {
                    lost = lost || (bytes < 0 && errno != EAGAIN);
                    break;
                }
                changed = true;
                for (std::size_t offset = 0;
                     offset + sizeof(inotify_event) <= static_cast<std::size_t>(bytes);) {
                    const auto *event =
                        reinterpret_cast<const inotify_event *>(buffer.data() + offset);
                    lost = lost || (event->mask &
                                    (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT));
                    offset += sizeof(inotify_event) + event->len;
                }
            }
            if (lost) {
                notifier->setEnabled(false);
            }
            if (changed || lost) {
                changed_(lost);
            }
        });
        return true;
#else
        static_cast<void>(entry_name);
        auto *watcher = new QFileSystemWatcher(this);
        connect(watcher, &QFileSystemWatcher::directoryChanged, this,
                [this, watcher] { changed_(watcher->directories().isEmpty()); });
        return watcher->addPath(path);
#endif
    }

  private:
    std::function<void(bool)> changed_;
#ifdef Q_OS_WIN
    bool relevant_change(const DWORD bytes) const {
        if (entryName_.isEmpty() || bytes == 0) {
            return true;
        }
        constexpr auto header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        std::size_t offset{};
        while (offset + header <= bytes) {
            const auto *event =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buffer_.data() + offset);
            if (event->FileNameLength > bytes - offset - header ||
                event->FileNameLength % sizeof(wchar_t) != 0) {
                return true;
            }
            const auto name = QString::fromWCharArray(
                event->FileName, static_cast<int>(event->FileNameLength / sizeof(wchar_t)));
            if (name.compare(entryName_, Qt::CaseInsensitive) == 0) {
                return true;
            }
            if (event->NextEntryOffset == 0) {
                return false;
            }
            if (event->NextEntryOffset < header || event->NextEntryOffset % alignof(DWORD) ||
                event->NextEntryOffset > bytes - offset) {
                return true;
            }
            offset += event->NextEntryOffset;
        }
        return true;
    }
    bool arm() {
        ResetEvent(event_);
        const DWORD filters = entryName_.isEmpty()
                                  ? FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                        FILE_NOTIFY_CHANGE_ATTRIBUTES
                                  : FILE_NOTIFY_CHANGE_DIR_NAME;
        return ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()),
                                     FALSE, filters, nullptr, &overlapped_, nullptr) != FALSE;
    }
    QString entryName_;
    HANDLE directory_{INVALID_HANDLE_VALUE};
    HANDLE event_{};
    OVERLAPPED overlapped_{};
    alignas(DWORD) std::array<char, 8192> buffer_{};
#elif defined(Q_OS_LINUX)
    int descriptor_{-1};
#endif
};

void report(const char *message) {
    std::fputs(message, stdout);
    std::fflush(stdout);
}

bool remote_directory(const QString &path) {
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path);
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return true;
    }
    const auto root = native.left(3);
    return GetDriveTypeW(reinterpret_cast<const wchar_t *>(root.utf16())) == DRIVE_REMOTE;
#else
    const auto type = QStorageInfo(path).fileSystemType().toLower();
    return type.isEmpty() || type == "cifs" || type == "smb3" || type.startsWith("smb") ||
           type.startsWith("nfs") || type.startsWith("fuse");
#endif
}
} // namespace

int run_directory_watch_helper(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() != 3 || !QDir::isAbsolutePath(arguments.at(2))) {
        return EXIT_FAILURE;
    }
    // The parent owns stdin. A closed pipe ends this read-only helper even after a parent crash.
    std::thread([] {
        // A stdio read holds a lock that can prevent normal process exit while stdin stays open.
        std::array<char, 64> buffer{};
#ifdef Q_OS_WIN
        DWORD bytes{};
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer.data(),
                        static_cast<DWORD>(buffer.size()), &bytes, nullptr) &&
               bytes != 0) {
        }
#else
        ssize_t bytes{};
        do {
            bytes = read(STDIN_FILENO, buffer.data(), buffer.size());
        } while (bytes > 0 || (bytes < 0 && errno == EINTR));
#endif
        std::_Exit(EXIT_SUCCESS);
    }).detach();

    const auto path = QDir::cleanPath(arguments.at(2));
    const auto remote = remote_directory(path);
    DirectoryWatch watcher(&application);
    QTimer changed;
    changed.setSingleShot(true);
    changed.setInterval(250);
    bool lost_watch{};
    QObject::connect(&changed, &QTimer::timeout, &application, [&] {
        report("CHANGED\n");
        if (lost_watch) {
            application.quit();
        }
    });
    const auto notify = [&](const bool lost) {
        lost_watch = lost_watch || lost;
        if (!changed.isActive()) {
            changed.start();
        }
    };
    bool watched = watcher.start(path, notify);
#ifdef Q_OS_WIN
    // Windows content notifications exclude renaming the watched directory itself.
    // Observe only its name in the parent, without scanning or watching descendants.
    DirectoryWatch parent_watcher(&application);
    const QFileInfo directory(path);
    if (watched && directory.absolutePath() != path) {
        watched = parent_watcher.start(
            directory.absolutePath(), [&](bool) { notify(true); }, directory.fileName());
    }
#endif
    report(remote ? (watched ? "READY REMOTE WATCH\n" : "READY REMOTE POLL\n")
                  : (watched ? "READY LOCAL WATCH\n" : "READY LOCAL POLL\n"));
    return watched ? application.exec() : EXIT_SUCCESS;
}
} // namespace vove::ui
