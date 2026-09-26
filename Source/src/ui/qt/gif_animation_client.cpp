#include "gif_animation_client.hpp"

#include "vove/cache/qoi_codec.hpp"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QtEndian>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace vove::ui {
namespace {

constexpr quint64 kMaximumSourceBytes = 64ULL * 1024 * 1024;
constexpr quint64 kMaximumQoiBytes = 24ULL * 1024 * 1024;
constexpr quint32 kMaximumEdge = 2048;
constexpr quint32 kMaximumMetadataBytes = 2U * 1024 * 1024;

std::span<const std::byte> bytes(const QByteArray &value) {
    return {reinterpret_cast<const std::byte *>(value.constData()),
            static_cast<std::size_t>(value.size())};
}

#ifdef Q_OS_WIN
qulonglong current_process_creation_time() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE)
        return 0;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}
#endif

} // namespace

GifAnimationClient::GifAnimationClient(QObject *parent, QString helper_program_override)
    : QObject(parent), helperProgramOverride_(std::move(helper_program_override)) {
    process_ = new QProcess(this);
    frameTimer_ = new QTimer(this);
    watchdog_ = new QTimer(this);
    frameTimer_->setSingleShot(true);
    frameTimer_->setTimerType(Qt::PreciseTimer);
    watchdog_->setSingleShot(true);
    clock_.start();
    connect(frameTimer_, &QTimer::timeout, this, [this] { acknowledge(); });
    connect(watchdog_, &QTimer::timeout, this, [this] {
        if (receiving())
            complete(false, QStringLiteral("GIF frame timed out"));
    });
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] { read_stdout(); });
    connect(process_, &QProcess::readyReadStandardError, this, [this] { drain_stderr(); });
    connect(process_, &QProcess::started, this, [this] {
        if (retiring_)
            process_->kill();
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        QPointer<GifAnimationClient> self(this);
        if (receiving())
            complete(false, QStringLiteral("GIF helper process failed"));
        if (!self)
            return;
        if (error == QProcess::FailedToStart) {
            retiring_ = false;
            schedule_start();
        }
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                QPointer<GifAnimationClient> self(this);
                read_stdout();
                if (!self)
                    return;
                if (receiving())
                    complete(false, QStringLiteral("GIF helper stream ended early"));
                if (!self)
                    return;
                retiring_ = false;
                schedule_start();
            });
}

GifAnimationClient::~GifAnimationClient() {
    process_->disconnect(this);
    stop();
    // A callback can destroy us inside QProcess::start/errorOccurred. Keep the
    // emitter alive until that stack unwinds, even when launch already failed.
    process_->setParent(nullptr);
    if (process_->state() == QProcess::NotRunning) {
        process_->deleteLater();
        return;
    }
    // A running QProcess destructor waits. Let its single killed child be reaped asynchronously.
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process_,
            &QObject::deleteLater);
    connect(process_, &QProcess::errorOccurred, process_, [process = process_](auto error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    process_->kill();
}

bool GifAnimationClient::receiving() const noexcept {
    return active_ && !retiring_ && runningGeneration_ == generation_;
}

bool GifAnimationClient::readers_idle() const noexcept {
    return process_->state() == QProcess::NotRunning && !pending_;
}

void GifAnimationClient::start(QString path, quint64 size, qint64 modified, QString revision,
                               QString key) {
    stop();
    paused_ = false;
    active_ = true;
    pending_ = Request{std::move(path), size, modified, std::move(revision), std::move(key)};
    schedule_start();
}

void GifAnimationClient::stop() {
    ++generation_;
    active_ = false;
    pending_.reset();
    frameTimer_->stop();
    watchdog_->stop();
    buffer_ = {};
    stagedImage_ = {};
    frame_ = {};
    key_.clear();
    haveFrame_ = false;
    retiring_ = process_->state() != QProcess::NotRunning;
    if (retiring_)
        process_->kill();
}

void GifAnimationClient::schedule_start() {
    if (!pending_ || startScheduled_ || process_->state() != QProcess::NotRunning)
        return;
    startScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        startScheduled_ = false;
        if (pending_ && process_->state() == QProcess::NotRunning)
            launch();
    });
}

void GifAnimationClient::launch() {
    auto request = std::move(*pending_);
    pending_.reset();
    retiring_ = false;
    runningGeneration_ = generation_;
    if (request.path.isEmpty() || request.path.contains(QChar::Null) || request.size == 0 ||
        request.size > kMaximumSourceBytes || request.revision.isEmpty() ||
        request.revision.size() > 4096 || request.revision.contains(QChar::Null)) {
        complete(false, QStringLiteral("GIF source identity is invalid"));
        return;
    }
    key_ = std::move(request.key);
    buffer_ = {};
    frame_ = {};
    readState_ = ReadState::header;
    needed_ = worker::kProtocolHeaderBytes;
    nextSequence_ = 0;
    haveFrame_ = false;
    frameDeadline_ = 0;
    holdStarted_ = clock_.elapsed();
    watchdogRemaining_ = 20'000;
    QStringList arguments{
        QStringLiteral("--gif-stream"),      request.path,
        QStringLiteral("--source-size"),     QString::number(request.size),
        QStringLiteral("--source-modified"), QString::number(request.modified),
        QStringLiteral("--source-revision"), request.revision,
        QStringLiteral("--parent-pid"),      QString::number(QCoreApplication::applicationPid())};
#ifdef Q_OS_WIN
    const auto created = current_process_creation_time();
    if (created == 0) {
        complete(false, QStringLiteral("GIF helper owner identity is unavailable"));
        return;
    }
    arguments << QStringLiteral("--parent-created") << QString::number(created);
    const auto name = QStringLiteral("vove-preview-helper.exe");
#else
    const auto name = QStringLiteral("vove-preview-helper");
#endif
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("VOVE_PREVIEW_AUTH_TOKEN"));
    process_->setProcessEnvironment(environment);
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    process_->setReadChannel(QProcess::StandardOutput);
    QPointer<GifAnimationClient> self(this);
    process_->start(helperProgramOverride_.isEmpty()
                        ? QDir(QCoreApplication::applicationDirPath()).filePath(name)
                        : helperProgramOverride_,
                    arguments);
    if (!self)
        return;
    update_timers();
}

void GifAnimationClient::drain_stderr() {
    process_->setReadChannel(QProcess::StandardError);
    for (int i = 0; i != 16 && process_->bytesAvailable() > 0; ++i)
        static_cast<void>(process_->read(4096));
    const bool remaining = process_->bytesAvailable() > 0;
    process_->setReadChannel(QProcess::StandardOutput);
    if (remaining)
        QTimer::singleShot(0, this, [this] { drain_stderr(); });
}

void GifAnimationClient::read_stdout() {
    process_->setReadChannel(QProcess::StandardOutput);
    if (!receiving()) {
        static_cast<void>(process_->read(64 * 1024));
        return;
    }
    while (receiving() && process_->bytesAvailable() > 0) {
        if (readState_ == ReadState::acknowledgement || readState_ == ReadState::staged) {
            complete(false, QStringLiteral("GIF helper sent an unrequested frame"));
            return;
        }
        const auto count = std::min<qint64>(needed_ - buffer_.size(), process_->bytesAvailable());
        buffer_.append(process_->read(count));
        if (buffer_.size() != needed_)
            return;
        if (readState_ == ReadState::header) {
            const auto *data = reinterpret_cast<const uchar *>(buffer_.constData());
            const auto kind = qFromLittleEndian<quint16>(data + 8);
            const auto size = qFromLittleEndian<quint32>(data + 12);
            if (qFromLittleEndian<quint32>(data) != worker::kProtocolMagic ||
                qFromLittleEndian<quint16>(data + 4) != worker::kProtocolMajor ||
                qFromLittleEndian<quint16>(data + 6) != worker::kProtocolMinor ||
                qFromLittleEndian<quint16>(data + 10) != 0 ||
                qFromLittleEndian<quint64>(data + 16) != 1 ||
                (kind != static_cast<quint16>(worker::MessageKind::animation_frame) &&
                 kind != static_cast<quint16>(worker::MessageKind::worker_result)) ||
                size <= worker::kProtocolHeaderBytes || size > kMaximumMetadataBytes) {
                complete(false, QStringLiteral("GIF stream header is invalid"));
                return;
            }
            needed_ = static_cast<qsizetype>(size);
            readState_ = ReadState::metadata;
        } else if (readState_ == ReadState::metadata) {
            if (!consume_metadata())
                return;
        } else if (readState_ == ReadState::image) {
            if (!display_frame())
                return;
        }
    }
}

bool GifAnimationClient::consume_metadata() {
    worker::DecodedFrame decoded;
    worker::DecodeError error;
    if (!worker::decode_frame(bytes(buffer_), decoded, error)) {
        complete(false, QStringLiteral("GIF stream metadata is invalid"));
        return false;
    }
    if (decoded.header.kind == worker::MessageKind::worker_result) {
        worker::WorkerResult result;
        if (!worker::decode_worker_result(bytes(buffer_), result, error)) {
            complete(false, QStringLiteral("GIF terminal result is invalid"));
        } else {
            const bool success = result.status == worker::ResultStatus::success && haveFrame_;
            auto detail = QString::fromUtf8(result.diagnostic_utf8);
            if (!success && detail.isEmpty())
                detail = QStringLiteral("GIF playback failed");
            complete(success, std::move(detail));
        }
        return false;
    }
    if (!worker::decode_animation_frame(bytes(buffer_), frame_, error) ||
        frame_.image.width > kMaximumEdge || frame_.image.height > kMaximumEdge ||
        frame_.image.bytes_written < 22 || frame_.image.bytes_written > kMaximumQoiBytes ||
        frame_.sequence != nextSequence_ || nextSequence_ == std::numeric_limits<quint64>::max()) {
        complete(false, QStringLiteral("GIF frame metadata exceeds playback limits"));
        return false;
    }
    ++nextSequence_;
    needed_ = static_cast<qsizetype>(frame_.image.bytes_written);
    buffer_ = {};
    readState_ = ReadState::image;
    return true;
}

bool GifAnimationClient::display_frame() {
    const auto *data = reinterpret_cast<const uchar *>(buffer_.constData());
    if (!buffer_.startsWith("qoif") || qFromBigEndian<quint32>(data + 4) != frame_.image.width ||
        qFromBigEndian<quint32>(data + 8) != frame_.image.height || data[12] != 4 || data[13] > 1) {
        complete(false, QStringLiteral("GIF QOI header does not match its metadata"));
        return false;
    }
    auto decoded = cache::decode_qoi_rgba8(bytes(buffer_));
    buffer_ = {};
    if (!decoded.ok() || decoded.width != frame_.image.width ||
        decoded.height != frame_.image.height ||
        decoded.rgba8.size() != static_cast<std::size_t>(decoded.width) * decoded.height * 4) {
        complete(false, QStringLiteral("GIF QOI frame is malformed"));
        return false;
    }
    QImage borrowed(reinterpret_cast<const uchar *>(decoded.rgba8.data()),
                    static_cast<int>(decoded.width), static_cast<int>(decoded.height),
                    static_cast<qsizetype>(decoded.width) * 4, QImage::Format_RGBA8888);
    auto image = borrowed.copy();
    if (image.isNull()) {
        complete(false, QStringLiteral("GIF frame allocation failed"));
        return false;
    }
    image.setColorSpace(QColorSpace::SRgb);
    watchdog_->stop();
    // Preserve the initial still, but never replace it with an in-flight response while held.
    if (haveFrame_ && (paused_ || suspended_)) {
        stagedImage_ = std::move(image);
        readState_ = ReadState::staged;
        return true;
    }
    return present_frame(std::move(image));
}

bool GifAnimationClient::present_frame(QImage image, const bool restart_delay) {
    const auto now = clock_.elapsed();
    // Carry the presentation deadline forward so transport/decode time does not accumulate.
    frameDeadline_ = haveFrame_ && !restart_delay ? std::max(frameDeadline_ + frame_.delay_ms, now)
                                                  : now + frame_.delay_ms;
    haveFrame_ = true;
    readState_ = ReadState::acknowledgement;
    if (paused_ || suspended_)
        holdStarted_ = now;
    const auto generation = generation_;
    const auto key = key_;
    const auto callback = frame_ready;
    QPointer<GifAnimationClient> self(this);
    if (callback)
        callback(std::move(image), key, frame_.animated);
    if (!self || generation != generation_ || !receiving())
        return false;
    update_timers();
    return true;
}

void GifAnimationClient::acknowledge() {
    if (!receiving() || paused_ || suspended_ || readState_ != ReadState::acknowledgement)
        return;
    if (clock_.elapsed() < frameDeadline_) {
        update_timers();
        return;
    }
    const char advance = 1;
    if (process_->write(&advance, 1) != 1) {
        complete(false, QStringLiteral("GIF acknowledgement failed"));
        return;
    }
    frame_ = {};
    readState_ = ReadState::header;
    needed_ = worker::kProtocolHeaderBytes;
    watchdogRemaining_ = 15'000;
    update_timers();
}

void GifAnimationClient::update_timers() {
    frameTimer_->stop();
    if (!receiving() || paused_ || suspended_ || readState_ == ReadState::staged) {
        watchdog_->stop();
        return;
    }
    if (readState_ == ReadState::acknowledgement) {
        watchdog_->stop();
        frameTimer_->start(static_cast<int>(std::clamp<qint64>(frameDeadline_ - clock_.elapsed(), 0,
                                                               std::numeric_limits<int>::max())));
    } else if (!watchdog_->isActive()) {
        watchdog_->start(static_cast<int>(watchdogRemaining_));
    }
}

void GifAnimationClient::change_hold(bool &flag, bool value) {
    if (flag == value)
        return;
    const bool wasHeld = paused_ || suspended_;
    flag = value;
    const bool held = paused_ || suspended_;
    if (held != wasHeld) {
        const auto now = clock_.elapsed();
        if (held) {
            holdStarted_ = now;
            if (watchdog_->isActive())
                watchdogRemaining_ = std::max(1, watchdog_->remainingTime());
        } else if (haveFrame_) {
            frameDeadline_ += now - holdStarted_;
        }
    }
    if (!held && receiving() && readState_ == ReadState::staged) {
        auto image = std::exchange(stagedImage_, {});
        static_cast<void>(present_frame(std::move(image), true));
        return;
    }
    update_timers();
}

void GifAnimationClient::set_paused(bool value) {
    change_hold(paused_, value);
}
void GifAnimationClient::set_suspended(bool value) {
    change_hold(suspended_, value);
}

void GifAnimationClient::complete(bool success, QString detail) {
    if (!receiving())
        return;
    auto callback = finished;
    stop();
    if (callback)
        callback(success, detail);
}

} // namespace vove::ui
