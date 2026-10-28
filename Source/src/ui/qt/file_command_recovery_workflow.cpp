#include "file_command_workflow.hpp"

namespace vove::ui {
using namespace file_command_detail;

CurrentOperationJournalState FileCommandController::Impl::refresh_current_operation_journal_state() {
    const auto loaded_operation =
        fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    const auto delete_recovery =
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        loaded_operation.kind == fileops::CurrentOperationKind::permanent_delete;
    const auto batch_recovery =
        loaded_operation.ok() &&
        (loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::legacy_untyped ||
         (loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
          loaded_operation.kind == fileops::CurrentOperationKind::batch_rename));
    const auto trash_recovery =
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded_operation.kind == fileops::CurrentOperationKind::trash_move ||
         loaded_operation.kind == fileops::CurrentOperationKind::trash_restore ||
         loaded_operation.kind == fileops::CurrentOperationKind::trash_purge);
    const auto transfer_recovery =
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded_operation.kind == fileops::CurrentOperationKind::file_transfer ||
         loaded_operation.kind == fileops::CurrentOperationKind::object_transfer);
    const auto journal_blocked =
        loaded_operation.status != fileops::DurableJournalStatus::not_found && !delete_recovery &&
        !batch_recovery && !trash_recovery && !transfer_recovery;
    set_property("batchRenameRecoveryPending", batch_recovery);
    set_property("permanentDeleteRecoveryPending", delete_recovery);
    set_property("trashRecoveryPending", trash_recovery);
    set_property("fileTransferRecoveryPending", transfer_recovery);
    const auto directory_transfer_recovery =
        directoryTransferStateCorrupt_ || !directoryTransferRecoveryManifest_.isEmpty() ||
        (directoryTransferRecoveryRequestId_ != 0U && !directoryTransferRecoverySource_.isEmpty() &&
         !directoryTransferRecoveryDestination_.isEmpty() &&
         !directoryTransferRecoverySourceRevision_.isEmpty());
    set_property("directoryTransferRecoveryPending", directory_transfer_recovery);
    set_property("fileOperationJournalBlocked", journal_blocked);

    CurrentOperationJournalState state = CurrentOperationJournalState::none;
    if (directory_transfer_recovery) {
        state = CurrentOperationJournalState::directory_transfer_recovery;
        operationStatus_ =
            directoryTransferStateCorrupt_
                ? QCoreApplication::translate(
                      "MainWindow", "Folder transfer state is corrupted; data was not changed and "
                                    "requires manual review")
                : QCoreApplication::translate(
                      "MainWindow", "Incomplete folder transfer. Press F5 to choose an action");
    } else if (trash_recovery) {
        state = CurrentOperationJournalState::trash_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete VO-VE Trash operation. Press F5 to recover");
    } else if (transfer_recovery) {
        state = CurrentOperationJournalState::transfer_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete copy or move. Press F5 to recover");
    } else if (delete_recovery) {
        state = CurrentOperationJournalState::delete_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete deletion. Press F5 to recover safely");
    } else if (batch_recovery) {
        state = CurrentOperationJournalState::batch_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete batch rename. Press F5 to recover");
    } else if (journal_blocked) {
        state = CurrentOperationJournalState::blocked;
        operationStatus_ =
            loaded_operation.status == fileops::DurableJournalStatus::corrupt
                ? QCoreApplication::translate(
                      "MainWindow",
                      "The current operation journal is corrupted. File commands are blocked")
                : QCoreApplication::translate(
                      "MainWindow", "The current operation journal was created by another version. "
                                    "File commands are blocked");
    }
    update_rename_action();
    publish_activity();
    if (state != CurrentOperationJournalState::none) publish_status();
    return state;
}

bool FileCommandController::Impl::submit_batch_rename_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || batchRenameCoordinator_.busy() ||
        !batchRenameCoordinator_.owns_recovery() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    set_rename_in_flight(true);
    const QPointer<Impl> window(this);
    const auto submitted = batchRenameCoordinator_.resume(
        [window](fileops::BatchRenameProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_batch_rename_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::BatchRenameResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_batch_rename_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_rename_in_flight(false);
    }
    return submitted;
}

bool FileCommandController::Impl::submit_delete_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        !permanentDeleteCoordinator_.owns_recovery() || batchRenameCoordinator_.busy() ||
        trashCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    set_delete_in_flight(true);
    const QPointer<Impl> window(this);
    const auto submitted = permanentDeleteCoordinator_.resume(
        [window](fileops::PermanentDeleteProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_delete_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::PermanentDeleteResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_delete_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
    }
    return submitted;
}

bool FileCommandController::Impl::submit_trash_recovery(const bool cancel_remaining) {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || trashCoordinator_.busy() ||
        !trashCoordinator_.owns_recovery() || permanentDeleteCoordinator_.busy() ||
        batchRenameCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    const auto loaded = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    const auto restoring = loaded.ok() &&
                           loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
                           loaded.kind == fileops::CurrentOperationKind::trash_restore;
    const auto purging = loaded.ok() &&
                         loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
                         loaded.kind == fileops::CurrentOperationKind::trash_purge;
    const auto intent = cancel_remaining ? TrashOperationIntent::cancel_move
                        : purging     ? TrashOperationIntent::purge
                        : restoring ? TrashOperationIntent::restore
                                    : TrashOperationIntent::move;
    set_delete_in_flight(true);
    const QPointer<Impl> window(this);
    const auto method = cancel_remaining ? &fileops::TrashCoordinator::cancel_remaining
                                        : &fileops::TrashCoordinator::resume;
    const auto submitted = (trashCoordinator_.*method)(
        [window](fileops::TrashProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window, intent](fileops::TrashResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, intent, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_result(result, intent);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
    }
    return submitted;
}

bool FileCommandController::Impl::submit_file_transfer_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || !fileTransferCoordinator_.owns_recovery() ||
        permanentDeleteCoordinator_.busy() || batchRenameCoordinator_.busy() ||
        trashCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    const auto loaded = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    fileops::FileTransferTransaction transaction;
    std::string detail;
    if (loaded.ok() && loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        loaded.kind == fileops::CurrentOperationKind::file_transfer &&
        fileops::decode_file_transfer_transaction(loaded.payload, transaction, detail)) {
        activeTransferKind_ = transaction.kind;
        activeFileTransferSources_.clear();
        for (const auto &item : transaction.items)
            activeFileTransferSources_.push_back(display_path(item.source));
    }
    set_transfer_in_flight(true);
    const QPointer<Impl> window(this);
    const auto submitted = fileTransferCoordinator_.resume(
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
    }
    return submitted;
}

void FileCommandController::Impl::offer_directory_transfer_discard() {
    const auto recovery_ready = directoryTransferRecoveryRequestId_ != 0U &&
                                !directoryTransferRecoverySource_.isEmpty() &&
                                !directoryTransferRecoveryDestination_.isEmpty() &&
                                !directoryTransferRecoverySourceRevision_.isEmpty();
    if (!recovery_ready || directoryTransferStateCorrupt_ || transferInFlight_ ||
        directoryTransferService_.busy()) {
        return;
    }
    if (directoryTransferDiscarding_) {
        if (!submit_directory_transfer_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to continue safe deletion");
            publish_status();
        }
        return;
    }
    QMessageBox decision(
        QMessageBox::Warning,
        QCoreApplication::translate("MainWindow", "Incomplete folder transfer"),
        QCoreApplication::translate("MainWindow",
                                    "Hidden data was kept. Continue the transfer now, leave it "
                                    "until the next launch, or delete it after another check?"),
        QMessageBox::NoButton, dialog_owner());
    decision.setObjectName(QStringLiteral("directoryTransferRecoveryDecision"));
    auto *resume = decision.addButton(QCoreApplication::translate("MainWindow", "Continue"),
                                      QMessageBox::AcceptRole);
    auto *keep = decision.addButton(QCoreApplication::translate("MainWindow", "Continue later"),
                                    QMessageBox::RejectRole);
    auto *discard =
        decision.addButton(QCoreApplication::translate("MainWindow", "Delete incomplete data"),
                           QMessageBox::DestructiveRole);
    discard->setObjectName(QStringLiteral("directoryTransferDiscardButton"));
    resume->setObjectName(QStringLiteral("directoryTransferResumeButton"));
    keep->setObjectName(QStringLiteral("directoryTransferKeepButton"));
    decision.setDefaultButton(qobject_cast<QPushButton *>(resume));
    decision.exec();
    if (decision.clickedButton() == keep || decision.clickedButton() == nullptr) {
        return;
    }
    const auto discarding = discard != nullptr && decision.clickedButton() == discard;
    directoryTransferDiscarding_ = discarding;
    if (!persist_directory_transfer_recovery_state()) {
        directoryTransferDiscarding_ = false;
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to save the selected action");
        publish_status();
        return;
    }
    operationStatus_ =
        discarding
            ? QCoreApplication::translate("MainWindow", "Checking and deleting incomplete data…")
            : QCoreApplication::translate("MainWindow", "Recovering folder transfer…");
    publish_status();
    if (!submit_directory_transfer_recovery()) {
        operationStatus_ =
            discarding
                ? QCoreApplication::translate("MainWindow", "Failed to start safe deletion")
                : QCoreApplication::translate("MainWindow", "Failed to start transfer recovery");
        publish_status();
    }
}

bool FileCommandController::Impl::submit_directory_transfer_recovery() {
    const auto exact_manifest = !directoryTransferRecoveryManifest_.isEmpty();
    const auto discoverable = directoryTransferRecoveryRequestId_ != 0U &&
                              !directoryTransferRecoverySource_.isEmpty() &&
                              !directoryTransferRecoveryDestination_.isEmpty() &&
                              !directoryTransferRecoverySourceRevision_.isEmpty();
    if ((!exact_manifest && !discoverable) || renameInFlight_ || deleteInFlight_ ||
        transferInFlight_ || createDirectoryInFlight_ || directoryTransferService_.busy() ||
        fileTransferCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        batchRenameCoordinator_.busy() || trashCoordinator_.busy() ||
        fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    const fileops::BasicDirectoryTransferStreamRequest request{
        .request_id = next_directory_transfer_request_id(),
        .operation_id = directoryTransferRecoveryRequestId_,
        .command =
            directoryTransferDiscarding_
                ? (exact_manifest ? fileops::BasicDirectoryTransferCommand::discard
                                  : fileops::BasicDirectoryTransferCommand::discover_and_discard)
                : (exact_manifest ? fileops::BasicDirectoryTransferCommand::resume
                                  : fileops::BasicDirectoryTransferCommand::discover_and_resume),
        .source = native_path(directoryTransferRecoverySource_),
        .destination = native_path(directoryTransferRecoveryDestination_),
        .manifest_path = exact_manifest ? native_path(directoryTransferRecoveryManifest_)
                                        : std::filesystem::path{},
        .expected_source_revision_utf8 =
            directoryTransferRecoverySourceRevision_.toUtf8().toStdString(),
    };
    set_transfer_in_flight(true);
    const QPointer<Impl> window(this);
    const auto completion_state = encode_directory_transfer_recovery_state(true);
    const auto completion_writer = ports_.write_recovery;
    if (completion_state.isEmpty()) {
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Recovery state exceeds the safe limit");
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
        set_transfer_in_flight(false);
    }
    return submitted;
}

bool FileCommandController::Impl::advance_directory_transfer_batch_after_success() {
    if (!pendingDirectoryTransferBatch_) {
        return false;
    }
    auto &batch = *pendingDirectoryTransferBatch_;
    ++batch.completed;
    ++batch.index;
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_.clear();
    directoryTransferRecoveryDestination_.clear();
    directoryTransferRecoverySourceRevision_.clear();
    directoryTransferRecoveryRequestId_ = 0U;
    directoryTransferCompletionKnown_ = false;
    set_property("directoryTransferRecoveryPending", false);
    if (batch.index < batch.source_paths.size()) {
        if (!submit_next_directory_transfer()) {
            set_transfer_in_flight(false);
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Failed to continue batch folder transfer");
            publish_status();
        }
        return true;
    }

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

QByteArray FileCommandController::Impl::encode_directory_transfer_recovery_state(const bool completion_known) const {
    QJsonObject state{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("operationId"),
         QString::number(static_cast<qulonglong>(directoryTransferRecoveryRequestId_))},
        {QStringLiteral("source"), directoryTransferRecoverySource_},
        {QStringLiteral("destination"), directoryTransferRecoveryDestination_},
        {QStringLiteral("sourceRevision"), directoryTransferRecoverySourceRevision_},
        {QStringLiteral("manifest"), directoryTransferRecoveryManifest_},
        {QStringLiteral("completionKnown"), completion_known},
        {QStringLiteral("discarding"), directoryTransferDiscarding_},
    };
    if (pendingDirectoryTransferBatch_) {
        const auto &batch = *pendingDirectoryTransferBatch_;
        QJsonArray sources;
        QJsonArray revisions;
        for (const auto &source : batch.source_paths) {
            sources.push_back(source);
        }
        for (const auto &revision : batch.source_revisions_utf8) {
            revisions.push_back(QString::fromUtf8(revision));
        }
        state.insert(QStringLiteral("batch"),
                     QJsonObject{
                         {QStringLiteral("kind"), static_cast<int>(batch.kind)},
                         {QStringLiteral("sources"), sources},
                         {QStringLiteral("revisions"), revisions},
                         {QStringLiteral("destination"), display_path(batch.destination_directory)},
                         {QStringLiteral("index"), static_cast<qint64>(batch.index)},
                         {QStringLiteral("completed"), static_cast<qint64>(batch.completed)},
                     });
    }
    return seal_directory_transfer_state(state);
}

bool FileCommandController::Impl::persist_directory_transfer_recovery_state() {
    if (directoryTransferStateCorrupt_) return false;
    QByteArray state;
    if (directoryTransferRecoveryRequestId_ != 0U && !directoryTransferRecoverySource_.isEmpty() &&
        !directoryTransferRecoveryDestination_.isEmpty() &&
        !directoryTransferRecoverySourceRevision_.isEmpty()) {
        state = encode_directory_transfer_recovery_state(directoryTransferCompletionKnown_);
        if (state.isEmpty()) return false;
    }
    return ports_.write_recovery(state);
}

void FileCommandController::Impl::clear_directory_transfer_recovery_state() {
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_.clear();
    directoryTransferRecoveryDestination_.clear();
    directoryTransferRecoverySourceRevision_.clear();
    directoryTransferRecoveryRequestId_ = 0U;
    directoryTransferCompletionKnown_ = false;
    directoryTransferDiscarding_ = false;
    directoryTransferStateCorrupt_ = false;
    set_property("directoryTransferRecoveryPending", false);
    static_cast<void>(persist_directory_transfer_recovery_state());
}

void FileCommandController::Impl::show_unfinished_operation_dialog() {
    const auto state = refresh_current_operation_journal_state();
    if (state == CurrentOperationJournalState::none ||
        !operation_availability().unfinished_operation_allowed()) return;
    const auto loaded = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    fileops::TrashTransaction transaction;
    std::string detail;
    const auto is_trash = loaded.ok() &&
        loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded.kind == fileops::CurrentOperationKind::trash_move ||
         loaded.kind == fileops::CurrentOperationKind::trash_restore ||
         loaded.kind == fileops::CurrentOperationKind::trash_purge);
    const auto decoded = is_trash &&
        fileops::decode_trash_transaction(loaded.payload, transaction, detail);
    const auto cancellable = decoded && loaded.kind == fileops::CurrentOperationKind::trash_move &&
        (transaction.phase == fileops::TrashPhase::prepared ||
         transaction.phase == fileops::TrashPhase::moving ||
         transaction.phase == fileops::TrashPhase::cancel_remaining_intent ||
         transaction.phase == fileops::TrashPhase::cancel_container_remove_intent);
    const auto text = [](const char *source) {
        return QCoreApplication::translate("TrashRecoveryDialog", source);
    };
    QDialog dialog(dialog_owner());
    dialog.setObjectName(QStringLiteral("unfinishedOperationDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(text("Unfinished operation…"));
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    const auto add_label = [&](const QString &value) {
        auto *label = new QLabel(value, &dialog);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addWidget(label);
    };
    if (decoded) {
        const auto stored = std::ranges::count_if(transaction.items, [](const auto &item) {
            return item.location == fileops::TrashItemLocation::stored;
        });
        add_label(text("Recorded moves to Trash: %1 of %2")
            .arg(static_cast<qulonglong>(stored)).arg(transaction.items.size()));
        if (transaction.failure_status != fileops::OperationStatus::success) {
            add_label(trash_operation_error_text(transaction.failure_status,
                                                 transaction.failure_detail_utf8));
        }
    } else {
        add_label(operationStatus_);
    }
    add_label(cancellable
        ? text("Already moved items stay in Trash. Remaining source files stay in place.")
        : text("This operation can be retried; cancelling it here is not supported."));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *retry = buttons->addButton(text("Retry recovery"), QDialogButtonBox::ActionRole);
    retry->setObjectName(QStringLiteral("retryRecoveryButton"));
    retry->setAutoDefault(false);
    connect(retry, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(1); });
    if (cancellable) {
        auto *cancel = buttons->addButton(text("Cancel remaining moves"), QDialogButtonBox::ActionRole);
        cancel->setObjectName(QStringLiteral("cancelRemainingMovesButton"));
        cancel->setAutoDefault(false);
        connect(cancel, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
    }
    auto *close = buttons->button(QDialogButtonBox::Close);
    close->setDefault(true);
    close->setFocus();
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto choice = dialog.exec();
    if (choice == 1) {
        if (ports_.refresh_catalog) ports_.refresh_catalog();
        else static_cast<void>(refresh_or_recover());
    }
    else if (choice == 2 && !submit_trash_recovery(true)) {
        operationStatus_ = text("The operation could not be started. Its record was kept.");
        publish_status();
    }
}

} // namespace vove::ui
