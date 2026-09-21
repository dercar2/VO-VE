#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <functional>

namespace vove::ui {

struct DirectoryMonitorOptions {
    int debounce_ms{750};
    int minimum_refresh_ms{2'000};
    int fallback_ms{30'000};
    int maximum_interval_ms{600'000};
    int busy_retry_ms{1'000};
    int startup_timeout_ms{5'000};
};

// Notifications are cheap hints; all directory reads use the existing asynchronous catalog source.
class DirectoryMonitor final : public QObject {
  public:
    explicit DirectoryMonitor(QObject *parent = nullptr, QString helper_program = {},
                              DirectoryMonitorOptions options = {});
    ~DirectoryMonitor() override;
    void set_directory(const QString &path);
    void set_visible(bool visible);
    void request_recheck();
    void scan_started();
    void scan_finished(bool successful);
    void stop();
    std::function<bool()> refresh_requested;

    [[nodiscard]] bool scanning() const noexcept {
        return scanning_;
    }
    [[nodiscard]] bool watching() const noexcept {
        return watching_;
    }
    [[nodiscard]] int fallback_interval() const noexcept {
        return fallbackInterval_;
    }

  private:
    void start_watch();
    void read_notifications();
    void mark_changed();
    void schedule();
    void request_scan();
    void finish_watch();

    QProcess process_;
    QTimer refreshTimer_;
    QTimer startupTimer_;
    QElapsedTimer clock_;
    DirectoryMonitorOptions options_;
    QString program_;
    QString path_;
    QString watchedPath_;
    QByteArray output_;
    qint64 scanStartedAt_{};
    qint64 nextAllowedAt_{};
    qint64 fallbackAt_{};
    qint64 nextWatchAttemptAt_{};
    int fallbackInterval_{};
    bool visible_{};
    bool dirty_{};
    bool scanning_{};
    bool watching_{};
    bool remote_{true};
    bool receivedReady_{};
    bool stopping_{};
};
} // namespace vove::ui
