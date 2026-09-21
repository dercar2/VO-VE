#include "directory_monitor.hpp"

#include <QCoreApplication>
#include <QDir>

#include <algorithm>
#include <utility>

namespace vove::ui {
DirectoryMonitor::DirectoryMonitor(QObject *parent, QString helper_program,
                                   DirectoryMonitorOptions options)
    : QObject(parent), options_(options), program_(std::move(helper_program)),
      fallbackInterval_(options.fallback_ms) {
    setObjectName(QStringLiteral("directoryMonitor"));
    if (program_.isEmpty()) {
        program_ = QCoreApplication::applicationDirPath() + QStringLiteral("/vove-open-helper");
#ifdef Q_OS_WIN
        program_ += QStringLiteral(".exe");
#endif
    }
    clock_.start();
    refreshTimer_.setSingleShot(true);
    startupTimer_.setSingleShot(true);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&refreshTimer_, &QTimer::timeout, this, &DirectoryMonitor::request_scan);
    connect(&process_, &QProcess::readyReadStandardOutput, this,
            &DirectoryMonitor::read_notifications);
    connect(&process_, &QProcess::readyReadStandardError, this,
            [this] { static_cast<void>(process_.readAllStandardError()); });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this] { finish_watch(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish_watch();
        }
    });
    connect(&startupTimer_, &QTimer::timeout, this, [this] {
        watching_ = false;
        process_.kill();
        schedule();
    });
}

DirectoryMonitor::~DirectoryMonitor() {
    stop();
    process_.disconnect(this);
    // Folder transitions never wait for a filesystem call.
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        static_cast<void>(process_.waitForFinished(500));
    }
}

void DirectoryMonitor::stop() {
    stopping_ = true;
    visible_ = false;
    refreshTimer_.stop();
    startupTimer_.stop();
    process_.closeWriteChannel();
    process_.kill();
}

void DirectoryMonitor::set_directory(const QString &path) {
    if (path_ == path) {
        return;
    }
    path_ = path;
    stopping_ = false;
    dirty_ = false;
    scanning_ = false;
    watching_ = false;
    remote_ = true;
    receivedReady_ = false;
    output_.clear();
    fallbackInterval_ = options_.fallback_ms;
    nextAllowedAt_ = clock_.elapsed();
    fallbackAt_ = nextAllowedAt_ + fallbackInterval_;
    nextWatchAttemptAt_ = 0;
    refreshTimer_.stop();
    startupTimer_.stop();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
    } else {
        start_watch();
    }
    schedule();
}

void DirectoryMonitor::set_visible(const bool visible) {
    if (visible_ == visible || stopping_) {
        return;
    }
    visible_ = visible;
    if (!visible_) {
        refreshTimer_.stop();
        startupTimer_.stop();
        watching_ = false;
        process_.kill();
        return;
    }
    dirty_ = true;
    nextWatchAttemptAt_ = 0;
    start_watch();
    schedule();
}

void DirectoryMonitor::start_watch() {
    if (!visible_ || stopping_ || path_.isEmpty() || process_.state() != QProcess::NotRunning) {
        return;
    }
    watchedPath_ = path_;
    output_.clear();
    receivedReady_ = false;
    nextWatchAttemptAt_ = clock_.elapsed() + fallbackInterval_;
    process_.start(program_, {QStringLiteral("watch-directory"), path_});
    startupTimer_.start(options_.startup_timeout_ms);
}

void DirectoryMonitor::read_notifications() {
    const auto bytes = process_.readAllStandardOutput();
    if (watchedPath_ != path_ || !visible_ || stopping_) {
        return;
    }
    output_.append(bytes);
    if (output_.size() > 4096) {
        process_.kill();
        return;
    }
    qsizetype newline;
    while ((newline = output_.indexOf('\n')) >= 0) {
        const auto line = output_.first(newline).trimmed();
        output_.remove(0, newline + 1);
        if (line == "CHANGED") {
            mark_changed();
        } else if (line == "READY LOCAL WATCH" || line == "READY REMOTE WATCH" ||
                   line == "READY LOCAL POLL" || line == "READY REMOTE POLL") {
            receivedReady_ = true;
            watching_ = line.endsWith("WATCH");
            remote_ = line.contains("REMOTE");
            startupTimer_.stop();
            if (watching_) {
                // Reconcile the gap between the initial listing and watch registration.
                mark_changed();
            } else {
                schedule();
            }
        }
    }
}

void DirectoryMonitor::finish_watch() {
    read_notifications();
    startupTimer_.stop();
    const bool was_watching = watching_;
    watching_ = false;
    if (stopping_ || !visible_) {
        return;
    }
    if (watchedPath_ != path_ || clock_.elapsed() >= nextWatchAttemptAt_) {
        start_watch();
    } else if (was_watching || !receivedReady_) {
        mark_changed();
    }
    schedule();
}

void DirectoryMonitor::mark_changed() {
    if (dirty_) {
        return;
    }
    dirty_ = true;
    nextAllowedAt_ = std::max(nextAllowedAt_, clock_.elapsed() + options_.debounce_ms);
    schedule();
}

void DirectoryMonitor::request_recheck() {
    mark_changed();
}

void DirectoryMonitor::scan_started() {
    scanning_ = true;
    dirty_ = false;
    scanStartedAt_ = clock_.elapsed();
    refreshTimer_.stop();
}

void DirectoryMonitor::scan_finished(const bool successful) {
    if (!scanning_) {
        return;
    }
    scanning_ = false;
    const auto now = clock_.elapsed();
    const auto duration = std::max<qint64>(0, now - scanStartedAt_);
    const auto cooldown = std::clamp<qint64>(duration * 10, options_.minimum_refresh_ms,
                                             options_.maximum_interval_ms);
    nextAllowedAt_ = now + cooldown;
    if (successful) {
        fallbackInterval_ = static_cast<int>(
            std::clamp<qint64>(duration * 20, options_.fallback_ms, options_.maximum_interval_ms));
    } else {
        fallbackInterval_ = std::min(options_.maximum_interval_ms,
                                     std::max(options_.fallback_ms, fallbackInterval_ * 2));
        nextAllowedAt_ = now + fallbackInterval_;
        dirty_ = true;
    }
    fallbackAt_ = now + fallbackInterval_;
    if (successful && !watching_ && now >= nextWatchAttemptAt_ &&
        process_.state() == QProcess::NotRunning) {
        start_watch();
    }
    schedule();
}

void DirectoryMonitor::schedule() {
    refreshTimer_.stop();
    if (!visible_ || stopping_ || path_.isEmpty() || scanning_) {
        return;
    }
    if (!dirty_ && watching_ && !remote_) {
        return;
    }
    const auto target = dirty_ ? nextAllowedAt_ : std::max(nextAllowedAt_, fallbackAt_);
    refreshTimer_.start(static_cast<int>(
        std::clamp<qint64>(target - clock_.elapsed(), 1, options_.maximum_interval_ms)));
}

void DirectoryMonitor::request_scan() {
    if (!visible_ || stopping_ || scanning_) {
        return;
    }
    if (refresh_requested && refresh_requested()) {
        return;
    }
    // A modal dialog, drag or file operation takes priority over automatic catalog changes.
    refreshTimer_.start(options_.busy_retry_ms);
}
} // namespace vove::ui
