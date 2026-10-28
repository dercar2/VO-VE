#pragma once

#include "operation_availability.hpp"
#include "selection_command.hpp"
#include "trash_dialog.hpp"
#include "vove/fileops/file_operation_service.hpp"
#include "vove/fileops/file_transfer_service.hpp"
#include "vove/platform/directory_service.hpp"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVariant>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QWidget;

namespace vove::fileops { struct TrashResult; }

namespace vove::ui {

enum class CurrentOperationJournalState : std::uint8_t {
    none, batch_recovery, delete_recovery, trash_recovery, transfer_recovery,
    directory_transfer_recovery, blocked,
};

enum class ExternalDirectoryDropIntent : std::uint8_t { open, pin, transfer };
enum class TrashOperationIntent : std::uint8_t { move, restore, purge, cancel_move };

struct FileCommandCatalogState {
    QString current_path;
    bool ready{};
    bool trash_view{};
    bool global_search{};
    bool recursive_view{};
    bool search_ready{};
    bool search_drag_enabled{};
    bool search_query_current{true};
    qsizetype removed_search_paths{};
    std::uint64_t search_generation{};
};

struct FileCommandActionState {
    OperationAvailability availability;
    QString rename_text;
    QString rename_tooltip;
    QString delete_text;
    bool rename_enabled{};
    bool delete_enabled{};
    bool trash_enabled{};
    bool unfinished_enabled{};
    bool create_directory_enabled{};
};

struct FileCommandPorts {
    QWidget *dialog_owner{};
    std::function<SelectionSnapshot()> selection;
    std::function<FileCommandCatalogState()> catalog;
    std::function<std::vector<std::filesystem::path>(const QString &parent)> entry_names;
    std::function<std::size_t(const std::filesystem::path &manifest)> trash_operation_items;

    std::function<void(bool clear)> begin_preview_generation;
    std::function<void()> clear_pending_mosaics;
    std::function<void()> stop_animation;
    std::function<void()> stop_preview_readers;
    std::function<bool()> readers_idle;
    std::function<void()> suspend_preview_dispatch;
    std::function<void(int delay_ms)> resume_preview_dispatch;
    std::function<void(const QString &path)> mark_preview_unverified;
    std::function<void()> mark_all_previews_unverified;

    std::function<void(const QString &status)> status_changed;
    std::function<void(const FileCommandActionState &)> actions_changed;
    std::function<void()> activity_changed;
    // Compatibility diagnostics only, not an input or command gate.
    std::function<void(const char *name, const QVariant &value)> diagnostic_changed;
    std::function<void()> file_in_use;
    std::function<void()> clear_selection;
    std::function<void()> refresh_directory_sources;
    // Retry uses the same mode-transition boundary as the user's F5 command.
    std::function<void()> refresh_catalog;
    // -1 preserves the current poll interval.
    std::function<void(int interval_ms)> poll_catalog;
    std::function<void(const QStringList &paths)> reveal_paths;
    std::function<void(const QString &path)> refresh_after_rename;
    std::function<void(const QStringList &paths)> refresh_after_batch_rename;
    std::function<void(bool discover_recovery_roots)> load_trash_catalog;
    std::function<void()> mark_saved_search_dirty;
    std::function<void(const QString &path)> observe_search_path;
    std::function<void(const QStringList &paths)> reconcile_search_move;
    std::function<void()> schedule_search;
    std::function<void(const QString &path, bool add_to_history)> open_path;
    std::function<void(const QStringList &paths)> open_external_paths;
    std::function<void(const QStringList &paths)> open_external_items_in_parent;
    std::function<void(const QString &path)> pin_path;

    // Optional test/storage injection; defaults use a fresh QSettings for every call.
    // Empty bytes remove the current state and obsolete keys. write_recovery MUST sync
    // before returning and be worker-callable:
    // the completed checkpoint is persisted before the backend may retire its manifest.
    std::function<QByteArray()> read_recovery;
    std::function<bool(const QByteArray &)> write_recovery;
};

struct FileCommandOptions {
    fileops::FileOperationServiceOptions file_operations;
    fileops::FileTransferServiceOptions file_transfers;
    QString journal_override;
    platform::DirectoryServiceOptions transfer_probes;
    platform::DirectoryServiceOptions drop_probes;
};

class FileCommandController final : public QObject {
  public:
    explicit FileCommandController(FileCommandPorts ports, FileCommandOptions options = {},
                                   QObject *parent = nullptr);
    ~FileCommandController() override;

    [[nodiscard]] OperationAvailability availability() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool mutation_in_flight() const;
    [[nodiscard]] bool catalog_tools_busy() const;
    [[nodiscard]] bool input_preparation_pending() const;
    [[nodiscard]] bool automatic_refresh_blocked() const;
    [[nodiscard]] bool recovery_pending() const;
    [[nodiscard]] bool journal_blocked() const;
    [[nodiscard]] bool readers_idle() const;
    [[nodiscard]] bool can_paste_objects() const;
    // Target/mode/authentication checks remain in DropRouter. Global search is a valid
    // outbound source; selection-action availability is deliberately not this gate.
    [[nodiscard]] bool drop_transfer_allowed() const;
    // Probe admission only: Favorites validation did not wait for unrelated mutations.
    [[nodiscard]] bool external_drop_validation_allowed() const;
    [[nodiscard]] bool trash_cleanup_allowed() const;
    [[nodiscard]] bool can_close() const;
    [[nodiscard]] bool refresh_or_recover();
    [[nodiscard]] CurrentOperationJournalState refresh_current_operation_journal_state();
    void update_actions();
    void load_recovery_state();
    [[nodiscard]] bool save_recovery_state();
    [[nodiscard]] const std::filesystem::path &journal_path() const;
    [[nodiscard]] const std::filesystem::path &trash_manifest_directory() const;
    [[nodiscard]] QString status() const;
    void clear_completed_status();
    void paste_objects();
    void set_trash_maximum_bytes(std::uint64_t maximum_bytes);

    void prompt_transfer_selected(fileops::FileTransferKind kind);
    void prompt_rename_selected();
    void prompt_delete_selected();
    void prompt_create_directory();
    void show_unfinished_operation_dialog();
    [[nodiscard]] bool start_transfer_selection(const QList<core::DirectoryEntry> &entries,
        const QString &destination, fileops::FileTransferKind kind);
    [[nodiscard]] bool start_object_transfer(const QList<core::DirectoryEntry> &entries,
        const QString &destination, fileops::FileTransferKind kind, bool resume = false);
    [[nodiscard]] bool start_file_transfer(const QList<core::DirectoryEntry> &entries,
        const QString &destination, fileops::FileTransferKind kind);
    [[nodiscard]] bool start_directory_transfer(const QList<core::DirectoryEntry> &entries,
        const QString &destination, fileops::FileTransferKind kind);
    [[nodiscard]] bool start_directory_transfer_paths(QStringList paths,
        std::vector<std::string> revisions, const QString &destination,
        fileops::FileTransferKind kind);
    [[nodiscard]] bool begin_external_directory_drop(ExternalDirectoryDropIntent intent,
        QStringList paths, fileops::FileTransferKind kind = fileops::FileTransferKind::copy,
        QString destination = {});
    [[nodiscard]] bool begin_external_transfer_drop(QStringList paths,
        const QString &destination, fileops::FileTransferKind kind,
        bool trash_target = false, bool clipboard_copy = false,
        QList<core::DirectoryEntry> search_entries = {});
    // Paths are already normalized by the caller; owns admission, queuing and dispatch.
    void request_external_open(QStringList normalized_paths);
    void queue_external_open(QStringList paths);
    void cancel_search_drop();
    [[nodiscard]] bool resume_queued_external_open();
    [[nodiscard]] bool prompt_move_entries_to_trash(const QList<core::DirectoryEntry> &entries);
    [[nodiscard]] bool start_trash_manifest_operation(const std::filesystem::path &manifest,
        TrashAction action, bool confirm_permanent_delete = true);
    [[nodiscard]] bool start_trash_purge(std::vector<std::filesystem::path> manifests);
    [[nodiscard]] bool submit_batch_rename_recovery();
    [[nodiscard]] bool submit_delete_recovery();
    [[nodiscard]] bool submit_trash_recovery(bool cancel_remaining = false);
    [[nodiscard]] bool submit_file_transfer_recovery();
    [[nodiscard]] bool submit_directory_transfer_recovery();
    [[nodiscard]] bool submit_pending_rename_reconciliation();
    void offer_directory_transfer_discard();

#ifdef VOVE_UI_TEST_HOOKS
    // Tests target the command owner, not a friend of the root window.
    void test_deliver_trash_result(const fileops::TrashResult &, TrashOperationIntent);
    [[nodiscard]] bool test_preparation_idle() const;
    void test_set_in_flight(bool rename, bool deleting, bool transfer, bool create);
    [[nodiscard]] static QByteArray test_seal_directory_transfer_payload(const QByteArray &);
#endif

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vove::ui
