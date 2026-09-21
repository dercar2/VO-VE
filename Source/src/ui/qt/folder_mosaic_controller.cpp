#include "folder_mosaic_controller.hpp"

#include "vove/core/catalog_visibility.hpp"

#include <QFileInfo>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <utility>
#include <vector>

namespace vove::ui {
namespace {

std::filesystem::path native_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}

constexpr std::uint64_t kMosaicGenerationBit = std::uint64_t{1} << 63U;

int mosaic_decode_rank(const QString &path) {
    const auto extension = QFileInfo(path).suffix().toLower();
    if (extension == QStringLiteral("jpg") || extension == QStringLiteral("jpeg") ||
        extension == QStringLiteral("png") || extension == QStringLiteral("webp") ||
        extension == QStringLiteral("bmp") || extension == QStringLiteral("gif") ||
        extension == QStringLiteral("tif") || extension == QStringLiteral("tiff") ||
        extension == QStringLiteral("heic") || extension == QStringLiteral("heif") ||
        extension == QStringLiteral("avif") || extension == QStringLiteral("jxl")) {
        return 0;
    }
    if (extension == QStringLiteral("pdf") || extension == QStringLiteral("ai") ||
        extension == QStringLiteral("eps") || extension == QStringLiteral("ps") ||
        extension == QStringLiteral("cdr") || extension == QStringLiteral("psd") ||
        extension == QStringLiteral("psb") || extension == QStringLiteral("kra") ||
        extension == QStringLiteral("ora") || extension == QStringLiteral("xcf") ||
        extension == QStringLiteral("idml") || extension == QStringLiteral("indd") ||
        extension == QStringLiteral("indt") || extension == QStringLiteral("afdesign") ||
        extension == QStringLiteral("afphoto") || extension == QStringLiteral("afpub") ||
        extension == QStringLiteral("af") || extension == QStringLiteral("aftemplate") ||
        extension == QStringLiteral("svg") || extension == QStringLiteral("svgz") ||
        extension == QStringLiteral("plt") || extension == QStringLiteral("hpgl")) {
        return 1;
    }
    return 2;
}

template <typename State>
bool same_source(const State &state, const std::uint64_t source_size,
                 const std::int64_t modified_unix_ns, const QString &source_revision) {
    return state.source_size == source_size && state.modified_unix_ns == modified_unix_ns &&
           state.source_revision == source_revision;
}

} // namespace

FolderMosaicController::ActiveFolder::ActiveFolder(
    FolderRequest request_value, const ActiveGenerations generations,
    const preview::FolderMosaicBuilder::TimePoint deadline)
    : request(std::move(request_value)), source_generation(generations.source),
      preview_generation(generations.preview),
      builder(preview::FolderMosaicBuilder::TimePoint::max()), enumeration_deadline(deadline) {}

FolderMosaicController::FolderMosaicController(PreviewClient &preview_client, QObject *parent,
                                               FolderMosaicControllerOptions options,
                                               std::unique_ptr<catalog::DirectorySource> source)
    : QObject(parent), previewClient_(preview_client),
      source_(source ? std::move(source) : std::make_unique<platform::DirectoryService>()),
      options_(options) {
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { poll(); });
}

FolderMosaicController::~FolderMosaicController() {
    abandon_active();
}

void FolderMosaicController::set_reply_handler(ReplyHandler handler) {
    replyHandler_ = std::move(handler);
}

void FolderMosaicController::reset(const std::uint64_t app_generation) {
    abandon_active();
    queue_.clear();
    queuedFolderIds_.clear();
    completedEdges_.clear();
    pendingRetries_.clear();
    appGeneration_ = app_generation == 0 ? 1 : app_generation;
    timer_->stop();
}

void FolderMosaicController::clear_pending() {
    abandon_active();
    discard_queued();
    for (auto retry = pendingRetries_.begin(); retry != pendingRetries_.end();) {
        if (retry->exhausted) {
            ++retry;
        } else {
            retry = pendingRetries_.erase(retry);
        }
    }
    if (!active_) {
        timer_->stop();
    }
}

void FolderMosaicController::discard_queued() {
    queue_.clear();
    queuedFolderIds_.clear();
    if (!active_) {
        timer_->stop();
    }
}

void FolderMosaicController::set_offline(const bool offline) {
    if (offline_ == offline) {
        return;
    }
    offline_ = offline;
    if (offline_) {
        clear_pending();
    }
}

void FolderMosaicController::set_allow_offline_fallback(const bool allow) {
    if (allowOfflineFallback_ != allow) {
        clear_pending();
    }
    allowOfflineFallback_ = allow;
}

void FolderMosaicController::invalidate_display_images() {
    clear_pending();
    completedEdges_.clear();
    pendingRetries_.clear();
}

void FolderMosaicController::enqueue(const qulonglong folder_id, const QString &path,
                                     const std::uint64_t source_size,
                                     const std::int64_t modified_unix_ns, QString source_revision,
                                     const std::uint16_t presentation_edge) {
    if (offline_ || folder_id == 0 || path.isEmpty() || presentation_edge == 0) {
        return;
    }
    const auto completed = completedEdges_.find(folder_id);
    if (completed != completedEdges_.end() &&
        !same_source(*completed, source_size, modified_unix_ns, source_revision)) {
        completedEdges_.erase(completed);
    }
    const auto retry = pendingRetries_.find(folder_id);
    if (retry != pendingRetries_.end() &&
        !same_source(*retry, source_size, modified_unix_ns, source_revision)) {
        pendingRetries_.erase(retry);
    }

    for (qsizetype index = queue_.size(); index > 0; --index) {
        const auto queued_index = index - 1;
        const auto &queued = queue_.at(queued_index);
        if (queued.folder_id == folder_id &&
            !same_source(queued, source_size, modified_unix_ns, source_revision)) {
            queue_.removeAt(queued_index);
            queuedFolderIds_.remove(folder_id);
        }
    }
    if (active_ && active_->request.folder_id == folder_id &&
        !same_source(active_->request, source_size, modified_unix_ns, source_revision)) {
        abandon_active();
    }

    const auto current = completedEdges_.constFind(folder_id);
    if (queue_.size() >= static_cast<qsizetype>(options_.maximum_queued_folders) ||
        queuedFolderIds_.contains(folder_id) || pendingRetries_.contains(folder_id) ||
        (active_ && active_->request.folder_id == folder_id) ||
        (current != completedEdges_.cend() && current->edge >= presentation_edge)) {
        return;
    }
    queue_.enqueue({.folder_id = folder_id,
                    .path = path,
                    .source_size = source_size,
                    .modified_unix_ns = modified_unix_ns,
                    .source_revision = std::move(source_revision),
                    .presentation_edge = presentation_edge});
    queuedFolderIds_.insert(folder_id);
    start_next();
}

bool FolderMosaicController::idle() const noexcept {
    return !active_ && queue_.isEmpty();
}

bool FolderMosaicController::readers_idle() const noexcept {
    const auto *service = dynamic_cast<const platform::DirectoryService *>(source_.get());
    return idle() && (service == nullptr || service->idle());
}

void FolderMosaicController::start_next() {
    if (offline_ || active_ || queue_.isEmpty()) {
        if (!active_ && queue_.isEmpty()) {
            timer_->stop();
        }
        return;
    }
    auto request = queue_.dequeue();
    queuedFolderIds_.remove(request.folder_id);
    ++serial_;
    if (serial_ == 0 || serial_ >= kMosaicGenerationBit) {
        serial_ = 1;
    }
    const auto source_generation = static_cast<catalog::RequestGeneration>(serial_);
    const auto preview_generation = kMosaicGenerationBit | serial_;
    const auto deadline = preview::FolderMosaicClock::now() + options_.budget;
    active_.emplace(std::move(request),
                    ActiveGenerations{.source = source_generation, .preview = preview_generation},
                    deadline);
    source_->submit({.generation = source_generation,
                     .path = native_path(active_->request.path),
                     .maximum_entries = preview::kMaximumFolderMosaicEntries});
    timer_->start();
    poll();
}

void FolderMosaicController::poll() {
    if (!active_) {
        start_next();
        return;
    }
    consume_catalog();
    if (active_ && !active_->enumeration_finished &&
        preview::FolderMosaicClock::now() >= active_->enumeration_deadline) {
        source_->cancel(active_->source_generation);
        active_->enumeration_finished = true;
        active_->enumeration_timed_out = true;
        const auto now = preview::FolderMosaicClock::now();
        for (auto &child : active_->deferred_children) {
            if (active_->builder.push_child(
                    {.id = child.id, .kind = preview::FolderMosaicChildKind::regular_file}, now) ==
                preview::FolderMosaicChildDisposition::accepted) {
                active_->children.insert(child.id, std::move(child.source));
            }
        }
        active_->deferred_children.clear();
        active_->builder.finish_children(now);
    }
    // PreviewClient starts each document timeout only when decoding actually begins.
    launch_probes();
    finish_if_ready();
}

void FolderMosaicController::consume_catalog() {
    if (!active_ || active_->enumeration_finished) {
        return;
    }
    for (int batch_index = 0; batch_index < 8; ++batch_index) {
        auto batch = source_->poll();
        if (!batch) {
            break;
        }
        if (batch->generation != active_->source_generation) {
            continue;
        }
        const auto now = preview::FolderMosaicClock::now();
        std::vector<EnumeratedChild> batch_children;
        batch_children.reserve(batch->entries.size());
        for (const auto &entry : batch->entries) {
            if (entry.kind == core::EntryKind::file && core::visible_in_preview_catalog(entry)) {
                batch_children.push_back(
                    {.id = static_cast<qulonglong>(entry.id),
                     .source = {.path = QString::fromUtf8(entry.path_utf8),
                                .size = entry.size_bytes,
                                .modified_unix_ns = entry.modified_unix_ns,
                                .source_revision = QString::fromUtf8(entry.source_revision_utf8)}});
            }
        }
        const auto practical_order = [](const EnumeratedChild &left, const EnumeratedChild &right) {
            const auto left_rank = mosaic_decode_rank(left.source.path);
            const auto right_rank = mosaic_decode_rank(right.source.path);
            if (left_rank != right_rank) {
                return left_rank < right_rank;
            }
            if (left.source.size != right.source.size) {
                return left.source.size < right.source.size;
            }
            return QString::compare(left.source.path, right.source.path, Qt::CaseInsensitive) < 0;
        };
        std::ranges::sort(batch_children, practical_order);
        const auto push_child = [this, now](EnumeratedChild child) {
            const auto disposition = active_->builder.push_child(
                {.id = child.id, .kind = preview::FolderMosaicChildKind::regular_file}, now);
            if (disposition == preview::FolderMosaicChildDisposition::accepted) {
                active_->children.insert(child.id, std::move(child.source));
            }
        };
        const auto finishes_enumeration = batch->is_final || batch->error || batch->truncated;
        if (finishes_enumeration) {
            active_->deferred_children.reserve(active_->deferred_children.size() +
                                               batch_children.size());
            std::ranges::move(batch_children, std::back_inserter(active_->deferred_children));
            std::ranges::sort(active_->deferred_children, practical_order);
            for (auto &child : active_->deferred_children) {
                push_child(std::move(child));
            }
            active_->deferred_children.clear();
        } else {
            for (auto &child : batch_children) {
                if (mosaic_decode_rank(child.source.path) == 0) {
                    push_child(std::move(child));
                } else {
                    active_->deferred_children.push_back(std::move(child));
                }
            }
        }
        if (batch->error) {
            using Kind = catalog::CatalogErrorKind;
            using Status = preview::helper_protocol::ResponseStatus;
            switch (batch->error.kind) {
            case Kind::network_disconnected:
            case Kind::timed_out:
            case Kind::io_error:
                active_->transient_failure = true;
                break;
            case Kind::authentication_required:
                active_->terminal_failure = Status::authentication_failed;
                break;
            case Kind::permission_denied:
                active_->terminal_failure = Status::permission_denied;
                break;
            case Kind::not_found:
                active_->terminal_failure = Status::not_found;
                break;
            case Kind::cancelled:
                active_->terminal_failure = Status::cancelled;
                break;
            case Kind::none:
                break;
            }
            if (active_->terminal_failure) {
                const auto status = *active_->terminal_failure;
                finish_terminal(status);
                return;
            }
        }
        if (finishes_enumeration) {
            active_->enumeration_finished = true;
            active_->builder.finish_children(now);
            break;
        }
    }
}

void FolderMosaicController::launch_probes() {
    if (!active_) {
        return;
    }
    while (const auto probe = active_->builder.next_probe(preview::FolderMosaicClock::now())) {
        if (active_->terminal_failure) {
            static_cast<void>(active_->builder.submit_probe(
                {.child_id = probe->child_id, .outcome = preview::FolderMosaicProbeOutcome::error},
                preview::FolderMosaicClock::now()));
            continue;
        }
        const auto child_id = static_cast<qulonglong>(probe->child_id);
        const auto child = active_->children.constFind(child_id);
        if (child == active_->children.cend()) {
            static_cast<void>(active_->builder.submit_probe(
                {.child_id = probe->child_id, .outcome = preview::FolderMosaicProbeOutcome::error},
                preview::FolderMosaicClock::now()));
            continue;
        }
        const auto preview_generation = active_->preview_generation;
        const auto request_id = previewClient_.request_preview(
            child_id, preview_generation, child->path, child->size, child->modified_unix_ns,
            child->source_revision, active_->request.presentation_edge,
            preview::ThumbnailPriority::folder,
            [this, preview_generation, child_id](const PreviewReply &reply) {
                handle_probe(preview_generation, child_id, reply);
            },
            0, {}, false, allowOfflineFallback_);
        if (request_id == 0) {
            static_cast<void>(active_->builder.submit_probe(
                {.child_id = probe->child_id, .outcome = preview::FolderMosaicProbeOutcome::error},
                preview::FolderMosaicClock::now()));
            continue;
        }
        active_->pending_children.insert(child_id);
    }
}

void FolderMosaicController::handle_probe(const std::uint64_t preview_generation,
                                          const qulonglong child_id, const PreviewReply &reply) {
    if (!active_ || active_->preview_generation != preview_generation ||
        !active_->pending_children.remove(child_id)) {
        return;
    }
    const bool success =
        (reply.status == preview::helper_protocol::ResponseStatus::success_cached ||
         reply.status == preview::helper_protocol::ResponseStatus::success_offline_cached ||
         reply.status == preview::helper_protocol::ResponseStatus::success_decoded) &&
        !reply.image.isNull();
    if (success) {
        active_->images.insert(child_id, reply.image);
    }
    using Status = preview::helper_protocol::ResponseStatus;
    active_->transient_failure =
        active_->transient_failure || reply.status == Status::disconnected ||
        reply.status == Status::timed_out || reply.status == Status::source_unavailable ||
        reply.status == Status::success_offline_cached;
    if (reply.status == Status::permission_denied ||
        reply.status == Status::authentication_failed) {
        finish_terminal(reply.status);
        return;
    }
    static_cast<void>(active_->builder.submit_probe(
        {.child_id = child_id,
         .outcome = success ? preview::FolderMosaicProbeOutcome::image
                            : preview::FolderMosaicProbeOutcome::unsupported},
        preview::FolderMosaicClock::now()));
    launch_probes();
    finish_if_ready();
}

void FolderMosaicController::finish_terminal(
    const preview::helper_protocol::ResponseStatus status) {
    if (!active_) {
        return;
    }
    FolderMosaicReply reply{.folder_id = active_->request.folder_id,
                            .app_generation = appGeneration_,
                            .source_size = active_->request.source_size,
                            .modified_unix_ns = active_->request.modified_unix_ns,
                            .source_revision = active_->request.source_revision,
                            .presentation_edge = active_->request.presentation_edge,
                            .status = status,
                            .image = {}};
    source_->cancel(active_->source_generation);
    previewClient_.cancel_generation(active_->preview_generation);
    active_.reset();
    if (replyHandler_) {
        replyHandler_(std::move(reply));
    }
    start_next();
}

void FolderMosaicController::finish_if_ready() {
    if (!active_) {
        return;
    }
    const auto result = active_->builder.result(preview::FolderMosaicClock::now());
    if (!result) {
        return;
    }
    auto status = preview::helper_protocol::ResponseStatus::success_decoded;
    if (active_->terminal_failure) {
        status = *active_->terminal_failure;
    } else if (active_->transient_failure) {
        status = result->candidate_count != 0
                     ? preview::helper_protocol::ResponseStatus::success_offline_cached
                     : preview::helper_protocol::ResponseStatus::source_unavailable;
    }
    auto reply =
        FolderMosaicReply{.folder_id = active_->request.folder_id,
                          .app_generation = appGeneration_,
                          .source_size = active_->request.source_size,
                          .modified_unix_ns = active_->request.modified_unix_ns,
                          .source_revision = active_->request.source_revision,
                          .presentation_edge = active_->request.presentation_edge,
                          .status = status,
                          .image = active_->terminal_failure ? QImage{} : compose(*result)};
    const auto retry = active_->transient_failure || active_->enumeration_timed_out;
    auto retry_request = active_->request;
    if (status == preview::helper_protocol::ResponseStatus::success_decoded && !retry) {
        completedEdges_.insert(active_->request.folder_id,
                               {.source_size = active_->request.source_size,
                                .modified_unix_ns = active_->request.modified_unix_ns,
                                .source_revision = active_->request.source_revision,
                                .edge = active_->request.presentation_edge});
    }
    source_->cancel(active_->source_generation);
    previewClient_.cancel_generation(active_->preview_generation);
    active_.reset();
    if (replyHandler_) {
        replyHandler_(std::move(reply));
    }
    if (retry) {
        schedule_retry(std::move(retry_request));
    }
    start_next();
}

void FolderMosaicController::schedule_retry(FolderRequest request) {
    if (request.retry_attempt >= options_.retry_delays.size()) {
        // A new viewport pass must not restart an exhausted retry cycle. Refresh or a
        // changed folder revision releases this marker through reset()/enqueue().
        pendingRetries_.insert(request.folder_id, {.source_size = request.source_size,
                                                   .modified_unix_ns = request.modified_unix_ns,
                                                   .source_revision = request.source_revision,
                                                   .attempt = request.retry_attempt,
                                                   .exhausted = true});
        return;
    }
    const auto delay = options_.retry_delays[request.retry_attempt];
    ++request.retry_attempt;
    pendingRetries_.insert(request.folder_id, {.source_size = request.source_size,
                                               .modified_unix_ns = request.modified_unix_ns,
                                               .source_revision = request.source_revision,
                                               .attempt = request.retry_attempt});
    const auto generation = appGeneration_;
    QTimer::singleShot(delay, this, [this, generation, request = std::move(request)]() mutable {
        const auto retry = pendingRetries_.find(request.folder_id);
        if (generation != appGeneration_ || retry == pendingRetries_.end() || retry->exhausted ||
            !same_source(*retry, request.source_size, request.modified_unix_ns,
                         request.source_revision) ||
            retry->attempt != request.retry_attempt) {
            return;
        }
        pendingRetries_.erase(retry);
        if (queue_.size() >= static_cast<qsizetype>(options_.maximum_queued_folders) ||
            queuedFolderIds_.contains(request.folder_id) ||
            (active_ && active_->request.folder_id == request.folder_id)) {
            return;
        }
        queuedFolderIds_.insert(request.folder_id);
        queue_.enqueue(std::move(request));
        start_next();
    });
}

void FolderMosaicController::abandon_active() {
    if (!active_) {
        return;
    }
    source_->cancel(active_->source_generation);
    previewClient_.cancel_generation(active_->preview_generation);
    active_.reset();
}

QImage FolderMosaicController::compose(const preview::FolderMosaicResult &result) const {
    if (!active_ || result.candidate_count == 0) {
        return {};
    }
    const auto edge = std::clamp<int>(active_->request.presentation_edge, 64, 320);
    QImage canvas(edge, edge, QImage::Format_RGBA8888);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    constexpr int gutter = 2;
    for (std::size_t tile_index = 0; tile_index < result.layout.tile_count; ++tile_index) {
        const auto &tile = result.layout.tiles[tile_index];
        const auto child_id = static_cast<qulonglong>(result.candidates[tile.candidate_index]);
        const auto image = active_->images.constFind(child_id);
        if (image == active_->images.cend() || image->isNull()) {
            continue;
        }
        const auto left = static_cast<int>(tile.column) * edge / result.layout.columns;
        const auto top = static_cast<int>(tile.row) * edge / result.layout.rows;
        const auto right =
            static_cast<int>(tile.column + tile.column_span) * edge / result.layout.columns;
        const auto bottom = static_cast<int>(tile.row + tile.row_span) * edge / result.layout.rows;
        const auto cell =
            QRect(left, top, right - left, bottom - top).adjusted(gutter, gutter, -gutter, -gutter);
        auto target_size = image->size();
        target_size.scale(cell.size(), Qt::KeepAspectRatio);
        const QRect target(QPoint(cell.center().x() - target_size.width() / 2,
                                  cell.center().y() - target_size.height() / 2),
                           target_size);
        painter.fillRect(target, Qt::white);
        painter.drawImage(target, *image);
    }
    return canvas;
}

} // namespace vove::ui
