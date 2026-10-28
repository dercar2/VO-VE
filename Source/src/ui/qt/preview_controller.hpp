#pragma once

#include "folder_mosaic_controller.hpp"
#include "gif_animation_client.hpp"
#include "preview_client.hpp"
#include "vove/catalog/catalog_session.hpp"

#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QSet>

#include <cstdint>
#include <functional>
#include <optional>

class QAction;
class QLabel;
class QListView;
class QTimer;
class QToolButton;
class QWidget;

namespace vove::ui {

class DirectoryListModel;
class PageButton;
class PreviewModifiedLabel;
class PreviewNameLabel;
class ZoomPreview;

enum class PreviewRuntimeState : std::uint8_t { online, offline, blocked };
enum class PreviewCatalogMode : std::uint8_t { directory, global_search, recursive, trash };

struct PreviewContext {
    PreviewCatalogMode catalog_mode{PreviewCatalogMode::directory};
    bool navigation_restore_pending{};
};

struct PreviewEdgeState {
    std::uint64_t source_size{};
    std::int64_t modified_unix_ns{};
    QString source_revision;
    std::uint16_t edge{};
};

// Catalog snapshots carry authority, not outstanding requests or live animation streams.
struct PreviewCatalogState {
    QHash<qulonglong, PreviewEdgeState> offline_previews;
    QHash<qulonglong, QSet<std::uint32_t>> offline_pages;
    QHash<qulonglong, PreviewEdgeState> revalidated_previews;
    QHash<qulonglong, QSet<std::uint32_t>> revalidated_pages;
    PreviewRuntimeState runtime{PreviewRuntimeState::online};
    bool restrict_offline_fallback{};
    bool revalidation_pending{};
};

// Borrowed views must outlive the controller, as with the shell's other controllers.
struct PreviewViews {
    QWidget *window{};
    DirectoryListModel *model{};
    QListView *catalog{};
    ZoomPreview *canvas{};
    PreviewNameLabel *name{};
    PreviewModifiedLabel *modified{};
    QLabel *format{};
    QLabel *support{};
    QWidget *page_navigation{};
    PageButton *previous_page{};
    PageButton *next_page{};
    QToolButton *password{};
    QLabel *page_indicator{};
    QAction *cancel_attempt{};
};

struct PreviewCallbacks {
    // Revalidate directory sources and resume catalog polling at its retry interval.
    std::function<void()> request_source_revalidation;
    std::function<void()> status_changed;
};

class PreviewController final : public QObject {
  public:
    explicit PreviewController(QObject *parent = nullptr, QString helper_program_override = {});
    ~PreviewController() override;

    PreviewController(const PreviewController &) = delete;
    PreviewController &operator=(const PreviewController &) = delete;

    // Call once, after the shell has built its widgets and assigned the catalog model.
    void attachViews(PreviewViews views, PreviewCallbacks callbacks = {});
    void bindMonitorProfile();
    void startGeneration(bool clear_previews = true);
    void viewportChanged();
    void selectedEntryChanged(const QModelIndex &index);
    // Refresh only the current entry's modified-time label, without resetting or rendering.
    void refreshSelectedMetadata();
    void refreshSelectedPreview();
    void ensureSelectedPreview(bool extended_limits = false);
    void clearSelection();
    void resetSelectedPreview(bool clear_image = true);
    void schedule(int delay_ms = -1);
    void scheduleIfIdle();
    // Block new dispatch until resume(), without cancelling or advancing generations.
    void suspendDispatch();
    void dispatchVisiblePreviews();

    void prepareCatalogNavigation();
    void catalogOpened();
    void catalogRefreshed();
    void catalogProgress(const catalog::CatalogSessionUpdate &update,
                         catalog::CatalogSessionState previous_state, bool automatic,
                         bool model_changed);
    void updatePreviewContext(PreviewContext context);
    void enterOfflineState(bool refresh_catalog = true);
    void enterBlockedState();
    void leaveProtectedState();
    [[nodiscard]] PreviewRuntimeState runtimeState() const noexcept;
    [[nodiscard]] PreviewCatalogState takeCatalogState();
    // Cancel the old generation before restoring the matching model snapshot, then restore
    // authority.
    void restoreCatalogState(PreviewCatalogState state);

    // Invalidate pending work and stop the preview helper before a file mutation.
    void pauseReaders();
    void resume(int delay_ms = -1);
    void setWindowAvailable(bool available);
    [[nodiscard]] bool readersIdle() const noexcept;
    void stop();

    void setThumbnailExtent(int extent);
    void requestSelectedPage(std::uint32_t page_index);
    void promptForDocumentPassword();
    [[nodiscard]] bool canForcePreview(const core::DirectoryEntry &entry) const;
    void forcePreview(qulonglong entry_id);
    void cancelForcedPreview();
    void stopSelectedAnimation();
    void cancelFolderPreviews();
    void markPreviewUnverified(const QString &path);
    void markAllPreviewsUnverified();
    void invalidateDisplayImages();
    void retranslate();
    // Completion follows component detection and any preview retry handling.
    void refreshExternalComponents(std::function<void()> completion = {});
    void setCacheMaximumBytes(std::uint64_t bytes);
    void cacheCleared();
    [[nodiscard]] preview::helper_protocol::RequestId
    inspectCache(PreviewClient::CacheHandler handler);
    [[nodiscard]] preview::helper_protocol::RequestId
    clearCache(PreviewClient::CacheHandler handler);

    [[nodiscard]] qulonglong selectedEntryId() const noexcept {
        return selectedEntryId_;
    }
    [[nodiscard]] const QString &selectedName() const noexcept {
        return selectedName_;
    }
    [[nodiscard]] const QString &selectedColorSummary() const noexcept {
        return selectedColorSummary_;
    }
    [[nodiscard]] bool forcedPreviewActive() const noexcept {
        return selectedPreviewExtended_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return previewGeneration_;
    }
    [[nodiscard]] std::uint64_t viewportGeneration() const noexcept {
        return viewportGeneration_;
    }
    [[nodiscard]] bool offlineCacheAllowed(const core::DirectoryEntry &entry,
                                           std::uint32_t page_index = 0) const noexcept;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
#ifdef VOVE_UI_TEST_HOOKS
    friend struct PreviewControllerTestAccess;
#endif
    [[nodiscard]] PreviewCatalogMode catalogMode() const;
    [[nodiscard]] bool catalogWideProtection() const;
    void notifyStatus();
    void publishRuntimeState();
    void monitor_profile_changed();
    void handle_preview_reply(PreviewReply reply);
    void handle_folder_mosaic(FolderMosaicReply reply);
    [[nodiscard]] bool request_preview(const QModelIndex &index, std::uint16_t edge,
                                       preview::ThumbnailPriority priority);
    void update_selected_preview(const QModelIndex &index);
    void handle_selected_page_reply(PreviewReply reply);
    void update_page_navigation(std::uint32_t page_index, std::uint32_t page_count);
    void update_document_password_action(preview::helper_protocol::ResponseStatus status);
    void update_preview_support_label(bool preview_ready);
    void retry_external_component_previews();
    [[nodiscard]] QString
    selected_preview_status_text(preview::helper_protocol::ResponseStatus status) const;
    [[nodiscard]] std::uint16_t selected_preview_edge() const;
    [[nodiscard]] static QString animation_source_key(const core::DirectoryEntry &entry);
    [[nodiscard]] static bool same_source(const PreviewEdgeState &state,
                                          const core::DirectoryEntry &entry) noexcept;
    [[nodiscard]] static bool same_source(const PreviewReply &reply,
                                          const core::DirectoryEntry &entry) noexcept;
    [[nodiscard]] std::uint16_t
    current_preview_edge(const QHash<qulonglong, PreviewEdgeState> &states,
                         const core::DirectoryEntry &entry) const noexcept;
    [[nodiscard]] bool offline_cache_only() const noexcept;
    [[nodiscard]] bool preview_failure_can_disconnect(const core::DirectoryEntry &entry) const;

    PreviewViews views_;
    PreviewCallbacks callbacks_;
    PreviewContext context_;
    PreviewClient previewClient_;
    GifAnimationClient gifAnimationClient_;
    FolderMosaicController folderMosaicController_;
    QTimer *previewTimer_{};
    QString selectedName_;
    QString selectedColorSummary_;
    qulonglong selectedCatalogEntryId_{};
    qulonglong selectedEntryId_{};
    std::uint32_t selectedPageIndex_{};
    std::uint32_t selectedPageCount_{};
    std::optional<PreviewReply> selectedPreview_;
    QString selectedAnimationKey_;
    QImage selectedAnimationImage_;
    bool selectedAnimationFailed_{};
    bool selectedAnimationAnimated_{};
    preview::helper_protocol::RequestId selectedPreviewRequest_{};
    std::uint16_t selectedPreviewPendingEdge_{};
    bool selectedPreviewExtended_{};
    std::uint64_t selectedPreviewGeneration_{1ULL << 62U};
    std::uint64_t previewGeneration_{1};
    std::uint64_t viewportGeneration_{1};
    QHash<qulonglong, PreviewEdgeState> pendingPreviewEdges_;
    QHash<qulonglong, PreviewEdgeState> completedPreviewEdges_;
    QHash<qulonglong, PreviewEdgeState> offlineEligiblePreviews_;
    QHash<qulonglong, QSet<std::uint32_t>> offlineEligiblePages_;
    QHash<qulonglong, PreviewEdgeState> strictRevalidatedPreviews_;
    QHash<qulonglong, QSet<std::uint32_t>> strictRevalidatedPages_;
    QHash<QString, QString> documentPasswords_;
    bool monitorProfileBound_{};
    bool previewRevalidationPending_{};
    bool restrictOfflineFallback_{};
    bool suspended_{};
    bool windowAvailable_{true};
    std::uint16_t thumbnailExtent_{160};
    PreviewRuntimeState previewRuntimeState_{PreviewRuntimeState::online};
};

} // namespace vove::ui
