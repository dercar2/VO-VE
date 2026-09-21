#include "service.hpp"

#include "vove/cache/artifact_store.hpp"
#include "vove/preview/helper_protocol.hpp"
#include "vove/preview/thumbnail_scheduler.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QString>
#include <QTimer>

#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vove::preview::helper {
namespace {

namespace protocol = helper_protocol;

inline constexpr std::size_t kMaximumQueuedRequests = 128;
inline constexpr qint64 kMaximumQueuedOutputBytes =
    static_cast<qint64>(protocol::kMaximumFrameBytes) * 2;
inline constexpr int kAuthenticationTimeoutMs = 2'000;

[[nodiscard]] std::span<const std::byte> bytes(const QByteArray &value) noexcept {
    return {reinterpret_cast<const std::byte *>(value.constData()),
            static_cast<std::size_t>(value.size())};
}

[[nodiscard]] QByteArray qt_bytes(const std::span<const std::byte> value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error("helper frame exceeds the Qt byte-array limit");
    }
    return {reinterpret_cast<const char *>(value.data()), static_cast<qsizetype>(value.size())};
}

[[nodiscard]] bool constant_time_equal(const std::string &left, const std::string &right,
                                       const std::size_t maximum) noexcept {
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < maximum; ++index) {
        const auto left_byte = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const auto right_byte =
            index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= static_cast<std::size_t>(left_byte ^ right_byte);
    }
    return difference == 0;
}

[[nodiscard]] std::string color_model_name(const cache::ColorModel model) {
    switch (model) {
    case cache::ColorModel::unknown:
        return {};
    case cache::ColorModel::gray:
        return "GRAY";
    case cache::ColorModel::rgb:
        return "RGB";
    case cache::ColorModel::cmyk:
        return "CMYK";
    case cache::ColorModel::lab:
        return "LAB";
    case cache::ColorModel::mixed:
        return "MIXED";
    case cache::ColorModel::indexed:
        return "INDEXED";
    }
    return {};
}

[[nodiscard]] protocol::PreviewProvenance
map_provenance(const cache::PreviewProvenance provenance) noexcept {
    return provenance == cache::PreviewProvenance::embedded_preview
               ? protocol::PreviewProvenance::embedded_preview
               : protocol::PreviewProvenance::primary_render;
}

[[nodiscard]] protocol::ResponseStatus map_status(const ThumbnailPipelineStatus status) noexcept {
    switch (status) {
    case ThumbnailPipelineStatus::success:
        return protocol::ResponseStatus::success_decoded;
    case ThumbnailPipelineStatus::unsupported:
        return protocol::ResponseStatus::unsupported;
    case ThumbnailPipelineStatus::malformed_source:
        return protocol::ResponseStatus::malformed;
    case ThumbnailPipelineStatus::timed_out:
        return protocol::ResponseStatus::timed_out;
    case ThumbnailPipelineStatus::processing_timed_out:
        return protocol::ResponseStatus::processing_timed_out;
    case ThumbnailPipelineStatus::cancelled:
        return protocol::ResponseStatus::cancelled;
    case ThumbnailPipelineStatus::resource_limit:
        return protocol::ResponseStatus::too_large;
    case ThumbnailPipelineStatus::pdf_input_too_large:
        return protocol::ResponseStatus::pdf_input_too_large;
    case ThumbnailPipelineStatus::memory_limit:
        return protocol::ResponseStatus::memory_limit;
    case ThumbnailPipelineStatus::source_unavailable:
        return protocol::ResponseStatus::source_unavailable;
    case ThumbnailPipelineStatus::source_changed:
        return protocol::ResponseStatus::source_changed;
    case ThumbnailPipelineStatus::source_not_found:
        return protocol::ResponseStatus::not_found;
    case ThumbnailPipelineStatus::permission_denied:
        return protocol::ResponseStatus::permission_denied;
    case ThumbnailPipelineStatus::authentication_failed:
        return protocol::ResponseStatus::authentication_failed;
    case ThumbnailPipelineStatus::disconnected:
        return protocol::ResponseStatus::disconnected;
    case ThumbnailPipelineStatus::color_profile_required:
        return protocol::ResponseStatus::color_profile_required;
    case ThumbnailPipelineStatus::password_required:
        return protocol::ResponseStatus::password_required;
    case ThumbnailPipelineStatus::document_password_incorrect:
        return protocol::ResponseStatus::document_password_incorrect;
    case ThumbnailPipelineStatus::ghostscript_required:
        return protocol::ResponseStatus::ghostscript_required;
    case ThumbnailPipelineStatus::embedded_preview_unavailable:
        return protocol::ResponseStatus::embedded_preview_unavailable;
    case ThumbnailPipelineStatus::internal_error:
        return protocol::ResponseStatus::internal_error;
    case ThumbnailPipelineStatus::worker_start_failed:
        return protocol::ResponseStatus::worker_start_failed;
    }
    return protocol::ResponseStatus::internal_error;
}

[[nodiscard]] protocol::PreviewResponse failure_response(const protocol::RequestId request_id,
                                                         const protocol::ResponseStatus status,
                                                         const std::uint32_t page_index = 0) {
    return {.request_id = request_id,
            .status = status,
            .width = 0,
            .height = 0,
            .source_color_model_utf8 = {},
            .source_color_profile_utf8 = {},
            .source_profile_fingerprint = {},
            .page_index = page_index,
            .page_count = 0,
            .rgba8 = {}};
}

[[nodiscard]] std::int64_t access_time_unix_ns() noexcept {
    constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
    return QDateTime::currentMSecsSinceEpoch() * nanoseconds_per_millisecond;
}

[[nodiscard]] cache::ArtifactIndex &
checked_index(const std::unique_ptr<cache::ArtifactIndex> &index) {
    if (!index) {
        throw std::invalid_argument("helper service requires an artifact index");
    }
    return *index;
}

} // namespace

class Service::Impl final {
  public:
    Impl(ServiceOptions options, std::unique_ptr<cache::ArtifactIndex> index,
         ThumbnailRenderer renderer, ThumbnailSourceResolver source_resolver,
         ThumbnailColorPolicyResolver color_policy_resolver)
        : options_(std::move(options)), index_(std::move(index)),
          cache_(cache::ArtifactStore(options_.cache_root / "artifacts"), checked_index(index_)),
          pipeline_(cache_, std::move(renderer), std::move(source_resolver),
                    std::move(color_policy_resolver)) {
        validate_options();
        const auto reconcile_artifacts = index_->recovered_corruption();
        record_cache_maintenance(cache_.prune(options_.disk_cache_limits, reconcile_artifacts));
        if (options_.abstract_namespace) {
            server_.setSocketOptions(QLocalServer::AbstractNamespaceOption);
        }
        server_.setMaxPendingConnections(1);
        QObject::connect(&server_, &QLocalServer::newConnection, &callback_context_,
                         [this] { accept_connections(); });
    }

    [[nodiscard]] bool listen() {
        if (server_.isListening()) {
            return true;
        }
        last_error_.clear();
        if (!server_.listen(QString::fromUtf8(options_.server_name_utf8))) {
            last_error_ = server_.errorString().toUtf8().toStdString();
            return false;
        }
        return true;
    }

    void close() {
        close_client(false);
        server_.close();
    }

    [[nodiscard]] bool is_listening() const noexcept {
        return server_.isListening();
    }

    [[nodiscard]] const std::string &last_error() const noexcept {
        return last_error_;
    }

  private:
    void validate_options() const {
        if (options_.server_name_utf8.empty() || options_.cache_root.empty()) {
            throw std::invalid_argument("helper service options are incomplete");
        }
#ifdef Q_OS_LINUX
        if (options_.abstract_namespace && options_.expected_peer_process_id == 0) {
            throw std::invalid_argument("abstract helper requires an owning peer process");
        }
#endif
        static_cast<void>(protocol::encode_hello({.build_id_utf8 = options_.build_id_utf8,
                                                  .auth_token_utf8 = options_.auth_token_utf8}));
    }

    [[nodiscard]] bool trusted_peer(QLocalSocket &candidate) const noexcept {
#ifdef Q_OS_LINUX
        if (options_.expected_peer_process_id == 0) {
            return true;
        }
        struct PeerCredentials {
            pid_t pid;
            uid_t uid;
            gid_t gid;
        } credentials{};
        socklen_t length = static_cast<socklen_t>(sizeof(credentials));
        const auto descriptor = candidate.socketDescriptor();
        return descriptor >= 0 &&
               descriptor <= static_cast<qintptr>(std::numeric_limits<int>::max()) &&
               ::getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_PEERCRED, &credentials,
                            &length) == 0 &&
               length == static_cast<socklen_t>(sizeof(credentials)) &&
               credentials.uid == ::geteuid() && credentials.pid > 0 &&
               static_cast<std::uint64_t>(credentials.pid) == options_.expected_peer_process_id;
#else
        static_cast<void>(candidate);
        return options_.expected_peer_process_id == 0;
#endif
    }

    void accept_connections() {
        while (auto *candidate = server_.nextPendingConnection()) {
            if (socket_ || !trusted_peer(*candidate)) {
                candidate->abort();
                candidate->deleteLater();
                continue;
            }

            socket_ = candidate;
            ++session_epoch_;
            authenticated_ = false;
            receive_buffer_.clear();
            scheduler_ = ThumbnailScheduler{kMaximumQueuedRequests};
            pending_requests_.clear();
            processing_scheduled_ = false;

            QObject::connect(candidate, &QLocalSocket::readyRead, &callback_context_,
                             [this, candidate] {
                                 if (socket_ == candidate) {
                                     receive();
                                 }
                             });
            QObject::connect(candidate, &QLocalSocket::bytesWritten, &callback_context_,
                             [this, candidate](qint64) {
                                 if (socket_ == candidate) {
                                     schedule_processing();
                                 }
                             });
            QObject::connect(candidate, &QLocalSocket::disconnected, &callback_context_,
                             [this, candidate] {
                                 if (socket_ == candidate) {
                                     const auto owner_session = authenticated_;
                                     reset_client();
                                     if (owner_session) {
                                         notify_client_disconnected();
                                     }
                                 }
                                 candidate->deleteLater();
                             });
            const auto epoch = session_epoch_;
            QTimer::singleShot(kAuthenticationTimeoutMs, &callback_context_, [this, epoch] {
                if (session_epoch_ == epoch && socket_ && !authenticated_) {
                    close_client();
                }
            });
            receive();
        }
    }

    void receive() {
        while (socket_ && socket_->bytesAvailable() > 0) {
            const auto room = static_cast<qint64>(protocol::kMaximumFrameBytes) -
                              static_cast<qint64>(receive_buffer_.size());
            if (room <= 0) {
                close_client();
                return;
            }
            const auto chunk = socket_->read(std::min(socket_->bytesAvailable(), room));
            if (chunk.isEmpty()) {
                close_client();
                return;
            }
            receive_buffer_.append(chunk);
            if (!consume_frames()) {
                return;
            }
        }
    }

    [[nodiscard]] bool consume_frames() {
        while (receive_buffer_.size() >= static_cast<qsizetype>(protocol::kFrameHeaderBytes)) {
            protocol::FrameHeader header;
            protocol::DecodeError error;
            if (!protocol::decode_header(bytes(receive_buffer_), header, error)) {
                close_client();
                return false;
            }
            const auto frame_size = static_cast<qsizetype>(header.total_size);
            if (receive_buffer_.size() < frame_size) {
                return true;
            }

            const auto frame = receive_buffer_.first(frame_size);
            receive_buffer_.remove(0, frame_size);
            if (!dispatch_frame(header, frame)) {
                close_client();
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool dispatch_frame(const protocol::FrameHeader &header,
                                      const QByteArray &frame) {
        protocol::DecodeError error;
        if (!authenticated_) {
            if (header.type != protocol::MessageType::hello) {
                return false;
            }
            protocol::Hello hello;
            if (!protocol::decode_hello(bytes(frame), hello, error) ||
                !constant_time_equal(hello.build_id_utf8, options_.build_id_utf8,
                                     protocol::kMaximumBuildIdBytes) ||
                !constant_time_equal(hello.auth_token_utf8, options_.auth_token_utf8,
                                     protocol::kMaximumAuthTokenBytes)) {
                return false;
            }
            authenticated_ = true;
            return write_frame(
                protocol::encode_hello_ack({.build_id_utf8 = options_.build_id_utf8,
                                            .auth_token_utf8 = options_.auth_token_utf8}));
        }

        switch (header.type) {
        case protocol::MessageType::preview_request: {
            protocol::PreviewRequest request;
            if (!protocol::decode_preview_request(bytes(frame), request, error)) {
                return false;
            }
            const auto request_id = request.request_id;
            const auto submitted = scheduler_.submit({.job_id = request_id,
                                                      .generation = request.generation,
                                                      .source_id = request_id,
                                                      .canonical_edge = request.canonical_edge,
                                                      .priority = request.priority});
            if (!submitted.accepted()) {
                return write_response(
                    failure_response(request_id,
                                     submitted.status == SubmitStatus::cancelled_generation
                                         ? protocol::ResponseStatus::cancelled
                                         : protocol::ResponseStatus::queue_busy,
                                     request.page_index));
            }
            if (submitted.evicted_job) {
                const auto evicted = pending_requests_.find(*submitted.evicted_job);
                if (evicted != pending_requests_.end()) {
                    static_cast<void>(write_response(
                        failure_response(evicted->first, protocol::ResponseStatus::cancelled,
                                         evicted->second.page_index)));
                    pending_requests_.erase(evicted);
                }
            }
            pending_requests_.insert_or_assign(request_id, std::move(request));
            schedule_processing();
            return true;
        }
        case protocol::MessageType::cancel_generation: {
            protocol::CancelGeneration cancel;
            if (!protocol::decode_cancel_generation(bytes(frame), cancel, error)) {
                return false;
            }
            static_cast<void>(scheduler_.cancel_generation(cancel.generation));
            std::vector<protocol::RequestId> cancelled_requests;
            for (const auto &[request_id, request] : pending_requests_) {
                if (request.generation == cancel.generation) {
                    cancelled_requests.push_back(request_id);
                }
            }
            for (const auto request_id : cancelled_requests) {
                const auto page_index = pending_requests_.at(request_id).page_index;
                pending_requests_.erase(request_id);
                static_cast<void>(write_response(
                    failure_response(request_id, protocol::ResponseStatus::cancelled, page_index)));
            }
            return true;
        }
        case protocol::MessageType::cache_request: {
            protocol::CacheRequest request;
            if (!protocol::decode_cache_request(bytes(frame), request, error)) {
                return false;
            }
            auto limits = options_.disk_cache_limits;
            if (request.operation == protocol::CacheOperation::clear) {
                limits.maximum_entries = 0;
                limits.maximum_bytes = 0;
            }
            record_cache_maintenance(cache_.prune(limits, true));
            auto status = cache_.disk_stats();
            if (!cache_maintenance_error_.empty()) {
                status.error = cache_maintenance_error_;
            }
            return write_frame(protocol::encode_cache_response(
                {.request_id = request.request_id,
                 .status =
                     status.ok() ? protocol::CacheStatus::success : protocol::CacheStatus::failed,
                 .entries = status.entries,
                 .bytes = status.bytes}));
        }
        case protocol::MessageType::hello:
        case protocol::MessageType::hello_ack:
        case protocol::MessageType::preview_response:
        case protocol::MessageType::job_started:
        case protocol::MessageType::cache_response:
            return false;
        }
        return false;
    }

    void schedule_processing() {
        if (!authenticated_ || !socket_ || scheduler_.empty() || processing_scheduled_ ||
            socket_->bytesToWrite() > kMaximumQueuedOutputBytes / 2) {
            return;
        }
        processing_scheduled_ = true;
        const auto epoch = session_epoch_;
        QTimer::singleShot(0, &callback_context_, [this, epoch] {
            if (session_epoch_ == epoch) {
                process_next();
            }
        });
    }

    void process_next() {
        processing_scheduled_ = false;
        if (!authenticated_ || !socket_ || scheduler_.empty()) {
            return;
        }

        const auto scheduled = scheduler_.take_next();
        if (!scheduled) {
            return;
        }
        const auto found = pending_requests_.find(scheduled->job_id);
        if (found == pending_requests_.end()) {
            schedule_processing();
            return;
        }
        auto request = std::move(found->second);
        pending_requests_.erase(found);

        if (!write_frame(protocol::encode_job_started({.request_id = request.request_id}))) {
            return;
        }
        static_cast<void>(socket_->flush());

        protocol::PreviewResponse response;
        response.request_id = request.request_id;
        response.page_index = request.page_index;
        try {
            const auto result = pipeline_.fetch(
                {.source = {.source_identity_utf8 = request.path_utf8,
                            .size_bytes = request.source_size,
                            .modified_unix_ns = request.modified_unix_ns,
                            .stable_file_id =
                                request.source_revision_utf8.empty()
                                    ? std::nullopt
                                    : std::optional<std::string>{request.source_revision_utf8}},
                 .access_unix_ns = access_time_unix_ns(),
                 .canonical_edge = request.canonical_edge,
                 .page_index = request.page_index,
                 .password_utf8 = request.password_utf8,
                 .offline_cache_only = request.offline_cache_only,
                 .allow_offline_fallback = request.allow_offline_fallback,
                 .selected_view = request.selected_view,
                 .extended_limits = request.extended_limits});
            response.status =
                result.ok() && result.freshness == PreviewFreshness::offline_unverified
                    ? protocol::ResponseStatus::success_offline_cached
                : result.from_cache && result.ok() ? protocol::ResponseStatus::success_cached
                                                   : map_status(result.status);
            if (result.status == ThumbnailPipelineStatus::worker_start_failed) {
                response.startup_diagnostic = result.startup_diagnostic;
            }
            if (result.ok()) {
                response.width = static_cast<std::uint16_t>(result.width);
                response.height = static_cast<std::uint16_t>(result.height);
                response.source_color_model_utf8 = color_model_name(result.source_color_model);
                response.source_color_profile_utf8 = result.source_profile_name;
                response.source_profile_fingerprint = result.source_profile_fingerprint;
                response.provenance = map_provenance(result.provenance);
                response.page_index = result.page_index;
                response.page_count = result.page_count;
                response.rgba8 = result.rgba8;
            }
            if (result.ok() && !result.from_cache && ++published_since_prune_ >= 32) {
                record_cache_maintenance(cache_.prune(options_.disk_cache_limits, false));
                published_since_prune_ = 0;
            }
        } catch (...) {
            response = failure_response(
                request.request_id, protocol::ResponseStatus::internal_error, request.page_index);
        }
        static_cast<void>(write_response(response));
        schedule_processing();
    }

    [[nodiscard]] bool write_response(const protocol::PreviewResponse &response) {
        try {
            return write_frame(protocol::encode_preview_response(response));
        } catch (...) {
            if (response.status == protocol::ResponseStatus::internal_error) {
                close_client();
                return false;
            }
            try {
                return write_frame(protocol::encode_preview_response(
                    failure_response(response.request_id, protocol::ResponseStatus::internal_error,
                                     response.page_index)));
            } catch (...) {
                close_client();
                return false;
            }
        }
    }

    void record_cache_maintenance(const cache::PruneResult &result) {
        if (result.ok()) {
            cache_maintenance_error_.clear();
            return;
        }
        cache_maintenance_error_ =
            result.error.empty() ? "preview cache maintenance failed" : result.error;
    }

    [[nodiscard]] bool write_frame(const std::vector<std::byte> &frame) {
        if (!socket_ || socket_->bytesToWrite() + static_cast<qint64>(frame.size()) >
                            kMaximumQueuedOutputBytes) {
            close_client();
            return false;
        }
        const auto payload = qt_bytes(frame);
        if (socket_->write(payload) != payload.size()) {
            close_client();
            return false;
        }
        return true;
    }

    void close_client(const bool notify_owner = true) {
        const auto client = socket_;
        const auto owner_session = !client.isNull() && authenticated_;
        reset_client();
        if (client) {
            client->abort();
        }
        if (owner_session && notify_owner) {
            notify_client_disconnected();
        }
    }

    void reset_client() {
        ++session_epoch_;
        socket_.clear();
        authenticated_ = false;
        receive_buffer_.clear();
        scheduler_ = ThumbnailScheduler{kMaximumQueuedRequests};
        pending_requests_.clear();
        processing_scheduled_ = false;
    }

    void notify_client_disconnected() {
        if (options_.client_disconnected) {
            QTimer::singleShot(0, &callback_context_, options_.client_disconnected);
        }
    }

    ServiceOptions options_;
    std::unique_ptr<cache::ArtifactIndex> index_;
    cache::PersistentThumbnailCache cache_;
    ThumbnailPipeline pipeline_;
    QLocalServer server_;
    QPointer<QLocalSocket> socket_;
    QByteArray receive_buffer_;
    ThumbnailScheduler scheduler_{kMaximumQueuedRequests};
    std::unordered_map<protocol::RequestId, protocol::PreviewRequest> pending_requests_;
    std::string last_error_;
    std::string cache_maintenance_error_;
    bool authenticated_{};
    bool processing_scheduled_{};
    std::uint8_t published_since_prune_{};
    std::uint64_t session_epoch_{};
    QObject callback_context_;
};

Service::Service(ServiceOptions options, std::unique_ptr<cache::ArtifactIndex> index,
                 ThumbnailRenderer renderer, ThumbnailSourceResolver source_resolver,
                 ThumbnailColorPolicyResolver color_policy_resolver, QObject *parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(std::move(options), std::move(index), std::move(renderer),
                                   std::move(source_resolver), std::move(color_policy_resolver))) {}

Service::~Service() = default;

bool Service::listen() {
    return impl_->listen();
}

void Service::close() {
    impl_->close();
}

bool Service::is_listening() const noexcept {
    return impl_->is_listening();
}

std::string Service::last_error() const {
    return impl_->last_error();
}

} // namespace vove::preview::helper
