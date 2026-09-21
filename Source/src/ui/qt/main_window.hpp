#pragma once

#include "directory_item_delegate.hpp"
#include "directory_list_model.hpp"
#include "directory_monitor.hpp"
#include "external_application_registry.hpp"
#include "external_open_coordinator.hpp"
#include "folder_mosaic_controller.hpp"
#include "global_search_client.hpp"
#include "preview_client.hpp"
#include "object_transfer_queue.hpp"
#include "trash_dialog.hpp"
#include "translation_manager.hpp"
#include "ui_text.hpp"
#include "vove/catalog/catalog_session.hpp"
#include "vove/fileops/batch_rename_coordinator.hpp"
#include "vove/fileops/file_operation_service.hpp"
#include "vove/fileops/file_transfer_coordinator.hpp"
#include "vove/fileops/permanent_delete_coordinator.hpp"
#include "vove/fileops/trash_coordinator.hpp"
#include "vove/platform/directory_service.hpp"

#include <QMainWindow>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class QButtonGroup;
class QCloseEvent;
class QComboBox;
class QEvent;
class QAction;
class QActionGroup;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QMenu;
class QMimeData;
class QPoint;
class QScrollBar;
class QSlider;
class QSplitter;
class QTimer;
class QToolButton;
class QTreeView;
class QUrl;
class QWidget;

namespace vove::ui {

[[nodiscard]] bool trusted_internal_drop_source(const QObject *source, const QObject *catalog,
                                                const QObject *catalog_viewport) noexcept;
#ifdef VOVE_UI_TEST_HOOKS
[[nodiscard]] QByteArray seal_directory_transfer_payload_for_test(const QByteArray &payload);
#endif

enum class AppTheme : int {
    white,
    vanilla,
    blue,
    dark_gray,
};

class WindowControls;
class NavigationButton;
class PageButton;
class SearchField;
class AddressField;
class LazyDirectoryTreeModel;
class FavoritesView;
class PreviewModifiedLabel;
class PreviewNameLabel;
class OperationNotice;
class ZoomPreview;
class CatalogToolButton;

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(QString initial_path = {}, QWidget *parent = nullptr,
                        QString preview_helper_override = {},
                        fileops::FileOperationServiceOptions file_operation_options = {},
                        const QString &batch_journal_override = {},
                        fileops::FileTransferServiceOptions file_transfer_options = {},
                        QString global_search_helper_override = {});
    ~MainWindow() override;

    void open_external_path(const QString &path, bool select_file = false);
    void open_external_paths(const QStringList &paths);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    enum class PreviewRuntimeState : std::uint8_t {
        online,
        offline,
        blocked,
    };

    enum class CurrentOperationJournalState : std::uint8_t {
        none,
        batch_recovery,
        delete_recovery,
        trash_recovery,
        transfer_recovery,
        directory_transfer_recovery,
        blocked,
    };

    enum class TrashOperationIntent : std::uint8_t {
        move,
        restore,
        purge,
    };

    struct GridViewState {
        std::vector<std::uint64_t> selected_ids;
        std::uint64_t current_id{};
        std::uint64_t top_id{};
        int top_offset{};
        int scroll_value{};
        bool has_top{};
    };

    struct NavigationEntry {
        QString path;
        GridViewState grid{};
        bool saved{};
    };

    struct PendingNavigationRestore {
        QString path;
        GridViewState grid;
        QString reveal_child;
    };

    struct PreviewEdgeState {
        std::uint64_t source_size{};
        std::int64_t modified_unix_ns{};
        QString source_revision;
        std::uint16_t edge{};
    };

    struct CatalogViewState {
        DirectoryListModel::Snapshot catalog;
        GridViewState grid;
        catalog::CatalogSessionUpdate update;
        QHash<qulonglong, PreviewEdgeState> offline_previews;
        QHash<qulonglong, QSet<std::uint32_t>> offline_pages;
        QHash<qulonglong, PreviewEdgeState> revalidated_previews;
        QHash<qulonglong, QSet<std::uint32_t>> revalidated_pages;
        PreviewRuntimeState preview_runtime{PreviewRuntimeState::online};
        bool restrict_offline_fallback{};
        bool preview_revalidation_pending{};
    };

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

    enum class ExternalDirectoryDropIntent : std::uint8_t {
        open,
        pin,
        transfer,
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

    struct PendingRenamePreparation {
        QList<core::DirectoryEntry> entries;
        QList<qsizetype> indices;
        qsizetype index{};
        catalog::RequestGeneration generation{};
        bool received{};
        bool awaiting_worker_reap{};
        std::chrono::steady_clock::time_point readers_deadline;
        std::function<void(QList<core::DirectoryEntry>)> ready;
    };

    void add_favorite(const QString &path);
    void add_trash_favorite();
    void add_current_favorite();
    void set_favorites_pin_drop_active(bool active);
    void apply_artwork_icons();
    void apply_filter();
    void apply_language(AppLanguage language, bool persist);
    void apply_sort();
    void apply_splitter_ratios();
    void apply_theme(AppTheme theme);
    void apply_thumbnail_extent(int extent);
    [[nodiscard]] QStringList allowed_search_roots() const;
    void begin_preview_generation(bool clear_previews = true);
    void begin_viewport_generation();
    void enter_preview_offline_state(bool refresh_catalog = true);
    void enter_preview_blocked_state();
    void leave_preview_protected_state();
    void bind_monitor_profile();
    void monitor_profile_changed();
    void build_ui();
    void go_back();
    void go_forward();
    void go_up();
    void load_settings();
    void leave_global_search(bool restore_catalog);
    [[nodiscard]] CatalogViewState take_catalog_view_state();
    void restore_catalog_view_state(CatalogViewState state);
    void set_recursive_view(bool enabled);
    void leave_recursive_view(bool restore_catalog);
    void update_catalog_tools();
    void show_settings_dialog();
    void show_about_dialog();
    void show_components_dialog();
    void show_contact_dialog(bool support);
    void show_update_dialog();
    void retranslate_shell();
    void open_external_items_in_parent(const QStringList &paths);
    void show_entry_in_catalog(const core::DirectoryEntry &entry);
    void open_entry(const core::DirectoryEntry &entry);
    void open_file_in_default_application(const QString &path);
    void open_file_with_application(const ExternalApplication &application, const QString &path);
    void reveal_file_in_file_manager(const QString &path);
    void prompt_add_external_application(const QString &file_path);
    void open_path(const QString &path, bool add_to_history,
                   const QString &reveal_child = {});
    void save_navigation_state();
    void open_history(int index);
    void restore_navigation_state();
    void poll_catalog();
    void handle_preview_reply(PreviewReply reply);
    void handle_folder_mosaic(FolderMosaicReply reply);
    [[nodiscard]] bool request_preview(const QModelIndex &index, std::uint16_t edge,
                                       preview::ThumbnailPriority priority);
    void schedule_visible_previews();
    void rebuild_history_menu();
    [[nodiscard]] CurrentOperationJournalState refresh_current_operation_journal_state();
    void refresh_catalog();
    [[nodiscard]] bool automatic_refresh_blocked() const;
    [[nodiscard]] bool refresh_catalog_automatically();
    void update_directory_monitor_visibility();
    void schedule_global_search();
    void start_global_search();
    void handle_global_search_batch(search::SearchBatch batch);
    [[nodiscard]] bool removed_global_search_path(const std::string &path) const;
    void allow_observed_global_search_path(const QString &path);
    void reconcile_global_search_move(const QStringList &paths);
    void show_global_search_message(const QString &message);
    void update_global_search_message_geometry();
    void copy_selected_names();
    void copy_selected_paths();
    void copy_selected_objects();
    void paste_objects();
    void update_clipboard_actions();
    void prompt_rename_selected();
    void prepare_rename_entries(QList<core::DirectoryEntry> entries, QList<qsizetype> indices,
                               std::function<void(QList<core::DirectoryEntry>)> ready);
    void poll_rename_preparation();
    void fail_rename_preparation();
    void submit_single_rename(const core::DirectoryEntry &entry, const QString &destination_name);
    void submit_batch_rename(const fileops::BatchRenamePlan &plan,
                             const QList<core::DirectoryEntry> &entries);
    void prompt_delete_selected();
    [[nodiscard]] bool prompt_move_entries_to_trash(const QList<core::DirectoryEntry> &entries);
    void prompt_batch_rename_selected(const QList<core::DirectoryEntry> &entries);
    void prompt_create_directory();
    void prompt_transfer_selected(fileops::FileTransferKind kind);
    [[nodiscard]] bool start_file_transfer(const QList<core::DirectoryEntry> &entries,
                                           const QString &destination_directory,
                                     fileops::FileTransferKind kind);
    [[nodiscard]] bool start_directory_transfer(const QList<core::DirectoryEntry> &entries,
                                                const QString &destination_directory,
                                                fileops::FileTransferKind kind);
    [[nodiscard]] bool start_directory_transfer_paths(
        QStringList source_paths, std::vector<std::string> source_revisions_utf8,
        const QString &destination_directory, fileops::FileTransferKind kind);
    [[nodiscard]] bool submit_next_directory_transfer();
    void poll_transfer_preparation();
    void poll_create_directory_preparation();
    [[nodiscard]] bool begin_external_directory_drop(
        ExternalDirectoryDropIntent intent, QStringList paths,
        fileops::FileTransferKind transfer_kind = fileops::FileTransferKind::copy,
        QString transfer_destination = {});
    [[nodiscard]] bool begin_external_transfer_drop(QStringList paths,
                                                    const QString &destination_directory,
                                                    fileops::FileTransferKind transfer_kind,
                                                    bool trash_target = false,
                                                    bool clipboard_copy = false,
                                                    QList<core::DirectoryEntry> search_entries = {});
    void poll_external_drop();
    void poll_external_directory_drop();
    void poll_external_transfer_drop();
    bool resume_queued_external_open();
    void fail_external_directory_drop(const QString &message);
    void fail_external_transfer_drop(const QString &message);
    void launch_prepared_file_transfer();
    void fail_transfer_preparation(const QString &message);
    void show_catalog_context_menu(const QPoint &position);
    void handle_rename_result(const fileops::OperationResult &result,
                              PendingRenameOperation operation);
    void handle_rename_reconciliation_result(const fileops::OperationResult &result);
    void handle_create_directory_result(const fileops::OperationResult &result,
                                        const QString &destination);
    void handle_batch_rename_progress(const fileops::BatchRenameProgress &progress);
    void handle_batch_rename_result(const fileops::BatchRenameResult &result);
    void handle_delete_progress(const fileops::PermanentDeleteProgress &progress);
    void handle_delete_result(const fileops::PermanentDeleteResult &result);
    void handle_trash_progress(const fileops::TrashProgress &progress);
    void handle_trash_result(const fileops::TrashResult &result, TrashOperationIntent intent);
    void start_trash_catalog_load(bool discover_recovery_roots = true);
    void poll_trash_catalog_loaders();
    void apply_trash_catalog(fileops::TrashCatalogResult catalog);
    void leave_trash_view();
    void show_trash_cleanup();
    void update_trash_favorite_state(const fileops::TrashCatalogResult &catalog);
    void show_trash_context_menu(const QPoint &position);
    [[nodiscard]] bool start_trash_manifest_operation(const std::filesystem::path &manifest,
                                                      TrashAction action,
                                                      bool confirm_permanent_delete = true);
    [[nodiscard]] bool submit_next_trash_purge();
    [[nodiscard]] bool confirm_and_start_trash_move(const QList<core::DirectoryEntry> &entries);
    void handle_file_transfer_progress(const fileops::FileTransferProgressUpdate &progress);
    void handle_file_transfer_conflict(const fileops::FileTransferConflict &conflict,
                                      bool object_queue = false, bool overwrite_allowed = true,
                                      bool copy_allowed = true);
    bool start_transfer_selection(const QList<core::DirectoryEntry> &entries,
                                  const QString &destination, fileops::FileTransferKind kind);
    bool start_object_transfer(const QList<core::DirectoryEntry> &entries,
                               const QString &destination, fileops::FileTransferKind kind,
                               bool resume = false);
    void handle_file_transfer_result(const fileops::FileTransferResult &result);
    void notify_file_in_use(fileops::OperationStatus status);
    void
    handle_directory_transfer_progress(const fileops::BasicDirectoryTransferProgress &progress);
    [[nodiscard]] bool record_directory_transfer_completion();
    void handle_directory_transfer_result(const fileops::BasicDirectoryTransferResult &result);
    void show_trash();
    void refresh_after_rename(const QString &selection_path);
    void refresh_after_batch_rename(const QStringList &selection_paths);
    void set_rename_in_flight(bool in_flight);
    [[nodiscard]] bool submit_batch_rename_recovery();
    [[nodiscard]] bool submit_delete_recovery();
    [[nodiscard]] bool submit_trash_recovery();
    [[nodiscard]] bool submit_file_transfer_recovery();
    [[nodiscard]] bool submit_directory_transfer_recovery();
    void offer_directory_transfer_discard();
    [[nodiscard]] bool persist_directory_transfer_recovery_state();
    [[nodiscard]] QByteArray encode_directory_transfer_recovery_state(bool completion_known) const;
    [[nodiscard]] bool advance_directory_transfer_batch_after_success();
    void clear_directory_transfer_recovery_state();
    [[nodiscard]] bool submit_pending_rename_reconciliation();
    void update_rename_action();
    void set_delete_in_flight(bool in_flight);
    void set_transfer_in_flight(bool in_flight);
    void update_transfer_drag_cursor(bool move);
    void clear_transfer_drag_cursor();
    void show_drag_status(bool transfer, bool move, int count, const QString &message = {});
    void clear_drag_status();
    void set_create_directory_in_flight(bool in_flight);
    [[nodiscard]] QList<core::DirectoryEntry> selected_entries_in_view_order() const;
    [[nodiscard]] QString selection_status_text() const;
    [[nodiscard]] QModelIndex index_for_path(const QString &path) const;
    void restore_pending_rename_selection();
    void rename_selected_favorite();
    void move_selected_favorite(int offset);
    void remove_selected_favorite();
    void save_settings();
    void schedule_settings_save();
    void show_selection(const QModelIndex &index);
    [[nodiscard]] GridViewState capture_grid_view_state() const;
    void restore_grid_view_state(const GridViewState &state);
    void update_directory_tree();
    void refresh_directory_sources();
    void update_location_selection();
    void choose_directory();
    void update_navigation();
    [[nodiscard]] bool operation_status_is_active() const noexcept;
    void update_operation_status_pulse();
    void apply_operation_status_color();
    void update_status(const catalog::CatalogSessionUpdate &update);
    void update_selected_preview(const QModelIndex &index);
    void reset_selected_preview(bool clear_image = true);
    void ensure_selected_preview(bool extended_limits = false);
    [[nodiscard]] bool can_force_preview(const core::DirectoryEntry &entry) const;
    void force_preview(qulonglong entry_id);
    void cancel_forced_preview();
    [[nodiscard]] std::uint16_t selected_preview_edge() const;
    void request_selected_page(std::uint32_t page_index);
    void handle_selected_page_reply(PreviewReply reply);
    void update_page_navigation(std::uint32_t page_index, std::uint32_t page_count);
    void prompt_for_document_password();
    void update_document_password_action(preview::helper_protocol::ResponseStatus status);
    void update_preview_support_label(bool preview_ready);
    void retry_external_component_previews();
    [[nodiscard]] QString
    selected_preview_status_text(preview::helper_protocol::ResponseStatus status) const;
    [[nodiscard]] static bool same_source(const PreviewEdgeState &state,
                                          const core::DirectoryEntry &entry) noexcept;
    [[nodiscard]] static bool same_source(const PreviewReply &reply,
                                          const core::DirectoryEntry &entry) noexcept;
    [[nodiscard]] std::uint16_t
    current_preview_edge(const QHash<qulonglong, PreviewEdgeState> &states,
                         const core::DirectoryEntry &entry) const noexcept;
    [[nodiscard]] bool offline_cache_only() const noexcept;
    [[nodiscard]] bool preview_failure_can_disconnect(const core::DirectoryEntry &entry) const;
    [[nodiscard]] bool offline_cache_allowed(const core::DirectoryEntry &entry,
                                             std::uint32_t page_index = 0) const noexcept;

    catalog::CatalogSessionUpdate lastUpdate_;
    catalog::CatalogSessionUpdate beforeAutomaticRefresh_;
    DirectoryListModel listModel_;
    DirectoryItemDelegate delegate_;
    PreviewClient previewClient_;
    FolderMosaicController folderMosaicController_;
    GlobalSearchClient globalSearchClient_;
    platform::DirectoryService source_;
    catalog::CatalogSession session_;
    DirectoryMonitor directoryMonitor_;
    platform::DirectoryService transferProbeSource_;
    platform::DirectoryService dropProbeSource_;
    platform::DirectoryService treeSource_;
    std::filesystem::path currentOperationJournalPath_;
    std::filesystem::path trashManifestDirectory_;
    fileops::FileOperationService fileOperationService_;
    fileops::FileTransferService directoryTransferService_;
    fileops::BatchRenameCoordinator batchRenameCoordinator_;
    fileops::PermanentDeleteCoordinator permanentDeleteCoordinator_;
    fileops::FileTransferCoordinator fileTransferCoordinator_;
    ObjectTransferQueue objectTransferQueue_;
    fileops::TrashCoordinator trashCoordinator_;
    TrashCatalogLoader trashCatalogLoader_;
    ExternalApplicationRegistry externalApplications_;
    ExternalOpenCoordinator externalOpenCoordinator_;

    QList<NavigationEntry> history_;
    std::optional<PendingNavigationRestore> pendingNavigationRestore_;
    bool navigationCatalogReady_{};
    int historyIndex_{-1};
    QString currentPath_;
    QString selectedName_;
    QString selectedColorSummary_;
    QString globalSearchStatus_;
    std::uint64_t cacheMaximumBytes_{10ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t trashMaximumBytes_{50ULL * 1024ULL * 1024ULL * 1024ULL};
    QStringList searchRoots_;
    QStringList pendingRenamePaths_;
    QStringList activeBatchSourcePaths_;
    QStringList activeBatchUnchangedPaths_;
    QString operationStatus_;
    QString dragStatus_;
    double mainShare_{0.60};
    double favoritesShare_{0.30};
    AppTheme theme_{AppTheme::blue};
    TranslationManager translations_;
    AppLanguage language_{AppLanguage::english};

    QWidget *toolbarContent_{};
    QListView *listView_{};
    FavoritesView *favorites_{};
    QScrollBar *favoritesScrollBar_{};
    FavoritesView *trashFavorite_{};
    QLabel *favoritesTitle_{};
    LazyDirectoryTreeModel *treeModel_{};
    QTreeView *directoryTree_{};
    bool treeInsertBatchPending_{};
    bool treeRevealCurrentAfterInsert_{};
    QSplitter *mainSplitter_{};
    QSplitter *placesSplitter_{};
    AddressField *pathEdit_{};
    QAction *chooseFolderAction_{};
    QWidget *favoritesHeader_{};
    QToolButton *addFavoriteButton_{};
    QWidget *trashFavoriteDock_{};
    QWidget *directoryHeader_{};
    SearchField *filterEdit_{};
    SearchField *globalSearchEdit_{};
    QWidget *sortControls_{};
    QComboBox *sortField_{};
    QToolButton *sortDirection_{};
    QSlider *sizeSlider_{};
    QWidget *previewPanel_{};
    ZoomPreview *previewCanvas_{};
    QLabel *globalSearchMessage_{};
    QWidget *pageNavigation_{};
    PageButton *previousPageButton_{};
    PageButton *nextPageButton_{};
    QToolButton *passwordButton_{};
    QLabel *pageIndicator_{};
    PreviewNameLabel *previewName_{};
    PreviewModifiedLabel *previewModified_{};
    QLabel *previewFormat_{};
    QLabel *previewSupport_{};
    QLabel *status_{};
    OperationNotice *operationNotice_{};
    QTimer *pollTimer_{};
    QTimer *previewTimer_{};
    QTimer *settingsTimer_{};
    QTimer *globalSearchTimer_{};
    QTimer *transferProbeTimer_{};
    QTimer *dropProbeTimer_{};
    QTimer *operationStatusPulseTimer_{};
    QTimer *trashCatalogTimer_{};
    NavigationButton *backButton_{};
    NavigationButton *forwardButton_{};
    NavigationButton *historyButton_{};
    NavigationButton *upButton_{};
    QToolButton *createFolderButton_{};
    QToolButton *smallerThumbnailsButton_{};
    QToolButton *largerThumbnailsButton_{};
    CatalogToolButton *recursiveViewButton_{};
    CatalogToolButton *refreshButton_{};
    QToolButton *logoButton_{};
    WindowControls *windowControls_{};
    QMenu *historyMenu_{};
    QMenu *mainMenu_{};
    QMenu *languageMenu_{};
    QAction *createFolderAction_{};
    QAction *renameAction_{};
    QAction *deleteAction_{};
    QAction *copyNamesAction_{};
    QAction *cancelPreviewAttemptAction_{};
    QAction *copyPathsAction_{};
    QAction *copyObjectsAction_{};
    QAction *pasteObjectsAction_{};
    QAction *trashAction_{};
    QAction *settingsAction_{};
    QAction *updateAction_{};
    QAction *aboutAction_{};
    QAction *componentsAction_{};
    QAction *donateAction_{};
    QAction *reportIssueAction_{};
    QActionGroup *languageActions_{};
    QButtonGroup *themeButtons_{};
    qulonglong selectedEntryId_{};
    std::uint32_t selectedPageIndex_{};
    std::uint32_t selectedPageCount_{};
    std::optional<PreviewReply> selectedPreview_;
    preview::helper_protocol::RequestId selectedPreviewRequest_{};
    std::uint16_t selectedPreviewPendingEdge_{};
    bool selectedPreviewExtended_{};
    std::uint64_t selectedPreviewGeneration_{1ULL << 62U};
    std::uint64_t previewGeneration_{1};
    std::uint64_t viewportGeneration_{1};
    std::uint64_t activeGlobalSearchGeneration_{};
    std::uint64_t globalSearchResultCount_{};
    QHash<qulonglong, PreviewEdgeState> pendingPreviewEdges_;
    QHash<qulonglong, PreviewEdgeState> completedPreviewEdges_;
    QHash<qulonglong, PreviewEdgeState> offlineEligiblePreviews_;
    QHash<qulonglong, QSet<std::uint32_t>> offlineEligiblePages_;
    QHash<qulonglong, PreviewEdgeState> strictRevalidatedPreviews_;
    QHash<qulonglong, QSet<std::uint32_t>> strictRevalidatedPages_;
    QHash<QString, QString> documentPasswords_;
    std::optional<PendingRenameOperation> pendingRenameOperation_;
    std::optional<PendingTransferPreparation> pendingTransferPreparation_;
    std::optional<PendingDirectoryTransferBatch> pendingDirectoryTransferBatch_;
    std::optional<PendingCreateDirectoryPreparation> pendingCreateDirectoryPreparation_;
    std::optional<PendingExternalDirectoryDrop> pendingExternalDirectoryDrop_;
    std::optional<PendingExternalTransferDrop> pendingExternalTransferDrop_;
    std::optional<PendingRenamePreparation> pendingRenamePreparation_;
    bool renamePreviewSuspended_{};
    std::optional<fileops::TrashCatalogResult> trashCatalog_;
    QHash<qulonglong, QString> trashManifestByEntry_;
    std::vector<std::filesystem::path> trashPurgeQueue_;
    std::size_t trashPurgeCompleted_{};
    std::size_t trashPurgeTotal_{};
    QStringList queuedExternalOpenPaths_;
    std::optional<CatalogViewState> catalogBeforeGlobalSearch_;
    std::optional<CatalogViewState> catalogBeforeRecursiveView_;
    const QMimeData *externalDragMime_{};
    QStringList externalDragPaths_;
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
    bool dragMoveCursorOverride_{};
    bool renameInFlight_{};
    bool deleteInFlight_{};
    bool trashConfirmationInProgress_{};
    bool transferInFlight_{};
    bool createDirectoryInFlight_{};
    bool monitorProfileBound_{};
    bool previewRevalidationPending_{};
    bool restrictOfflineFallback_{};
    bool globalSearchActive_{};
    bool globalSearchReady_{};
    bool catalogBeforeGlobalSearchDirty_{};
    QString globalSearchQuery_;
    QSet<QString> globalSearchRemovedPaths_;
    QSet<QString> globalSearchObservedPaths_;
    bool recursiveViewActive_{};
    bool globalSearchTruncated_{};
    bool pendingCatalogReveal_{};
    bool automaticRefreshInFlight_{};
    bool trashViewActive_{};
    bool pendingTrashCleanup_{};
    int operationStatusPulsePhase_{};
    PreviewRuntimeState previewRuntimeState_{PreviewRuntimeState::online};
};

} // namespace vove::ui
