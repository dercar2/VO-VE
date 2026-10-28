#pragma once

#include "file_command_controller.hpp"
#include "object_transfer_queue.hpp"
#include "batch_rename_dialog.hpp"
#include "rename_dialog.hpp"
#include "diagnostic_sink.hpp"
#include "vove/search/global_search.hpp"
#include "vove/fileops/batch_rename_coordinator.hpp"
#include "vove/fileops/permanent_delete_coordinator.hpp"
#include "vove/fileops/trash_coordinator.hpp"
#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/directory_transfer_manifest.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vove::ui::file_command_detail {
std::filesystem::path native_path(const QString &);
QString display_path(const std::filesystem::path &);
QString transfer_path_key(const std::filesystem::path &);
bool same_directory_path(const QString &, const QString &);
bool same_entry_snapshots(const QList<core::DirectoryEntry> &, const QList<core::DirectoryEntry> &) noexcept;
#ifdef Q_OS_WIN
bool is_unc_path(const QString &);
#endif
std::filesystem::path current_journal_path(const QString &);
std::filesystem::path trash_manifest_directory(const std::filesystem::path &);
fileops::FileTransferServiceOptions directory_transfer_service_options(fileops::FileTransferServiceOptions);
std::uint64_t next_directory_transfer_request_id() noexcept;
QString operation_status_text(fileops::OperationStatus);
bool ambiguous_rename_status(fileops::OperationStatus);
bool confirms_no_committed_rename(const fileops::OperationResult &);
QString trash_operation_error_text(fileops::OperationStatus, std::string_view detail = {});
QString directory_transfer_error_text(fileops::BasicDirectoryTransferStatus);
QByteArray seal_directory_transfer_payload(const QByteArray &);
QByteArray seal_directory_transfer_state(const QJsonObject &);
std::optional<QJsonObject> open_directory_transfer_state(const QByteArray &);
} // namespace vove::ui::file_command_detail

namespace vove::ui {

class FileCommandController::Impl final : public QObject {
  public:
    Impl(FileCommandPorts ports, FileCommandOptions options, QObject *parent);
    ~Impl() override;
    struct PendingRenameOperation {
        fileops::RenameRequest request;
        QString source_path;
        QString destination_path;
    };

    struct TransferProbeTarget {
        catalog::RequestGeneration generation{};
        std::filesystem::path path;
        std::string revision_utf8;
        bool complete{};
    };

    struct PendingTransferPreparation {
        fileops::FileTransferKind kind{fileops::FileTransferKind::copy};
        QList<core::DirectoryEntry> entries;
        std::filesystem::path destination_directory;
        std::vector<TransferProbeTarget> probes;
    };

    struct PendingDirectoryTransferBatch {
        fileops::FileTransferKind kind{fileops::FileTransferKind::copy};
        QStringList source_paths;
        std::vector<std::string> source_revisions_utf8;
        std::filesystem::path destination_directory;
        qsizetype index{};
        qsizetype completed{};
    };

    struct PendingCreateDirectoryPreparation {
        catalog::RequestGeneration generation{};
        std::filesystem::path destination;
    };

    struct PendingExternalDirectoryDrop {
        ExternalDirectoryDropIntent intent{ExternalDirectoryDropIntent::open};
        QStringList paths;
        QStringList confirmed_paths;
        std::vector<std::string> confirmed_revisions_utf8;
        qsizetype index{};
        catalog::RequestGeneration generation{};
        bool awaiting_worker_reap{};
        fileops::FileTransferKind transfer_kind{fileops::FileTransferKind::copy};
        QString transfer_destination;
    };

    struct PendingExternalTransferDrop {
        QStringList paths;
        QList<core::DirectoryEntry> confirmed_entries;
        QList<core::DirectoryEntry> search_entries;
        std::uint64_t search_generation{};
        std::chrono::steady_clock::time_point search_deadline;
        qsizetype index{};
        catalog::RequestGeneration generation{};
        bool awaiting_worker_reap{};
        fileops::FileTransferKind transfer_kind{fileops::FileTransferKind::copy};
        QString transfer_destination;
        bool trash_target{};
        bool clipboard_copy{};
    };

    struct PendingDirectoryPreparation {
        QList<core::DirectoryEntry> entries;
        QList<qsizetype> indices;
        qsizetype index{};
        catalog::RequestGeneration generation{};
        bool received{};
        bool awaiting_worker_reap{};
        bool trash_target{};
        std::chrono::steady_clock::time_point readers_deadline;
        std::function<void(QList<core::DirectoryEntry>)> ready;
    };


    void prompt_transfer_selected(const fileops::FileTransferKind kind);
    bool start_transfer_selection(const QList<core::DirectoryEntry> &entries,
        const QString &destination, const fileops::FileTransferKind kind);
    bool start_object_transfer(const QList<core::DirectoryEntry> &entries,
        const QString &destination, const fileops::FileTransferKind kind,
        const bool resume = false);
    bool start_file_transfer(const QList<core::DirectoryEntry> &entries,
        const QString &destination_directory,
        const fileops::FileTransferKind kind);
    bool start_directory_transfer(const QList<core::DirectoryEntry> &entries,
        const QString &destination_directory,
        const fileops::FileTransferKind kind);
    bool start_directory_transfer_paths(QStringList source_paths,
        std::vector<std::string> source_revisions_utf8,
        const QString &destination_directory,
        const fileops::FileTransferKind kind);
    bool submit_next_directory_transfer();
    void launch_prepared_file_transfer();
    void handle_file_transfer_progress(
        const fileops::FileTransferProgressUpdate &progress);
    void handle_file_transfer_conflict(const fileops::FileTransferConflict &conflict,
        const bool object_queue = false, const bool overwrite_allowed = true,
        const bool copy_allowed = true);
    void handle_file_transfer_result(const fileops::FileTransferResult &result);
    void handle_directory_transfer_progress(
        const fileops::BasicDirectoryTransferProgress &progress);
    bool record_directory_transfer_completion();
    void handle_directory_transfer_result(
        const fileops::BasicDirectoryTransferResult &result);
    void poll_transfer_preparation();
    void poll_create_directory_preparation();
    bool begin_external_directory_drop(const ExternalDirectoryDropIntent intent,
        QStringList paths,
        const fileops::FileTransferKind transfer_kind,
        QString transfer_destination);
    bool begin_external_transfer_drop(QStringList paths,
        const QString &destination_directory,
        const fileops::FileTransferKind transfer_kind,
        const bool trash_target,
        const bool clipboard_copy,
        QList<core::DirectoryEntry> search_entries);
    void poll_external_drop();
    void poll_external_directory_drop();
    void poll_external_transfer_drop();
    bool resume_queued_external_open();
    void fail_external_directory_drop(const QString &message);
    void fail_external_transfer_drop(const QString &message);
    void fail_transfer_preparation(const QString &message);
    void prepare_directory_entries(
        QList<core::DirectoryEntry> entries, QList<qsizetype> indices,
        std::function<void(QList<core::DirectoryEntry>)> ready, const bool trash_target = false);
    void poll_directory_preparation();
    void fail_directory_preparation();
    void prompt_create_directory();
    void prompt_rename_selected();
    void submit_single_rename(const core::DirectoryEntry &entry,
        const QString &destination_name);
    void prompt_batch_rename_selected(const QList<core::DirectoryEntry> &entries);
    void submit_batch_rename(const fileops::BatchRenamePlan &plan,
        const QList<core::DirectoryEntry> &entries);
    void prompt_delete_selected();
    void handle_rename_result(const fileops::OperationResult &result,
        PendingRenameOperation operation);
    void handle_create_directory_result(const fileops::OperationResult &result,
        const QString &destination);
    bool submit_pending_rename_reconciliation();
    void handle_rename_reconciliation_result(const fileops::OperationResult &result);
    void handle_batch_rename_progress(const fileops::BatchRenameProgress &progress);
    void handle_batch_rename_result(const fileops::BatchRenameResult &result);
    void handle_delete_progress(const fileops::PermanentDeleteProgress &progress);
    void handle_delete_result(const fileops::PermanentDeleteResult &result);
    bool prompt_move_entries_to_trash(const QList<core::DirectoryEntry> &entries);
    bool confirm_and_start_trash_move(const QList<core::DirectoryEntry> &entries);
    bool submit_trash_move(const QList<core::DirectoryEntry> &entries);
    void handle_trash_progress(const fileops::TrashProgress &progress);
    void handle_trash_result(const fileops::TrashResult &result,
        const TrashOperationIntent intent);
    bool submit_next_trash_purge();
    bool start_trash_manifest_operation(const std::filesystem::path &manifest,
        const TrashAction action,
        const bool confirm_permanent_delete = true);
    CurrentOperationJournalState refresh_current_operation_journal_state();
    bool submit_batch_rename_recovery();
    bool submit_delete_recovery();
    bool submit_trash_recovery(const bool cancel_remaining = false);
    bool submit_file_transfer_recovery();
    void offer_directory_transfer_discard();
    bool submit_directory_transfer_recovery();
    bool advance_directory_transfer_batch_after_success();
    QByteArray encode_directory_transfer_recovery_state(const bool completion_known) const;
    bool persist_directory_transfer_recovery_state();
    void clear_directory_transfer_recovery_state();
    void show_unfinished_operation_dialog();
    void set_rename_in_flight(const bool in_flight);
    void set_delete_in_flight(const bool in_flight);
    void set_transfer_in_flight(const bool in_flight);
    void set_create_directory_in_flight(const bool in_flight);
    OperationAvailability operation_availability() const;
    void update_rename_action();
    void notify_file_in_use(const fileops::OperationStatus status);
    FileCommandCatalogState catalog() const;
    QList<core::DirectoryEntry> selected_entries_in_view_order() const;
    bool mutation_in_flight() const;
    bool catalog_tools_busy() const;
    bool automatic_refresh_blocked() const;
    bool backend_busy() const;
    bool can_close() const;
    bool trash_cleanup_allowed() const;
    bool readers_idle() const;
    bool recovery_pending() const;
    bool refresh_or_recover();
    void load_recovery_state();
    void queue_external_open(QStringList);
    void cancel_search_drop();
    bool start_trash_purge(std::vector<std::filesystem::path>);
    void publish_status();
    void publish_activity();
    void set_property(const char *name, const QVariant &value);
    QWidget *dialog_owner() const { return dialogOwner_.data(); }
    void begin_preview_generation(bool clear = true);
    void stop_selected_animation();
    void refresh_directory_sources();
    void start_trash_catalog_load(bool discover = true);
    void refresh_after_rename(const QString &);
    void refresh_after_batch_rename(const QStringList &);
    void open_path(const QString &, bool);
    void open_external_paths(const QStringList &);
    void open_external_items_in_parent(const QStringList &);
    void add_favorite(const QString &);
    void allow_observed_global_search_path(const QString &);
    void reconcile_global_search_move(const QStringList &);
    void schedule_global_search();
    void poll_catalog(int interval = -1);
    void resume_preview_dispatch(int delay = -1);

    FileCommandPorts ports_;
    QPointer<QWidget> dialogOwner_;
    QTimer *transferProbeTimer_{};
    QTimer *dropProbeTimer_{};
    platform::DirectoryService transferProbeSource_;
    platform::DirectoryService dropProbeSource_;
    std::filesystem::path currentOperationJournalPath_;
    std::filesystem::path trashManifestDirectory_;
    fileops::FileOperationService fileOperationService_;
    fileops::FileTransferService directoryTransferService_;
    fileops::BatchRenameCoordinator batchRenameCoordinator_;
    fileops::PermanentDeleteCoordinator permanentDeleteCoordinator_;
    fileops::FileTransferCoordinator fileTransferCoordinator_;
    ObjectTransferQueue objectTransferQueue_;
    fileops::TrashCoordinator trashCoordinator_;
    QStringList activeBatchSourcePaths_;
    QStringList activeBatchUnchangedPaths_;
    QString operationStatus_;
    std::optional<PendingRenameOperation> pendingRenameOperation_;
    std::optional<PendingTransferPreparation> pendingTransferPreparation_;
    std::optional<PendingDirectoryTransferBatch> pendingDirectoryTransferBatch_;
    std::optional<PendingCreateDirectoryPreparation> pendingCreateDirectoryPreparation_;
    std::optional<PendingExternalDirectoryDrop> pendingExternalDirectoryDrop_;
    std::optional<PendingExternalTransferDrop> pendingExternalTransferDrop_;
    std::optional<PendingDirectoryPreparation> pendingDirectoryPreparation_;
    bool renamePreviewSuspended_{};
    std::vector<std::filesystem::path> trashPurgeQueue_;
    std::size_t trashPurgeCompleted_{};
    std::size_t trashPurgeTotal_{};
    QStringList queuedExternalOpenPaths_;
    catalog::RequestGeneration nextTransferProbeGeneration_{1};
    catalog::RequestGeneration nextDropProbeGeneration_{1};
    fileops::FileTransferKind activeTransferKind_{fileops::FileTransferKind::copy};
    QStringList activeFileTransferSources_;
    QString directoryTransferRecoveryManifest_;
    QString directoryTransferRecoverySource_;
    QString directoryTransferRecoveryDestination_;
    QString directoryTransferRecoverySourceRevision_;
    std::uint64_t directoryTransferRecoveryRequestId_{};
    bool directoryTransferCompletionKnown_{};
    bool directoryTransferDiscarding_{};
    bool directoryTransferStateCorrupt_{};
    bool renameInFlight_{};
    bool deleteInFlight_{};
    bool trashConfirmationInProgress_{};
    bool transferInFlight_{};
    bool createDirectoryInFlight_{};
};

} // namespace vove::ui
