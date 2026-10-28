#include "preview_controller.hpp"

#include "directory_list_model.hpp"
#include "external_component_detection.hpp"
#include "format_style.hpp"
#include "page_button.hpp"
#include "preview_modified_label.hpp"
#include "preview_name_label.hpp"
#include "worker_startup_text.hpp"
#include "zoom_preview.hpp"
#include "vove/color/color_transform.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vove::ui {
namespace {

#ifdef Q_OS_WIN
bool is_unc_path(const QString &path) {
    return QDir::fromNativeSeparators(path).startsWith(QStringLiteral("//"));
}
#endif

QString preview_status_text(const preview::helper_protocol::ResponseStatus status) {
    using Status = preview::helper_protocol::ResponseStatus;
    switch (status) {
    case Status::unsupported:
        return QCoreApplication::translate("MainWindow", "This format is not supported yet");
    case Status::malformed:
        return QCoreApplication::translate("MainWindow", "The file is damaged");
    case Status::disconnected:
    case Status::source_unavailable:
    case Status::source_changed:
    case Status::not_found:
        return QCoreApplication::translate("MainWindow", "The file is currently unavailable");
    case Status::authentication_failed:
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow", "Cannot access the file");
    case Status::too_large:
        return QCoreApplication::translate("MainWindow", "Preview exceeds image processing limits");
    case Status::pdf_input_too_large:
        return QCoreApplication::translate("MainWindow",
                                           "PDF file exceeds the 2 GiB viewing limit");
    case Status::memory_limit:
        return QCoreApplication::translate("MainWindow", "Preview memory limit exceeded");
    case Status::timed_out:
    case Status::processing_timed_out:
        return QCoreApplication::translate("MainWindow", "Preview processing time limit exceeded");
    case Status::cancelled:
        return QCoreApplication::translate("MainWindow", "Preview cancelled");
    case Status::queue_busy:
        return QCoreApplication::translate("MainWindow", "Preview is waiting for the queue");
    case Status::color_profile_required:
        return QCoreApplication::translate("MainWindow",
                                           "CMYK: no color profile is specified in the file");
    case Status::password_required:
        return QCoreApplication::translate("MainWindow", "The document is password-protected");
    case Status::document_password_incorrect:
        return QCoreApplication::translate("MainWindow", "Incorrect password");
    case Status::ghostscript_required:
        return ghostscript_available(false)
                   ? QCoreApplication::translate(
                         "MainWindow",
                         "Ghostscript is installed, but it could not be started for this preview")
                   : QCoreApplication::translate(
                         "MainWindow",
                         "Ghostscript is not installed\n\nIt is required to preview PS, EPS, "
                         "and legacy AI files without an embedded thumbnail");
    case Status::embedded_preview_unavailable:
        return QCoreApplication::translate(
            "MainWindow",
            "Embedded preview is unavailable\n\nThe file does not contain a recognized embedded "
            "thumbnail");
    case Status::internal_error:
        return QCoreApplication::translate("MainWindow", "Failed to generate preview");
    case Status::worker_start_failed:
        return worker_startup_message();
    case Status::success_cached:
    case Status::success_offline_cached:
    case Status::success_decoded:
        return QCoreApplication::translate("MainWindow", "Preparing preview…");
    }
    return QCoreApplication::translate("MainWindow", "Failed to generate preview");
}

QString localized_color_model(QString model) {
    if (model == QStringLiteral("MIXED")) {
        return QCoreApplication::translate("MainWindow", "Mixed");
    }
    if (model == QStringLiteral("LAB")) {
        return QStringLiteral("Lab");
    }
    return model;
}

QString color_summary(QString model, const QString &profile) {
    if (model == QStringLiteral("CMYK") &&
        profile == QLatin1StringView(color::kUnprofiledCmykApproximation)) {
        return QCoreApplication::translate("MainWindow",
                                           "CMYK without profile · approximate colors");
    }
    model = localized_color_model(std::move(model));
    if (profile.isEmpty()) {
        return model;
    }
    return model.isEmpty() ? profile : model + QStringLiteral(": ") + profile;
}

} // namespace

PreviewController::PreviewController(QObject *parent, QString helper_program_override)
    : QObject(parent), previewClient_(this, 12'000, helper_program_override),
      gifAnimationClient_(this, std::move(helper_program_override)),
      folderMosaicController_(previewClient_, this) {
    previewTimer_ = new QTimer(this);
    previewTimer_->setObjectName(QStringLiteral("previewDispatchTimer"));
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(40);
    connect(previewTimer_, &QTimer::timeout, this, &PreviewController::dispatchVisiblePreviews);
    previewClient_.set_reply_handler(
        [this](PreviewReply reply) { handle_preview_reply(std::move(reply)); });
    folderMosaicController_.set_reply_handler(
        [this](FolderMosaicReply reply) { handle_folder_mosaic(std::move(reply)); });
}

PreviewController::~PreviewController() {
    if (views_.canvas != nullptr) {
        views_.canvas->animation_toggle = {};
    }
    gifAnimationClient_.frame_ready = {};
    gifAnimationClient_.finished = {};
    folderMosaicController_.set_reply_handler({});
    previewClient_.set_reply_handler({});
    stop();
}

void PreviewController::attachViews(PreviewViews views, PreviewCallbacks callbacks) {
    Q_ASSERT(views_.model == nullptr);
    Q_ASSERT(views.window && views.model && views.catalog && views.canvas && views.name &&
             views.modified && views.format && views.support && views.page_navigation &&
             views.previous_page && views.next_page && views.password && views.page_indicator &&
             views.cancel_attempt);
    views_ = views;
    callbacks_ = std::move(callbacks);
    windowAvailable_ = views_.window->isVisible() && !views_.window->isMinimized();
    views_.window->installEventFilter(this);
    views_.canvas->installEventFilter(this);
    views_.modified->installEventFilter(this);
    views_.canvas->set_indicator_font(views_.modified->font());
    connect(
        views_.catalog->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex &current, const QModelIndex &) { selectedEntryChanged(current); });
    connect(views_.catalog->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { scheduleIfIdle(); });
    connect(views_.model, &QAbstractItemModel::rowsInserted, this, [this] { scheduleIfIdle(); });
    connect(views_.model, &QAbstractItemModel::modelReset, this, [this] {
        views_.modified->set_modified_time(std::nullopt);
        scheduleIfIdle();
    });
    connect(views_.model, &QAbstractItemModel::dataChanged, this, [this] { scheduleIfIdle(); });
    connect(views_.model, &QAbstractItemModel::layoutChanged, this, [this] { scheduleIfIdle(); });
    connect(views_.previous_page, &QToolButton::clicked, this, [this] {
        if (selectedPageIndex_ > 0) {
            requestSelectedPage(selectedPageIndex_ - 1U);
        }
    });
    connect(views_.next_page, &QToolButton::clicked, this, [this] {
        if (selectedPageIndex_ + 1U < selectedPageCount_) {
            requestSelectedPage(selectedPageIndex_ + 1U);
        }
    });
    connect(views_.password, &QToolButton::clicked, this,
            &PreviewController::promptForDocumentPassword);
    connect(views_.cancel_attempt, &QAction::triggered, this,
            &PreviewController::cancelForcedPreview);
    gifAnimationClient_.frame_ready = [this](QImage image, const QString &key, bool animated) {
        if (key != selectedAnimationKey_ || selectedEntryId_ == 0)
            return;
        const bool first_frame = selectedAnimationImage_.isNull();
        selectedAnimationImage_ = std::move(image);
        selectedAnimationAnimated_ = animated;
        views_.canvas->set_animation_frame(selectedAnimationImage_, key);
        views_.canvas->set_animation_state(!animated ? ZoomPreview::AnimationState::hidden
                                           : gifAnimationClient_.paused()
                                               ? ZoomPreview::AnimationState::paused
                                               : ZoomPreview::AnimationState::playing);
        if (first_frame) {
            update_page_navigation(0, 1);
            update_preview_support_label(true);
            selectedColorSummary_ = QStringLiteral("RGB");
            notifyStatus();
        }
    };
    gifAnimationClient_.finished = [this](bool success, const QString &detail) {
        if (selectedAnimationKey_.isEmpty())
            return;
        if (success) {
            views_.canvas->set_animation_state(selectedAnimationAnimated_
                                                   ? ZoomPreview::AnimationState::finished
                                                   : ZoomPreview::AnimationState::hidden);
        } else {
            selectedAnimationFailed_ = true;
            views_.canvas->set_animation_state(ZoomPreview::AnimationState::hidden);
            views_.support->setText(
                QCoreApplication::translate("MainWindow", "Animation unavailable"));
            views_.support->setToolTip(detail);
            views_.support->show();
            if (selectedAnimationImage_.isNull())
                ensureSelectedPreview();
        }
    };
    views_.canvas->animation_toggle = [this] {
        if (gifAnimationClient_.active()) {
            gifAnimationClient_.set_paused(!gifAnimationClient_.paused());
            views_.canvas->set_animation_state(gifAnimationClient_.paused()
                                                   ? ZoomPreview::AnimationState::paused
                                                   : ZoomPreview::AnimationState::playing);
        } else if (!selectedAnimationKey_.isEmpty()) {
            stopSelectedAnimation();
            ensureSelectedPreview();
        }
    };

    publishRuntimeState();
    refreshExternalComponents();
    QTimer::singleShot(0, this, [this] { bindMonitorProfile(); });
}

bool PreviewController::eventFilter(QObject *watched, QEvent *event) {
    if (event == nullptr) {
        return false;
    }
    if (watched == views_.window &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowActivate)) {
        QTimer::singleShot(0, this, [this] {
            views_.modified->refresh();
            setWindowAvailable(views_.window->isVisible() && !views_.window->isMinimized());
            bindMonitorProfile();
        });
    }
    if (watched == views_.modified &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
         event->type() == QEvent::Polish)) {
        QTimer::singleShot(0, this,
                           [this] { views_.canvas->set_indicator_font(views_.modified->font()); });
    }
    if (watched == views_.canvas &&
        (event->type() == QEvent::Resize || event->type() == QEvent::DevicePixelRatioChange)) {
        if (selectedEntryId_ != 0) {
            refreshSelectedPreview();
        }
        schedule();
    }
    return false;
}

PreviewCatalogMode PreviewController::catalogMode() const {
    return context_.catalog_mode;
}

bool PreviewController::catalogWideProtection() const {
    return catalogMode() != PreviewCatalogMode::global_search &&
           catalogMode() != PreviewCatalogMode::recursive;
}

void PreviewController::notifyStatus() {
    if (callbacks_.status_changed) {
        callbacks_.status_changed();
    }
}

void PreviewController::publishRuntimeState() {
    const auto value =
        previewRuntimeState_ == PreviewRuntimeState::offline   ? QStringLiteral("offline")
        : previewRuntimeState_ == PreviewRuntimeState::blocked ? QStringLiteral("blocked")
                                                               : QStringLiteral("online");
    setProperty("previewRuntimeState", value);
    if (views_.window != nullptr) {
        views_.window->setProperty("previewRuntimeState", value);
    }
}

void PreviewController::updatePreviewContext(PreviewContext context) {
    context_ = context;
}

void PreviewController::schedule(const int delay_ms) {
    if (!suspended_) {
        if (delay_ms < 0) {
            previewTimer_->start();
        } else {
            previewTimer_->start(delay_ms);
        }
    }
}

void PreviewController::scheduleIfIdle() {
    if (!previewTimer_->isActive()) {
        schedule();
    }
}

void PreviewController::suspendDispatch() {
    suspended_ = true;
    previewTimer_->stop();
}

void PreviewController::refreshSelectedMetadata() {
    const auto *entry = views_.model->entry_at(views_.catalog->currentIndex());
    views_.modified->set_modified_time(entry != nullptr && entry->kind == core::EntryKind::file
                                           ? std::optional{entry->modified_unix_ns}
                                           : std::nullopt);
}

void PreviewController::refreshSelectedPreview() {
    update_selected_preview(views_.catalog->currentIndex());
}

void PreviewController::clearSelection() {
    selectedEntryChanged({});
}

void PreviewController::prepareCatalogNavigation() {
    const auto was_protected = previewRuntimeState_ != PreviewRuntimeState::online;
    restrictOfflineFallback_ = restrictOfflineFallback_ || was_protected;
    if (was_protected) {
        strictRevalidatedPreviews_.clear();
        strictRevalidatedPages_.clear();
    }
    leaveProtectedState();
}

void PreviewController::catalogOpened() {
    prepareCatalogNavigation();
    startGeneration();
    clearSelection();
}

void PreviewController::catalogRefreshed() {
    prepareCatalogNavigation();
    startGeneration(false);
    views_.model->mark_previews_verification_pending();
    if (views_.catalog->currentIndex().isValid()) {
        refreshSelectedPreview();
    }
}

void PreviewController::catalogProgress(const catalog::CatalogSessionUpdate &update,
                                        const catalog::CatalogSessionState previous_state,
                                        const bool automatic, const bool model_changed) {
    if (model_changed) {
        const auto current = views_.catalog->currentIndex();
        const auto *entry = views_.model->entry_at(current);
        views_.modified->set_modified_time(entry != nullptr && entry->kind == core::EntryKind::file
                                               ? std::optional{entry->modified_unix_ns}
                                               : std::nullopt);
        if (automatic && (entry == nullptr || entry->id != selectedCatalogEntryId_ ||
                          (selectedPreview_ && !same_source(*selectedPreview_, *entry)) ||
                          (!selectedAnimationKey_.isEmpty() &&
                           selectedAnimationKey_ != animation_source_key(*entry)))) {
            selectedEntryChanged(current);
        }
        if (update.state == catalog::CatalogSessionState::ready) {
            schedule();
        }
        if (catalogMode() == PreviewCatalogMode::recursive) {
            scheduleIfIdle();
        }
    }
    using State = catalog::CatalogSessionState;
    if (update.state == State::network_disconnected || update.state == State::timed_out ||
        update.state == State::waiting_retry) {
        const auto entered_offline = previous_state != State::network_disconnected &&
                                     previous_state != State::timed_out &&
                                     previous_state != State::waiting_retry;
        if (entered_offline || previewRuntimeState_ != PreviewRuntimeState::offline) {
            enterOfflineState(false);
        }
    } else if (update.state == State::ready &&
               (previewRevalidationPending_ ||
                previewRuntimeState_ == PreviewRuntimeState::blocked)) {
        previewRevalidationPending_ = false;
        leaveProtectedState();
        startGeneration(false);
        views_.model->mark_previews_verification_pending();
        if (views_.catalog->currentIndex().isValid()) {
            refreshSelectedPreview();
        }
        schedule();
    } else if (update.state == State::ready) {
        previewRuntimeState_ = PreviewRuntimeState::online;
        folderMosaicController_.set_offline(false);
        folderMosaicController_.set_allow_offline_fallback(!restrictOfflineFallback_);
    } else if (update.state == State::authentication_required ||
               update.state == State::permission_denied || update.state == State::unavailable) {
        enterBlockedState();
    }
}

PreviewRuntimeState PreviewController::runtimeState() const noexcept {
    return previewRuntimeState_;
}

PreviewCatalogState PreviewController::takeCatalogState() {
    return {.offline_previews = std::exchange(offlineEligiblePreviews_, {}),
            .offline_pages = std::exchange(offlineEligiblePages_, {}),
            .revalidated_previews = std::exchange(strictRevalidatedPreviews_, {}),
            .revalidated_pages = std::exchange(strictRevalidatedPages_, {}),
            .runtime = previewRuntimeState_,
            .restrict_offline_fallback = restrictOfflineFallback_,
            .revalidation_pending = previewRevalidationPending_};
}

void PreviewController::restoreCatalogState(PreviewCatalogState state) {
    offlineEligiblePreviews_ = std::move(state.offline_previews);
    offlineEligiblePages_ = std::move(state.offline_pages);
    strictRevalidatedPreviews_ = std::move(state.revalidated_previews);
    strictRevalidatedPages_ = std::move(state.revalidated_pages);
    previewRuntimeState_ = state.runtime;
    restrictOfflineFallback_ = state.restrict_offline_fallback;
    previewRevalidationPending_ = state.revalidation_pending;
    publishRuntimeState();
    const auto protected_preview = previewRuntimeState_ != PreviewRuntimeState::online;
    folderMosaicController_.set_offline(protected_preview);
    folderMosaicController_.set_allow_offline_fallback(!protected_preview &&
                                                       !restrictOfflineFallback_);
    selectedName_.clear();
    selectedColorSummary_.clear();
    selectedCatalogEntryId_ = 0;
    selectedEntryId_ = 0;
    views_.canvas->setPixmap({});
    views_.canvas->setText(QCoreApplication::translate("MainWindow", "Select a file"));
}

void PreviewController::pauseReaders() {
    suspendDispatch();
    startGeneration(false);
    previewClient_.stop();
}

void PreviewController::resume(const int delay_ms) {
    suspended_ = false;
    schedule(delay_ms);
}

void PreviewController::setWindowAvailable(const bool available) {
    windowAvailable_ = available;
    gifAnimationClient_.set_suspended(!available);
}

bool PreviewController::readersIdle() const noexcept {
    return folderMosaicController_.readers_idle() && previewClient_.readers_idle() &&
           gifAnimationClient_.readers_idle();
}

void PreviewController::stop() {
    previewTimer_->stop();
    folderMosaicController_.clear_pending();
    gifAnimationClient_.stop();
    previewClient_.stop();
}

void PreviewController::setThumbnailExtent(const int extent) {
    thumbnailExtent_ = static_cast<std::uint16_t>(std::clamp(extent, 1, 2048));
}

void PreviewController::cancelFolderPreviews() {
    folderMosaicController_.clear_pending();
}

void PreviewController::markPreviewUnverified(const QString &path) {
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (int row = 0; row < views_.model->rowCount(); ++row) {
        const auto *entry = views_.model->entry_at(views_.model->index(row, 0));
        if (entry != nullptr && QDir::cleanPath(QDir::fromNativeSeparators(
                                    QString::fromUtf8(entry->path_utf8))) == normalized) {
            views_.model->mark_preview_verification_pending(static_cast<qulonglong>(entry->id));
            return;
        }
    }
}

void PreviewController::markAllPreviewsUnverified() {
    views_.model->mark_previews_verification_pending();
}

void PreviewController::invalidateDisplayImages() {
    monitor_profile_changed();
}

void PreviewController::refreshExternalComponents(std::function<void()> completion) {
    previewClient_.refresh_external_components(
        [this, completion = std::move(completion)](const bool changed) {
            if (changed && views_.model != nullptr) {
                retry_external_component_previews();
            }
            if (completion) {
                completion();
            }
        });
}

void PreviewController::setCacheMaximumBytes(const std::uint64_t bytes) {
    previewClient_.set_cache_maximum_bytes(bytes);
}

void PreviewController::cacheCleared() {
    views_.model->clear_previews();
    completedPreviewEdges_.clear();
    folderMosaicController_.invalidate_display_images();
    startGeneration(true);
}

preview::helper_protocol::RequestId
PreviewController::inspectCache(PreviewClient::CacheHandler handler) {
    return previewClient_.inspect_cache(std::move(handler));
}

preview::helper_protocol::RequestId
PreviewController::clearCache(PreviewClient::CacheHandler handler) {
    return previewClient_.clear_cache(std::move(handler));
}

void PreviewController::retranslate() {
    const auto current = views_.catalog->currentIndex();
    const auto *entry = views_.model->entry_at(current);
    if (entry == nullptr) {
        views_.canvas->setText(QCoreApplication::translate("MainWindow", "Select a file"));
    } else if (entry->kind == core::EntryKind::directory) {
        views_.canvas->setText(QCoreApplication::translate(
            "MainWindow", "Folder\n\nA mosaic will appear after thumbnails are connected"));
        update_preview_support_label(false);
    } else {
        selectedColorSummary_ =
            color_summary(current.data(DirectoryListModel::SourceColorModelRole).toString(),
                          current.data(DirectoryListModel::SourceColorProfileRole).toString());
        if (selectedPageIndex_ == 0) {
            update_selected_preview(current);
        } else {
            const auto ready = !views_.canvas->pixmap().isNull();
            update_preview_support_label(ready);
            if (!ready) {
                requestSelectedPage(selectedPageIndex_);
            }
        }
    }
}

void PreviewController::bindMonitorProfile() {
    if (views_.window == nullptr) {
        return;
    }
    auto *active_screen = views_.window->windowHandle() != nullptr
                              ? views_.window->windowHandle()->screen()
                              : views_.window->screen();
    if (previewClient_.set_monitor_name(active_screen != nullptr ? active_screen->name()
                                                                 : QString{})) {
        monitor_profile_changed();
    }
    if (monitorProfileBound_ || views_.window->windowHandle() == nullptr) {
        return;
    }
    monitorProfileBound_ = true;
    connect(views_.window->windowHandle(), &QWindow::screenChanged, this, [this](QScreen *changed) {
        if (previewClient_.set_monitor_name(changed != nullptr ? changed->name() : QString{})) {
            monitor_profile_changed();
        }
    });
}

void PreviewController::monitor_profile_changed() {
    resetSelectedPreview();
    views_.model->clear_preview_images();
    completedPreviewEdges_.clear();
    folderMosaicController_.invalidate_display_images();
    if (views_.catalog != nullptr && views_.catalog->currentIndex().isValid()) {
        if (selectedPageIndex_ > 0 && selectedPageIndex_ < selectedPageCount_) {
            requestSelectedPage(selectedPageIndex_);
        } else {
            update_selected_preview(views_.catalog->currentIndex());
            ensureSelectedPreview();
        }
    }
    if (previewTimer_ != nullptr) {
        previewTimer_->start();
    }
}

void PreviewController::startGeneration(const bool clear_previews) {
    resetSelectedPreview(clear_previews);
    const auto previous_preview = previewGeneration_;
    const auto previous_viewport = viewportGeneration_;
    ++previewGeneration_;
    ++viewportGeneration_;
    if (previewGeneration_ == 0) {
        previewGeneration_ = 1;
    }
    if (viewportGeneration_ == 0) {
        viewportGeneration_ = 1;
    }
    previewClient_.cancel_generation(previous_viewport);
    if (previous_preview != previous_viewport) {
        previewClient_.cancel_generation(previous_preview);
    }
    folderMosaicController_.reset(previewGeneration_);
    pendingPreviewEdges_.clear();
    completedPreviewEdges_.clear();
    if (clear_previews) {
        offlineEligiblePreviews_.clear();
        offlineEligiblePages_.clear();
        views_.model->clear_previews();
    }
}

bool PreviewController::offline_cache_only() const noexcept {
    return previewRuntimeState_ == PreviewRuntimeState::offline;
}

bool PreviewController::preview_failure_can_disconnect(const core::DirectoryEntry &entry) const {
#ifdef VOVE_UI_TEST_HOOKS
    if (views_.window->property("testRemotePreviewSource").toBool()) {
        return true;
    }
#endif
#ifdef Q_OS_WIN
    const auto path = QDir::toNativeSeparators(QString::fromUtf8(entry.path_utf8));
    if (!is_unc_path(path) && path.size() >= 3 && path.at(1) == QLatin1Char(':')) {
        const auto root = path.left(3).toStdWString();
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
            return false;
        }
    }
#else
    Q_UNUSED(entry);
#endif
    return true;
}

void PreviewController::enterOfflineState(const bool refresh_catalog) {
    if (previewRuntimeState_ == PreviewRuntimeState::offline) {
        return;
    }
    startGeneration(false);
    previewRuntimeState_ = PreviewRuntimeState::offline;
    restrictOfflineFallback_ = true;
    strictRevalidatedPreviews_.clear();
    strictRevalidatedPages_.clear();
    publishRuntimeState();
    folderMosaicController_.set_offline(true);
    folderMosaicController_.set_allow_offline_fallback(false);
    previewRevalidationPending_ = true;
    views_.model->mark_previews_offline();
    if (views_.catalog->currentIndex().isValid()) {
        update_selected_preview(views_.catalog->currentIndex());
    }
    if (refresh_catalog && catalogWideProtection()) {
        if (callbacks_.request_source_revalidation) {
            callbacks_.request_source_revalidation();
        }
    }
    previewTimer_->start();
}

void PreviewController::enterBlockedState() {
    if (previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    startGeneration(false);
    previewRuntimeState_ = PreviewRuntimeState::blocked;
    publishRuntimeState();
    folderMosaicController_.set_offline(true);
    folderMosaicController_.set_allow_offline_fallback(false);
    previewRevalidationPending_ = false;
    restrictOfflineFallback_ = true;
    strictRevalidatedPreviews_.clear();
    strictRevalidatedPages_.clear();
    offlineEligiblePreviews_.clear();
    offlineEligiblePages_.clear();
    views_.model->clear_previews();
    selectedPreview_.reset();
    views_.canvas->clear();
    views_.password->hide();
    update_page_navigation(0, 0);
    update_preview_support_label(false);
}

void PreviewController::leaveProtectedState() {
    previewRuntimeState_ = PreviewRuntimeState::online;
    publishRuntimeState();
    folderMosaicController_.set_offline(false);
    folderMosaicController_.set_allow_offline_fallback(!restrictOfflineFallback_);
}

bool PreviewController::offlineCacheAllowed(const core::DirectoryEntry &entry,
                                            const std::uint32_t page_index) const noexcept {
    const auto found = offlineEligiblePreviews_.constFind(static_cast<qulonglong>(entry.id));
    const auto pages = offlineEligiblePages_.constFind(static_cast<qulonglong>(entry.id));
    return found != offlineEligiblePreviews_.cend() && same_source(*found, entry) &&
           pages != offlineEligiblePages_.cend() && pages->contains(page_index);
}

void PreviewController::viewportChanged() {
    const auto previous = viewportGeneration_;
    ++viewportGeneration_;
    if (viewportGeneration_ == 0) {
        viewportGeneration_ = 1;
    }
    previewClient_.cancel_generation(previous);
    folderMosaicController_.clear_pending();
    pendingPreviewEdges_.clear();
    previewTimer_->start();
}

bool PreviewController::same_source(const PreviewEdgeState &state,
                                    const core::DirectoryEntry &entry) noexcept {
    return state.source_size == entry.size_bytes &&
           state.modified_unix_ns == entry.modified_unix_ns &&
           state.source_revision == QString::fromUtf8(entry.source_revision_utf8);
}

bool PreviewController::same_source(const PreviewReply &reply,
                                    const core::DirectoryEntry &entry) noexcept {
    return reply.source_size == entry.size_bytes &&
           reply.modified_unix_ns == entry.modified_unix_ns &&
           reply.source_revision == QString::fromUtf8(entry.source_revision_utf8);
}

std::uint16_t
PreviewController::current_preview_edge(const QHash<qulonglong, PreviewEdgeState> &states,
                                        const core::DirectoryEntry &entry) const noexcept {
    const auto found = states.constFind(static_cast<qulonglong>(entry.id));
    return found != states.cend() && same_source(*found, entry) ? found->edge : 0;
}

bool PreviewController::request_preview(const QModelIndex &index, const std::uint16_t edge,
                                        const preview::ThumbnailPriority priority) {
    if (suspended_ || previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return false;
    }
    const auto *entry = views_.model->entry_at(index);
    if (entry == nullptr || entry->kind != core::EntryKind::file) {
        return false;
    }
    const auto id = static_cast<qulonglong>(entry->id);
    const auto known_offline = offline_cache_only();
    if (known_offline && !offlineCacheAllowed(*entry)) {
        return false;
    }
    if (current_preview_edge(pendingPreviewEdges_, *entry) >= edge ||
        current_preview_edge(completedPreviewEdges_, *entry) >= edge) {
        return false;
    }
    const auto request_id = previewClient_.request_preview(
        id, viewportGeneration_, QString::fromUtf8(entry->path_utf8), entry->size_bytes,
        entry->modified_unix_ns, QString::fromUtf8(entry->source_revision_utf8), edge, priority, {},
        0, documentPasswords_.value(QString::fromUtf8(entry->path_utf8)), known_offline,
        !restrictOfflineFallback_ || current_preview_edge(strictRevalidatedPreviews_, *entry) != 0);
    if (request_id != 0) {
        pendingPreviewEdges_.insert(
            id, {.source_size = entry->size_bytes,
                 .modified_unix_ns = entry->modified_unix_ns,
                 .source_revision = QString::fromUtf8(entry->source_revision_utf8),
                 .edge = std::max(current_preview_edge(pendingPreviewEdges_, *entry), edge)});
        return true;
    }
    return false;
}

void PreviewController::dispatchVisiblePreviews() {
    if (suspended_) {
        return;
    }
    // Streamed batches show a transient viewport before Back/Up restores its saved anchor.
    if ((context_.catalog_mode != PreviewCatalogMode::recursive) &&
        context_.navigation_restore_pending && views_.model->loading()) {
        return;
    }
    ensureSelectedPreview();
    if (views_.model->rowCount() == 0 || !views_.catalog->isVisible()) {
        return;
    }
    const auto viewport = views_.catalog->viewport()->rect().adjusted(-32, -32, 32, 32);
    // Locate the first visible row even when the top-left point falls in tile spacing.
    int first{};
    int end = views_.model->rowCount();
    while (first < end) {
        const int middle = first + (end - first) / 2;
        if (views_.catalog->visualRect(views_.model->index(middle, 0)).bottom() < viewport.top()) {
            first = middle + 1;
        } else {
            end = middle;
        }
    }
    const auto last = std::min(views_.model->rowCount() - 1, first + 127);
    std::size_t requested{};
    QList<QModelIndex> visible_folders;
    QList<QModelIndex> visible_files;
    int first_visible{-1};
    int last_visible{-1};
    for (auto row = first; row <= last; ++row) {
        const auto index = views_.model->index(row, 0);
        if (!views_.catalog->visualRect(index).intersects(viewport)) {
            continue;
        }
        first_visible = first_visible < 0 ? row : first_visible;
        last_visible = row;
        const auto *entry = views_.model->entry_at(index);
        if (entry == nullptr) {
            continue;
        }
        if (entry->kind == core::EntryKind::directory) {
            visible_folders.push_back(index);
            continue;
        }
        visible_files.push_back(index);
    }
    const auto edge = static_cast<std::uint16_t>(thumbnailExtent_);
    QSet<qulonglong> retained_entries;
    if (first_visible >= 0) {
        for (int row = std::max(0, first_visible - 24);
             row <= std::min(views_.model->rowCount() - 1, last_visible + 48); ++row) {
            if (const auto *entry = views_.model->entry_at(views_.model->index(row, 0))) {
                retained_entries.insert(static_cast<qulonglong>(entry->id));
            }
        }
    }
    previewClient_.retain_generation_entries(viewportGeneration_, retained_entries);
    for (const auto &index : visible_files) {
        if (requested >= 96) {
            break;
        }
        if (request_preview(index, edge, preview::ThumbnailPriority::visible)) {
            ++requested;
        }
    }
    if (offline_cache_only()) {
        folderMosaicController_.clear_pending();
    } else {
        folderMosaicController_.discard_queued();
        for (const auto &index : visible_folders) {
            const auto *entry = views_.model->entry_at(index);
            if (entry != nullptr) {
                folderMosaicController_.enqueue(
                    static_cast<qulonglong>(entry->id), QString::fromUtf8(entry->path_utf8),
                    entry->size_bytes, entry->modified_unix_ns,
                    QString::fromUtf8(entry->source_revision_utf8), edge);
            }
        }
    }
    // Speculative files must not age ahead of the visible folder mosaics in the shared queue.
    if (views_.model->loading() || !folderMosaicController_.idle()) {
        return;
    }
    const auto request_background_row =
        [this, edge, &requested](const int row, const preview::ThumbnailPriority priority,
                                 const std::size_t limit) {
            if (row < 0 || row >= views_.model->rowCount() || requested >= limit) {
                return;
            }
            if (request_preview(views_.model->index(row, 0), edge, priority)) {
                ++requested;
            }
        };
    if (first_visible >= 0 && last_visible >= first_visible) {
        const auto visible_requested = requested;
        for (int distance = 1; distance <= 24 && requested < visible_requested + 32; ++distance) {
            request_background_row(first_visible - distance, preview::ThumbnailPriority::nearby,
                                   visible_requested + 32);
            request_background_row(last_visible + distance, preview::ThumbnailPriority::nearby,
                                   visible_requested + 32);
        }
        const auto nearby_requested = requested;
        for (int distance = 25; distance <= 48 && requested < nearby_requested + 16; ++distance) {
            request_background_row(last_visible + distance, preview::ThumbnailPriority::rest,
                                   nearby_requested + 16);
        }
    }
}

void PreviewController::handle_preview_reply(PreviewReply reply) {
    if (reply.generation != viewportGeneration_) {
        return;
    }
    const auto pending = pendingPreviewEdges_.constFind(reply.entry_id);
    if (pending != pendingPreviewEdges_.cend() && pending->source_size == reply.source_size &&
        pending->modified_unix_ns == reply.modified_unix_ns &&
        pending->source_revision == reply.source_revision &&
        pending->edge <= reply.presentation_edge) {
        pendingPreviewEdges_.remove(reply.entry_id);
    }

    const auto *entry = views_.model->entry_for_id(reply.entry_id);
    if (entry == nullptr) {
        return;
    }
    if (!same_source(reply, *entry)) {
        const auto completed = completedPreviewEdges_.constFind(reply.entry_id);
        if (completed != completedPreviewEdges_.cend() &&
            completed->source_size == reply.source_size &&
            completed->modified_unix_ns == reply.modified_unix_ns &&
            completed->source_revision == reply.source_revision) {
            completedPreviewEdges_.remove(reply.entry_id);
        }
        previewTimer_->start();
        return;
    }
    using Status = preview::helper_protocol::ResponseStatus;
    if (reply.status == Status::queue_busy || reply.status == Status::cancelled) {
        previewTimer_->start();
        return;
    }
    views_.model->set_preview(
        reply.entry_id, reply.source_size, reply.modified_unix_ns, reply.source_revision,
        std::move(reply.image), static_cast<int>(reply.status), reply.presentation_edge,
        reply.page_count, std::move(reply.source_color_model),
        std::move(reply.source_color_profile), std::move(reply.source_profile_fingerprint),
        static_cast<int>(reply.provenance), reply.startup_diagnostic);

    if (reply.status == Status::success_cached || reply.status == Status::success_decoded) {
        strictRevalidatedPreviews_.insert(
            reply.entry_id,
            {.source_size = reply.source_size,
             .modified_unix_ns = reply.modified_unix_ns,
             .source_revision = reply.source_revision,
             .edge = std::max(current_preview_edge(strictRevalidatedPreviews_, *entry),
                              reply.presentation_edge)});
        strictRevalidatedPages_[reply.entry_id].insert(0);
        offlineEligiblePreviews_.insert(
            reply.entry_id,
            {.source_size = reply.source_size,
             .modified_unix_ns = reply.modified_unix_ns,
             .source_revision = reply.source_revision,
             .edge = std::max(current_preview_edge(offlineEligiblePreviews_, *entry),
                              reply.presentation_edge)});
        offlineEligiblePages_[reply.entry_id].insert(0);
    } else if (reply.status == Status::source_changed || reply.status == Status::not_found ||
               reply.status == Status::permission_denied ||
               reply.status == Status::authentication_failed) {
        offlineEligiblePreviews_.remove(reply.entry_id);
        offlineEligiblePages_.remove(reply.entry_id);
        strictRevalidatedPreviews_.remove(reply.entry_id);
        strictRevalidatedPages_.remove(reply.entry_id);
    }

    if (reply.status == Status::disconnected || reply.status == Status::timed_out ||
        reply.status == Status::source_unavailable ||
        reply.status == Status::success_offline_cached) {
        if (catalogWideProtection() && preview_failure_can_disconnect(*entry)) {
            enterOfflineState();
            return;
        }
    }
    if (reply.status == Status::permission_denied ||
        reply.status == Status::authentication_failed) {
        if (catalogWideProtection()) {
            enterBlockedState();
            return;
        }
    }
    completedPreviewEdges_.insert(
        reply.entry_id, {.source_size = reply.source_size,
                         .modified_unix_ns = reply.modified_unix_ns,
                         .source_revision = reply.source_revision,
                         .edge = std::max(current_preview_edge(completedPreviewEdges_, *entry),
                                          reply.presentation_edge)});
    if (selectedPageIndex_ == 0 &&
        views_.catalog->currentIndex().data(DirectoryListModel::EntryIdRole).toULongLong() ==
            reply.entry_id) {
        update_selected_preview(views_.catalog->currentIndex());
    }
    previewTimer_->start();
}

void PreviewController::handle_folder_mosaic(FolderMosaicReply reply) {
    if (reply.app_generation != previewGeneration_) {
        return;
    }
    previewTimer_->start();
    const auto status = reply.status;
    views_.model->set_preview(reply.folder_id, reply.source_size, reply.modified_unix_ns,
                              reply.source_revision, std::move(reply.image),
                              static_cast<int>(status), reply.presentation_edge, 0, {}, {});
    if (status == preview::helper_protocol::ResponseStatus::disconnected ||
        status == preview::helper_protocol::ResponseStatus::timed_out ||
        status == preview::helper_protocol::ResponseStatus::source_unavailable ||
        status == preview::helper_protocol::ResponseStatus::success_offline_cached) {
        const auto *entry = views_.model->entry_for_id(reply.folder_id);
        if (catalogWideProtection() && entry != nullptr && preview_failure_can_disconnect(*entry)) {
            enterOfflineState();
        }
    } else if (status == preview::helper_protocol::ResponseStatus::authentication_failed) {
        // A denied child folder does not revoke access to its parent or siblings.
        // Its own pixels were invalidated by set_preview above; only authentication
        // failure retains the catalog-wide protection policy.
        if (catalogWideProtection()) {
            enterBlockedState();
        }
    }
}

void PreviewController::selectedEntryChanged(const QModelIndex &index) {
    resetSelectedPreview();
    const auto *entry = views_.model->entry_at(index);
    selectedCatalogEntryId_ = entry != nullptr ? static_cast<qulonglong>(entry->id) : 0;
    views_.modified->set_modified_time(entry != nullptr && entry->kind == core::EntryKind::file
                                           ? std::optional{entry->modified_unix_ns}
                                           : std::nullopt);
    if (entry == nullptr) {
        selectedEntryId_ = 0;
        update_page_navigation(0, 0);
        selectedName_.clear();
        selectedColorSummary_.clear();
        views_.canvas->setText(QCoreApplication::translate("MainWindow", "Select a file"));
        views_.name->clear();
        views_.name->setToolTip({});
        views_.format->clear();
        views_.format->hide();
        update_preview_support_label(false);
        views_.password->hide();
        notifyStatus();
        return;
    }

    const auto file_name = QString::fromUtf8(entry->name_utf8);
    const QFileInfo file_info(file_name);
    const auto base_name = file_info.completeBaseName();
    // An unusually long suffix belongs in the wrapped literal name, not an unbounded format badge.
    const auto suffix = entry->kind != core::EntryKind::directory && !base_name.isEmpty() &&
                                file_info.suffix().size() <= 16
                            ? file_info.suffix().toUpper()
                            : QString{};
    selectedName_ = file_name;
    views_.name->set_full_text(suffix.isEmpty() ? file_name : base_name);
    views_.name->setToolTip(QString::fromUtf8(entry->path_utf8));
    if (entry->kind == core::EntryKind::directory) {
        selectedEntryId_ = 0;
        update_page_navigation(0, 0);
        views_.canvas->setText(QCoreApplication::translate(
            "MainWindow", "Folder\n\nA mosaic will appear after thumbnails are connected"));
        views_.format->clear();
        views_.format->hide();
        update_preview_support_label(false);
        selectedColorSummary_.clear();
        views_.password->hide();
    } else {
        selectedEntryId_ = static_cast<qulonglong>(entry->id);
        selectedPageIndex_ = 0;
        selectedPageCount_ = 0;
        views_.format->setText(suffix);
        views_.format->setVisible(!suffix.isEmpty());
        views_.format->setStyleSheet(
            QStringLiteral("font-weight: 700; color: %1;")
                .arg(format_color(suffix, views_.format->palette().window().color()).name()));
        update_preview_support_label(false);
        update_selected_preview(index);
        ensureSelectedPreview();
    }
    notifyStatus();
}

void PreviewController::update_selected_preview(const QModelIndex &index) {
    views_.canvas->setToolTip({});
    const auto *entry = views_.model->entry_at(index);
    if (entry == nullptr || entry->id != selectedEntryId_) {
        return;
    }
    if (selectedAnimationKey_ == animation_source_key(*entry) &&
        !selectedAnimationImage_.isNull()) {
        views_.canvas->set_animation_frame(selectedAnimationImage_, selectedAnimationKey_);
        update_page_navigation(0, 1);
        return;
    }
    const auto current = selectedPreview_ && same_source(*selectedPreview_, *entry) &&
                         selectedPreview_->page_index == selectedPageIndex_;
    const auto image = current ? selectedPreview_->image : QImage{};
    views_.canvas->setProperty("previewSourceSize", image.size());
    views_.canvas->setProperty("previewRequestedEdge",
                               current ? selectedPreview_->presentation_edge : 0);
    if (!image.isNull()) {
        const auto key = QString::fromUtf8(entry->path_utf8) + QChar::Null +
                         QString::fromUtf8(entry->source_revision_utf8) + QChar::Null +
                         QString::number(entry->size_bytes) + QLatin1Char(':') +
                         QString::number(entry->modified_unix_ns) + QLatin1Char(':') +
                         QString::number(selectedPageIndex_);
        views_.canvas->set_preview(image, key);
        views_.password->hide();
        update_preview_support_label(true);
    } else {
        const auto status = current ? QVariant(static_cast<int>(selectedPreview_->status))
                                    : index.data(DirectoryListModel::PreviewStatusRole);
        using Status = preview::helper_protocol::ResponseStatus;
        if (selectedPreviewExtended_) {
            views_.canvas->setText(QCoreApplication::translate("MainWindow", "Force preview…"));
            views_.password->hide();
        } else if (!status.isValid() ||
                   status.toInt() == static_cast<int>(Status::success_cached) ||
                   status.toInt() == static_cast<int>(Status::success_decoded) ||
                   status.toInt() == static_cast<int>(Status::success_offline_cached)) {
            views_.canvas->setText(QCoreApplication::translate("MainWindow", "Preparing preview…"));
            views_.password->hide();
        } else {
            const auto response_status =
                static_cast<preview::helper_protocol::ResponseStatus>(status.toInt());
            views_.canvas->setText(selected_preview_status_text(response_status));
            if (response_status == Status::worker_start_failed) {
                views_.canvas->setToolTip(
                    current ? worker_startup_tooltip(selectedPreview_->startup_diagnostic)
                            : index.data(DirectoryListModel::StartupDiagnosticRole).toString());
            }
            update_document_password_action(response_status);
        }
        update_preview_support_label(false);
    }
    const auto page_count =
        current && selectedPreview_->page_count != 0 ? selectedPreview_->page_count
        : selectedPageCount_ != 0                    ? selectedPageCount_
                                  : index.data(DirectoryListModel::PageCountRole).toUInt();
    update_page_navigation(selectedPageIndex_, page_count);
    const auto model = current ? selectedPreview_->source_color_model
                               : index.data(DirectoryListModel::SourceColorModelRole).toString();
    const auto profile = current
                             ? selectedPreview_->source_color_profile
                             : index.data(DirectoryListModel::SourceColorProfileRole).toString();
    selectedColorSummary_ = color_summary(model, profile);
    notifyStatus();
}

void PreviewController::update_preview_support_label(const bool preview_ready) {
    const auto row = views_.model->row_for_id(selectedEntryId_);
    const auto embedded =
        selectedPreview_
            ? selectedPreview_->provenance ==
                  preview::helper_protocol::PreviewProvenance::embedded_preview
            : row >= 0 && views_.model->index(row, 0)
                                  .data(DirectoryListModel::PreviewProvenanceRole)
                                  .toInt() ==
                              static_cast<int>(
                                  preview::helper_protocol::PreviewProvenance::embedded_preview);
    const auto offline =
        offline_cache_only() ||
        (selectedPreview_
             ? selectedPreview_->status ==
                   preview::helper_protocol::ResponseStatus::success_offline_cached
             : row >= 0 &&
                   views_.model->index(row, 0)
                           .data(DirectoryListModel::PreviewStatusRole)
                           .toInt() ==
                       static_cast<int>(
                           preview::helper_protocol::ResponseStatus::success_offline_cached));
    const auto checking =
        !selectedPreview_ && row >= 0 &&
        views_.model->index(row, 0).data(DirectoryListModel::PreviewStatusRole).toInt() ==
            DirectoryListModel::VerificationPendingStatus;
    const auto profile = selectedPreview_ ? selectedPreview_->source_color_profile
                                          : views_.model->index(row, 0)
                                                .data(DirectoryListModel::SourceColorProfileRole)
                                                .toString();
    const auto approximate = profile == QLatin1StringView(color::kUnprofiledCmykApproximation);
    views_.support->setVisible(preview_ready && (checking || offline || embedded || approximate));
    views_.support->setText(
        preview_ready && checking   ? QCoreApplication::translate("MainWindow", "Checking…")
        : preview_ready && offline  ? QCoreApplication::translate("MainWindow", "Cache · offline")
        : preview_ready && embedded ? QCoreApplication::translate("MainWindow", "Embedded preview")
                                    : QString{});
    if (preview_ready && approximate) {
        const auto warning =
            QCoreApplication::translate("MainWindow", "CMYK without profile · approximate colors");
        views_.support->setText(views_.support->text().isEmpty()
                                    ? warning
                                    : views_.support->text() + QStringLiteral(" · ") + warning);
    }
    if (selectedAnimationFailed_) {
        views_.support->setText(QCoreApplication::translate("MainWindow", "Animation unavailable"));
        views_.support->show();
    }
}

QString PreviewController::animation_source_key(const core::DirectoryEntry &entry) {
    return QString::fromUtf8(entry.path_utf8) + QChar::Null +
           QString::fromUtf8(entry.source_revision_utf8) + QChar::Null +
           QString::number(entry.size_bytes) + QLatin1Char(':') +
           QString::number(entry.modified_unix_ns);
}

void PreviewController::stopSelectedAnimation() {
    selectedAnimationKey_.clear();
    selectedAnimationImage_ = {};
    selectedAnimationFailed_ = false;
    selectedAnimationAnimated_ = false;
    gifAnimationClient_.stop();
    if (views_.canvas != nullptr)
        views_.canvas->set_animation_state(ZoomPreview::AnimationState::hidden);
}

void PreviewController::resetSelectedPreview(const bool clear_image) {
    stopSelectedAnimation();
    selectedPreviewExtended_ = false;
    if (views_.cancel_attempt != nullptr) {
        views_.cancel_attempt->setEnabled(false);
    }
    previewClient_.cancel_generation(selectedPreviewGeneration_);
    constexpr auto generation_bit = 1ULL << 62U;
    selectedPreviewGeneration_ =
        generation_bit | ((selectedPreviewGeneration_ + 1U) & (generation_bit - 1U));
    selectedPreviewRequest_ = 0;
    selectedPreviewPendingEdge_ = 0;
    if (clear_image) {
        selectedPreview_.reset();
        if (views_.canvas != nullptr) {
            views_.canvas->clear();
            views_.canvas->setToolTip({});
            views_.canvas->setProperty("previewSourceSize", QSize{});
            views_.canvas->setProperty("previewRequestedEdge", 0);
        }
    }
}

std::uint16_t PreviewController::selected_preview_edge() const {
    const auto size = views_.canvas->contentsRect().size();
    const auto physical_edge =
        std::ceil(std::max(size.width(), size.height()) * views_.canvas->devicePixelRatioF());
    return static_cast<std::uint16_t>(
        std::clamp(physical_edge, 512.0,
                   static_cast<double>(preview::helper_protocol::kMaximumCanonicalEdge)));
}

bool PreviewController::canForcePreview(const core::DirectoryEntry &entry) const {
    if (suspended_ || entry.kind != core::EntryKind::file || selectedPreviewExtended_ ||
        previewRuntimeState_ != PreviewRuntimeState::online) {
        return false;
    }
    const auto retryable = [](const int status) {
        using Status = preview::helper_protocol::ResponseStatus;
        return status == static_cast<int>(Status::memory_limit) ||
               status == static_cast<int>(Status::worker_start_failed) ||
               status == static_cast<int>(Status::timed_out) ||
               status == static_cast<int>(Status::processing_timed_out);
    };
    if (selectedPreview_ && selectedPreview_->entry_id == entry.id &&
        same_source(*selectedPreview_, entry) &&
        selectedPreview_->page_index == selectedPageIndex_) {
        if (retryable(static_cast<int>(selectedPreview_->status)) ||
            selectedPreview_->status == preview::helper_protocol::ResponseStatus::cancelled) {
            return true;
        }
        if (selectedPageIndex_ != 0 || selectedPreview_->image.isNull()) {
            return false;
        }
    }
    const auto row = views_.model->row_for_id(static_cast<qulonglong>(entry.id));
    const auto status = views_.model->index(row, 0).data(DirectoryListModel::PreviewStatusRole);
    return status.isValid() && retryable(status.toInt());
}

void PreviewController::forcePreview(const qulonglong entry_id) {
    const auto *entry = views_.model->entry_for_id(entry_id);
    if (entry == nullptr || !canForcePreview(*entry)) {
        return;
    }
    if (selectedEntryId_ != entry_id) {
        const auto index = views_.model->index(views_.model->row_for_id(entry_id), 0);
        views_.catalog->selectionModel()->setCurrentIndex(index,
                                                          QItemSelectionModel::ClearAndSelect);
    }
    resetSelectedPreview();
    views_.password->hide();
    views_.canvas->setText(QCoreApplication::translate("MainWindow", "Force preview…"));
    ensureSelectedPreview(true);
}

void PreviewController::cancelForcedPreview() {
    if (!selectedPreviewExtended_) {
        return;
    }
    const auto *entry = views_.model->entry_for_id(selectedEntryId_);
    resetSelectedPreview();
    if (entry == nullptr) {
        return;
    }
    // Keep a terminal result for this selection so an idle/resize tick cannot restart it.
    selectedPreview_ =
        PreviewReply{.entry_id = selectedEntryId_,
                     .generation = selectedPreviewGeneration_,
                     .source_size = entry->size_bytes,
                     .modified_unix_ns = entry->modified_unix_ns,
                     .source_revision = QString::fromUtf8(entry->source_revision_utf8),
                     .presentation_edge = preview::helper_protocol::kMaximumCanonicalEdge,
                     .status = preview::helper_protocol::ResponseStatus::cancelled,
                     .image = {},
                     .source_color_model = {},
                     .source_color_profile = {},
                     .source_profile_fingerprint = {},
                     .page_index = selectedPageIndex_,
                     .page_count = selectedPageCount_};
    update_selected_preview(views_.catalog->currentIndex());
}

void PreviewController::ensureSelectedPreview(const bool extended_limits) {
    if (suspended_ || selectedEntryId_ == 0 || views_.canvas == nullptr ||
        previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    const auto *entry = views_.model->entry_for_id(selectedEntryId_);
    if (entry == nullptr || entry->kind != core::EntryKind::file) {
        return;
    }
    if ((selectedPreview_ && !same_source(*selectedPreview_, *entry)) ||
        (!selectedAnimationKey_.isEmpty() &&
         selectedAnimationKey_ != animation_source_key(*entry))) {
        resetSelectedPreview();
        update_page_navigation(0, 0);
        update_selected_preview(views_.catalog->currentIndex());
    }
    if (!selectedAnimationFailed_ && !offline_cache_only() &&
        QString::fromUtf8(entry->name_utf8).endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)) {
        if (selectedAnimationKey_.isEmpty()) {
            selectedAnimationKey_ = animation_source_key(*entry);
            gifAnimationClient_.start(
                QString::fromUtf8(entry->path_utf8), entry->size_bytes, entry->modified_unix_ns,
                QString::fromUtf8(entry->source_revision_utf8), selectedAnimationKey_);
            gifAnimationClient_.set_suspended(!windowAvailable_);
        }
        return;
    }
    const auto edge =
        extended_limits ? preview::helper_protocol::kMaximumCanonicalEdge : selected_preview_edge();
    if (selectedPreviewExtended_ && selectedPreviewRequest_ != 0) {
        return;
    }
    if (selectedPreviewRequest_ != 0 && selectedPreviewPendingEdge_ >= edge) {
        return;
    }
    if (selectedPreview_ && selectedPreview_->generation == selectedPreviewGeneration_ &&
        selectedPreview_->page_index == selectedPageIndex_ &&
        selectedPreview_->presentation_edge >= edge) {
        return;
    }
    const auto known_offline = offline_cache_only();
    if (known_offline && !offlineCacheAllowed(*entry, selectedPageIndex_)) {
        views_.canvas->setPixmap({});
        views_.canvas->setText(
            QCoreApplication::translate("MainWindow", "No verified offline copy of this page"));
        return;
    }

    if (selectedPreviewRequest_ != 0) {
        resetSelectedPreview(false);
    }
    selectedPreviewPendingEdge_ = edge;
    selectedPreviewRequest_ = previewClient_.request_preview(
        selectedEntryId_, selectedPreviewGeneration_, QString::fromUtf8(entry->path_utf8),
        entry->size_bytes, entry->modified_unix_ns, QString::fromUtf8(entry->source_revision_utf8),
        edge, preview::ThumbnailPriority::visible_selected,
        [this](PreviewReply reply) { handle_selected_page_reply(std::move(reply)); },
        selectedPageIndex_, documentPasswords_.value(QString::fromUtf8(entry->path_utf8)),
        known_offline,
        !restrictOfflineFallback_ ||
            (current_preview_edge(strictRevalidatedPreviews_, *entry) != 0 &&
             strictRevalidatedPages_.value(selectedEntryId_).contains(selectedPageIndex_)),
        true, extended_limits);
    selectedPreviewExtended_ = extended_limits && selectedPreviewRequest_ != 0;
    views_.cancel_attempt->setEnabled(selectedPreviewExtended_);
    if (selectedPreviewRequest_ == 0) {
        views_.canvas->setText(
            QCoreApplication::translate("MainWindow", "Failed to queue the page"));
    }
}

void PreviewController::requestSelectedPage(const std::uint32_t page_index) {
    if (selectedEntryId_ == 0 || page_index >= selectedPageCount_ ||
        previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    if (page_index != selectedPageIndex_) {
        resetSelectedPreview();
    }
    selectedPageIndex_ = page_index;
    update_page_navigation(page_index, selectedPageCount_);
    update_selected_preview(views_.catalog->currentIndex());
    ensureSelectedPreview();
    if (selectedPreviewRequest_ != 0 && (!selectedPreview_ || selectedPreview_->image.isNull())) {
        views_.canvas->setPixmap({});
        views_.canvas->setText(
            QCoreApplication::translate("MainWindow", "Preparing page %1…").arg(page_index + 1U));
    }
}

void PreviewController::handle_selected_page_reply(PreviewReply reply) {
    if (reply.generation != selectedPreviewGeneration_ || reply.entry_id != selectedEntryId_ ||
        reply.page_index != selectedPageIndex_ || reply.request_id != selectedPreviewRequest_) {
        return;
    }
    const bool extended_attempt = selectedPreviewExtended_;
    selectedPreviewExtended_ = false;
    views_.cancel_attempt->setEnabled(false);
    selectedPreviewRequest_ = 0;
    selectedPreviewPendingEdge_ = 0;
    const auto *entry = views_.model->entry_for_id(reply.entry_id);
    if (entry == nullptr) {
        return;
    }
    if (!same_source(reply, *entry)) {
        resetSelectedPreview();
        selectedPageIndex_ = 0;
        selectedPageCount_ = 0;
        update_selected_preview(views_.catalog->currentIndex());
        previewTimer_->start();
        return;
    }

    using Status = preview::helper_protocol::ResponseStatus;
    const auto offline_reply = reply.status == Status::success_offline_cached;
    if (reply.status == Status::disconnected || reply.status == Status::timed_out ||
        reply.status == Status::source_unavailable) {
        if (catalogWideProtection() && preview_failure_can_disconnect(*entry)) {
            enterOfflineState();
            return;
        }
    }
    if (reply.status == Status::permission_denied ||
        reply.status == Status::authentication_failed) {
        enterBlockedState();
        return;
    }
    if ((reply.status == Status::success_cached || reply.status == Status::success_offline_cached ||
         reply.status == Status::success_decoded) &&
        !reply.image.isNull()) {
        selectedColorSummary_ = color_summary(reply.source_color_model, reply.source_color_profile);
        views_.password->hide();
        if (reply.status != Status::success_offline_cached) {
            strictRevalidatedPreviews_.insert(
                reply.entry_id,
                {.source_size = reply.source_size,
                 .modified_unix_ns = reply.modified_unix_ns,
                 .source_revision = reply.source_revision,
                 .edge = std::max(current_preview_edge(strictRevalidatedPreviews_, *entry),
                                  reply.presentation_edge)});
            strictRevalidatedPages_[reply.entry_id].insert(reply.page_index);
            offlineEligiblePreviews_.insert(
                reply.entry_id,
                {.source_size = reply.source_size,
                 .modified_unix_ns = reply.modified_unix_ns,
                 .source_revision = reply.source_revision,
                 .edge = std::max(current_preview_edge(offlineEligiblePreviews_, *entry),
                                  reply.presentation_edge)});
            offlineEligiblePages_[reply.entry_id].insert(reply.page_index);
        }
    } else {
        if (reply.status == Status::source_changed || reply.status == Status::not_found ||
            reply.status == Status::permission_denied ||
            reply.status == Status::authentication_failed) {
            offlineEligiblePreviews_.remove(reply.entry_id);
            offlineEligiblePages_.remove(reply.entry_id);
            strictRevalidatedPreviews_.remove(reply.entry_id);
            strictRevalidatedPages_.remove(reply.entry_id);
        }
        views_.canvas->setPixmap({});
        views_.canvas->setText(selected_preview_status_text(reply.status));
        update_document_password_action(reply.status);
    }
    if (!extended_attempt &&
        (reply.status == Status::cancelled || reply.status == Status::queue_busy)) {
        previewTimer_->start();
        return;
    }
    selectedPreview_ = std::move(reply);
    if (extended_attempt && selectedPreview_->page_index == 0 &&
        !selectedPreview_->image.isNull()) {
        auto tile = *selectedPreview_;
        tile.generation = viewportGeneration_;
        tile.presentation_edge = static_cast<std::uint16_t>(thumbnailExtent_);
        tile.image = tile.image.scaled(tile.presentation_edge, tile.presentation_edge,
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
        handle_preview_reply(std::move(tile));
    }
    if (offline_reply && catalogWideProtection() && preview_failure_can_disconnect(*entry)) {
        enterOfflineState();
    }
    update_selected_preview(views_.catalog->currentIndex());
}

void PreviewController::promptForDocumentPassword() {
    const auto *entry = views_.model->entry_for_id(selectedEntryId_);
    if (entry == nullptr || entry->kind != core::EntryKind::file ||
        previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    bool accepted{};
    const auto password = QInputDialog::getText(
        views_.window, QCoreApplication::translate("MainWindow", "Document password"),
        QCoreApplication::translate("MainWindow", "Enter password:"), QLineEdit::Password, {},
        &accepted, Qt::Dialog, Qt::ImhHiddenText);
    if (!accepted || password.isEmpty()) {
        return;
    }

    const auto path = QString::fromUtf8(entry->path_utf8);
    documentPasswords_.insert(path, password);
    viewportChanged();
    resetSelectedPreview();
    completedPreviewEdges_.remove(selectedEntryId_);
    selectedPageIndex_ = 0;
    selectedPageCount_ = 0;
    update_page_navigation(0, 0);
    views_.canvas->setPixmap({});
    views_.canvas->setText(QCoreApplication::translate("MainWindow", "Checking password…"));
    views_.password->hide();
    const auto row = views_.model->row_for_id(selectedEntryId_);
    ensureSelectedPreview();
    if (row < 0 || selectedPreviewRequest_ == 0) {
        views_.canvas->setText(
            QCoreApplication::translate("MainWindow", "Failed to verify the password"));
    }
}

void PreviewController::update_document_password_action(
    const preview::helper_protocol::ResponseStatus status) {
    const auto wrong_password =
        status == preview::helper_protocol::ResponseStatus::document_password_incorrect;
    const auto visible =
        previewRuntimeState_ != PreviewRuntimeState::blocked &&
        (status == preview::helper_protocol::ResponseStatus::password_required || wrong_password);
    views_.password->setVisible(visible);
    views_.password->setText(wrong_password
                                 ? QCoreApplication::translate("MainWindow", "Change password")
                                 : QCoreApplication::translate("MainWindow", "Enter password"));
}

QString PreviewController::selected_preview_status_text(
    const preview::helper_protocol::ResponseStatus status) const {
    return preview_status_text(status);
}

void PreviewController::update_page_navigation(const std::uint32_t page_index,
                                               const std::uint32_t page_count) {
    selectedPageIndex_ = page_index;
    selectedPageCount_ = page_count;
    const auto multiple_pages = page_count > 1;
    views_.page_navigation->setVisible(multiple_pages);
    views_.page_indicator->setText(
        multiple_pages ? QStringLiteral("%1 / %2").arg(page_index + 1U).arg(page_count)
                       : QString{});
    views_.previous_page->setEnabled(multiple_pages && page_index > 0);
    views_.next_page->setEnabled(multiple_pages && page_index + 1U < page_count);
}

void PreviewController::retry_external_component_previews() {
    using Status = preview::helper_protocol::ResponseStatus;
    const auto component_missing = [](const Status status) {
        return status == Status::ghostscript_required;
    };
    if (selectedPreview_ && component_missing(selectedPreview_->status)) {
        resetSelectedPreview();
        previewTimer_->start(0);
    }
    auto retry_ids =
        views_.model->clear_previews_with_status(static_cast<int>(Status::ghostscript_required));
    for (const auto id : retry_ids) {
        completedPreviewEdges_.remove(id);
    }
    folderMosaicController_.invalidate_display_images();
    if (!retry_ids.isEmpty() || views_.model->visible_directories() != 0) {
        update_selected_preview(views_.catalog->currentIndex());
        previewTimer_->start(0);
    }
}

} // namespace vove::ui
