#pragma once

#include "vove/worker/protocol.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QProcess;
class QTimer;

namespace vove::ui {

class GifAnimationClient final : public QObject {
  public:
    explicit GifAnimationClient(QObject *parent = nullptr, QString helper_program_override = {});
    ~GifAnimationClient() override;

    void start(QString path, quint64 size, qint64 modified, QString revision, QString key);
    void stop();
    void set_paused(bool paused);
    void set_suspended(bool suspended);
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool readers_idle() const noexcept;

    std::function<void(QImage, const QString &, bool animated)> frame_ready;
    std::function<void(bool success, const QString &detail)> finished;

  private:
    struct Request {
        QString path;
        quint64 size{};
        qint64 modified{};
        QString revision;
        QString key;
    };
    enum class ReadState { header, metadata, image, staged, acknowledgement };

    void schedule_start();
    void launch();
    void read_stdout();
    void drain_stderr();
    bool consume_metadata();
    bool display_frame();
    bool present_frame(QImage image, bool restart_delay = false);
    void acknowledge();
    void update_timers();
    void change_hold(bool &flag, bool value);
    void complete(bool success, QString detail);
    [[nodiscard]] bool receiving() const noexcept;

    QProcess *process_{};
    QTimer *frameTimer_{};
    QTimer *watchdog_{};
    QElapsedTimer clock_;
    std::optional<Request> pending_;
    QString helperProgramOverride_;
    QString key_;
    QByteArray buffer_;
    QImage stagedImage_;
    worker::AnimationFrame frame_;
    ReadState readState_{ReadState::header};
    qsizetype needed_{worker::kProtocolHeaderBytes};
    quint64 generation_{};
    quint64 runningGeneration_{};
    quint64 nextSequence_{};
    qint64 frameDeadline_{};
    qint64 holdStarted_{};
    qint64 watchdogRemaining_{20'000};
    bool active_{};
    bool retiring_{};
    bool startScheduled_{};
    bool paused_{};
    bool suspended_{};
    bool haveFrame_{};
};

} // namespace vove::ui
