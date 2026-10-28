#include "file_command_workflow.hpp"

namespace vove::ui {
using namespace file_command_detail;

void FileCommandController::Impl::poll_transfer_preparation() {
    if (pendingCreateDirectoryPreparation_) {
        poll_create_directory_preparation();
        return;
    }
    if (!pendingTransferPreparation_) {
        transferProbeTimer_->stop();
        return;
    }
    for (int index{}; index < 32; ++index) {
        auto batch = transferProbeSource_.poll();
        if (!batch) {
            break;
        }
        const auto target = std::ranges::find(pendingTransferPreparation_->probes,
                                              batch->generation, &TransferProbeTarget::generation);
        if (target == pendingTransferPreparation_->probes.end() || target->complete) {
            continue;
        }
        if (batch->error) {
            QString message;
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                message = QCoreApplication::translate("MainWindow",
                                                      "The folder requires network sign-in");
                break;
            case catalog::CatalogErrorKind::permission_denied:
                message = QCoreApplication::translate("MainWindow",
                                                      "Cannot access the destination folder");
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                message = QCoreApplication::translate(
                    "MainWindow", "Network unavailable. Transfer was not started");
                break;
            case catalog::CatalogErrorKind::not_found:
                message = QCoreApplication::translate("MainWindow", "Destination folder not found");
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                message = QCoreApplication::translate("MainWindow", "Failed to check the folder");
                break;
            }
            fail_transfer_preparation(message);
            return;
        }
        if (batch->is_final) {
            if (batch->directory_revision_utf8.empty()) {
                fail_transfer_preparation(
                    QCoreApplication::translate("MainWindow", "Failed to verify folder identity"));
                return;
            }
            target->revision_utf8 = std::move(batch->directory_revision_utf8);
            target->complete = true;
            const auto next = std::ranges::find(pendingTransferPreparation_->probes, false,
                                                &TransferProbeTarget::complete);
            if (next != pendingTransferPreparation_->probes.end()) {
                transferProbeSource_.submit(
                    {.generation = next->generation, .path = next->path, .maximum_entries = 1});
            }
        }
    }
    if (std::ranges::all_of(pendingTransferPreparation_->probes, &TransferProbeTarget::complete)) {
        launch_prepared_file_transfer();
    }
}

void FileCommandController::Impl::poll_create_directory_preparation() {
    if (!pendingCreateDirectoryPreparation_) {
        transferProbeTimer_->stop();
        return;
    }
    for (int index{}; index < 32; ++index) {
        auto batch = transferProbeSource_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != pendingCreateDirectoryPreparation_->generation) {
            continue;
        }
        if (batch->error) {
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "The folder requires network sign-in");
                break;
            case catalog::CatalogErrorKind::permission_denied:
                operationStatus_ =
                    QCoreApplication::translate("MainWindow", "Cannot access the current folder");
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "Network unavailable. Folder was not created");
                break;
            case catalog::CatalogErrorKind::not_found:
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "The current folder no longer exists");
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                operationStatus_ =
                    QCoreApplication::translate("MainWindow", "Failed to check the current folder");
                break;
            }
            pendingCreateDirectoryPreparation_.reset();
            transferProbeTimer_->stop();
            set_create_directory_in_flight(false);
            publish_status();
            return;
        }
        if (!batch->is_final) {
            continue;
        }
        if (batch->directory_revision_utf8.empty()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to verify the current folder");
            pendingCreateDirectoryPreparation_.reset();
            transferProbeTimer_->stop();
            set_create_directory_in_flight(false);
            publish_status();
            return;
        }

        const auto destination = pendingCreateDirectoryPreparation_->destination;
        pendingCreateDirectoryPreparation_.reset();
        transferProbeTimer_->stop();
        static std::atomic_uint64_t next_operation{1};
        fileops::CreateDirectoryRequest request{
            .operation_id = next_operation.fetch_add(1, std::memory_order_relaxed),
            .destination = destination,
            .destination_parent_revision_utf8 = std::move(batch->directory_revision_utf8)};
        const auto destination_text = display_path(destination);
        operationStatus_ = QCoreApplication::translate("MainWindow", "Creating folder…");
        publish_status();
        const QPointer<Impl> window(this);
        if (!fileOperationService_.submit_create_directory(
                std::move(request),
                [window, destination_text](fileops::OperationResult result) mutable {
                    if (!window.isNull()) {
                        QMetaObject::invokeMethod(
                            window,
                            [window, destination_text, result = std::move(result)]() mutable {
                                if (!window.isNull()) {
                                    window->handle_create_directory_result(result,
                                                                           destination_text);
                                }
                            },
                            Qt::QueuedConnection);
                    }
                })) {
            set_create_directory_in_flight(false);
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Another file operation is still running");
            publish_status();
        }
        return;
    }
}

bool FileCommandController::Impl::begin_external_directory_drop(const ExternalDirectoryDropIntent intent,
                                               QStringList paths,
                                               const fileops::FileTransferKind transfer_kind,
                                               QString transfer_destination) {
    if (paths.isEmpty() || pendingExternalDirectoryDrop_ || pendingExternalTransferDrop_ ||
        pendingDirectoryPreparation_ || !dropProbeSource_.idle() ||
        (intent == ExternalDirectoryDropIntent::open && paths.size() != 1) ||
        (intent == ExternalDirectoryDropIntent::transfer && transfer_destination.isEmpty())) {
        return false;
    }

    PendingExternalDirectoryDrop pending;
    pending.intent = intent;
    pending.paths = std::move(paths);
    pending.transfer_kind = transfer_kind;
    pending.transfer_destination = std::move(transfer_destination);
    pending.generation = nextDropProbeGeneration_++;
    const auto generation = pending.generation;
    const auto first_path = native_path(pending.paths.front());
    pendingExternalDirectoryDrop_ = std::move(pending);
    update_rename_action();
    operationStatus_ =
        intent == ExternalDirectoryDropIntent::pin
            ? QCoreApplication::translate("MainWindow", "Checking folders for Favorites…")
        : intent == ExternalDirectoryDropIntent::transfer
            ? (transfer_kind == fileops::FileTransferKind::move
                   ? QCoreApplication::translate("MainWindow", "Checking folders for moving…")
                   : QCoreApplication::translate("MainWindow", "Checking folders for copying…"))
            : QCoreApplication::translate("MainWindow", "Checking folder…");
    publish_status();
    dropProbeSource_.submit({.generation = generation, .path = first_path, .maximum_entries = 1});
    dropProbeTimer_->start();
    return true;
}

bool FileCommandController::Impl::begin_external_transfer_drop(QStringList paths,
                                              const QString &destination_directory,
                                              const fileops::FileTransferKind transfer_kind,
                                              const bool trash_target,
                                              const bool clipboard_copy,
                                              QList<core::DirectoryEntry> search_entries) {
    if (paths.isEmpty() ||
        static_cast<std::size_t>(paths.size()) > fileops::kMaximumFileTransferItems ||
        (!search_entries.isEmpty() && search_entries.size() != paths.size()) ||
        (!trash_target && destination_directory.trimmed().isEmpty()) ||
        pendingExternalDirectoryDrop_ || pendingExternalTransferDrop_ ||
        pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        return false;
    }

    PendingExternalTransferDrop pending;
    pending.paths = std::move(paths);
    pending.transfer_kind = transfer_kind;
    pending.transfer_destination = destination_directory;
    pending.trash_target = trash_target;
    pending.clipboard_copy = clipboard_copy;
    pending.search_entries = std::move(search_entries);
    pending.search_generation = pending.search_entries.isEmpty() ? 0 : catalog().search_generation;
    pending.search_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (const auto &path : std::as_const(pending.paths)) {
        const auto parent = native_path(path).parent_path();
        if (parent.empty()) {
            return false;
        }
    }

    pending.generation = nextDropProbeGeneration_++;
    const auto generation = pending.generation;
    const auto first_path = native_path(pending.paths.front());
    pendingExternalTransferDrop_ = std::move(pending);
    update_rename_action();
    operationStatus_ =
        trash_target
            ? QCoreApplication::translate("MainWindow", "Checking objects for VO-VE Trash…")
        : transfer_kind == fileops::FileTransferKind::move
            ? QCoreApplication::translate("MainWindow", "Checking objects before moving…")
            : QCoreApplication::translate("MainWindow", "Checking objects before copying…");
    publish_status();
    dropProbeSource_.submit({.generation = generation,
                             .path = first_path.parent_path(),
                             .maximum_entries = 1,
                             .exact_entry_path = first_path});
    dropProbeTimer_->start();
    return true;
}

void FileCommandController::Impl::poll_external_drop() {
    if (pendingDirectoryPreparation_) {
        poll_directory_preparation();
    } else if (pendingExternalTransferDrop_) {
        poll_external_transfer_drop();
    } else if (pendingExternalDirectoryDrop_) {
        poll_external_directory_drop();
    } else if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
        resume_queued_external_open();
    }
    update_rename_action();
}

void FileCommandController::Impl::poll_external_directory_drop() {
    if (!pendingExternalDirectoryDrop_) {
        if (dropProbeSource_.idle()) {
            dropProbeTimer_->stop();
        }
        return;
    }
    for (int batch_index{}; batch_index < 32; ++batch_index) {
        auto batch = dropProbeSource_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != pendingExternalDirectoryDrop_->generation) {
            continue;
        }
        if (batch->error) {
            if (pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open &&
                batch->error.kind == catalog::CatalogErrorKind::cancelled &&
                !queuedExternalOpenPaths_.isEmpty()) {
                pendingExternalDirectoryDrop_.reset();
                if (dropProbeSource_.idle()) {
                    dropProbeTimer_->stop();
                    resume_queued_external_open();
                }
                return;
            }
            if (pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open &&
                batch->error.kind == catalog::CatalogErrorKind::io_error) {
                auto paths = std::move(pendingExternalDirectoryDrop_->paths);
                pendingExternalDirectoryDrop_.reset();
                if (dropProbeSource_.idle()) {
                    dropProbeTimer_->stop();
                }
                operationStatus_.clear();
                if (!resume_queued_external_open()) {
                    open_external_items_in_parent(paths);
                }
                return;
            }
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                fail_external_directory_drop(QCoreApplication::translate(
                    "MainWindow", "The folder requires network sign-in"));
                break;
            case catalog::CatalogErrorKind::permission_denied:
                fail_external_directory_drop(
                    QCoreApplication::translate("MainWindow", "Cannot access the folder"));
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                fail_external_directory_drop(QCoreApplication::translate(
                    "MainWindow", "The network folder did not respond"));
                break;
            case catalog::CatalogErrorKind::not_found:
                fail_external_directory_drop(
                    QCoreApplication::translate("MainWindow", "The dragged folder was not found"));
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                fail_external_directory_drop(
                    QCoreApplication::translate("MainWindow", "Only folders can be dragged here"));
                break;
            }
            return;
        }
        if (!batch->is_final) {
            continue;
        }
        if (batch->directory_revision_utf8.empty()) {
            fail_external_directory_drop(
                QCoreApplication::translate("MainWindow", "Only folders can be dragged here"));
            return;
        }

        pendingExternalDirectoryDrop_->confirmed_paths.push_back(
            pendingExternalDirectoryDrop_->paths.at(pendingExternalDirectoryDrop_->index));
        pendingExternalDirectoryDrop_->confirmed_revisions_utf8.push_back(
            std::move(batch->directory_revision_utf8));
        ++pendingExternalDirectoryDrop_->index;
        pendingExternalDirectoryDrop_->awaiting_worker_reap = true;
        break;
    }

    if (pendingExternalDirectoryDrop_ &&
        pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open &&
        !queuedExternalOpenPaths_.isEmpty() && dropProbeSource_.idle()) {
        pendingExternalDirectoryDrop_.reset();
        dropProbeTimer_->stop();
        resume_queued_external_open();
        return;
    }

    if (!pendingExternalDirectoryDrop_ || !pendingExternalDirectoryDrop_->awaiting_worker_reap ||
        !dropProbeSource_.idle()) {
        return;
    }
    pendingExternalDirectoryDrop_->awaiting_worker_reap = false;
    if (pendingExternalDirectoryDrop_->index < pendingExternalDirectoryDrop_->paths.size()) {
        const auto index = pendingExternalDirectoryDrop_->index;
        const auto generation = nextDropProbeGeneration_++;
        const auto next_path = native_path(pendingExternalDirectoryDrop_->paths.at(index));
        pendingExternalDirectoryDrop_->generation = generation;
        dropProbeSource_.submit(
            {.generation = generation, .path = next_path, .maximum_entries = 1});
        return;
    }

    auto completed = std::move(*pendingExternalDirectoryDrop_);
    pendingExternalDirectoryDrop_.reset();
    if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
    } else {
        dropProbeTimer_->start();
    }
    if (completed.intent == ExternalDirectoryDropIntent::open) {
        operationStatus_.clear();
        if (!resume_queued_external_open()) {
            open_path(completed.confirmed_paths.front(), true);
        }
    } else if (completed.intent == ExternalDirectoryDropIntent::pin) {
        for (const auto &path : completed.confirmed_paths) {
            add_favorite(path);
        }
        operationStatus_ = QCoreApplication::translate("MainWindow", "Added to Favorites: %1")
                               .arg(completed.confirmed_paths.size());
        publish_status();
    } else if (!start_directory_transfer_paths(std::move(completed.confirmed_paths),
                                               std::move(completed.confirmed_revisions_utf8),
                                               completed.transfer_destination,
                                               completed.transfer_kind)) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to start folder transfer");
        publish_status();
    }
    resume_queued_external_open();
}

void FileCommandController::Impl::poll_external_transfer_drop() {
    if (!pendingExternalTransferDrop_) {
        return;
    }
    if (pendingExternalTransferDrop_->search_generation != 0 &&
        (!catalog().global_search || !catalog().search_ready ||
         !catalog().search_query_current ||
         pendingExternalTransferDrop_->search_generation != catalog().search_generation)) {
        fail_external_transfer_drop(QCoreApplication::translate("MainWindow", "Transfer cancelled. Objects transferred: %1").arg(0));
        return;
    }
    if (pendingExternalTransferDrop_->search_generation != 0 &&
        std::chrono::steady_clock::now() >= pendingExternalTransferDrop_->search_deadline) {
        fail_external_transfer_drop(QCoreApplication::translate(
            "MainWindow", "A dragged object was not found or could not be checked"));
        return;
    }
    for (int batch_index{}; batch_index < 32; ++batch_index) {
        auto batch = dropProbeSource_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != pendingExternalTransferDrop_->generation) {
            continue;
        }
        if (batch->error) {
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                fail_external_transfer_drop(QCoreApplication::translate(
                    "MainWindow", "The source folder requires network sign-in"));
                break;
            case catalog::CatalogErrorKind::permission_denied:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "Cannot access the source folder"));
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "The source folder did not respond"));
                break;
            case catalog::CatalogErrorKind::not_found:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "The source folder was not found"));
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "Failed to check dragged objects"));
                break;
            }
            return;
        }

        for (const auto &entry : batch->entries) {
            const auto expected_path = native_path(
                pendingExternalTransferDrop_->paths.at(pendingExternalTransferDrop_->index));
            if (transfer_path_key(native_path(QString::fromUtf8(entry.path_utf8))) ==
                transfer_path_key(expected_path)) {
                if (!pendingExternalTransferDrop_->search_entries.isEmpty() &&
                    (entry.kind != pendingExternalTransferDrop_->search_entries.at(
                         pendingExternalTransferDrop_->index).kind || entry.source_revision_utf8.empty())) {
                    fail_external_transfer_drop(QCoreApplication::translate(
                        "MainWindow", "A dragged object was not found or could not be checked"));
                    return;
                }
                pendingExternalTransferDrop_->confirmed_entries.push_back(entry);
            }
        }
        if (batch->is_final) {
            pendingExternalTransferDrop_->awaiting_worker_reap = true;
            break;
        }
    }

    if (!pendingExternalTransferDrop_ || !pendingExternalTransferDrop_->awaiting_worker_reap ||
        !dropProbeSource_.idle()) {
        return;
    }
    pendingExternalTransferDrop_->awaiting_worker_reap = false;
    if (pendingExternalTransferDrop_->confirmed_entries.size() !=
        pendingExternalTransferDrop_->index + 1) {
        fail_external_transfer_drop(QCoreApplication::translate(
            "MainWindow", "A dragged object was not found or could not be checked"));
        return;
    }
    ++pendingExternalTransferDrop_->index;
    if (pendingExternalTransferDrop_->index < pendingExternalTransferDrop_->paths.size()) {
        pendingExternalTransferDrop_->generation = nextDropProbeGeneration_++;
        const auto next_path = native_path(
            pendingExternalTransferDrop_->paths.at(pendingExternalTransferDrop_->index));
        dropProbeSource_.submit({.generation = pendingExternalTransferDrop_->generation,
                                 .path = next_path.parent_path(),
                                 .maximum_entries = 1,
                                 .exact_entry_path = next_path});
        return;
    }

    auto completed = std::move(*pendingExternalTransferDrop_);
    pendingExternalTransferDrop_.reset();
    dropProbeTimer_->stop();
    auto ordered_entries = std::move(completed.confirmed_entries);

    const auto all_trash_objects = std::ranges::all_of(ordered_entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
    });
    if (completed.trash_target) {
        if (!all_trash_objects) {
            fail_external_transfer_drop(QCoreApplication::translate(
                "MainWindow", "Only files and folders can be moved to VO-VE Trash"));
        } else {
            static_cast<void>(prompt_move_entries_to_trash(ordered_entries));
        }
        resume_queued_external_open();
        return;
    }
    const auto started = completed.clipboard_copy || !completed.search_entries.isEmpty()
        ? start_object_transfer(ordered_entries, completed.transfer_destination,
              completed.clipboard_copy ? fileops::FileTransferKind::copy : completed.transfer_kind)
        : start_transfer_selection(ordered_entries, completed.transfer_destination, completed.transfer_kind);
    if (!started) {
        fail_external_transfer_drop(
            QCoreApplication::translate("MainWindow", "Failed to start object transfer"));
    }
    resume_queued_external_open();
}

bool FileCommandController::Impl::resume_queued_external_open() {
    if (queuedExternalOpenPaths_.isEmpty() || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        return false;
    }
    auto paths = std::move(queuedExternalOpenPaths_);
    queuedExternalOpenPaths_.clear();
    open_external_paths(paths);
    return true;
}

void FileCommandController::Impl::fail_external_directory_drop(const QString &message) {
    if (pendingExternalDirectoryDrop_) {
        dropProbeSource_.cancel(pendingExternalDirectoryDrop_->generation);
    }
    pendingExternalDirectoryDrop_.reset();
    if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
    } else {
        dropProbeTimer_->start();
    }
    operationStatus_ = message;
    publish_status();
    resume_queued_external_open();
}

void FileCommandController::Impl::fail_external_transfer_drop(const QString &message) {
    if (pendingExternalTransferDrop_) {
        dropProbeSource_.cancel(pendingExternalTransferDrop_->generation);
    }
    pendingExternalTransferDrop_.reset();
    if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
    } else {
        dropProbeTimer_->start();
    }
    operationStatus_ = message;
    publish_status();
    resume_queued_external_open();
}

void FileCommandController::Impl::fail_transfer_preparation(const QString &message) {
    if (pendingTransferPreparation_) {
        for (const auto &probe : pendingTransferPreparation_->probes) {
            if (!probe.complete) {
                transferProbeSource_.cancel(probe.generation);
            }
        }
    }
    pendingTransferPreparation_.reset();
    transferProbeTimer_->stop();
    set_transfer_in_flight(false);
    operationStatus_ = message;
    publish_status();
}

void FileCommandController::Impl::prepare_directory_entries(
    QList<core::DirectoryEntry> entries, QList<qsizetype> indices,
    std::function<void(QList<core::DirectoryEntry>)> ready, const bool trash_target) {
    if (pendingDirectoryPreparation_ || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || !dropProbeSource_.idle()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
        return;
    }
    if (indices.isEmpty()) {
        ready(std::move(entries));
        return;
    }
    PendingDirectoryPreparation pending;
    pending.entries = std::move(entries);
    pending.indices = std::move(indices);
    pending.ready = std::move(ready);
    pending.trash_target = trash_target;
    pending.readers_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    pending.generation = nextDropProbeGeneration_++;
    const auto path = native_path(QString::fromUtf8(pending.entries.at(pending.indices.front()).path_utf8));
    const auto generation = pending.generation;
    pendingDirectoryPreparation_ = std::move(pending);
    renamePreviewSuspended_ = true;
    begin_preview_generation(false);
    ports_.suspend_preview_dispatch();
    set_rename_in_flight(true);
    operationStatus_ = trash_target
        ? QCoreApplication::translate("MainWindow", "VO-VE Trash: preparing…")
        : QCoreApplication::translate("MainWindow", "Batch rename: preparing…");
    publish_status();
    dropProbeSource_.submit({.generation = generation, .path = path.parent_path(),
                             .maximum_entries = 1, .exact_entry_path = path});
    dropProbeTimer_->start();
}

void FileCommandController::Impl::poll_directory_preparation() {
    if (!pendingDirectoryPreparation_) return;
    if ((!readers_idle()) &&
        std::chrono::steady_clock::now() >= pendingDirectoryPreparation_->readers_deadline) {
        fail_directory_preparation();
        return;
    }
    for (int batch_index{}; batch_index < 32; ++batch_index) {
        auto batch = dropProbeSource_.poll();
        if (!batch) break;
        auto &pending = *pendingDirectoryPreparation_;
        if (batch->generation != pending.generation) continue;
        if (batch->error) {
            fail_directory_preparation();
            return;
        }
        auto &expected = pending.entries[pending.indices.at(pending.index)];
        for (const auto &entry : batch->entries) {
            // Directory enumeration metadata can lag behind the handle snapshot on Windows.
            // Refresh metadata only for the same selected native object; mutation checks stay strict.
            if (pending.received || entry.kind != core::EntryKind::directory ||
                entry.path_utf8 != expected.path_utf8 ||
                !fileops::same_object_identity(entry.source_revision_utf8, expected.source_revision_utf8)) {
                fail_directory_preparation();
                return;
            }
            expected = entry;
            pending.received = true;
        }
        if (batch->is_final) {
            if (!pending.received) {
                fail_directory_preparation();
                return;
            }
            pending.awaiting_worker_reap = true;
            break;
        }
    }
    if (!pendingDirectoryPreparation_ || !pendingDirectoryPreparation_->awaiting_worker_reap ||
        !dropProbeSource_.idle()) return;
    if (!readers_idle()) return;
    auto &pending = *pendingDirectoryPreparation_;
    ++pending.index;
    if (pending.index < pending.indices.size()) {
        pending.generation = nextDropProbeGeneration_++;
        pending.received = false;
        pending.awaiting_worker_reap = false;
        const auto path = native_path(QString::fromUtf8(pending.entries.at(pending.indices.at(pending.index)).path_utf8));
        dropProbeSource_.submit({.generation = pending.generation, .path = path.parent_path(),
                                 .maximum_entries = 1, .exact_entry_path = path});
        return;
    }
    auto completed = std::move(pending);
    pendingDirectoryPreparation_.reset();
    dropProbeTimer_->stop();
    completed.ready(std::move(completed.entries));
    resume_queued_external_open();
}

void FileCommandController::Impl::fail_directory_preparation() {
    const auto trash_target = pendingDirectoryPreparation_ && pendingDirectoryPreparation_->trash_target;
    if (pendingDirectoryPreparation_) dropProbeSource_.cancel(pendingDirectoryPreparation_->generation);
    pendingDirectoryPreparation_.reset();
    set_rename_in_flight(false);
    operationStatus_ = trash_target
        ? QCoreApplication::translate("MainWindow", "The file list changed. Retry deletion")
        : QCoreApplication::translate("MainWindow", "The file list changed. Retry renaming");
    publish_status();
    if (dropProbeSource_.idle()) dropProbeTimer_->stop();
    else dropProbeTimer_->start();
    resume_queued_external_open();
}

} // namespace vove::ui
