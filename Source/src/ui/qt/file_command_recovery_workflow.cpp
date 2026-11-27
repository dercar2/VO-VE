#include "file_command_workflow.hpp"
#include "directory_transfer_settings.hpp"

#include <QApplication>
#include <QClipboard>

namespace vove::ui {
using namespace file_command_detail;

namespace {
enum class RecoveryChoice { close, retry, stop };

QString recovery_text(const char *source) {
    return QCoreApplication::translate("TrashRecoveryDialog", source);
}

RecoveryChoice show_recovery_actions(QWidget *owner, const QStringList &report,
                                    const bool can_retry, const bool can_stop) {
    QDialog dialog(owner);
    dialog.setObjectName(QStringLiteral("unfinishedOperationDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(recovery_text("Unfinished operation…"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *message = new QLabel(recovery_text("An operation could not be completed."), &dialog);
    message->setObjectName(QStringLiteral("recoveryMessage"));
    message->setWordWrap(true);
    layout->addWidget(message);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *retry = buttons->addButton(recovery_text("Retry"), QDialogButtonBox::ActionRole);
    auto *stop = buttons->addButton(recovery_text("Stop…"), QDialogButtonBox::ActionRole);
    auto *copy = buttons->addButton(recovery_text("Copy operation details"), QDialogButtonBox::ActionRole);
    auto *close = buttons->button(QDialogButtonBox::Close);
    retry->setObjectName(QStringLiteral("retryRecoveryButton"));
    stop->setObjectName(QStringLiteral("stopRecoveryButton"));
    copy->setObjectName(QStringLiteral("copyRecoveryDetailsButton"));
    close->setObjectName(QStringLiteral("closeRecoveryButton"));
    close->setText(recovery_text("Close"));
    retry->setEnabled(can_retry);
    stop->setEnabled(can_stop);
    for (auto *button : {retry, stop, copy}) button->setAutoDefault(false);
    close->setDefault(true);
    close->setFocus();
    QObject::connect(retry, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(1); });
    QObject::connect(stop, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
    QObject::connect(copy, &QPushButton::clicked, &dialog, [&report] {
        QApplication::clipboard()->setText(report.join(QLatin1Char('\n')));
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    switch (dialog.exec()) {
    case 1: return RecoveryChoice::retry;
    case 2: return RecoveryChoice::stop;
    default: return RecoveryChoice::close;
    }
}
} // namespace

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
    const auto transfer_recovery = objectTransferQueue_.archive_pending() || (
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded_operation.kind == fileops::CurrentOperationKind::file_transfer ||
         loaded_operation.kind == fileops::CurrentOperationKind::object_transfer));
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
    fileops::TrashTransaction move;
    std::string detail;
    const auto move_record = loaded.ok() &&
        loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        loaded.kind == fileops::CurrentOperationKind::trash_move &&
        fileops::decode_trash_transaction(loaded.payload, move, detail);
    const auto cancelling = cancel_remaining || (move_record &&
        (move.phase == fileops::TrashPhase::cancel_remaining_intent ||
         move.phase == fileops::TrashPhase::cancel_container_remove_intent));
    const auto report_id = cancelling && move_record ? move.operation_id : 0U;
    const auto intent = cancelling ? TrashOperationIntent::cancel_move
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
        [window, intent, report_id](fileops::TrashResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, intent, report_id, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->trashCancellationReportId_ = report_id;
                            window->trashCancellationReport_ = report_id != 0U && !result.ok()
                                ? trash_operation_error_text(result.operation_status, result.detail_utf8) : QString{};
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
    auto expected = ports_.read_recovery();
    if (expected.isEmpty()) {
        // A previous retirement may only need its final durability check. The store
        // accepts an absent key only when the exact archive already exists.
        expected = directoryTransferArchiveExpected_;
    }
    const auto expected_matches_loaded = [&] {
        const auto decoded = open_directory_transfer_state(expected);
        return decoded && decoded == open_directory_transfer_state(
            encode_directory_transfer_recovery_state(directoryTransferCompletionKnown_));
    };
    QStringList report{operationStatus_,
        QStringLiteral("VO-VE directory transfer %1").arg(directoryTransferRecoveryRequestId_),
        directoryTransferRecoverySource_, directoryTransferRecoveryDestination_};
    if (!directoryTransferRecoveryManifest_.isEmpty())
        report << directoryTransferRecoveryManifest_;
    if (directoryTransferDiscarding_) {
        report << recovery_text("Retry will check whether unfinished data can be safely removed.");
    }
    if (pendingDirectoryTransferBatch_) {
        const auto &batch = *pendingDirectoryTransferBatch_;
        report << QStringLiteral("Batch: %1/%2").arg(batch.completed).arg(batch.source_paths.size());
    }
    const auto profile = transfer_settings::profile_path(QSettings{});
    if (!profile.empty() && !expected.isEmpty()) {
        report << recovery_text("Record location after stopping: %1").arg(
            QDir::toNativeSeparators(display_path(transfer_settings::archive_path(profile, expected))));
    }
    const auto stopping = [&] {
        return !directoryTransferArchiveExpected_.isEmpty() ||
               transfer_settings::archive_pending(QSettings{}, expected);
    };
    const auto stop_pending = stopping();
    if (stop_pending) report << recovery_text("Finish stopping");
    const auto state_matches = expected_matches_loaded();
    const auto choice = show_recovery_actions(dialog_owner(), report,
        state_matches && !stop_pending && ports_.read_recovery() == expected,
        state_matches && bool(ports_.archive_recovery));
    if (choice == RecoveryChoice::close) return;
    bool request_discard = false;
    if (choice == RecoveryChoice::stop) {
        QMessageBox confirm(QMessageBox::Question, recovery_text("Stop…"),
            recovery_text("Stop processing the remaining queue? Original files, destinations and hidden "
                          "partial copies will stay where they are. You do not need to do anything else to use VO-VE."),
            QMessageBox::Yes | QMessageBox::Cancel, dialog_owner());
        confirm.setObjectName(QStringLiteral("directoryTransferArchiveConfirmation"));
        QPushButton *discard = nullptr;
        if (!stop_pending && !directoryTransferDiscarding_ && ports_.select_recovery) {
            discard = confirm.addButton(QCoreApplication::translate(
                "MainWindow", "Delete incomplete data"), QMessageBox::DestructiveRole);
            discard->setObjectName(QStringLiteral("directoryTransferDiscardButton"));
            confirm.setInformativeText(recovery_text(
                "If you choose to delete incomplete data instead, VO-VE will first check that it is safe."));
        }
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.setEscapeButton(QMessageBox::Cancel);
        const auto answer = confirm.exec();
        request_discard = discard && confirm.clickedButton() == discard;
        if (!request_discard) {
            if (answer != QMessageBox::Yes) return;
            bool archived = false;
            if (!mutation_in_flight() && !backend_busy() && !pendingRenameOperation_ &&
                expected_matches_loaded() && ports_.archive_recovery) {
                directoryTransferArchiveExpected_ = expected;
                archived = ports_.archive_recovery(expected);
            }
            if (!archived) {
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "Could not archive the local recovery record. Check the unfinished operation.");
            } else {
                pendingDirectoryTransferBatch_.reset();
                forget_directory_transfer_recovery_state();
                if (refresh_current_operation_journal_state() == CurrentOperationJournalState::none)
                    operationStatus_ = recovery_text(
                        "Operation stopped. Files were left as they are; the previous result is unknown. No action is needed.");
            }
            publish_status();
            return;
        }
    } else if (directoryTransferDiscarding_) {
        QMessageBox confirm(QMessageBox::Question, recovery_text("Retry"),
            recovery_text("Retry will check whether unfinished data can be safely removed."),
            QMessageBox::Yes | QMessageBox::Cancel, dialog_owner());
        confirm.setObjectName(QStringLiteral("directoryDiscardRetryConfirmation"));
        confirm.setDefaultButton(QMessageBox::Cancel);
        if (confirm.exec() != QMessageBox::Yes) return;
    }
    if (mutation_in_flight() || backend_busy() || stopping() || !expected_matches_loaded() ||
        ports_.read_recovery() != expected) {
        operationStatus_ = QCoreApplication::translate("TrashRecoveryDialog",
            "The operation could not be started. Its record was kept.");
        publish_status();
        return;
    }
    const auto previous = directoryTransferDiscarding_;
    directoryTransferDiscarding_ = previous || request_discard;
    const auto discarding = directoryTransferDiscarding_;
    if (discarding != previous && (!ports_.select_recovery ||
        !ports_.select_recovery(expected, encode_directory_transfer_recovery_state(directoryTransferCompletionKnown_)))) {
        directoryTransferDiscarding_ = previous;
        operationStatus_ = QCoreApplication::translate("MainWindow", "Failed to save the selected action");
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
    if (!directoryTransferArchiveExpected_.isEmpty() ||
        transfer_settings::archive_pending(QSettings{}, ports_.read_recovery())) return false;
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
                            window->handle_directory_transfer_result(*sharedResult, true);
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
    forget_directory_transfer_recovery_state();
    static_cast<void>(persist_directory_transfer_recovery_state());
}

void FileCommandController::Impl::forget_directory_transfer_recovery_state() {
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_.clear();
    directoryTransferRecoveryDestination_.clear();
    directoryTransferRecoverySourceRevision_.clear();
    directoryTransferRecoveryRequestId_ = 0U;
    directoryTransferCompletionKnown_ = false;
    directoryTransferDiscarding_ = false;
    directoryTransferArchiveExpected_.clear();
    directoryTransferStateCorrupt_ = false;
    set_property("directoryTransferRecoveryPending", false);
}

bool FileCommandController::Impl::show_unfinished_operation_dialog() {
    if (recoveryDialogOpen_) return true;
    if (mutation_in_flight() || backend_busy()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Waiting for the file operation result…");
        publish_status();
        return true;
    }
    const auto state = refresh_current_operation_journal_state();
    if (state == CurrentOperationJournalState::none && !pendingRenameOperation_) return false;
    const QScopedValueRollback<bool> guard(recoveryDialogOpen_, true);
    if (state == CurrentOperationJournalState::directory_transfer_recovery && !directoryTransferStateCorrupt_) {
        offer_directory_transfer_discard();
        return true;
    }
    const auto loaded = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    const auto archiving = objectTransferQueue_.archive_pending();
    const auto diagnostic_only = state == CurrentOperationJournalState::blocked || directoryTransferStateCorrupt_;
    const auto object_transfer = !diagnostic_only && (archiving || (loaded.ok() &&
        loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        loaded.kind == fileops::CurrentOperationKind::object_transfer));
    fileops::TrashTransaction transaction;
    std::string detail;
    const auto is_trash = !diagnostic_only && loaded.ok() &&
        loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded.kind == fileops::CurrentOperationKind::trash_move ||
         loaded.kind == fileops::CurrentOperationKind::trash_restore ||
         loaded.kind == fileops::CurrentOperationKind::trash_purge);
    const auto decoded = is_trash &&
        fileops::decode_trash_transaction(loaded.payload, transaction, detail);
    const auto cancellable = decoded && !archiving && loaded.kind == fileops::CurrentOperationKind::trash_move &&
        (transaction.phase == fileops::TrashPhase::prepared ||
         transaction.phase == fileops::TrashPhase::moving ||
         transaction.phase == fileops::TrashPhase::cancel_remaining_intent ||
         transaction.phase == fileops::TrashPhase::cancel_container_remove_intent);
    const auto last_cancel_report = cancellable && trashCancellationReportId_ == transaction.operation_id
                                       ? trashCancellationReport_ : QString{};
    QStringList report{operationStatus_};
    report << (directoryTransferStateCorrupt_
        ? QSettings{}.fileName()
        : QDir::toNativeSeparators(display_path(currentOperationJournalPath_)));
    if (decoded) {
        const auto stored = std::ranges::count_if(transaction.items, [](const auto &item) {
            return item.location == fileops::TrashItemLocation::stored;
        });
        report << QStringLiteral("VO-VE Trash operation %1").arg(transaction.operation_id)
               << recovery_text("Recorded moves to Trash: %1 of %2")
                      .arg(static_cast<qulonglong>(stored)).arg(transaction.items.size());
        if (!last_cancel_report.isEmpty()) report << last_cancel_report;
        if (transaction.failure_status != fileops::OperationStatus::success) {
            report << trash_operation_error_text(transaction.failure_status, transaction.failure_detail_utf8)
                   << QString::fromStdString(transaction.failure_detail_utf8);
        }
        const auto limit = std::min<std::size_t>(transaction.items.size(), 64U);
        for (std::size_t i = 0; i < limit; ++i) {
            const auto &item = transaction.items[i];
            report << QStringLiteral("%1/%2: %3 -> %4").arg(i + 1U).arg(transaction.items.size())
                .arg(display_path(item.restore_path), display_path(item.stored));
        }
        if (limit < transaction.items.size()) report << QStringLiteral("…");
    } else if (object_transfer) {
        const auto value = QJsonDocument::fromJson(QByteArray(
            reinterpret_cast<const char *>(loaded.payload.data()), qsizetype(loaded.payload.size()))).object();
        const auto items = value.value("items").toArray();
        const auto index = value.value("index").toInteger(-1);
        if (index >= 0 && index < items.size()) {
            const auto item = items[index].toObject();
            report << item.value("source").toString() << item.value("destination").toString();
        }
        const auto failure = property("objectTransferDetail").toString();
        if (!failure.isEmpty()) report << failure;
        report << (archiving
            ? recovery_text("Finish stopping")
            : recovery_text("Stopping leaves original files, destinations and hidden partial copies in place. The outcome remains unknown. You do not need to do anything else to use VO-VE."));
    } else if (!diagnostic_only && loaded.ok() &&
               loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
               loaded.kind == fileops::CurrentOperationKind::file_transfer) {
        fileops::FileTransferTransaction transfer;
        if (fileops::decode_file_transfer_transaction(loaded.payload, transfer, detail)) {
            report << QStringLiteral("VO-VE file transfer %1").arg(transfer.operation_id);
            if (transfer.active_step != fileops::FileTransferStep::none &&
                transfer.active_index < transfer.items.size()) {
                const auto &item = transfer.items[transfer.active_index];
                report << display_path(item.source) << display_path(item.destination);
            }
            if (!transfer.failure_detail_utf8.empty())
                report << QString::fromStdString(transfer.failure_detail_utf8);
        }
    } else if (pendingRenameOperation_) {
        report << pendingRenameOperation_->source_path << pendingRenameOperation_->destination_path;
    }
    if (diagnostic_only) {
        report << recovery_text("The record cannot be safely read. No action was taken. Copy its location for support.");
    }
    const auto can_retry = !diagnostic_only && !archiving &&
        (object_transfer || trashCoordinator_.owns_recovery() || fileTransferCoordinator_.owns_recovery() ||
         permanentDeleteCoordinator_.owns_recovery() || batchRenameCoordinator_.owns_recovery() ||
         pendingRenameOperation_);
    if (cancellable) {
        report << recovery_text("Cancel checks where the recorded items are. Confirmed moves stay in Trash. "
                               "If verification fails, the operation stays paused. Copy its details for support; "
                               "do not delete hidden files or reset the record.");
    } else if (can_retry && !object_transfer) {
        report << recovery_text("This operation can be retried; cancelling it here is not supported.");
    }
    const auto choice = show_recovery_actions(dialog_owner(), report, can_retry, cancellable || object_transfer);
    if (choice == RecoveryChoice::close) return true;
    if (choice == RecoveryChoice::stop) {
        QMessageBox confirm(QMessageBox::Question, recovery_text("Stop…"),
            cancellable
                ? recovery_text("Stop the remaining moves to Trash? Confirmed moves stay in Trash.")
                : recovery_text("Stop processing the remaining queue? Original files, destinations and hidden "
                                "partial copies will stay where they are. You do not need to do anything else to use VO-VE."),
            QMessageBox::Yes | QMessageBox::Cancel, dialog_owner());
        confirm.setObjectName(cancellable ? QStringLiteral("trashStopConfirmation")
                                          : QStringLiteral("stopObjectTransferConfirmation"));
        confirm.setDefaultButton(QMessageBox::Cancel);
        if (confirm.exec() != QMessageBox::Yes) return true;
    }
    const auto current = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    bool submitted = false;
    if (!mutation_in_flight() && !backend_busy() && current.status == loaded.status &&
        current.kind == loaded.kind && current.encoding == loaded.encoding &&
        current.payload == loaded.payload && objectTransferQueue_.archive_pending() == archiving) {
        if (choice == RecoveryChoice::stop) {
            if (cancellable) submitted = submit_trash_recovery(true);
            else if (object_transfer) {
                const auto payload = QByteArray(reinterpret_cast<const char *>(loaded.payload.data()),
                                                qsizetype(loaded.payload.size()));
                const auto digest = archiving ? QByteArray{} :
                    QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
                submitted = start_object_transfer({}, {}, fileops::FileTransferKind::copy, true, digest);
            }
        } else if (object_transfer) {
            if (ports_.prepare_recovery_catalog) ports_.prepare_recovery_catalog();
            submitted = start_object_transfer({}, {}, fileops::FileTransferKind::copy, true);
        } else if (trashCoordinator_.owns_recovery()) submitted = submit_trash_recovery();
        else if (fileTransferCoordinator_.owns_recovery()) {
            if (ports_.prepare_recovery_catalog) ports_.prepare_recovery_catalog();
            submitted = submit_file_transfer_recovery();
        }
        else if (permanentDeleteCoordinator_.owns_recovery()) submitted = submit_delete_recovery();
        else if (batchRenameCoordinator_.owns_recovery()) submitted = submit_batch_rename_recovery();
        else if (pendingRenameOperation_) submitted = submit_pending_rename_reconciliation();
    }
    if (!submitted) {
        operationStatus_ = recovery_text("The operation could not be started. Its record was kept.");
        publish_status();
    }
    return true;
}

} // namespace vove::ui
