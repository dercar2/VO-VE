#include "preview_client.hpp"
#include "diagnostic_sink.hpp"

#include "external_component_detection.hpp"

#include "vove/color/color_transform.h"
#include "vove/color/monitor_profile.h"

#include <QCoreApplication>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace vove::ui {
namespace {

namespace protocol = preview::helper_protocol;
inline constexpr int kMaximumConnectAttempts = 40;
inline constexpr int kConnectRetryMs = 50;
inline constexpr int kIdleStopMs = 60'000;
inline constexpr int kCacheMaintenanceTimeoutMs = 10 * 60 * 1000;
inline constexpr qsizetype kMaximumQueuedFrames = 512;
// Keep most catalog work locally prunable while feeding the serial helper ahead of completion.
inline constexpr qsizetype kMaximumDispatchedRequests = 8;
inline constexpr qsizetype kSelectedDispatchReserve = 1;
inline constexpr std::uint8_t kMaximumBackpressureAttempts = 8;
inline constexpr int kBackpressureRetryMs = 25;

[[nodiscard]] std::span<const std::byte> bytes(const QByteArray &value) noexcept {
    return {reinterpret_cast<const std::byte *>(value.constData()),
            static_cast<std::size_t>(value.size())};
}

[[nodiscard]] QByteArray byte_array(const std::vector<std::byte> &value) {
    return {reinterpret_cast<const char *>(value.data()), static_cast<qsizetype>(value.size())};
}

[[nodiscard]] std::string utf8(const QString &value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] QString random_hex(const int bytes) {
    QByteArray value(bytes, Qt::Uninitialized);
    for (auto &character : value) {
        character = static_cast<char>(QRandomGenerator::system()->generate() & 0xFFU);
    }
    return QString::fromLatin1(value.toHex());
}

[[nodiscard]] QImage transformed_image(const color::TransformResult &transformed) {
    if (!transformed.ok() || transformed.width == 0 || transformed.height == 0 ||
        transformed.rgba_pixels.empty()) {
        return {};
    }
    const QImage borrowed(reinterpret_cast<const uchar *>(transformed.rgba_pixels.data()),
                          static_cast<int>(transformed.width), static_cast<int>(transformed.height),
                          static_cast<qsizetype>(transformed.rgba_stride), QImage::Format_RGBA8888);
    auto result = borrowed.copy();
    result.setColorSpace({});
    return result;
}

[[nodiscard]] QString canonical_pixel_key(const QImage &image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("VOVE-DISPLAY-PIXELS-V1", 22));
    hash.addData(QByteArray::number(image.width()));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArray::number(image.height()));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArray::number(image.bytesPerLine()));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(
        QByteArrayView(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()));
    return QString::fromLatin1(hash.result().toHex());
}

#ifdef Q_OS_WIN
[[nodiscard]] qulonglong current_process_creation_time() noexcept {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}
#endif

} // namespace

PreviewClient::PreviewClient(QObject *parent, const int request_timeout_ms,
                             QString helper_program_override, const int selected_request_timeout_ms,
                             const int extended_request_timeout_ms)
    : QObject(parent), helperProgramOverride_(std::move(helper_program_override)),
      requestTimeoutMs_(std::clamp(request_timeout_ms, 100, 120'000)),
      selectedRequestTimeoutMs_(std::clamp(selected_request_timeout_ms, 100, 120'000)),
      extendedRequestTimeoutMs_(
          std::clamp(extended_request_timeout_ms, requestTimeoutMs_, 135'000)) {
    process_ = new QProcess(this);
    connectTimer_ = new QTimer(this);
    idleTimer_ = new QTimer(this);
    requestTimer_ = new QTimer(this);
    connectTimer_->setSingleShot(true);
    idleTimer_->setSingleShot(true);
    requestTimer_->setSingleShot(true);
    replace_socket();

    QObject::connect(process_, &QProcess::started, this, [this] {
        const auto pid = std::to_string(process_->processId());
        DiagnosticSink::instance().record("preview.helper_started", {{"pid", pid}});
        connect_to_helper();
    });
    QObject::connect(
        process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            const auto code = std::to_string(static_cast<int>(error));
            DiagnosticSink::instance().record("preview.helper_error", {{"code", code}});
            if (!stopping_ && !restarting_) {
                fail_pending(protocol::ResponseStatus::internal_error);
            }
        });
    QObject::connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                     [this](int exit_code, QProcess::ExitStatus exit_status) {
                         const auto code = std::to_string(exit_code);
                         const auto status = std::to_string(static_cast<int>(exit_status));
                         DiagnosticSink::instance().record("preview.helper_finished",
                                                           {{"code", code}, {"status", status}});
                         authenticated_ = false;
                         if (restarting_) {
                             QTimer::singleShot(0, this, [this] {
                                 if (restarting_ && process_->state() == QProcess::NotRunning) {
                                     finish_restart();
                                 }
                             });
                         } else if (!stopping_) {
                             cleanup_server_endpoint();
                             fail_pending(protocol::ResponseStatus::internal_error);
                         }
                     });
    QObject::connect(connectTimer_, &QTimer::timeout, this, [this] { connect_to_helper(); });
    QObject::connect(idleTimer_, &QTimer::timeout, this, [this] {
        if (pending_.isEmpty() && pendingCache_.isEmpty() && queuedFrames_.isEmpty()) {
            stop();
        }
    });
    QObject::connect(requestTimer_, &QTimer::timeout, this, [this] { handle_request_timeout(); });
}

PreviewClient::~PreviewClient() {
    stop();
}

void PreviewClient::replace_socket() {
    if (socket_ != nullptr) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
    }
    socket_ = new QLocalSocket(this);
#ifdef Q_OS_LINUX
    socket_->setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    QObject::connect(socket_, &QLocalSocket::connected, this, [this] { authenticate(); });
    QObject::connect(socket_, &QLocalSocket::readyRead, this, [this] { receive(); });
    QObject::connect(socket_, &QLocalSocket::bytesWritten, this,
                     [this](qint64) { flush_frames(); });
    QObject::connect(socket_, &QLocalSocket::disconnected, this, [this] {
        authenticated_ = false;
        receiveBuffer_.clear();
        if (!stopping_ && !restarting_ && process_->state() != QProcess::NotRunning) {
            fail_pending(protocol::ResponseStatus::disconnected);
            process_->kill();
        }
    });
    QObject::connect(
        socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
            const auto helper_not_ready = error == QLocalSocket::ServerNotFoundError ||
                                          error == QLocalSocket::ConnectionRefusedError;
            if (stopping_ || !helper_not_ready || process_->state() == QProcess::NotRunning ||
                connectAttempts_ >= kMaximumConnectAttempts) {
                if (!stopping_ && connectAttempts_ >= kMaximumConnectAttempts) {
                    fail_pending(protocol::ResponseStatus::internal_error);
                }
                return;
            }
            ++connectAttempts_;
            connectTimer_->start(kConnectRetryMs);
        });
}

void PreviewClient::set_reply_handler(ReplyHandler handler) {
    replyHandler_ = std::move(handler);
}

void PreviewClient::set_cache_maximum_bytes(const std::uint64_t bytes) {
    if (bytes == 0 || cacheMaximumBytes_ == bytes) {
        return;
    }
    cacheMaximumBytes_ = bytes;
    if (process_->state() != QProcess::NotRunning) {
        restart_helper();
    }
}

protocol::RequestId PreviewClient::inspect_cache(CacheHandler handler) {
    return request_cache(protocol::CacheOperation::inspect, std::move(handler));
}

protocol::RequestId PreviewClient::clear_cache(CacheHandler handler) {
    return request_cache(protocol::CacheOperation::clear, std::move(handler));
}

protocol::RequestId PreviewClient::request_cache(const protocol::CacheOperation operation,
                                                 CacheHandler handler) {
    if (!handler || pendingCache_.size() >= 4) {
        return 0;
    }
    if (nextRequestId_ == 0 || nextRequestId_ == std::numeric_limits<protocol::RequestId>::max()) {
        nextRequestId_ = 1;
    }
    const auto request_id = nextRequestId_++;
    try {
        const auto frame =
            protocol::encode_cache_request({.request_id = request_id, .operation = operation});
        pendingCache_.insert(static_cast<qulonglong>(request_id), std::move(handler));
        if (!queue_frame(frame, request_id)) {
            auto failed = pendingCache_.take(static_cast<qulonglong>(request_id));
            failed({});
            return 0;
        }
        ensure_started();
        idleTimer_->stop();
        QTimer::singleShot(kCacheMaintenanceTimeoutMs, this, [this, request_id] {
            const auto key = static_cast<qulonglong>(request_id);
            if (!pendingCache_.contains(key)) {
                return;
            }
            auto failed = pendingCache_.take(key);
            failed({});
            restart_helper();
        });
        return request_id;
    } catch (...) {
        pendingCache_.remove(static_cast<qulonglong>(request_id));
        return 0;
    }
}

void PreviewClient::refresh_external_components(std::function<void(bool)> completion) {
    if (completion) {
        externalComponentRefreshCallbacks_.push_back(std::move(completion));
    }
    if (externalComponentRefreshRunning_) {
        return;
    }
    if (qEnvironmentVariableIsSet("VOVE_GHOSTSCRIPT_EXECUTABLE")) {
        finish_external_component_refresh(ghostscript_executable(false));
        return;
    }
    externalComponentRefreshRunning_ = true;
    auto result = std::make_shared<QString>();
    auto *probe = QThread::create(
        [result] { *result = external_component_detail::detect_ghostscript_on_system(); });
    connect(probe, &QThread::finished, probe, &QObject::deleteLater);
    connect(probe, &QThread::finished, this, [this, result] {
        externalComponentRefreshRunning_ = false;
        finish_external_component_refresh(std::move(*result));
    });
    probe->start();
}

void PreviewClient::finish_external_component_refresh(QString ghostscript) {
    cache_ghostscript_executable(std::move(ghostscript));
    const auto refreshed_ghostscript = ghostscript_executable(false);
    const auto changed = refreshed_ghostscript != ghostscriptExecutable_;
    ghostscriptExecutable_ = refreshed_ghostscript;
    if (changed && process_->state() != QProcess::NotRunning) {
        restart_helper();
    }
    auto callbacks = std::move(externalComponentRefreshCallbacks_);
    externalComponentRefreshCallbacks_.clear();
    for (auto &callback : callbacks) {
        callback(changed);
    }
}

bool PreviewClient::set_monitor_name(const QString &screen_name) {
#ifdef Q_OS_WIN
    const auto encoded_name = screen_name.toUtf8();
#else
    QString monitor_key;
    const auto screens = QGuiApplication::screens();
    for (qsizetype index = 0; index < screens.size(); ++index) {
        if (screens[index] != nullptr && screens[index]->name() == screen_name) {
            monitor_key = QString::number(index);
            break;
        }
    }
    const auto encoded_name = monitor_key.toUtf8();
#endif
    const auto profile = color::load_monitor_icc(
        {encoded_name.constData(), static_cast<std::size_t>(encoded_name.size())});
    QByteArray bytes_value;
    if (!profile.empty()) {
        bytes_value = QByteArray(reinterpret_cast<const char *>(profile.data()),
                                 static_cast<qsizetype>(profile.size()));
    }
    return set_monitor_profile(std::move(bytes_value));
}

bool PreviewClient::set_monitor_profile(QByteArray profile) {
    const auto fingerprint =
        profile.isEmpty()
            ? QString{}
            : QString::fromLatin1(
                  QCryptographicHash::hash(profile, QCryptographicHash::Sha256).toHex());
    if (fingerprint == monitorFingerprint_) {
        return false;
    }
    monitorIcc_ = std::move(profile);
    monitorFingerprint_ = fingerprint;
    return true;
}

protocol::RequestId PreviewClient::request_preview(
    const qulonglong entry_id, const std::uint64_t generation, const QString &path,
    const std::uint64_t source_size, const std::int64_t modified_unix_ns,
    const QString &source_revision, const std::uint16_t presentation_edge,
    const preview::ThumbnailPriority priority, ReplyHandler handler, const std::uint32_t page_index,
    const QString &password, const bool offline_cache_only, const bool allow_offline_fallback,
    const bool selected_view, const bool extended_limits) {
    if (path.isEmpty() || generation == 0 || presentation_edge == 0 ||
        presentation_edge > protocol::kMaximumCanonicalEdge ||
        page_index >= protocol::kMaximumPageCount ||
        password.toUtf8().size() > static_cast<qsizetype>(protocol::kMaximumPasswordBytes) ||
        pending_.size() >= kMaximumQueuedFrames) {
        return 0;
    }
    if (extended_limits &&
        std::any_of(pending_.cbegin(), pending_.cend(),
                    [](const Pending &pending) { return pending.extended_limits; })) {
        return 0;
    }
    if (nextRequestId_ == 0 || nextRequestId_ == std::numeric_limits<protocol::RequestId>::max()) {
        nextRequestId_ = 1;
    }
    const auto request_id = nextRequestId_++;
    try {
        auto frame =
            protocol::encode_preview_request({.request_id = request_id,
                                              .generation = generation,
                                              .path_utf8 = utf8(path),
                                              .source_size = source_size,
                                              .modified_unix_ns = modified_unix_ns,
                                              .source_revision_utf8 = utf8(source_revision),
                                              .canonical_edge = presentation_edge,
                                              .priority = priority,
                                              .page_index = page_index,
                                              .password_utf8 = utf8(password),
                                              .offline_cache_only = offline_cache_only,
                                              .allow_offline_fallback = allow_offline_fallback,
                                              .selected_view = selected_view,
                                              .extended_limits = extended_limits});
        pending_.insert(static_cast<qulonglong>(request_id),
                        {.entry_id = entry_id,
                         .generation = generation,
                         .source_size = source_size,
                         .modified_unix_ns = modified_unix_ns,
                         .path = path,
                         .source_revision = source_revision,
                         .presentation_edge = presentation_edge,
                         .page_index = page_index,
                         .priority = priority,
                         .dispatched = false,
                         .active = false,
                         .selected_view = selected_view,
                         .extended_limits = extended_limits,
                         .backpressure_attempts = 0,
                         .retry_not_before = {},
                         .deadline = std::chrono::steady_clock::time_point::max(),
                         .request_frame = byte_array(frame),
                         .handler = std::move(handler)});
        const auto diagnostic_path = utf8(path);
        const auto diagnostic_size = std::to_string(source_size);
        const auto diagnostic_request = std::to_string(request_id);
        DiagnosticSink::instance().record("preview.request",
                                          {{"id", diagnostic_request},
                                           {"path", diagnostic_path},
                                           {"bytes", diagnostic_size},
                                           {"selected", selected_view ? "1" : "0"},
                                           {"extended", extended_limits ? "1" : "0"}});
        dispatch_deferred_requests();
        ensure_started();
        arm_request_timeout();
        idleTimer_->stop();
        return request_id;
    } catch (...) {
        pending_.remove(static_cast<qulonglong>(request_id));
        return 0;
    }
}

bool PreviewClient::readers_idle() const noexcept {
    return pending_.isEmpty() && !restarting_ && !stopping_ && activeRequestId_ == 0 &&
           (process_->state() == QProcess::NotRunning || authenticated_);
}

void PreviewClient::cancel_generation(const std::uint64_t generation) {
    if (generation == 0) {
        return;
    }
    QList<qulonglong> removed_ids;
    bool cancelled_inflight{};
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (iterator->generation == generation) {
            removed_ids.push_back(iterator.key());
            cancelled_inflight = cancelled_inflight || iterator->dispatched || iterator->active ||
                                 iterator.key() == static_cast<qulonglong>(activeRequestId_);
            iterator = pending_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = queuedFrames_.begin(); iterator != queuedFrames_.end();) {
        if (iterator->request_id != 0 &&
            removed_ids.contains(static_cast<qulonglong>(iterator->request_id))) {
            iterator = queuedFrames_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    try {
        const auto frame = protocol::encode_cancel_generation({.generation = generation});
        if (!queue_frame(frame)) {
            socket_->abort();
        }
        flush_frames();
    } catch (...) {
        socket_->abort();
    }
    if (cancelled_inflight && process_->state() != QProcess::NotRunning) {
        restart_helper(/*preserve_extended_attempts=*/true);
    }
    arm_request_timeout();
    schedule_idle_stop();
}

void PreviewClient::retain_generation_entries(const std::uint64_t generation,
                                              const QSet<qulonglong> &entry_ids) {
    if (generation == 0) {
        return;
    }
    QHash<qulonglong, Pending> removed;
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (iterator->generation == generation && !iterator->dispatched && !iterator->active &&
            iterator.key() != static_cast<qulonglong>(activeRequestId_) &&
            !entry_ids.contains(iterator->entry_id)) {
            removed.insert(iterator.key(), std::move(iterator.value()));
            iterator = pending_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (removed.isEmpty()) {
        return;
    }
    queuedFrames_.removeIf([&removed](const QueuedFrame &frame) {
        return frame.request_id != 0 && removed.contains(static_cast<qulonglong>(frame.request_id));
    });
    arm_request_timeout();
    schedule_idle_stop();

    // Detach the whole batch before callbacks can enqueue, cancel, or destroy the client.
    QPointer<PreviewClient> self(this);
    for (auto iterator = removed.begin(); iterator != removed.end(); ++iterator) {
        if (!self) {
            return;
        }
        self->deliver_completion(static_cast<protocol::RequestId>(iterator.key()),
                                 std::move(iterator.value()), protocol::ResponseStatus::cancelled);
    }
}

void PreviewClient::ensure_started() {
    if (process_->state() != QProcess::NotRunning) {
        flush_frames();
        return;
    }
    stopping_ = false;
    authenticated_ = false;
    connectAttempts_ = 0;
    cleanup_server_endpoint();
#ifdef Q_OS_WIN
    serverName_ = QStringLiteral("vove-preview-%1-%2")
                      .arg(QCoreApplication::applicationPid())
                      .arg(random_hex(8));
#elif defined(Q_OS_LINUX)
    serverName_ = QStringLiteral("vove-preview-%1-%2")
                      .arg(QCoreApplication::applicationPid())
                      .arg(random_hex(8));
#else
    if (!serverDirectory_) {
        const auto directory_template = QDir(QDir::tempPath())
                                            .filePath(QStringLiteral("vove-preview-%1-XXXXXX")
                                                          .arg(QCoreApplication::applicationPid()));
        serverDirectory_ = std::make_unique<QTemporaryDir>(directory_template);
        if (!serverDirectory_->isValid()) {
            serverDirectory_.reset();
            fail_pending(protocol::ResponseStatus::internal_error);
            return;
        }
    }
    serverName_ = QDir(serverDirectory_->path()).filePath(QStringLiteral("endpoint"));
#endif
    authToken_ = random_hex(32);
    auto helper = helperProgramOverride_.isEmpty()
                      ? QDir(QCoreApplication::applicationDirPath())
                            .filePath(
#ifdef Q_OS_WIN
                                QStringLiteral("vove-preview-helper.exe"))
#else
                                QStringLiteral("vove-preview-helper"))
#endif
                      : helperProgramOverride_;
    auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty()) {
        cache = QDir::tempPath() + QStringLiteral("/VO-VE-cache");
    }
    cache = QDir(cache).filePath(QStringLiteral("previews"));
    process_->setProgram(helper);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("VOVE_PREVIEW_AUTH_TOKEN"), authToken_);
    process_->setProcessEnvironment(environment);
    QStringList arguments{QStringLiteral("--server"),
                          serverName_,
                          QStringLiteral("--cache"),
                          cache,
                          QStringLiteral("--cache-limit-bytes"),
                          QString::number(cacheMaximumBytes_),
                          QStringLiteral("--build"),
                          buildId_,
                          QStringLiteral("--parent-pid"),
                          QString::number(QCoreApplication::applicationPid())};
#ifdef Q_OS_WIN
    const auto parent_created = current_process_creation_time();
    if (parent_created == 0) {
        fail_pending(protocol::ResponseStatus::internal_error);
        return;
    }
    arguments.append({QStringLiteral("--parent-created"), QString::number(parent_created)});
#endif
    ghostscriptExecutable_ = ghostscript_executable();
    if (!ghostscriptExecutable_.isEmpty()) {
        arguments.append(
            {QStringLiteral("--ghostscript"), QDir::toNativeSeparators(ghostscriptExecutable_)});
    }
    process_->setArguments(arguments);
    process_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    const auto diagnostic_helper = utf8(helper);
    DiagnosticSink::instance().record("preview.helper_launch", {{"program", diagnostic_helper}});
    process_->start();
}

void PreviewClient::connect_to_helper() {
    if (stopping_ || serverName_.isEmpty() || socket_->state() != QLocalSocket::UnconnectedState) {
        return;
    }
#ifdef Q_OS_LINUX
    socket_->setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    socket_->connectToServer(serverName_, QIODevice::ReadWrite);
}

void PreviewClient::authenticate() {
    try {
        const auto hello = protocol::encode_hello(
            {.build_id_utf8 = utf8(buildId_), .auth_token_utf8 = utf8(authToken_)});
        const auto payload = byte_array(hello);
        if (socket_->write(payload) != payload.size()) {
            socket_->abort();
        }
    } catch (...) {
        socket_->abort();
    }
}

bool PreviewClient::queue_frame(const std::vector<std::byte> &frame,
                                const protocol::RequestId request_id) {
    if (queuedFrames_.size() >= kMaximumQueuedFrames) {
        return false;
    }
    queuedFrames_.push_back({.request_id = request_id, .bytes = byte_array(frame)});
    flush_frames();
    return true;
}

void PreviewClient::flush_frames() {
    if (!authenticated_ || socket_->state() != QLocalSocket::ConnectedState) {
        return;
    }
    while (!queuedFrames_.isEmpty() &&
           socket_->bytesToWrite() < static_cast<qint64>(protocol::kMaximumFrameBytes)) {
        const auto &frame = queuedFrames_.front().bytes;
        if (socket_->write(frame) != frame.size()) {
            socket_->abort();
            return;
        }
        queuedFrames_.pop_front();
    }
}

qsizetype PreviewClient::dispatched_request_count() const noexcept {
    qsizetype count{};
    for (auto iterator = pending_.cbegin(); iterator != pending_.cend(); ++iterator) {
        count += iterator->dispatched ? 1 : 0;
    }
    return count;
}

bool PreviewClient::has_dispatched_requests() const noexcept {
    return dispatched_request_count() != 0;
}

void PreviewClient::dispatch_deferred_requests() {
    auto dispatched = dispatched_request_count();
    const auto now = std::chrono::steady_clock::now();
    while (dispatched < kMaximumDispatchedRequests + kSelectedDispatchReserve &&
           queuedFrames_.size() < kMaximumQueuedFrames) {
        auto best = pending_.end();
        for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
            // A selected preview can reach the helper without waiting for a catalog slot.
            if (iterator->dispatched || iterator->retry_not_before > now ||
                (dispatched >= kMaximumDispatchedRequests &&
                 iterator->priority != preview::ThumbnailPriority::visible_selected)) {
                continue;
            }
            if (best == pending_.end() ||
                static_cast<std::uint8_t>(iterator->priority) <
                    static_cast<std::uint8_t>(best->priority) ||
                (iterator->priority == best->priority && iterator.key() < best.key())) {
                best = iterator;
            }
        }
        if (best == pending_.end()) {
            break;
        }
        queuedFrames_.push_back({.request_id = static_cast<protocol::RequestId>(best.key()),
                                 .bytes = best->request_frame});
        best->dispatched = true;
        ++dispatched;
    }
    if (authenticated_ && activeRequestId_ == 0 && has_dispatched_requests() &&
        progressDeadline_ == std::chrono::steady_clock::time_point::max()) {
        progressDeadline_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{requestTimeoutMs_};
    }
    flush_frames();
    arm_request_timeout();
}

void PreviewClient::receive() {
    const auto room = static_cast<qint64>(protocol::kMaximumFrameBytes) - receiveBuffer_.size();
    if (room <= 0) {
        socket_->abort();
        return;
    }
    receiveBuffer_.append(socket_->read(std::min(socket_->bytesAvailable(), room)));
    if (!consume_frames()) {
        socket_->abort();
    }
}

bool PreviewClient::consume_frames() {
    while (receiveBuffer_.size() >= static_cast<qsizetype>(protocol::kFrameHeaderBytes)) {
        protocol::FrameHeader header;
        protocol::DecodeError error;
        if (!protocol::decode_header(bytes(receiveBuffer_), header, error)) {
            return false;
        }
        if (receiveBuffer_.size() < static_cast<qsizetype>(header.total_size)) {
            return true;
        }
        const auto frame = receiveBuffer_.first(static_cast<qsizetype>(header.total_size));
        receiveBuffer_.remove(0, static_cast<qsizetype>(header.total_size));
        dispatch_frame(frame);
    }
    return true;
}

void PreviewClient::dispatch_frame(const QByteArray &frame) {
    protocol::FrameHeader header;
    protocol::DecodeError error;
    if (!protocol::decode_header(bytes(frame), header, error)) {
        socket_->abort();
        return;
    }
    if (!authenticated_) {
        protocol::HelloAck ack;
        if (!protocol::decode_hello_ack(bytes(frame), ack, error) ||
            ack.build_id_utf8 != utf8(buildId_) || ack.auth_token_utf8 != utf8(authToken_)) {
            socket_->abort();
            return;
        }
        authenticated_ = true;
        DiagnosticSink::instance().record("preview.helper_authenticated");
        dispatch_deferred_requests();
        if (has_dispatched_requests()) {
            progressDeadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{requestTimeoutMs_};
        }
        flush_frames();
        arm_request_timeout();
        return;
    }

    if (header.type == protocol::MessageType::job_started) {
        protocol::JobStarted started;
        if (!protocol::decode_job_started(bytes(frame), started, error)) {
            socket_->abort();
            return;
        }
        const auto found = pending_.find(static_cast<qulonglong>(started.request_id));
        if (found == pending_.end()) {
            return;
        }
        if (activeRequestId_ != 0 && activeRequestId_ != started.request_id) {
            socket_->abort();
            return;
        }
        activeRequestId_ = started.request_id;
        found->active = true;
        if (found->deadline == std::chrono::steady_clock::time_point::max()) {
            found->deadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds{
                    found->extended_limits
                        ? extendedRequestTimeoutMs_
                        : (found->selected_view ? selectedRequestTimeoutMs_ : requestTimeoutMs_)};
        }
        progressDeadline_ = std::chrono::steady_clock::time_point::max();
        arm_request_timeout();
        return;
    }

    if (header.type == protocol::MessageType::cache_response) {
        protocol::CacheResponse response;
        if (!protocol::decode_cache_response(bytes(frame), response, error)) {
            socket_->abort();
            return;
        }
        const auto key = static_cast<qulonglong>(response.request_id);
        if (!pendingCache_.contains(key)) {
            return;
        }
        auto handler = pendingCache_.take(key);
        const auto success = response.status == protocol::CacheStatus::success;
        if (success) {
            displayCache_.clear();
        }
        handler({.success = success, .entries = response.entries, .bytes = response.bytes});
        schedule_idle_stop();
        return;
    }

    protocol::PreviewResponse response;
    if (!protocol::decode_preview_response(bytes(frame), response, error)) {
        socket_->abort();
        return;
    }
    const auto found = pending_.find(static_cast<qulonglong>(response.request_id));
    if (found == pending_.end()) {
        return;
    }
    if (response.status == protocol::ResponseStatus::queue_busy) {
        if (activeRequestId_ == response.request_id) {
            activeRequestId_ = 0;
        }
        found->active = false;
        found->dispatched = false;
        found->deadline = std::chrono::steady_clock::time_point::max();
        ++found->backpressure_attempts;
        if (found->backpressure_attempts > kMaximumBackpressureAttempts) {
            complete_request(response.request_id, protocol::ResponseStatus::timed_out);
        } else {
            const auto delay = kBackpressureRetryMs * found->backpressure_attempts;
            found->retry_not_before =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{delay};
            QTimer::singleShot(delay, this, [this] { retry_backpressured_requests(); });
        }
        progressDeadline_ =
            has_dispatched_requests()
                ? std::chrono::steady_clock::now() + std::chrono::milliseconds{requestTimeoutMs_}
                : std::chrono::steady_clock::time_point::max();
        dispatch_deferred_requests();
        return;
    }
    auto pending = std::move(found.value());
    pending_.erase(found);
    if (activeRequestId_ == response.request_id) {
        activeRequestId_ = 0;
    }
    PreviewReply reply{
        .request_id = response.request_id,
        .entry_id = pending.entry_id,
        .generation = pending.generation,
        .source_size = pending.source_size,
        .modified_unix_ns = pending.modified_unix_ns,
        .source_revision = pending.source_revision,
        .presentation_edge = pending.presentation_edge,
        .status = response.status,
        .image = {},
        .source_color_model = QString::fromUtf8(response.source_color_model_utf8),
        .source_color_profile = QString::fromUtf8(response.source_color_profile_utf8),
        .source_profile_fingerprint = QString::fromUtf8(response.source_profile_fingerprint),
        .provenance = response.provenance,
        .page_index = response.status == protocol::ResponseStatus::success_cached ||
                              response.status == protocol::ResponseStatus::success_offline_cached ||
                              response.status == protocol::ResponseStatus::success_decoded
                          ? response.page_index
                          : pending.page_index,
        .page_count = response.page_count,
        .startup_diagnostic = response.startup_diagnostic};
    const auto diagnostic_request = std::to_string(response.request_id);
    const auto diagnostic_status = std::to_string(static_cast<unsigned>(response.status));
    const auto diagnostic_path = utf8(pending.path);
    const auto diagnostic_stage =
        std::to_string(static_cast<unsigned>(response.startup_diagnostic.stage));
    const auto diagnostic_worker =
        std::to_string(static_cast<unsigned>(response.startup_diagnostic.worker));
    const auto diagnostic_system_error = std::to_string(response.startup_diagnostic.system_error);
    const auto diagnostic_exit_code = std::to_string(response.startup_diagnostic.exit_code);
    DiagnosticSink::instance().record("preview.result", {{"id", diagnostic_request},
                                                         {"path", diagnostic_path},
                                                         {"status", diagnostic_status},
                                                         {"worker_stage", diagnostic_stage},
                                                         {"worker", diagnostic_worker},
                                                         {"system_error", diagnostic_system_error},
                                                         {"exit_code", diagnostic_exit_code}});
    if ((response.status == protocol::ResponseStatus::success_cached ||
         response.status == protocol::ResponseStatus::success_offline_cached ||
         response.status == protocol::ResponseStatus::success_decoded) &&
        response.width != 0 && response.height != 0 &&
        response.rgba8.size() == static_cast<std::size_t>(response.width) * response.height * 4U) {
        const QImage borrowed(reinterpret_cast<const uchar *>(response.rgba8.data()),
                              response.width, response.height,
                              static_cast<qsizetype>(response.width) * 4, QImage::Format_RGBA8888);
        reply.image = borrowed.copy();
        reply.image.setColorSpace(QColorSpace::SRgb);
    }
    dispatch_deferred_requests();
    if (has_dispatched_requests()) {
        progressDeadline_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{requestTimeoutMs_};
    } else {
        progressDeadline_ = std::chrono::steady_clock::time_point::max();
    }
    arm_request_timeout();
    schedule_idle_stop();
    deliver_reply(std::move(reply), std::move(pending.handler));
}

void PreviewClient::deliver_reply(PreviewReply reply, ReplyHandler handler) {
    if (reply.image.isNull() || monitorIcc_.isEmpty() || monitorFingerprint_.isEmpty()) {
        deliver_final(std::move(reply), std::move(handler));
        return;
    }
    auto key = canonical_pixel_key(reply.image);
    const auto cache_key = utf8(key);
    const auto monitor_key = utf8(monitorFingerprint_);
    if (const auto cached = displayCache_.find(cache_key, monitor_key)) {
        auto image = transformed_image(*cached);
        if (!image.isNull()) {
            reply.image = std::move(image);
        }
        deliver_final(std::move(reply), std::move(handler));
        return;
    }

    const auto monitor_icc = monitorIcc_;
    const auto monitor_fingerprint = monitorFingerprint_;
    QPointer<PreviewClient> self(this);
    QThreadPool::globalInstance()->start([self, reply = std::move(reply),
                                          handler = std::move(handler), key = std::move(key),
                                          monitor_icc, monitor_fingerprint]() mutable {
        const auto &canonical = reply.image;
        const auto pixels =
            std::span<const std::byte>(reinterpret_cast<const std::byte *>(canonical.constBits()),
                                       static_cast<std::size_t>(canonical.sizeInBytes()));
        const auto profile =
            std::span<const std::byte>(reinterpret_cast<const std::byte *>(monitor_icc.constData()),
                                       static_cast<std::size_t>(monitor_icc.size()));
        auto transformed = color::to_monitor_rgba8({
            .width = static_cast<std::uint32_t>(canonical.width()),
            .height = static_cast<std::uint32_t>(canonical.height()),
            .canonical_stride = static_cast<std::size_t>(canonical.bytesPerLine()),
            .canonical_srgb_rgba = pixels,
            .monitor_icc = profile,
        });
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, reply = std::move(reply), handler = std::move(handler), key = std::move(key),
             monitor_fingerprint, transformed = std::move(transformed)]() mutable {
                if (!self) {
                    return;
                }
                if (monitor_fingerprint != self->monitorFingerprint_) {
                    self->deliver_reply(std::move(reply), std::move(handler));
                    return;
                }
                if (transformed.ok()) {
                    auto image = transformed_image(transformed);
                    if (!image.isNull()) {
                        reply.image = std::move(image);
                    }
                    static_cast<void>(self->displayCache_.store(
                        utf8(key), utf8(monitor_fingerprint), std::move(transformed)));
                }
                self->deliver_final(std::move(reply), std::move(handler));
            },
            Qt::QueuedConnection);
    });
}

void PreviewClient::deliver_final(PreviewReply reply, ReplyHandler &&handler) {
    if (handler) {
        handler(std::move(reply));
    } else if (replyHandler_) {
        replyHandler_(std::move(reply));
    }
}

void PreviewClient::fail_pending(const protocol::ResponseStatus status) {
    requestTimer_->stop();
    auto pending = std::exchange(pending_, {});
    queuedFrames_.clear();
    activeRequestId_ = 0;
    progressDeadline_ = std::chrono::steady_clock::time_point::max();
    for (auto iterator = pending.cbegin(); iterator != pending.cend(); ++iterator) {
        PreviewReply reply{.request_id = static_cast<protocol::RequestId>(iterator.key()),
                           .entry_id = iterator->entry_id,
                           .generation = iterator->generation,
                           .source_size = iterator->source_size,
                           .modified_unix_ns = iterator->modified_unix_ns,
                           .source_revision = iterator->source_revision,
                           .presentation_edge = iterator->presentation_edge,
                           .status = status,
                           .image = {},
                           .source_color_model = {},
                           .source_color_profile = {},
                           .source_profile_fingerprint = {},
                           .provenance = protocol::PreviewProvenance::primary_render,
                           .page_index = iterator->page_index,
                           .page_count = 0};
        if (iterator->handler) {
            iterator->handler(std::move(reply));
        } else if (replyHandler_) {
            replyHandler_(std::move(reply));
        }
    }
    fail_cache_requests();
    schedule_idle_stop();
}

void PreviewClient::fail_cache_requests() {
    auto pending = std::exchange(pendingCache_, {});
    for (auto iterator = pending.begin(); iterator != pending.end(); ++iterator) {
        if (*iterator) {
            (*iterator)({});
        }
    }
}

void PreviewClient::complete_request(const protocol::RequestId request_id,
                                     const protocol::ResponseStatus status) {
    const auto found = pending_.find(static_cast<qulonglong>(request_id));
    if (found == pending_.end()) {
        return;
    }
    auto pending = std::move(found.value());
    pending_.erase(found);
    if (activeRequestId_ == request_id) {
        activeRequestId_ = 0;
    }
    deliver_completion(request_id, std::move(pending), status);
}

void PreviewClient::deliver_completion(const protocol::RequestId request_id, Pending pending,
                                       const protocol::ResponseStatus status) {
    const auto diagnostic_request = std::to_string(request_id);
    const auto diagnostic_status = std::to_string(static_cast<unsigned>(status));
    const auto diagnostic_path = utf8(pending.path);
    DiagnosticSink::instance().record(
        "preview.result",
        {{"id", diagnostic_request}, {"path", diagnostic_path}, {"status", diagnostic_status}});
    PreviewReply reply{.request_id = request_id,
                       .entry_id = pending.entry_id,
                       .generation = pending.generation,
                       .source_size = pending.source_size,
                       .modified_unix_ns = pending.modified_unix_ns,
                       .source_revision = pending.source_revision,
                       .presentation_edge = pending.presentation_edge,
                       .status = status,
                       .image = {},
                       .source_color_model = {},
                       .source_color_profile = {},
                       .source_profile_fingerprint = {},
                       .provenance = protocol::PreviewProvenance::primary_render,
                       .page_index = pending.page_index,
                       .page_count = 0};
    auto handler = pending.handler ? std::move(pending.handler) : replyHandler_;
    deliver_final(std::move(reply), std::move(handler));
}

void PreviewClient::restart_helper(const bool preserve_extended_attempts) {
    if (restarting_) {
        return;
    }
    restarting_ = true;
    requestTimer_->stop();
    authenticated_ = false;
    const auto previous_active_request_id = activeRequestId_;
    activeRequestId_ = 0;
    progressDeadline_ = std::chrono::steady_clock::time_point::max();
    fail_cache_requests();
    receiveBuffer_.clear();
    queuedFrames_.clear();
    QHash<qulonglong, Pending> cancelled;
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (iterator->extended_limits && iterator->dispatched && !preserve_extended_attempts) {
            cancelled.insert(iterator.key(), std::move(iterator.value()));
            iterator = pending_.erase(iterator);
            continue;
        }
        const auto preserve_deadline =
            preserve_extended_attempts && iterator->extended_limits && iterator->active &&
            iterator.key() == static_cast<qulonglong>(previous_active_request_id);
        iterator->dispatched = false;
        iterator->active = false;
        iterator->retry_not_before = {};
        if (!preserve_deadline) {
            iterator->deadline = std::chrono::steady_clock::time_point::max();
        }
        ++iterator;
    }
    replace_socket();
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
    } else {
        finish_restart();
    }
    // A user-requested enlarged attempt must not acquire a fresh budget after reconfiguration.
    QPointer<PreviewClient> self(this);
    for (auto iterator = cancelled.begin(); iterator != cancelled.end(); ++iterator) {
        if (!self) {
            return;
        }
        self->deliver_completion(static_cast<protocol::RequestId>(iterator.key()),
                                 std::move(iterator.value()), protocol::ResponseStatus::cancelled);
    }
}

void PreviewClient::finish_restart() {
    if (!restarting_) {
        return;
    }
    restarting_ = false;
    cleanup_server_endpoint();
    if (pending_.isEmpty()) {
        schedule_idle_stop();
        return;
    }
    rebuild_request_queue();
    ensure_started();
}

void PreviewClient::cleanup_server_endpoint() {
    if (serverName_.isEmpty() || process_->state() != QProcess::NotRunning) {
        return;
    }
#ifdef Q_OS_LINUX
    serverName_.clear();
    return;
#else
    const auto removed = QLocalServer::removeServer(serverName_);
#ifndef Q_OS_WIN
    const auto owned_endpoint =
        serverDirectory_ ? QDir(serverDirectory_->path()).filePath(QStringLiteral("endpoint"))
                         : QString{};
    if (!removed && !owned_endpoint.isEmpty() && serverName_ == owned_endpoint) {
        static_cast<void>(QFile::remove(owned_endpoint));
    }
#else
    Q_UNUSED(removed);
#endif
    serverName_.clear();
#endif
}

void PreviewClient::rebuild_request_queue() {
    queuedFrames_.clear();
    for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
        iterator->dispatched = false;
        iterator->active = false;
        iterator->retry_not_before = {};
    }
    dispatch_deferred_requests();
}

void PreviewClient::retry_backpressured_requests() {
    if (stopping_ || restarting_) {
        return;
    }
    dispatch_deferred_requests();
    ensure_started();
}

void PreviewClient::arm_request_timeout() {
    if (pending_.isEmpty() || !has_dispatched_requests()) {
        requestTimer_->stop();
        return;
    }
    auto earliest = progressDeadline_;
    if (activeRequestId_ != 0) {
        const auto active = pending_.constFind(static_cast<qulonglong>(activeRequestId_));
        if (active != pending_.cend()) {
            earliest = active->deadline;
        }
    }
    if (earliest == std::chrono::steady_clock::time_point::max()) {
        requestTimer_->stop();
        return;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        earliest - std::chrono::steady_clock::now());
    requestTimer_->start(std::max(1, static_cast<int>(std::min<std::int64_t>(
                                         remaining.count(), std::numeric_limits<int>::max()))));
}

void PreviewClient::handle_request_timeout() {
    if (pending_.isEmpty()) {
        return;
    }
    protocol::RequestId timed_out{};
    auto status = protocol::ResponseStatus::timed_out;
    const auto active = pending_.constFind(static_cast<qulonglong>(activeRequestId_));
    if (activeRequestId_ != 0 && active != pending_.cend()) {
        timed_out = activeRequestId_;
        if (active->extended_limits) {
            status = protocol::ResponseStatus::processing_timed_out;
        }
    } else {
        for (auto iterator = pending_.cbegin(); iterator != pending_.cend(); ++iterator) {
            if (!iterator->dispatched) {
                continue;
            }
            const auto request_id = static_cast<protocol::RequestId>(iterator.key());
            if (timed_out == 0 || request_id < timed_out) {
                timed_out = request_id;
            }
        }
    }
    complete_request(timed_out, status);
    restart_helper();
}

void PreviewClient::schedule_idle_stop() {
    if (pending_.isEmpty() && pendingCache_.isEmpty() && queuedFrames_.isEmpty() &&
        process_->state() != QProcess::NotRunning) {
        idleTimer_->start(kIdleStopMs);
    }
}

void PreviewClient::stop() {
    stopping_ = true;
    restarting_ = false;
    connectTimer_->stop();
    idleTimer_->stop();
    requestTimer_->stop();
    authenticated_ = false;
    receiveBuffer_.clear();
    queuedFrames_.clear();
    pending_.clear();
    pendingCache_.clear();
    activeRequestId_ = 0;
    progressDeadline_ = std::chrono::steady_clock::time_point::max();
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->abort();
    }
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(500)) {
            process_->kill();
            static_cast<void>(process_->waitForFinished(500));
        }
    }
    if (process_->state() == QProcess::NotRunning) {
        cleanup_server_endpoint();
        serverDirectory_.reset();
    } else if (serverDirectory_) {
        serverDirectory_->setAutoRemove(false);
    }
    stopping_ = false;
}

} // namespace vove::ui
