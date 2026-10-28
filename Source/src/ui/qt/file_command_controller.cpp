#include "file_command_workflow.hpp"
#include "drop_router.hpp"
#include <QApplication>
#include <QClipboard>
#include <QSettings>

namespace vove::ui::file_command_detail {
namespace {
constexpr auto directoryTransferStateMagic = "VOVE-DIRECTORY-STATE-1\n";
constexpr qsizetype directoryTransferStateDigestBytes = 32;
constexpr qsizetype directoryTransferStateMaximumBytes = qsizetype{2} * 1'024 * 1'024;
}
bool same_directory_path(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(left))
               .compare(QDir::cleanPath(QDir::fromNativeSeparators(right)), sensitivity) == 0;
}


QByteArray seal_directory_transfer_payload(const QByteArray &payload) {
    const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    auto sealed = QByteArray(directoryTransferStateMagic) + digest + payload;
    return sealed.size() <= directoryTransferStateMaximumBytes ? sealed : QByteArray{};
}

QByteArray seal_directory_transfer_state(const QJsonObject &state) {
    return seal_directory_transfer_payload(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

std::optional<QJsonObject> open_directory_transfer_state(const QByteArray &sealed) {
    const QByteArray magic(directoryTransferStateMagic);
    if (sealed.size() > directoryTransferStateMaximumBytes || !sealed.startsWith(magic) ||
        sealed.size() <= magic.size() + directoryTransferStateDigestBytes) {
        return std::nullopt;
    }
    const auto digest = sealed.mid(magic.size(), directoryTransferStateDigestBytes);
    const auto payload = sealed.mid(magic.size() + directoryTransferStateDigestBytes);
    if (QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != digest) {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    auto state = document.object();
    if (state.value(QStringLiteral("schema")).toInt() != 1) {
        return std::nullopt;
    }
    return state;
}


std::filesystem::path native_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}


QString display_path(const std::filesystem::path &path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    const auto text = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(text.data()),
                             static_cast<qsizetype>(text.size()));
#endif
}


QString transfer_path_key(const std::filesystem::path &path) {
    auto key = QDir::cleanPath(QDir::fromNativeSeparators(display_path(path)));
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

#ifdef Q_OS_WIN
bool is_unc_path(const QString &path) {
    return QDir::fromNativeSeparators(path).startsWith(QStringLiteral("//"));
}
#endif

std::filesystem::path current_journal_path(const QString &override_path) {
    if (!override_path.isEmpty()) {
        return native_path(override_path);
    }
    const auto state_root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return native_path(
        QDir(state_root).filePath(QString::fromLatin1(fileops::kCurrentOperationJournalFilename)));
}

std::filesystem::path trash_manifest_directory(const std::filesystem::path &journal_path) {
    return journal_path.parent_path() / "trash";
}

bool same_entry_snapshot(const core::DirectoryEntry &left,
                         const core::DirectoryEntry &right) noexcept {
    return left.kind == right.kind && left.path_utf8 == right.path_utf8 &&
           left.size_bytes == right.size_bytes && left.modified_unix_ns == right.modified_unix_ns &&
           left.source_revision_utf8 == right.source_revision_utf8;
}

bool same_entry_snapshots(const QList<core::DirectoryEntry> &left,
                          const QList<core::DirectoryEntry> &right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index{}; index < left.size(); ++index) {
        if (!same_entry_snapshot(left[index], right[index])) {
            return false;
        }
    }
    return true;
}


QString operation_status_text(const fileops::OperationStatus status) {
    using Status = fileops::OperationStatus;
    switch (status) {
    case Status::success:
        return QCoreApplication::translate("MainWindow", "Item renamed");
    case Status::invalid_request:
        return QCoreApplication::translate("MainWindow", "Invalid item name");
    case Status::not_found:
        return QCoreApplication::translate("MainWindow", "The source item is no longer present");
    case Status::conflict:
        return QCoreApplication::translate("MainWindow", "An item with this name already exists");
    case Status::source_changed:
        return QCoreApplication::translate("MainWindow",
                                           "The item changed. Refresh the folder and try again");
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow", "No permission to rename");
    case Status::authentication_required:
        return QCoreApplication::translate("MainWindow", "Network folder sign-in required");
    case Status::disconnected:
        return QCoreApplication::translate("MainWindow",
                                           "Connection to the network folder was lost");
    case Status::timed_out:
        return QCoreApplication::translate(
            "MainWindow", "The network folder did not respond. State was verified");
    case Status::unsupported:
        return QCoreApplication::translate("MainWindow", "This item cannot be renamed safely");
    case Status::unknown_outcome:
        return QCoreApplication::translate(
            "MainWindow", "The outcome is unclear. The operation was not repeated");
    case Status::io_error:
        return QCoreApplication::translate("MainWindow", "Failed to rename the item");
    case Status::cross_device:
        return QCoreApplication::translate("MainWindow", "The item is on another drive");
    case Status::file_in_use:
        return QCoreApplication::translate("OperationNotice", "File is in use by another application");
    }
    return QCoreApplication::translate("MainWindow", "Failed to rename the item");
}

bool ambiguous_rename_status(const fileops::OperationStatus status) {
    using Status = fileops::OperationStatus;
    return status == Status::disconnected || status == Status::timed_out ||
           status == Status::unknown_outcome || status == Status::io_error;
}

QString trash_operation_error_text(const fileops::OperationStatus status,
                                  const std::string_view detail) {
    if (detail.find("DACL") != std::string_view::npos ||
        detail.find("security baseline") != std::string_view::npos ||
        detail.find("foreign owner") != std::string_view::npos) {
        return QCoreApplication::translate("TrashRecoveryDialog",
                                           "Trash permissions differ from the recorded state");
    }
    if ((detail.find("manifest") != std::string_view::npos ||
         detail.find("plan") != std::string_view::npos) &&
        (detail.find("identity differs") != std::string_view::npos ||
         detail.find("different operation") != std::string_view::npos)) {
        return QCoreApplication::translate("TrashRecoveryDialog",
                                           "Trash records do not match; recovery is required");
    }
    using Status = fileops::OperationStatus;
    switch (status) {
    case Status::success:
        return {};
    case Status::invalid_request:
        return QCoreApplication::translate("MainWindow", "Invalid Trash operation request");
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow", "No permission for this Trash operation");
    case Status::timed_out:
        return QCoreApplication::translate("MainWindow", "The operation timed out");
    case Status::unsupported:
        return QCoreApplication::translate("MainWindow", "This Trash operation is not supported");
    case Status::io_error:
        return QCoreApplication::translate("MainWindow", "Trash operation I/O error");
    default:
        return operation_status_text(status);
    }
}

bool confirms_no_committed_rename(const fileops::OperationResult &result) {
    return ambiguous_rename_status(result.status) && result.source_matches_expected &&
           !result.destination_present;
}

std::uint64_t next_directory_transfer_request_id() noexcept {
    static std::atomic_uint64_t next{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    auto value = next.fetch_add(1U, std::memory_order_relaxed);
    if (value == 0U) {
        value = next.fetch_add(1U, std::memory_order_relaxed);
    }
    return value;
}

QString directory_transfer_error_text(const fileops::BasicDirectoryTransferStatus status) {
    using Status = fileops::BasicDirectoryTransferStatus;
    switch (status) {
    case Status::success:
        return {};
    case Status::move_pending_publication:
        return QCoreApplication::translate("MainWindow",
                                           "Move is waiting to be published: free the occupied "
                                           "name in the destination folder and choose Continue");
    case Status::recovery_required:
        return QCoreApplication::translate("MainWindow", "An incomplete operation was found");
    case Status::unknown_outcome:
        return QCoreApplication::translate("MainWindow", "The publication outcome is unknown");
    case Status::invalid_request:
        return QCoreApplication::translate(
            "MainWindow", "This folder cannot be transferred to the selected location");
    case Status::not_found:
        return QCoreApplication::translate("MainWindow",
                                           "The source or destination folder was not found");
    case Status::conflict:
        return QCoreApplication::translate(
            "MainWindow",
            "Name conflict: an item with this name already exists in the destination folder");
    case Status::source_changed:
        return QCoreApplication::translate("MainWindow",
                                           "The source folder changed during transfer");
    case Status::staging_changed:
        return QCoreApplication::translate("MainWindow", "Temporary transfer data was modified");
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow",
                                           "Insufficient permissions to transfer the folder");
    case Status::file_in_use:
        return QCoreApplication::translate("OperationNotice", "File is in use by another application");
    case Status::authentication_required:
        return QCoreApplication::translate("MainWindow", "The network folder requires sign-in");
    case Status::disconnected:
        return QCoreApplication::translate("MainWindow",
                                           "Connection to the network folder was lost");
    case Status::timed_out:
        return QCoreApplication::translate("MainWindow",
                                           "The network folder did not respond in time");
    case Status::unsupported:
        return QCoreApplication::translate(
            "MainWindow", "This network resource does not support safe folder transfer");
    case Status::io_error:
        return QCoreApplication::translate("MainWindow", "Failed to transfer the folder");
    }
    return QCoreApplication::translate("MainWindow", "Failed to transfer the folder");
}

fileops::FileTransferServiceOptions
directory_transfer_service_options(fileops::FileTransferServiceOptions options) {
    if (options.idle_timeout == std::chrono::seconds(15)) {
        options.idle_timeout = std::chrono::minutes(30);
    }
    return options;
}


} // namespace vove::ui::file_command_detail

namespace vove::ui {
using namespace file_command_detail;

namespace {
constexpr auto directoryTransferStateKey = "operations/directoryTransferStateV1";
QByteArray read_recovery_settings() {
    QSettings settings;
    return settings.value(QString::fromLatin1(directoryTransferStateKey)).toByteArray();
}
bool write_recovery_settings(const QByteArray &state) {
    QSettings settings;
    if (state.isEmpty()) settings.remove(QString::fromLatin1(directoryTransferStateKey));
    else settings.setValue(QString::fromLatin1(directoryTransferStateKey), state);
    settings.remove(QStringLiteral("operations/directoryTransferRecoveryRequestId"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoverySource"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoveryDestination"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoverySourceRevision"));
    settings.remove(QStringLiteral("operations/directoryTransferCompletionKnown"));
    settings.remove(QStringLiteral("operations/directoryTransferDiscarding"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoveryManifest"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchKind"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchSources"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchRevisions"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchDestination"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchIndex"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchCompleted"));
    settings.sync();
    return settings.status() == QSettings::NoError;
}
} // namespace

FileCommandController::Impl::Impl(FileCommandPorts ports, FileCommandOptions options, QObject *parent)
    : QObject(parent), ports_(std::move(ports)), dialogOwner_(ports_.dialog_owner),
      transferProbeSource_(options.transfer_probes), dropProbeSource_(options.drop_probes),
      currentOperationJournalPath_(current_journal_path(options.journal_override)),
      trashManifestDirectory_(file_command_detail::trash_manifest_directory(currentOperationJournalPath_)),
      fileOperationService_(options.file_operations),
      directoryTransferService_(directory_transfer_service_options(options.file_transfers)),
      batchRenameCoordinator_({.file_operations = options.file_operations,
                               .journal_path = currentOperationJournalPath_}),
      permanentDeleteCoordinator_({.file_operations = options.file_operations,
                                   .journal_path = currentOperationJournalPath_}),
      fileTransferCoordinator_({.file_operations = options.file_operations,
                                .file_transfers = options.file_transfers,
                                .journal_path = currentOperationJournalPath_}),
      objectTransferQueue_({.file_operations = options.file_operations,
                            .file_transfers = directory_transfer_service_options(options.file_transfers),
                            .journal_path = currentOperationJournalPath_}),
      trashCoordinator_({.file_operations = std::move(options.file_operations),
                         .journal_path = currentOperationJournalPath_,
                         .manifest_directory = trashManifestDirectory_}) {
    if (!ports_.clear_pending_mosaics) ports_.clear_pending_mosaics = [](auto &&...) {};
    if (!ports_.stop_animation) ports_.stop_animation = [](auto &&...) {};
    if (!ports_.stop_preview_readers) ports_.stop_preview_readers = [](auto &&...) {};
    if (!ports_.suspend_preview_dispatch) ports_.suspend_preview_dispatch = [](auto &&...) {};
    if (!ports_.mark_all_previews_unverified) ports_.mark_all_previews_unverified = [](auto &&...) {};
    if (!ports_.activity_changed) ports_.activity_changed = [](auto &&...) {};
    if (!ports_.file_in_use) ports_.file_in_use = [](auto &&...) {};
    if (!ports_.clear_selection) ports_.clear_selection = [](auto &&...) {};
    if (!ports_.refresh_directory_sources) ports_.refresh_directory_sources = [](auto &&...) {};
    if (!ports_.mark_saved_search_dirty) ports_.mark_saved_search_dirty = [](auto &&...) {};
    if (!ports_.schedule_search) ports_.schedule_search = [](auto &&...) {};
    if (!ports_.begin_preview_generation) ports_.begin_preview_generation = [](auto &&...) {};
    if (!ports_.resume_preview_dispatch) ports_.resume_preview_dispatch = [](auto &&...) {};
    if (!ports_.mark_preview_unverified) ports_.mark_preview_unverified = [](auto &&...) {};
    if (!ports_.status_changed) ports_.status_changed = [](auto &&...) {};
    if (!ports_.actions_changed) ports_.actions_changed = [](auto &&...) {};
    if (!ports_.diagnostic_changed) ports_.diagnostic_changed = [](auto &&...) {};
    if (!ports_.poll_catalog) ports_.poll_catalog = [](auto &&...) {};
    if (!ports_.reveal_paths) ports_.reveal_paths = [](auto &&...) {};
    if (!ports_.refresh_after_rename) ports_.refresh_after_rename = [](auto &&...) {};
    if (!ports_.refresh_after_batch_rename) ports_.refresh_after_batch_rename = [](auto &&...) {};
    if (!ports_.load_trash_catalog) ports_.load_trash_catalog = [](auto &&...) {};
    if (!ports_.observe_search_path) ports_.observe_search_path = [](auto &&...) {};
    if (!ports_.reconcile_search_move) ports_.reconcile_search_move = [](auto &&...) {};
    if (!ports_.open_path) ports_.open_path = [](auto &&...) {};
    if (!ports_.open_external_paths) ports_.open_external_paths = [](auto &&...) {};
    if (!ports_.open_external_items_in_parent) ports_.open_external_items_in_parent = [](auto &&...) {};
    if (!ports_.pin_path) ports_.pin_path = [](auto &&...) {};
    if (!ports_.selection) ports_.selection = [] { return SelectionSnapshot{}; };
    if (!ports_.catalog) ports_.catalog = [] { return FileCommandCatalogState{}; };
    if (!ports_.entry_names) ports_.entry_names = [](const QString &) { return std::vector<std::filesystem::path>{}; };
    if (!ports_.trash_operation_items) ports_.trash_operation_items = [](const auto &) { return std::size_t{1}; };
    if (!ports_.readers_idle) ports_.readers_idle = [] { return true; };
    if (!ports_.read_recovery) ports_.read_recovery = read_recovery_settings;
    if (!ports_.write_recovery) ports_.write_recovery = write_recovery_settings;
    transferProbeTimer_ = new QTimer(this);
    transferProbeTimer_->setInterval(16);
    connect(transferProbeTimer_, &QTimer::timeout, this, &Impl::poll_transfer_preparation);
    dropProbeTimer_ = new QTimer(this);
    dropProbeTimer_->setInterval(16);
    connect(dropProbeTimer_, &QTimer::timeout, this, &Impl::poll_external_drop);
}

FileCommandController::Impl::~Impl() {
    transferProbeTimer_->stop();
    dropProbeTimer_->stop();
    trashCoordinator_.stop();
    objectTransferQueue_.stop();
    fileTransferCoordinator_.stop();
    directoryTransferService_.stop();
    permanentDeleteCoordinator_.stop();
    batchRenameCoordinator_.stop();
    fileOperationService_.stop();
}

void FileCommandController::Impl::set_rename_in_flight(const bool in_flight) {
    if (in_flight) stop_selected_animation();
    renameInFlight_ = in_flight;
    if (!in_flight && renamePreviewSuspended_) {
        renamePreviewSuspended_ = false;
        resume_preview_dispatch();
    }
    set_property("renameInFlight", in_flight);
    update_rename_action();
    publish_activity();
}

void FileCommandController::Impl::set_delete_in_flight(const bool in_flight) {
    const auto was_in_flight = deleteInFlight_;
    if (in_flight) stop_selected_animation();
    deleteInFlight_ = in_flight;
    if (in_flight) ports_.suspend_preview_dispatch();
    else if (was_in_flight) resume_preview_dispatch();
    set_property("deleteInFlight", in_flight);
    update_rename_action();
    publish_activity();
}

void FileCommandController::Impl::set_transfer_in_flight(const bool in_flight) {
    if (in_flight) stop_selected_animation();
    transferInFlight_ = in_flight;
    set_property("fileTransferInFlight", in_flight);
    update_rename_action();
    publish_activity();
}

void FileCommandController::Impl::set_create_directory_in_flight(const bool in_flight) {
    createDirectoryInFlight_ = in_flight;
    set_property("createDirectoryInFlight", in_flight);
    update_rename_action();
    publish_activity();
}

OperationAvailability FileCommandController::Impl::operation_availability() const {
    return {
        .mutation_busy = renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
            createDirectoryInFlight_ || fileOperationService_.busy() ||
            batchRenameCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
            trashCoordinator_.busy() || fileTransferCoordinator_.busy() ||
            directoryTransferService_.busy(),
        .pending_rename = pendingRenameOperation_.has_value(),
        .external_drop_pending = pendingExternalDirectoryDrop_.has_value() ||
                                 pendingExternalTransferDrop_.has_value(),
        .drop_probe_idle = dropProbeSource_.idle(),
        .trash_confirmation = trashConfirmationInProgress_,
        .batch_recovery = batchRenameCoordinator_.recovery_pending(),
        .delete_recovery = permanentDeleteCoordinator_.recovery_pending(),
        .transfer_recovery = fileTransferCoordinator_.recovery_pending(),
        .trash_recovery = trashCoordinator_.recovery_pending(),
        .directory_recovery = property("directoryTransferRecoveryPending").toBool(),
        .journal_blocked = property("fileOperationJournalBlocked").toBool(),
        .unfinished_journal = property("trashRecoveryPending").toBool() ||
            property("batchRenameRecoveryPending").toBool() ||
            property("permanentDeleteRecoveryPending").toBool() ||
            property("fileTransferRecoveryPending").toBool() ||
            property("directoryTransferRecoveryPending").toBool() ||
            property("fileOperationJournalBlocked").toBool(),
        .trash_view = catalog().trash_view,
        .global_search = catalog().global_search,
        .recursive_view = catalog().recursive_view,
        .catalog_ready = catalog().ready,
    };
}

FileCommandCatalogState FileCommandController::Impl::catalog() const { return ports_.catalog(); }

QList<core::DirectoryEntry> FileCommandController::Impl::selected_entries_in_view_order() const {
    QList<core::DirectoryEntry> entries;
    const auto snapshot = ports_.selection();
    entries.reserve(static_cast<qsizetype>(snapshot.records().size()));
    for (const auto &record : snapshot.records()) {
        entries.push_back({.id = record.id, .kind = record.kind,
            .state = core::EntryState::metadata_ready, .name_utf8 = record.name_utf8,
            .search_key_utf8 = {}, .path_utf8 = record.original_path_utf8,
            .source_revision_utf8 = record.source_revision_utf8, .size_bytes = record.size_bytes,
            .modified_unix_ns = record.modified_unix_ns});
    }
    return entries;
}

bool FileCommandController::Impl::mutation_in_flight() const {
    return renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_;
}
bool FileCommandController::Impl::catalog_tools_busy() const {
    return mutation_in_flight() || pendingExternalDirectoryDrop_ || pendingExternalTransferDrop_ ||
           trashConfirmationInProgress_;
}
bool FileCommandController::Impl::backend_busy() const {
    return fileOperationService_.busy() || batchRenameCoordinator_.busy() ||
           permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
           fileTransferCoordinator_.busy() || directoryTransferService_.busy() ||
           objectTransferQueue_.busy();
}
bool FileCommandController::Impl::automatic_refresh_blocked() const {
    return mutation_in_flight() || pendingTransferPreparation_ || pendingDirectoryTransferBatch_ ||
           pendingCreateDirectoryPreparation_ || pendingRenameOperation_ || backend_busy();
}
bool FileCommandController::Impl::can_close() const {
    return !mutation_in_flight() && !backend_busy();
}
bool FileCommandController::Impl::trash_cleanup_allowed() const {
    return !deleteInFlight_ && !trashCoordinator_.busy();
}
bool FileCommandController::Impl::readers_idle() const { return ports_.readers_idle(); }
bool FileCommandController::Impl::recovery_pending() const {
    return property("directoryTransferRecoveryPending").toBool() ||
           property("fileTransferRecoveryPending").toBool() || property("trashRecoveryPending").toBool() ||
           property("permanentDeleteRecoveryPending").toBool() || property("batchRenameRecoveryPending").toBool();
}
void FileCommandController::Impl::publish_status() {
    ports_.status_changed(operationStatus_);
}
void FileCommandController::Impl::publish_activity() { ports_.activity_changed(); }
void FileCommandController::Impl::set_property(const char *name, const QVariant &value) {
    QObject::setProperty(name, value);
    ports_.diagnostic_changed(name, value);
}
void FileCommandController::Impl::begin_preview_generation(bool clear) { ports_.begin_preview_generation(clear); }
void FileCommandController::Impl::stop_selected_animation() { ports_.stop_animation(); }
void FileCommandController::Impl::refresh_directory_sources() { ports_.refresh_directory_sources(); }
void FileCommandController::Impl::start_trash_catalog_load(bool discover) { ports_.load_trash_catalog(discover); }
void FileCommandController::Impl::refresh_after_rename(const QString &path) { ports_.refresh_after_rename(path); }
void FileCommandController::Impl::refresh_after_batch_rename(const QStringList &paths) { ports_.refresh_after_batch_rename(paths); }
void FileCommandController::Impl::open_path(const QString &path, bool history) { ports_.open_path(path, history); }
void FileCommandController::Impl::open_external_paths(const QStringList &paths) { ports_.open_external_paths(paths); }
void FileCommandController::Impl::open_external_items_in_parent(const QStringList &paths) { ports_.open_external_items_in_parent(paths); }
void FileCommandController::Impl::add_favorite(const QString &path) { ports_.pin_path(path); }
void FileCommandController::Impl::allow_observed_global_search_path(const QString &path) { ports_.observe_search_path(path); }
void FileCommandController::Impl::reconcile_global_search_move(const QStringList &paths) { ports_.reconcile_search_move(paths); }
void FileCommandController::Impl::schedule_global_search() { ports_.schedule_search(); }
void FileCommandController::Impl::poll_catalog(int interval) { ports_.poll_catalog(interval); }
void FileCommandController::Impl::resume_preview_dispatch(int delay) { ports_.resume_preview_dispatch(delay); }

void FileCommandController::Impl::notify_file_in_use(fileops::OperationStatus status) {
    if (status == fileops::OperationStatus::file_in_use) ports_.file_in_use();
}

void FileCommandController::Impl::update_rename_action() {
    const auto entries = selected_entries_in_view_order();
    const auto availability = operation_availability();
    const auto same_parent = entries.size() < 2 || std::ranges::all_of(entries, [&](const auto &entry) {
        return same_directory_path(QFileInfo(QString::fromUtf8(entry.path_utf8)).absolutePath(),
            QFileInfo(QString::fromUtf8(entries.front().path_utf8)).absolutePath());
    });
    const auto all_objects = !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
    });
    FileCommandActionState state;
    state.availability = availability;
    state.rename_text = entries.size() > 1 ? QCoreApplication::translate("MainWindow", "Batch rename")
                                          : QCoreApplication::translate("MainWindow", "Rename");
    state.rename_tooltip = same_parent ? QString{} : QCoreApplication::translate(
        "MainWindow", "Batch renaming requires items from one folder");
    state.delete_text = entries.size() > 1 ? QCoreApplication::translate("MainWindow", "Delete Objects…")
        : entries.size() == 1 && entries.front().kind == core::EntryKind::directory
            ? QCoreApplication::translate("MainWindow", "Delete Folder…")
            : QCoreApplication::translate("MainWindow", "Delete…");
    state.rename_enabled = all_objects && same_parent && availability.selection_mutation_allowed();
    state.delete_enabled = all_objects && availability.selection_mutation_allowed();
    state.trash_enabled = availability.trash_catalog_allowed();
    state.unfinished_enabled = availability.unfinished_operation_allowed();
    state.create_directory_enabled = availability.create_directory_allowed();
    ports_.actions_changed(state);
}

bool FileCommandController::Impl::refresh_or_recover() {
if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        batchRenameCoordinator_.busy() || fileTransferCoordinator_.busy() ||
        directoryTransferService_.busy() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Waiting for the file operation result…");
        publish_status();
        return true;
    }
    const auto journal_state = refresh_current_operation_journal_state();
    if (journal_state == CurrentOperationJournalState::blocked) {
        publish_status();
        return true;
    }
    if (journal_state == CurrentOperationJournalState::directory_transfer_recovery) {
        offer_directory_transfer_discard();
        return true;
    }
    if (trashCoordinator_.owns_recovery()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Recovering VO-VE Trash operation…");
        publish_status();
        if (!submit_trash_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start VO-VE Trash recovery");
            publish_status();
        }
        return true;
    }
    if (journal_state == CurrentOperationJournalState::transfer_recovery &&
        !fileTransferCoordinator_.owns_recovery()) {
        static_cast<void>(start_object_transfer({}, {}, fileops::FileTransferKind::copy, true));
        return true;
    }
    if (fileTransferCoordinator_.owns_recovery()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Recovering copy or move…");
        publish_status();
        if (!submit_file_transfer_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start transfer recovery");
            publish_status();
        }
        return true;
    }
    if (permanentDeleteCoordinator_.owns_recovery()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Recovering safe deletion…");
        publish_status();
        if (!submit_delete_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start deletion recovery");
            publish_status();
        }
        return true;
    }
    if (batchRenameCoordinator_.owns_recovery()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Recovering batch rename…");
        publish_status();
        if (!submit_batch_rename_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start name recovery");
            publish_status();
        }
        return true;
    }
    if (pendingRenameOperation_) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Checking rename result…");
        publish_status();
        if (!submit_pending_rename_reconciliation()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start result verification");
            publish_status();
        }
        return true;
    }

    return false;
}

void FileCommandController::Impl::load_recovery_state() {
    const auto sealed_state = ports_.read_recovery();
    const auto decoded_state = open_directory_transfer_state(sealed_state);
    directoryTransferStateCorrupt_ = !sealed_state.isEmpty() && !decoded_state;
    if (decoded_state) {
        const auto &state = *decoded_state;
        bool operation_ok{};
        const auto operation_id =
            state.value(QStringLiteral("operationId")).toString().toULongLong(&operation_ok);
        const auto source = state.value(QStringLiteral("source")).toString();
        const auto destination = state.value(QStringLiteral("destination")).toString();
        const auto revision = state.value(QStringLiteral("sourceRevision")).toString();
        const auto manifest = state.value(QStringLiteral("manifest")).toString();
        auto state_valid = operation_ok && operation_id != 0U &&
                           native_path(source).is_absolute() &&
                           native_path(destination).is_absolute() && !revision.isEmpty() &&
                           (manifest.isEmpty() || native_path(manifest).is_absolute()) &&
                           state.value(QStringLiteral("completionKnown")).isBool() &&
                           state.value(QStringLiteral("discarding")).isBool();
        std::optional<PendingDirectoryTransferBatch> restored_batch;
        if (state_valid && state.contains(QStringLiteral("batch"))) {
            const auto batch = state.value(QStringLiteral("batch")).toObject();
            const auto sources_json = batch.value(QStringLiteral("sources")).toArray();
            const auto revisions_json = batch.value(QStringLiteral("revisions")).toArray();
            const auto batch_kind = batch.value(QStringLiteral("kind")).toInt(-1);
            const auto batch_index = batch.value(QStringLiteral("index")).toInteger(-1);
            const auto batch_completed = batch.value(QStringLiteral("completed")).toInteger(-1);
            const auto batch_destination_text =
                batch.value(QStringLiteral("destination")).toString();
            const auto batch_destination = native_path(batch_destination_text);
            const auto kind_valid =
                batch_kind == static_cast<int>(fileops::FileTransferKind::copy) ||
                batch_kind == static_cast<int>(fileops::FileTransferKind::move);
            state_valid = kind_valid && !sources_json.isEmpty() &&
                          static_cast<std::size_t>(sources_json.size()) <=
                              fileops::kMaximumDirectoryTransferRoots &&
                          sources_json.size() == revisions_json.size() &&
                          batch_destination.is_absolute() && batch_index >= 0 &&
                          batch_index < sources_json.size() && batch_completed >= 0 &&
                          batch_completed <= batch_index;
            QStringList batch_sources;
            std::vector<std::string> batch_revisions;
            batch_sources.reserve(sources_json.size());
            batch_revisions.reserve(static_cast<std::size_t>(revisions_json.size()));
            for (qsizetype index{}; state_valid && index < sources_json.size(); ++index) {
                const auto source_value = sources_json.at(index);
                const auto revision_value = revisions_json.at(index);
                const auto source_text = source_value.toString();
                const auto revision_text = revision_value.toString();
                const auto source_path = native_path(source_text);
                state_valid = source_value.isString() && revision_value.isString() &&
                              source_path.is_absolute() && !source_path.filename().empty() &&
                              !revision_text.isEmpty();
                batch_sources.push_back(source_text);
                batch_revisions.push_back(revision_text.toUtf8().toStdString());
            }
            if (state_valid) {
                const auto current_source = native_path(batch_sources.at(batch_index));
                state_valid =
                    current_source.lexically_normal() == native_path(source).lexically_normal() &&
                    (batch_destination / current_source.filename()).lexically_normal() ==
                        native_path(destination).lexically_normal() &&
                    QString::fromUtf8(batch_revisions.at(static_cast<std::size_t>(batch_index))) ==
                        revision;
            }
            if (state_valid) {
                restored_batch = PendingDirectoryTransferBatch{
                    .kind = static_cast<fileops::FileTransferKind>(batch_kind),
                    .source_paths = batch_sources,
                    .source_revisions_utf8 = std::move(batch_revisions),
                    .destination_directory = batch_destination,
                    .index = static_cast<qsizetype>(batch_index),
                    .completed = static_cast<qsizetype>(batch_completed),
                };
            }
        }
        if (state_valid) {
            directoryTransferRecoveryRequestId_ = operation_id;
            directoryTransferRecoverySource_ = source;
            directoryTransferRecoveryDestination_ = destination;
            directoryTransferRecoverySourceRevision_ = revision;
            directoryTransferRecoveryManifest_ = manifest;
            directoryTransferCompletionKnown_ =
                state.value(QStringLiteral("completionKnown")).toBool();
            directoryTransferDiscarding_ = state.value(QStringLiteral("discarding")).toBool();
            pendingDirectoryTransferBatch_ = std::move(restored_batch);
        } else {
            directoryTransferStateCorrupt_ = true;
        }
    }

}

void FileCommandController::Impl::queue_external_open(QStringList paths) {
    queuedExternalOpenPaths_ = std::move(paths);
    if (pendingExternalDirectoryDrop_ &&
        pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open)
        dropProbeSource_.cancel(pendingExternalDirectoryDrop_->generation);
    operationStatus_ = QCoreApplication::translate(
        "MainWindow", "Wait for the current dragged-object check to finish");
    publish_status();
}

void FileCommandController::Impl::cancel_search_drop() {
    if (pendingExternalTransferDrop_ && pendingExternalTransferDrop_->search_generation != 0)
        fail_external_transfer_drop(QCoreApplication::translate(
            "MainWindow", "Transfer cancelled. Objects transferred: %1").arg(0));
}

bool FileCommandController::Impl::start_trash_purge(std::vector<std::filesystem::path> manifests) {
    if (manifests.empty() || !trash_cleanup_allowed()) return false;
    trashPurgeQueue_ = std::move(manifests);
    trashPurgeCompleted_ = 0;
    trashPurgeTotal_ = trashPurgeQueue_.size();
    if (submit_next_trash_purge()) return true;
    trashPurgeQueue_.clear();
    trashPurgeTotal_ = 0;
    return false;
}

FileCommandController::FileCommandController(FileCommandPorts ports, FileCommandOptions options, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(std::move(ports), std::move(options), this)) {
    setObjectName(QStringLiteral("fileCommandController"));
}
FileCommandController::~FileCommandController() = default;
OperationAvailability FileCommandController::availability() const { return impl_->operation_availability(); }
bool FileCommandController::busy() const { return mutation_in_flight() || impl_->backend_busy(); }
bool FileCommandController::mutation_in_flight() const { return impl_->mutation_in_flight(); }
bool FileCommandController::catalog_tools_busy() const { return impl_->catalog_tools_busy(); }
bool FileCommandController::input_preparation_pending() const {
    return impl_->pendingExternalDirectoryDrop_ || impl_->pendingExternalTransferDrop_ ||
           impl_->pendingDirectoryPreparation_;
}
bool FileCommandController::automatic_refresh_blocked() const { return impl_->automatic_refresh_blocked(); }
bool FileCommandController::recovery_pending() const { return impl_->recovery_pending(); }
bool FileCommandController::journal_blocked() const { return impl_->property("fileOperationJournalBlocked").toBool(); }
bool FileCommandController::readers_idle() const { return impl_->readers_idle(); }
bool FileCommandController::trash_cleanup_allowed() const { return impl_->trash_cleanup_allowed(); }
void FileCommandController::set_trash_maximum_bytes(std::uint64_t maximum_bytes) {
    impl_->trashCoordinator_.set_maximum_bytes(maximum_bytes);
}
bool FileCommandController::can_paste_objects() const {
    const auto state = availability();
    return state.create_directory_allowed() && state.catalog_ready && state.drop_idle() &&
           !state.trash_confirmation && !impl_->pendingDirectoryPreparation_;
}
bool FileCommandController::drop_transfer_allowed() const {
    return !busy() && !recovery_pending() && !journal_blocked();
}
bool FileCommandController::external_drop_validation_allowed() const {
    return !input_preparation_pending() && impl_->dropProbeSource_.idle();
}
bool FileCommandController::can_close() const { return impl_->can_close(); }
bool FileCommandController::refresh_or_recover() { return impl_->refresh_or_recover(); }
CurrentOperationJournalState FileCommandController::refresh_current_operation_journal_state() { return impl_->refresh_current_operation_journal_state(); }
void FileCommandController::update_actions() { impl_->update_rename_action(); }
void FileCommandController::load_recovery_state() { impl_->load_recovery_state(); }
bool FileCommandController::save_recovery_state() { return impl_->persist_directory_transfer_recovery_state(); }
const std::filesystem::path &FileCommandController::journal_path() const { return impl_->currentOperationJournalPath_; }
const std::filesystem::path &FileCommandController::trash_manifest_directory() const { return impl_->trashManifestDirectory_; }
QString FileCommandController::status() const { return impl_->operationStatus_; }
void FileCommandController::clear_completed_status() {
    if (busy()) return;
    impl_->operationStatus_.clear();
    impl_->publish_status();
}
void FileCommandController::paste_objects() {
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        impl_->publish_status();
        return;
    }
    if (!can_paste_objects()) return;
    const auto paths = local_file_paths(QApplication::clipboard()->mimeData());
    if (!paths) {
        impl_->operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Clipboard does not contain valid file paths");
        impl_->publish_status();
        return;
    }
    static_cast<void>(begin_external_transfer_drop(*paths, impl_->catalog().current_path,
        fileops::FileTransferKind::copy, false, true));
}
void FileCommandController::prompt_transfer_selected(fileops::FileTransferKind kind) { impl_->prompt_transfer_selected(kind); }
void FileCommandController::prompt_rename_selected() { impl_->prompt_rename_selected(); }
void FileCommandController::prompt_delete_selected() { impl_->prompt_delete_selected(); }
void FileCommandController::prompt_create_directory() { impl_->prompt_create_directory(); }
void FileCommandController::show_unfinished_operation_dialog() { impl_->show_unfinished_operation_dialog(); }
bool FileCommandController::start_transfer_selection(const QList<core::DirectoryEntry> &entries, const QString &destination, fileops::FileTransferKind kind) { return impl_->start_transfer_selection(entries, destination, kind); }
bool FileCommandController::start_object_transfer(const QList<core::DirectoryEntry> &entries, const QString &destination, fileops::FileTransferKind kind, bool resume) { return impl_->start_object_transfer(entries, destination, kind, resume); }
bool FileCommandController::start_file_transfer(const QList<core::DirectoryEntry> &entries, const QString &destination, fileops::FileTransferKind kind) { return impl_->start_file_transfer(entries, destination, kind); }
bool FileCommandController::start_directory_transfer(const QList<core::DirectoryEntry> &entries, const QString &destination, fileops::FileTransferKind kind) { return impl_->start_directory_transfer(entries, destination, kind); }
bool FileCommandController::start_directory_transfer_paths(QStringList paths, std::vector<std::string> revisions, const QString &destination, fileops::FileTransferKind kind) { return impl_->start_directory_transfer_paths(std::move(paths), std::move(revisions), destination, kind); }
bool FileCommandController::begin_external_directory_drop(ExternalDirectoryDropIntent intent, QStringList paths, fileops::FileTransferKind kind, QString destination) { return impl_->begin_external_directory_drop(intent, std::move(paths), kind, std::move(destination)); }
bool FileCommandController::begin_external_transfer_drop(QStringList paths, const QString &destination, fileops::FileTransferKind kind, bool trash_target, bool clipboard_copy, QList<core::DirectoryEntry> search_entries) { return impl_->begin_external_transfer_drop(std::move(paths), destination, kind, trash_target, clipboard_copy, std::move(search_entries)); }
void FileCommandController::request_external_open(QStringList paths) {
    if (paths.isEmpty()) return;
    if (!external_drop_validation_allowed()) {
        queue_external_open(std::move(paths));
        return;
    }
    if (paths.size() == 1) {
        if (!begin_external_directory_drop(ExternalDirectoryDropIntent::open, std::move(paths))) {
            impl_->operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Wait for the current dragged-object check to finish");
            impl_->publish_status();
        }
        return;
    }
    impl_->open_external_items_in_parent(paths);
}
void FileCommandController::queue_external_open(QStringList paths) { impl_->queue_external_open(std::move(paths)); }
void FileCommandController::cancel_search_drop() { impl_->cancel_search_drop(); }
bool FileCommandController::resume_queued_external_open() { return impl_->resume_queued_external_open(); }
bool FileCommandController::prompt_move_entries_to_trash(const QList<core::DirectoryEntry> &entries) { return impl_->prompt_move_entries_to_trash(entries); }
bool FileCommandController::start_trash_manifest_operation(const std::filesystem::path &manifest, TrashAction action, bool confirm) { return impl_->start_trash_manifest_operation(manifest, action, confirm); }
bool FileCommandController::start_trash_purge(std::vector<std::filesystem::path> manifests) { return impl_->start_trash_purge(std::move(manifests)); }
bool FileCommandController::submit_batch_rename_recovery() { return impl_->submit_batch_rename_recovery(); }
bool FileCommandController::submit_delete_recovery() { return impl_->submit_delete_recovery(); }
bool FileCommandController::submit_trash_recovery(bool cancel) { return impl_->submit_trash_recovery(cancel); }
bool FileCommandController::submit_file_transfer_recovery() { return impl_->submit_file_transfer_recovery(); }
bool FileCommandController::submit_directory_transfer_recovery() { return impl_->submit_directory_transfer_recovery(); }
bool FileCommandController::submit_pending_rename_reconciliation() { return impl_->submit_pending_rename_reconciliation(); }
void FileCommandController::offer_directory_transfer_discard() { impl_->offer_directory_transfer_discard(); }
#ifdef VOVE_UI_TEST_HOOKS
void FileCommandController::test_deliver_trash_result(const fileops::TrashResult &result, TrashOperationIntent intent) { impl_->handle_trash_result(result, intent); }
bool FileCommandController::test_preparation_idle() const { return !impl_->pendingDirectoryPreparation_ && !impl_->renameInFlight_ && !impl_->deleteInFlight_; }
void FileCommandController::test_set_in_flight(bool rename, bool deleting, bool transfer, bool create) {
    impl_->set_rename_in_flight(rename);
    impl_->set_delete_in_flight(deleting);
    impl_->set_transfer_in_flight(transfer);
    impl_->set_create_directory_in_flight(create);
}
QByteArray FileCommandController::test_seal_directory_transfer_payload(const QByteArray &payload) { return seal_directory_transfer_payload(payload); }
#endif

} // namespace vove::ui
