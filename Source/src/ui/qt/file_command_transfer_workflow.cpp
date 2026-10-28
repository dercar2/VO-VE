#include "file_command_workflow.hpp"

namespace vove::ui {
using namespace file_command_detail;

void FileCommandController::Impl::prompt_transfer_selected(const fileops::FileTransferKind kind) {
    const auto entries = selected_entries_in_view_order();
    if (entries.isEmpty() || !std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        })) {
        return;
    }
    const auto destination = QFileDialog::getExistingDirectory(
        dialog_owner(),
        kind == fileops::FileTransferKind::copy
            ? QCoreApplication::translate("MainWindow", "Copy to Folder")
            : QCoreApplication::translate("MainWindow", "Move to Folder"),
        catalog().current_path, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!destination.isEmpty()) {
        static_cast<void>(start_transfer_selection(entries, destination, kind));
    }
}

bool FileCommandController::Impl::start_transfer_selection(const QList<core::DirectoryEntry> &entries,
                                           const QString &destination, const fileops::FileTransferKind kind) {
    if (entries.isEmpty()) return false;
    if (catalog().global_search) {
        if (!catalog().search_ready || !catalog().search_drag_enabled ||
            !catalog().search_query_current ||
            catalog().removed_search_paths + entries.size() >
                static_cast<qsizetype>(search::kMaximumSearchResults))
            return false;
        QStringList paths;
        for (const auto &entry : entries) paths.push_back(QString::fromUtf8(entry.path_utf8));
        return begin_external_transfer_drop(paths, destination, kind, false, false, entries);
    }
    if (catalog().recursive_view) {
        if (!catalog().ready) return false;
        return start_object_transfer(entries, destination, kind);
    }
    if (std::ranges::all_of(entries, [](const auto &entry) { return entry.kind == core::EntryKind::file; })) {
        return start_file_transfer(entries, destination, kind);
    }
    if (std::ranges::all_of(entries, [](const auto &entry) { return entry.kind == core::EntryKind::directory; })) {
        return start_directory_transfer(entries, destination, kind);
    }
    return start_object_transfer(entries, destination, kind);
}

bool FileCommandController::Impl::start_object_transfer(const QList<core::DirectoryEntry> &entries,
                                        const QString &destination, const fileops::FileTransferKind kind,
                                        const bool resume) {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileOperationService_.busy() || pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        objectTransferQueue_.busy() || fileTransferCoordinator_.busy() || directoryTransferService_.busy()) return false;
    if (!resume && refresh_current_operation_journal_state() != CurrentOperationJournalState::none) return false;
    ports_.clear_pending_mosaics();
    activeTransferKind_ = kind;
    set_transfer_in_flight(true);
    operationStatus_ = QCoreApplication::translate("MainWindow", "Checking the selected objects…");
    publish_status();
    const QPointer<Impl> window(this);
    const auto progress = [window](fileops::FileTransferProgressUpdate value) {
        if (window) QMetaObject::invokeMethod(window, [window, value = std::move(value)] {
            if (window) window->handle_file_transfer_progress(value);
        }, Qt::QueuedConnection);
    };
    const auto conflict = [window](fileops::FileTransferConflict value, bool overwrite_allowed, bool copy_allowed) {
        if (window) QMetaObject::invokeMethod(window, [window, value = std::move(value), overwrite_allowed, copy_allowed] {
            if (window) window->handle_file_transfer_conflict(value, true, overwrite_allowed, copy_allowed);
        }, Qt::QueuedConnection);
    };
    const auto completion = [window](ObjectTransferResult result) {
        if (window) QMetaObject::invokeMethod(window, [window, result = std::move(result)] {
            if (!window) return;
            window->set_transfer_in_flight(false);
            window->notify_file_in_use(result.operation_status);
            static_cast<void>(window->refresh_current_operation_journal_state());
            if (result.success) {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Objects transferred: %1").arg(result.completed);
            } else if (result.recovery) {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Transfer paused. Check the network and press F5");
            } else if (result.cancelled) {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Transfer cancelled. Objects transferred: %1").arg(result.completed);
            } else {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Cannot transfer the selected objects: %1").arg(result.detail);
            }
            window->set_property("objectTransferDetail", result.detail);
            if (window->catalog().global_search) window->ports_.mark_saved_search_dirty();
            for (const auto &path : result.published_paths)
                window->allow_observed_global_search_path(path);
            window->reconcile_global_search_move(result.moved_sources);
            window->begin_preview_generation();
            window->refresh_directory_sources();
            if (!window->catalog().global_search) window->poll_catalog();
            window->publish_status();
            if (window->catalog().global_search &&
                !window->catalog().search_query_current)
                window->schedule_global_search();
        }, Qt::QueuedConnection);
    };
    const auto submitted = resume ? objectTransferQueue_.resume(progress, conflict, completion)
        : objectTransferQueue_.start(kind, {entries.begin(), entries.end()}, native_path(destination), progress, conflict, completion);
    if (!submitted) set_transfer_in_flight(false);
    return submitted;
}

bool FileCommandController::Impl::start_file_transfer(const QList<core::DirectoryEntry> &entries,
                                     const QString &destination_directory,
                                     const fileops::FileTransferKind kind) {
    const auto all_regular_files =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file;
        });
    if (!all_regular_files || destination_directory.trimmed().isEmpty()) {
        return false;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return false;
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileOperationService_.busy() || pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        fileTransferCoordinator_.busy()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
        return false;
    }

    PendingTransferPreparation pending;
    pending.kind = kind;
    pending.entries = entries;
    pending.destination_directory =
        native_path(QDir::cleanPath(QDir::fromNativeSeparators(destination_directory.trimmed())));
    QSet<QString> probe_keys;
    const auto add_probe = [this, &pending, &probe_keys](const std::filesystem::path &path) {
        const auto normalized = path.lexically_normal();
        const auto key = transfer_path_key(normalized);
        if (normalized.empty() || probe_keys.contains(key)) {
            return;
        }
        probe_keys.insert(key);
        pending.probes.push_back({.generation = nextTransferProbeGeneration_++,
                                  .path = normalized,
                                  .revision_utf8 = {},
                                  .complete = false});
    };
    for (const auto &entry : entries) {
        add_probe(native_path(QString::fromUtf8(entry.path_utf8)).parent_path());
    }
    add_probe(pending.destination_directory);
    if (pending.probes.empty()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to determine the operation folders");
        publish_status();
        return false;
    }

    const auto first_probe_generation = pending.probes.front().generation;
    const auto first_probe_path = pending.probes.front().path;
    pendingTransferPreparation_ = std::move(pending);
    set_transfer_in_flight(true);
    operationStatus_ =
        kind == fileops::FileTransferKind::copy
            ? QCoreApplication::translate("MainWindow", "Checking folders before copying…")
            : QCoreApplication::translate("MainWindow", "Checking folders before moving…");
    publish_status();
    transferProbeSource_.submit(
        {.generation = first_probe_generation, .path = first_probe_path, .maximum_entries = 1});
    transferProbeTimer_->start();
    return true;
}

bool FileCommandController::Impl::start_directory_transfer(const QList<core::DirectoryEntry> &entries,
                                          const QString &destination_directory,
                                          const fileops::FileTransferKind kind) {
    if (entries.isEmpty() || !std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::directory;
        })) {
        return false;
    }
    QStringList paths;
    std::vector<std::string> revisions;
    paths.reserve(entries.size());
    revisions.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        if (entry.source_revision_utf8.empty()) {
            return false;
        }
        paths.push_back(QString::fromUtf8(entry.path_utf8));
        revisions.push_back(entry.source_revision_utf8);
    }
    return start_directory_transfer_paths(std::move(paths), std::move(revisions),
                                          destination_directory, kind);
}

bool FileCommandController::Impl::start_directory_transfer_paths(QStringList source_paths,
                                                std::vector<std::string> source_revisions_utf8,
                                                const QString &destination_directory,
                                                const fileops::FileTransferKind kind) {
    if (source_paths.isEmpty() ||
        static_cast<std::size_t>(source_paths.size()) > fileops::kMaximumDirectoryTransferRoots ||
        source_revisions_utf8.size() != static_cast<std::size_t>(source_paths.size()) ||
        std::ranges::any_of(source_revisions_utf8, &std::string::empty) ||
        destination_directory.trimmed().isEmpty() ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileOperationService_.busy() || directoryTransferService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        fileTransferCoordinator_.busy()) {
        operationStatus_ =
            static_cast<std::size_t>(source_paths.size()) > fileops::kMaximumDirectoryTransferRoots
                ? QCoreApplication::translate("MainWindow",
                                              "No more than %1 folders can be transferred at once")
                      .arg(fileops::kMaximumDirectoryTransferRoots)
                : QCoreApplication::translate("MainWindow",
                                              "Another file operation is still running");
        publish_status();
        return false;
    }

    const auto destination =
        native_path(QDir::cleanPath(QDir::fromNativeSeparators(destination_directory.trimmed())));
    if (!destination.is_absolute()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to determine the destination folder");
        publish_status();
        return false;
    }
    for (auto &path : source_paths) {
        path = QDir::cleanPath(QDir::fromNativeSeparators(path));
        if (!native_path(path).is_absolute()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to determine the source folder");
            publish_status();
            return false;
        }
    }

    pendingDirectoryTransferBatch_ = PendingDirectoryTransferBatch{
        .kind = kind,
        .source_paths = std::move(source_paths),
        .source_revisions_utf8 = std::move(source_revisions_utf8),
        .destination_directory = destination,
    };
    // Folder mosaics may still be reading children of a directory selected for transfer.
    // Release those preview handles before the transfer engine validates and retires the source.
    ports_.clear_pending_mosaics();
    set_transfer_in_flight(true);
    return submit_next_directory_transfer();
}

bool FileCommandController::Impl::submit_next_directory_transfer() {
    if (!pendingDirectoryTransferBatch_) {
        return false;
    }
    auto &batch = *pendingDirectoryTransferBatch_;
    if (batch.index >= batch.source_paths.size()) {
        const auto completed = batch.completed;
        const auto kind = batch.kind;
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            kind == fileops::FileTransferKind::move
                ? QCoreApplication::translate("MainWindow", "Folders moved: %1").arg(completed)
                : QCoreApplication::translate("MainWindow", "Folders copied: %1").arg(completed);
        begin_preview_generation();
        refresh_directory_sources();
        poll_catalog(16);
        publish_status();
        return true;
    }

    const auto source = native_path(batch.source_paths.at(batch.index));
    const auto filename = source.filename();
    if (filename.empty()) {
        pendingDirectoryTransferBatch_.reset();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file-system root cannot be transferred");
        publish_status();
        return false;
    }
    const auto operation_id = next_directory_transfer_request_id();
    const fileops::BasicDirectoryTransferStreamRequest request{
        .request_id = operation_id,
        .operation_id = operation_id,
        .command = batch.kind == fileops::FileTransferKind::move
                       ? fileops::BasicDirectoryTransferCommand::move
                       : fileops::BasicDirectoryTransferCommand::copy,
        .source = source,
        .destination = batch.destination_directory / filename,
        .manifest_path = {},
        .expected_source_revision_utf8 =
            batch.source_revisions_utf8.at(static_cast<std::size_t>(batch.index)),
    };
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_ = display_path(request.source);
    directoryTransferRecoveryDestination_ = display_path(request.destination);
    directoryTransferRecoverySourceRevision_ =
        QString::fromUtf8(request.expected_source_revision_utf8);
    directoryTransferRecoveryRequestId_ = request.request_id;
    directoryTransferCompletionKnown_ = false;
    set_property("directoryTransferRecoveryPending", true);
    if (!persist_directory_transfer_recovery_state()) {
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to save folder transfer state");
        publish_status();
        return false;
    }
    operationStatus_ = batch.kind == fileops::FileTransferKind::move
                           ? QCoreApplication::translate("MainWindow", "Moving folder %1 of %2…")
                                 .arg(batch.index + 1)
                                 .arg(batch.source_paths.size())
                           : QCoreApplication::translate("MainWindow", "Copying folder %1 of %2…")
                                 .arg(batch.index + 1)
                                 .arg(batch.source_paths.size());
    publish_status();

    const QPointer<Impl> window(this);
    const auto completion_state = encode_directory_transfer_recovery_state(true);
    const auto completion_writer = ports_.write_recovery;
    if (completion_state.isEmpty()) {
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder transfer state exceeds the safe limit");
        publish_status();
        return false;
    }
    const auto submitted = directoryTransferService_.submit_directory(
        request,
        [window, completion_state, completion_writer](const fileops::BasicDirectoryTransferProgress &progress) {
            const auto sharedProgress =
                std::make_shared<fileops::BasicDirectoryTransferProgress>(progress);
            if (progress.phase == fileops::BasicDirectoryTransferPhase::completed) {
                if (!completion_writer(completion_state)) {
                    throw std::runtime_error("directory completion evidence was not persisted");
                }
            }
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, sharedProgress] {
                    if (!window.isNull()) {
                        window->handle_directory_transfer_progress(*sharedProgress);
                        if (sharedProgress->phase ==
                            fileops::BasicDirectoryTransferPhase::completed) {
                            static_cast<void>(window->record_directory_transfer_completion());
                        }
                    }
                },
                Qt::QueuedConnection);
        },
        [window](fileops::BasicDirectoryTransferResult result) mutable {
            if (!window.isNull()) {
                const auto sharedResult =
                    std::make_shared<fileops::BasicDirectoryTransferResult>(std::move(result));
                QMetaObject::invokeMethod(
                    window,
                    [window, sharedResult] {
                        if (!window.isNull()) {
                            window->handle_directory_transfer_result(*sharedResult);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to start folder transfer");
        publish_status();
    }
    return submitted;
}

void FileCommandController::Impl::launch_prepared_file_transfer() {
    if (!pendingTransferPreparation_) {
        return;
    }
    auto pending = std::move(*pendingTransferPreparation_);
    pendingTransferPreparation_.reset();
    transferProbeTimer_->stop();
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        set_transfer_in_flight(false);
        publish_status();
        return;
    }
    const auto revision_for = [&pending](const std::filesystem::path &path) -> std::string {
        const auto key = transfer_path_key(path.lexically_normal());
        const auto match = std::ranges::find_if(pending.probes, [&key](const auto &probe) {
            return transfer_path_key(probe.path) == key;
        });
        return match == pending.probes.end() ? std::string{} : match->revision_utf8;
    };

    std::vector<fileops::FileTransferSource> sources;
    sources.reserve(static_cast<std::size_t>(pending.entries.size()));
    QSet<QString> destinations;
    for (const auto &entry : pending.entries) {
        const auto source = native_path(QString::fromUtf8(entry.path_utf8));
        const auto destination = pending.destination_directory / source.filename();
        const auto destination_key = transfer_path_key(destination);
        if (source.empty() || destination.empty() || transfer_path_key(source) == destination_key ||
            destinations.contains(destination_key)) {
            fail_transfer_preparation(QCoreApplication::translate(
                "MainWindow", "Source and destination paths are identical or duplicated"));
            return;
        }
        destinations.insert(destination_key);
        sources.push_back(
            {.path = source,
             .destination = destination,
             .snapshot = {.size_bytes = entry.size_bytes,
                          .modified_unix_ns = entry.modified_unix_ns,
                          .source_revision_utf8 = entry.source_revision_utf8},
             .source_parent_revision_utf8 = revision_for(source.parent_path()),
             .destination_parent_revision_utf8 = revision_for(pending.destination_directory)});
    }
    if (std::ranges::any_of(sources, [](const auto &source) {
            return source.source_parent_revision_utf8.empty() ||
                   source.destination_parent_revision_utf8.empty();
        })) {
        fail_transfer_preparation(
            QCoreApplication::translate("MainWindow", "Failed to verify the operation folders"));
        return;
    }

    activeTransferKind_ = pending.kind;
    activeFileTransferSources_.clear();
    for (const auto &entry : pending.entries)
        activeFileTransferSources_.push_back(QString::fromUtf8(entry.path_utf8));
    operationStatus_ = pending.kind == fileops::FileTransferKind::copy
                           ? QCoreApplication::translate("MainWindow", "Copying: preparing…")
                           : QCoreApplication::translate("MainWindow", "Moving: preparing…");
    publish_status();
    const QPointer<Impl> window(this);
    const auto submitted = fileTransferCoordinator_.start(
        pending.kind, std::move(sources),
        [window](fileops::FileTransferProgressUpdate progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_file_transfer_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::FileTransferResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_file_transfer_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::FileTransferConflict conflict) {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, conflict = std::move(conflict)] {
                        if (!window.isNull()) {
                            window->handle_file_transfer_conflict(conflict);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
    }
}

void FileCommandController::Impl::handle_file_transfer_progress(
    const fileops::FileTransferProgressUpdate &progress) {
    QString phase;
    switch (progress.step) {
    case fileops::FileTransferStep::none:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::FileTransferStep::atomic_move:
    case fileops::FileTransferStep::publish_temp:
        phase = QCoreApplication::translate("MainWindow", "publishing");
        break;
    case fileops::FileTransferStep::stage_source:
        phase = QCoreApplication::translate("MainWindow", "committing source");
        break;
    case fileops::FileTransferStep::reserve_temp:
        phase = QCoreApplication::translate("MainWindow", "preparing space");
        break;
    case fileops::FileTransferStep::stream_temp:
        phase = QCoreApplication::translate("MainWindow", "transferring data");
        break;
    case fileops::FileTransferStep::delete_source:
        phase = QCoreApplication::translate("MainWindow", "finishing move");
        break;
    case fileops::FileTransferStep::restore_staged_source:
        phase = QCoreApplication::translate("MainWindow", "restoring source");
        break;
    case fileops::FileTransferStep::cleanup_temp:
        phase = QCoreApplication::translate("MainWindow", "cleaning incomplete file");
        break;
    case fileops::FileTransferStep::evacuate_overwrite_destination:
        phase = QCoreApplication::translate("MainWindow", "preparing overwrite");
        break;
    case fileops::FileTransferStep::restore_overwrite_destination:
        phase = QCoreApplication::translate("MainWindow", "restoring destination");
        break;
    case fileops::FileTransferStep::cleanup_overwrite_destination:
        phase = QCoreApplication::translate("MainWindow", "finishing overwrite");
        break;
    }
    auto detail = QCoreApplication::translate("MainWindow", "%1: %2 · %3 of %4")
                      .arg(activeTransferKind_ == fileops::FileTransferKind::copy
                               ? QCoreApplication::translate("MainWindow", "Copying")
                               : QCoreApplication::translate("MainWindow", "Moving"),
                           phase)
                      .arg(progress.completed)
                      .arg(progress.total);
    if (progress.bytes_total != 0) {
        detail += QStringLiteral(" · %1 / %2")
                      .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.bytes_written)))
                      .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.bytes_total)));
    }
    operationStatus_ = std::move(detail);
    publish_status();
}

void FileCommandController::Impl::handle_file_transfer_conflict(const fileops::FileTransferConflict &conflict,
                                               const bool object_queue, const bool overwrite_allowed,
                                               const bool copy_allowed) {
    if (conflict.items.empty()) {
        if (object_queue) objectTransferQueue_.resolve_conflict(fileops::FileTransferConflictDecision::cancel);
        else fileTransferCoordinator_.resolve_conflict(fileops::FileTransferConflictDecision::cancel);
        return;
    }
    QStringList names;
    names.reserve(static_cast<qsizetype>(conflict.items.size()));
    for (const auto &item : conflict.items) {
        names.push_back(display_path(item.destination.filename()));
    }
    const auto single = conflict.items.size() == 1U;
    auto *dialog = new QMessageBox(
        QMessageBox::Question, QCoreApplication::translate("MainWindow", "Name conflict"),
        single
            ? QCoreApplication::translate("MainWindow", "An object with this name already exists")
            : QCoreApplication::translate(
                  "MainWindow", "%1 already exist in the destination. What should be done?")
                  .arg(names.join(QStringLiteral(", "))),
        QMessageBox::NoButton, dialog_owner());
    dialog->setObjectName(QStringLiteral("fileOverwriteConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setWindowModality(Qt::WindowModal);
    if (single) {
        const auto &item = conflict.items.front();
        dialog->setInformativeText(
            QCoreApplication::translate("MainWindow", "Object: %1\nFrom: %2\nTo: %3")
                .arg(display_path(item.destination.filename()),
                     QDir::toNativeSeparators(display_path(item.source.parent_path())),
                     QDir::toNativeSeparators(display_path(item.destination.parent_path()))));
    } else {
        dialog->setInformativeText(
            QCoreApplication::translate("MainWindow", "The selected action applies to all items"));
    }
    auto *create_copy = dialog->addButton(QCoreApplication::translate("MainWindow", "Create copy"),
                                          QMessageBox::ActionRole);
    create_copy->setObjectName(QStringLiteral("fileCreateCopyConfirmButton"));
    create_copy->setEnabled(copy_allowed);
    if (!copy_allowed) dialog->setInformativeText(QCoreApplication::translate("MainWindow",
        "The destination changed during transfer. Cancel to choose a new copy name."));
    auto *overwrite = dialog->addButton(QCoreApplication::translate("MainWindow", "Overwrite"),
                                        QMessageBox::DestructiveRole);
    overwrite->setObjectName(QStringLiteral("fileOverwriteConfirmButton"));
    overwrite->setEnabled(overwrite_allowed);
    if (!overwrite_allowed) {
        dialog->setInformativeText(QCoreApplication::translate("MainWindow",
            "An existing folder cannot be replaced. Create a copy or cancel the entire selection."));
    }
    auto *cancel = dialog->addButton(QCoreApplication::translate("MainWindow", "Cancel"),
                                     QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("fileOverwriteCancelButton"));
    dialog->setDefaultButton(cancel);
    dialog->setEscapeButton(cancel);
    connect(dialog, &QDialog::finished, this, [this, dialog, create_copy, overwrite, object_queue] {
        auto decision = fileops::FileTransferConflictDecision::cancel;
        if (dialog->clickedButton() == create_copy) {
            decision = fileops::FileTransferConflictDecision::create_copy;
        } else if (dialog->clickedButton() == overwrite) {
            decision = fileops::FileTransferConflictDecision::overwrite;
        }
        if (object_queue) objectTransferQueue_.resolve_conflict(decision);
        else fileTransferCoordinator_.resolve_conflict(decision);
    });
    operationStatus_ = QCoreApplication::translate("MainWindow", "Waiting for conflict decision");
    publish_status();
    dialog->open();
}

void FileCommandController::Impl::handle_file_transfer_result(const fileops::FileTransferResult &result) {
    notify_file_in_use(result.operation_status);
    for (const auto &destination : result.destinations)
        allow_observed_global_search_path(display_path(destination));
    if (catalog().global_search && !result.destinations.empty()) ports_.mark_saved_search_dirty();
    if (activeTransferKind_ == fileops::FileTransferKind::move)
        reconcile_global_search_move(activeFileTransferSources_.first(static_cast<qsizetype>(
            std::min(result.completed, static_cast<std::size_t>(activeFileTransferSources_.size())))));
    const auto owns_recovery = fileTransferCoordinator_.owns_recovery();
    if (!owns_recovery) activeFileTransferSources_.clear();
    set_property("fileTransferRecoveryPending", owns_recovery);
    if (result.ok()) {
        operationStatus_ = activeTransferKind_ == fileops::FileTransferKind::copy
                               ? QCoreApplication::translate("MainWindow", "Files copied: %1")
                                     .arg(result.completed)
                               : QCoreApplication::translate("MainWindow", "Files moved: %1")
                                     .arg(result.completed);
        QStringList visible_destinations;
        const auto current_key = transfer_path_key(native_path(catalog().current_path));
        for (const auto &destination : result.destinations) {
            if (transfer_path_key(destination.parent_path()) == current_key) {
                visible_destinations.push_back(display_path(destination));
            }
        }
        ports_.reveal_paths(visible_destinations);
        ports_.clear_selection();
        begin_preview_generation();
        refresh_directory_sources();
        poll_catalog(16);
    } else if (result.status == fileops::FileTransferRunStatus::cancelled && !owns_recovery) {
        operationStatus_ =
            activeTransferKind_ == fileops::FileTransferKind::copy
                ? QCoreApplication::translate("MainWindow", "Copy cancelled. Files copied: %1")
                      .arg(result.completed)
                : QCoreApplication::translate("MainWindow", "Move cancelled. Files moved: %1")
                      .arg(result.completed);
        refresh_directory_sources();
        poll_catalog();
    } else if (owns_recovery ||
               result.status == fileops::FileTransferRunStatus::recovery_required ||
               result.status == fileops::FileTransferRunStatus::journal_error ||
               result.status == fileops::FileTransferRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Transfer paused. Check the network and press F5");
        begin_preview_generation(false);
        ports_.mark_all_previews_unverified();
    } else if (result.status == fileops::FileTransferRunStatus::partial_failure) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow",
                                        "Files transferred: %1 of %2. The rest were not changed")
                .arg(result.completed)
                .arg(result.total);
        refresh_directory_sources();
        poll_catalog();
    } else if (result.status == fileops::FileTransferRunStatus::unsupported) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Transfer is unavailable for the selected resource");
    } else if (result.operation_status == fileops::OperationStatus::conflict) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "A file with this name already exists");
    } else if (result.operation_status == fileops::OperationStatus::source_changed) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The source file changed. Retry the operation");
    } else if (result.operation_status == fileops::OperationStatus::permission_denied) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "No permission to write to the selected folder");
    } else {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Copy or move failed");
    }
    set_transfer_in_flight(false);
    publish_status();
}

void FileCommandController::Impl::handle_directory_transfer_progress(
    const fileops::BasicDirectoryTransferProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::BasicDirectoryTransferPhase::enumerating:
        phase = QCoreApplication::translate("MainWindow", "counting contents");
        break;
    case fileops::BasicDirectoryTransferPhase::staging_ready:
        phase = QCoreApplication::translate("MainWindow", "space prepared");
        break;
    case fileops::BasicDirectoryTransferPhase::copying:
        phase = QCoreApplication::translate("MainWindow", "transferring files");
        break;
    case fileops::BasicDirectoryTransferPhase::verifying:
        phase = QCoreApplication::translate("MainWindow", "verifying copy");
        break;
    case fileops::BasicDirectoryTransferPhase::publishing:
        phase = QCoreApplication::translate("MainWindow", "publishing folder");
        break;
    case fileops::BasicDirectoryTransferPhase::retiring_source:
    case fileops::BasicDirectoryTransferPhase::deleting_source:
        phase = QCoreApplication::translate("MainWindow", "finishing move");
        break;
    case fileops::BasicDirectoryTransferPhase::completed:
        phase = QCoreApplication::translate("MainWindow", "done");
        break;
    }
    auto detail = QCoreApplication::translate("MainWindow", "Folder transfer: %1").arg(phase);
    if (progress.total_entries != 0U) {
        detail += QCoreApplication::translate("MainWindow", " · %1 of %2")
                      .arg(progress.completed_entries)
                      .arg(progress.total_entries);
    }
    if (progress.total_bytes != 0U) {
        detail +=
            QStringLiteral(" · %1 / %2")
                .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.completed_bytes)))
                .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.total_bytes)));
    }
    operationStatus_ = std::move(detail);
    publish_status();
}

bool FileCommandController::Impl::record_directory_transfer_completion() {
    directoryTransferCompletionKnown_ = true;
    return persist_directory_transfer_recovery_state();
}

void FileCommandController::Impl::handle_directory_transfer_result(
    const fileops::BasicDirectoryTransferResult &result) {
    if (result.status == fileops::BasicDirectoryTransferStatus::file_in_use)
        notify_file_in_use(fileops::OperationStatus::file_in_use);
    if (!directoryTransferDiscarding_ && (result.ok() ||
        (directoryTransferCompletionKnown_ && result.status == fileops::BasicDirectoryTransferStatus::not_found))) {
        allow_observed_global_search_path(directoryTransferRecoveryDestination_);
        if (pendingDirectoryTransferBatch_ && pendingDirectoryTransferBatch_->kind == fileops::FileTransferKind::move)
            reconcile_global_search_move({directoryTransferRecoverySource_});
        if (catalog().global_search) ports_.mark_saved_search_dirty();
    }
    if (directoryTransferDiscarding_) {
        set_transfer_in_flight(false);
        if (result.ok() || result.status == fileops::BasicDirectoryTransferStatus::not_found) {
            pendingDirectoryTransferBatch_.reset();
            clear_directory_transfer_recovery_state();
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Incomplete data was safely deleted");
            refresh_directory_sources();
            poll_catalog();
            publish_status();
            return;
        }
        if (!result.manifest_path.empty()) {
            directoryTransferRecoveryManifest_ = display_path(result.manifest_path);
        }
        set_property("directoryTransferRecoveryPending", true);
        const auto choice_persisted = persist_directory_transfer_recovery_state();
        operationStatus_ =
            result.status == fileops::BasicDirectoryTransferStatus::recovery_required
                ? QCoreApplication::translate(
                      "MainWindow",
                      "Cannot delete safely: hidden data may be the only complete copy")
                : QCoreApplication::translate("MainWindow", "Incomplete data was not deleted: %1")
                      .arg(directory_transfer_error_text(result.status));
        operationStatus_ +=
            choice_persisted ? QCoreApplication::translate(
                                   "MainWindow", ". Your choice was saved; press F5 to retry")
                             : QCoreApplication::translate("MainWindow",
                                                           ". Failed to update the recovery state");
        publish_status();
        return;
    }
    if (directoryTransferCompletionKnown_ &&
        result.status == fileops::BasicDirectoryTransferStatus::not_found) {
        if (advance_directory_transfer_batch_after_success()) {
            return;
        }
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder transfer completed and internal cleanup verified");
        begin_preview_generation();
        refresh_directory_sources();
        poll_catalog();
        publish_status();
        return;
    }
    if (!directoryTransferCompletionKnown_ && pendingDirectoryTransferBatch_ &&
        result.status == fileops::BasicDirectoryTransferStatus::not_found) {
        set_transfer_in_flight(false);
        directoryTransferRecoveryManifest_.clear();
        directoryTransferDiscarding_ = false;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Internal data has not been created yet. Retrying the "
                          "current folder without skipping the queue…");
        publish_status();
        if (!submit_next_directory_transfer()) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Failed to retry the current folder; the queue was kept");
            static_cast<void>(persist_directory_transfer_recovery_state());
            publish_status();
        }
        return;
    }
    if (result.recovery_available &&
        (!result.manifest_path.empty() || directoryTransferRecoveryRequestId_ != 0U)) {
        set_transfer_in_flight(false);
        if (!result.manifest_path.empty()) {
            directoryTransferRecoveryManifest_ = display_path(result.manifest_path);
        }
        set_property("directoryTransferRecoveryPending", true);
        static_cast<void>(persist_directory_transfer_recovery_state());
        if (result.ok()) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Folder transferred. Press F5 to finish internal cleanup");
        } else {
            operationStatus_ = directory_transfer_error_text(result.status);
            const auto retryable =
                result.status == fileops::BasicDirectoryTransferStatus::disconnected ||
                result.status == fileops::BasicDirectoryTransferStatus::timed_out ||
                result.status == fileops::BasicDirectoryTransferStatus::io_error ||
                result.status == fileops::BasicDirectoryTransferStatus::unknown_outcome ||
                result.status == fileops::BasicDirectoryTransferStatus::recovery_required;
            operationStatus_ +=
                retryable
                    ? QCoreApplication::translate("MainWindow", ". Check the network and press F5")
                    : QCoreApplication::translate("MainWindow", ". Incomplete data was kept");
        }
        begin_preview_generation(false);
        ports_.mark_all_previews_unverified();
        publish_status();
        offer_directory_transfer_discard();
        return;
    }
    if (result.ok()) {
        if (advance_directory_transfer_batch_after_success()) {
            return;
        }
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Folder transfer recovered and completed");
        begin_preview_generation();
        refresh_directory_sources();
        poll_catalog(16);
        publish_status();
        return;
    }

    set_transfer_in_flight(false);
    pendingDirectoryTransferBatch_.reset();
    clear_directory_transfer_recovery_state();
    operationStatus_ = directory_transfer_error_text(result.status);
    refresh_directory_sources();
    poll_catalog();
    publish_status();
}

} // namespace vove::ui
