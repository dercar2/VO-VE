#pragma once

#include "vove/preview/helper_protocol.hpp"
#include "vove/color/color_transform.h"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

#include <cstdint>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

class QLocalSocket;
class QProcess;
class QTemporaryDir;
class QTimer;

namespace vove::ui {

struct PreviewReply {
    preview::helper_protocol::RequestId request_id{};
    qulonglong entry_id{};
    std::uint64_t generation{};
    std::uint64_t source_size{};
    std::int64_t modified_unix_ns{};
    QString source_revision;
    std::uint16_t presentation_edge{};
    preview::helper_protocol::ResponseStatus status{
        preview::helper_protocol::ResponseStatus::internal_error};
    QImage image;
    QString source_color_model;
    QString source_color_profile;
    QString source_profile_fingerprint;
    preview::helper_protocol::PreviewProvenance provenance{
        preview::helper_protocol::PreviewProvenance::primary_render};
    std::uint32_t page_index{};
    std::uint32_t page_count{};
    preview::WorkerStartupDiagnostic startup_diagnostic{};
};

struct CacheReply {
    bool success{};
    std::uint64_t entries{};
    std::uint64_t bytes{};
};

class PreviewClient final : public QObject {
  public:
    using ReplyHandler = std::function<void(PreviewReply)>;
    using CacheHandler = std::function<void(CacheReply)>;

    explicit PreviewClient(QObject *parent = nullptr, int request_timeout_ms = 12'000,
                           QString helper_program_override = {},
                           int selected_request_timeout_ms = 35'000,
                           int extended_request_timeout_ms = 135'000);
    ~PreviewClient() override;

    PreviewClient(const PreviewClient &) = delete;
    PreviewClient &operator=(const PreviewClient &) = delete;

    void set_reply_handler(ReplyHandler handler);
    [[nodiscard]] bool set_monitor_name(const QString &screen_name);
    [[nodiscard]] bool set_monitor_profile(QByteArray profile);
    void set_cache_maximum_bytes(std::uint64_t bytes);
    void refresh_external_components(std::function<void(bool)> completion = {});
    [[nodiscard]] preview::helper_protocol::RequestId inspect_cache(CacheHandler handler);
    [[nodiscard]] preview::helper_protocol::RequestId clear_cache(CacheHandler handler);
    // Only one extended request may be queued or active; another returns zero.
    [[nodiscard]] preview::helper_protocol::RequestId
    request_preview(qulonglong entry_id, std::uint64_t generation, const QString &path,
                    std::uint64_t source_size, std::int64_t modified_unix_ns,
                    const QString &source_revision, std::uint16_t presentation_edge,
                    preview::ThumbnailPriority priority, ReplyHandler handler = {},
                    std::uint32_t page_index = 0, const QString &password = {},
                    bool offline_cache_only = false, bool allow_offline_fallback = true,
                    bool selected_view = false, bool extended_limits = false);
    void cancel_generation(std::uint64_t generation);
    [[nodiscard]] bool readers_idle() const noexcept;
    // Synchronously cancels only undispatched entries outside entry_ids; active work is retained.
    void retain_generation_entries(std::uint64_t generation, const QSet<qulonglong> &entry_ids);
    void stop();

  private:
    struct Pending {
        qulonglong entry_id{};
        std::uint64_t generation{};
        std::uint64_t source_size{};
        std::int64_t modified_unix_ns{};
        QString path;
        QString source_revision;
        std::uint16_t presentation_edge{};
        std::uint32_t page_index{};
        preview::ThumbnailPriority priority{preview::ThumbnailPriority::rest};
        bool dispatched{};
        bool active{};
        bool selected_view{};
        bool extended_limits{};
        std::uint8_t backpressure_attempts{};
        std::chrono::steady_clock::time_point retry_not_before{};
        std::chrono::steady_clock::time_point deadline{
            std::chrono::steady_clock::time_point::max()};
        QByteArray request_frame;
        ReplyHandler handler;
    };

    struct QueuedFrame {
        preview::helper_protocol::RequestId request_id{};
        QByteArray bytes;
    };

    void ensure_started();
    void replace_socket();
    void finish_external_component_refresh(QString ghostscript);
    [[nodiscard]] preview::helper_protocol::RequestId
    request_cache(preview::helper_protocol::CacheOperation operation, CacheHandler handler);
    void fail_cache_requests();
    void connect_to_helper();
    void authenticate();
    [[nodiscard]] bool queue_frame(const std::vector<std::byte> &frame,
                                   preview::helper_protocol::RequestId request_id = 0);
    void flush_frames();
    void dispatch_deferred_requests();
    [[nodiscard]] qsizetype dispatched_request_count() const noexcept;
    [[nodiscard]] bool has_dispatched_requests() const noexcept;
    void receive();
    [[nodiscard]] bool consume_frames();
    void dispatch_frame(const QByteArray &frame);
    void deliver_reply(PreviewReply reply, ReplyHandler handler);
    void deliver_final(PreviewReply reply, ReplyHandler &&handler);
    void complete_request(preview::helper_protocol::RequestId request_id,
                          preview::helper_protocol::ResponseStatus status);
    void deliver_completion(preview::helper_protocol::RequestId request_id, Pending pending,
                            preview::helper_protocol::ResponseStatus status);
    void fail_pending(preview::helper_protocol::ResponseStatus status);
    void restart_helper(bool preserve_extended_attempts = false);
    void finish_restart();
    void cleanup_server_endpoint();
    void rebuild_request_queue();
    void retry_backpressured_requests();
    void schedule_idle_stop();
    void arm_request_timeout();
    void handle_request_timeout();

    QProcess *process_{};
    QLocalSocket *socket_{};
    std::unique_ptr<QTemporaryDir> serverDirectory_;
    QTimer *connectTimer_{};
    QTimer *idleTimer_{};
    QTimer *requestTimer_{};
    QString serverName_;
    QString authToken_;
    QString buildId_{QStringLiteral("vove-stage5-preview-1")};
    QString helperProgramOverride_;
    QString ghostscriptExecutable_;
    std::vector<std::function<void(bool)>> externalComponentRefreshCallbacks_;
    QByteArray receiveBuffer_;
    QList<QueuedFrame> queuedFrames_;
    QHash<qulonglong, Pending> pending_;
    QHash<qulonglong, CacheHandler> pendingCache_;
    ReplyHandler replyHandler_;
    color::ScreenProfileCache displayCache_;
    QByteArray monitorIcc_;
    QString monitorFingerprint_;
    preview::helper_protocol::RequestId nextRequestId_{1};
    std::uint64_t cacheMaximumBytes_{10ULL * 1024ULL * 1024ULL * 1024ULL};
    int connectAttempts_{};
    bool authenticated_{};
    bool stopping_{};
    bool externalComponentRefreshRunning_{};
    bool restarting_{};
    preview::helper_protocol::RequestId activeRequestId_{};
    std::chrono::steady_clock::time_point progressDeadline_{
        std::chrono::steady_clock::time_point::max()};
    int requestTimeoutMs_{12'000};
    int selectedRequestTimeoutMs_{35'000};
    int extendedRequestTimeoutMs_{135'000};
};

} // namespace vove::ui
