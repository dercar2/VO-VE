#pragma once

#include "preview_client.hpp"
#include "vove/platform/directory_service.hpp"
#include "vove/preview/folder_mosaic.hpp"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

#include <chrono>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QTimer;

namespace vove::ui {

struct FolderMosaicReply {
    qulonglong folder_id{};
    std::uint64_t app_generation{};
    std::uint64_t source_size{};
    std::int64_t modified_unix_ns{};
    QString source_revision;
    std::uint16_t presentation_edge{};
    preview::helper_protocol::ResponseStatus status{
        preview::helper_protocol::ResponseStatus::success_decoded};
    QImage image;
};

struct FolderMosaicControllerOptions {
    // Bound directory enumeration, not time spent waiting in the shared preview queue.
    std::chrono::milliseconds budget{preview::kFolderMosaicBudget};
    std::size_t maximum_queued_folders{128};
    std::array<std::chrono::milliseconds, 3> retry_delays{
        std::chrono::seconds(1), std::chrono::seconds(3), std::chrono::seconds(10)};
};

class FolderMosaicController final : public QObject {
  public:
    using ReplyHandler = std::function<void(FolderMosaicReply)>;

    explicit FolderMosaicController(PreviewClient &preview_client, QObject *parent = nullptr,
                                    FolderMosaicControllerOptions options = {},
                                    std::unique_ptr<catalog::DirectorySource> source = {});
    ~FolderMosaicController() override;

    FolderMosaicController(const FolderMosaicController &) = delete;
    FolderMosaicController &operator=(const FolderMosaicController &) = delete;

    void set_reply_handler(ReplyHandler handler);
    void reset(std::uint64_t app_generation);
    void clear_pending();
    void discard_queued();
    void set_offline(bool offline);
    void set_allow_offline_fallback(bool allow);
    void invalidate_display_images();
    void enqueue(qulonglong folder_id, const QString &path, std::uint64_t source_size,
                 std::int64_t modified_unix_ns, QString source_revision,
                 std::uint16_t presentation_edge);
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] bool readers_idle() const noexcept;

  private:
    struct FolderRequest {
        qulonglong folder_id{};
        QString path;
        std::uint64_t source_size{};
        std::int64_t modified_unix_ns{};
        QString source_revision;
        std::uint16_t presentation_edge{};
        std::size_t retry_attempt{};
    };

    struct ChildSource {
        QString path;
        std::uint64_t size{};
        std::int64_t modified_unix_ns{};
        QString source_revision;
    };

    struct EnumeratedChild {
        qulonglong id{};
        ChildSource source;
    };

    struct CompletedFolder {
        std::uint64_t source_size{};
        std::int64_t modified_unix_ns{};
        QString source_revision;
        std::uint16_t edge{};
    };

    struct PendingRetry {
        std::uint64_t source_size{};
        std::int64_t modified_unix_ns{};
        QString source_revision;
        std::size_t attempt{};
        bool exhausted{};
    };

    struct ActiveGenerations {
        catalog::RequestGeneration source{};
        std::uint64_t preview{};
    };

    struct ActiveFolder {
        FolderRequest request;
        catalog::RequestGeneration source_generation{};
        std::uint64_t preview_generation{};
        preview::FolderMosaicBuilder builder;
        preview::FolderMosaicBuilder::TimePoint enumeration_deadline;
        QHash<qulonglong, ChildSource> children;
        std::vector<EnumeratedChild> deferred_children;
        QHash<qulonglong, QImage> images;
        QSet<qulonglong> pending_children;
        bool enumeration_finished{};
        bool enumeration_timed_out{};
        bool transient_failure{};
        std::optional<preview::helper_protocol::ResponseStatus> terminal_failure;

        ActiveFolder(FolderRequest request_value, ActiveGenerations generations,
                     preview::FolderMosaicBuilder::TimePoint deadline);
    };

    void start_next();
    void poll();
    void consume_catalog();
    void launch_probes();
    void handle_probe(std::uint64_t preview_generation, qulonglong child_id,
                      const PreviewReply &reply);
    void finish_terminal(preview::helper_protocol::ResponseStatus status);
    void finish_if_ready();
    void abandon_active();
    void schedule_retry(FolderRequest request);
    [[nodiscard]] QImage compose(const preview::FolderMosaicResult &result) const;

    PreviewClient &previewClient_;
    std::unique_ptr<catalog::DirectorySource> source_;
    FolderMosaicControllerOptions options_;
    QTimer *timer_{};
    QQueue<FolderRequest> queue_;
    QSet<qulonglong> queuedFolderIds_;
    QHash<qulonglong, CompletedFolder> completedEdges_;
    QHash<qulonglong, PendingRetry> pendingRetries_;
    std::optional<ActiveFolder> active_;
    ReplyHandler replyHandler_;
    std::uint64_t appGeneration_{1};
    std::uint64_t serial_{};
    bool offline_{};
    bool allowOfflineFallback_{true};
};

} // namespace vove::ui
