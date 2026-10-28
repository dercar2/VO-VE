#include "file_command_workflow.hpp"

namespace vove::ui {
using namespace file_command_detail;

bool FileCommandController::Impl::prompt_move_entries_to_trash(const QList<core::DirectoryEntry> &entries) {
    const auto all_deletable_objects =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        });
    if (!all_deletable_objects) {
        return false;
    }
#ifdef Q_OS_WIN
    const auto all_local = std::ranges::all_of(entries, [](const auto &entry) {
        return entry.source_revision_utf8.starts_with("win-file128:") &&
               !is_unc_path(QString::fromUtf8(entry.path_utf8));
    });
    if (!all_local) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash is unavailable for this resource");
        publish_status();
        return false;
    }
#endif
    if (trashConfirmationInProgress_ ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        directoryTransferService_.busy() || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        publish_status();
        return false;
    }

    QList<qsizetype> directory_indices;
    for (qsizetype index{}; index < entries.size(); ++index) {
        if (entries.at(index).kind == core::EntryKind::directory) {
            directory_indices.push_back(index);
        }
    }
    if (!directory_indices.isEmpty()) {
        prepare_directory_entries(entries, std::move(directory_indices),
            [this](QList<core::DirectoryEntry> prepared) {
                set_rename_in_flight(false);
                static_cast<void>(confirm_and_start_trash_move(prepared));
            }, true);
        return true;
    }
    return confirm_and_start_trash_move(entries);
}

bool FileCommandController::Impl::confirm_and_start_trash_move(const QList<core::DirectoryEntry> &entries) {
    if (entries.isEmpty() || trashConfirmationInProgress_) {
        return false;
    }

    std::uint64_t total_bytes{};
    bool contains_directory = false;
    for (const auto &entry : entries) {
        total_bytes += entry.kind == core::EntryKind::file ? entry.size_bytes : 0U;
        contains_directory = contains_directory || entry.kind == core::EntryKind::directory;
    }
    trashConfirmationInProgress_ = true;
    set_property("trashConfirmationInProgress", true);
    update_rename_action();
    QMessageBox confirmation(
        QMessageBox::Question, QCoreApplication::translate("MainWindow", "Move to VO-VE Trash?"),
        QCoreApplication::translate("MainWindow", "Objects: %1 · folders: %2 · files: %3")
            .arg(entries.size())
            .arg(std::ranges::count_if(
                entries,
                [](const auto &entry) { return entry.kind == core::EntryKind::directory; }))
            .arg(std::ranges::count_if(
                entries, [](const auto &entry) { return entry.kind == core::EntryKind::file; })),
        QMessageBox::NoButton, dialog_owner());
    confirmation.setObjectName(QStringLiteral("trashConfirmation"));
    if (!contains_directory) {
        confirmation.setInformativeText(
            QCoreApplication::translate("MainWindow", "Total size: %1")
                .arg(QLocale().formattedDataSize(static_cast<qint64>(total_bytes))));
    }
    auto *move = confirmation.addButton(QCoreApplication::translate("MainWindow", "Move to Trash"),
                                        QMessageBox::AcceptRole);
    move->setObjectName(QStringLiteral("moveToTrashConfirmButton"));
    confirmation.addButton(QCoreApplication::translate("MainWindow", "Cancel"),
                           QMessageBox::RejectRole);
    confirmation.exec();
    trashConfirmationInProgress_ = false;
    set_property("trashConfirmationInProgress", false);
    update_rename_action();
    if (confirmation.clickedButton() != move ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        directoryTransferService_.busy() || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        return false;
    }

    return submit_trash_move(entries);
}

bool FileCommandController::Impl::submit_trash_move(const QList<core::DirectoryEntry> &entries) {
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy()) {
        publish_status();
        return false;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "VO-VE Trash: preparing…");
    set_delete_in_flight(true);
    publish_status();
    ports_.suspend_preview_dispatch();
    begin_preview_generation();
    ports_.stop_preview_readers();

    std::vector<fileops::TrashSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back(
            {.path = native_path(QString::fromUtf8(entry.path_utf8)),
             .snapshot = {.size_bytes = entry.kind == core::EntryKind::file ? entry.size_bytes : 0U,
                          .modified_unix_ns = entry.modified_unix_ns,
                          .source_revision_utf8 = entry.source_revision_utf8},
             .kind = entry.kind == core::EntryKind::directory
                         ? fileops::TrashItemKind::directory
                         : fileops::TrashItemKind::regular_file,
             .payload_bytes = entry.kind == core::EntryKind::file ? entry.size_bytes : 0U,
             .directory_entries = {},
             .directory_security_descriptors_sddl_utf8 = {},
             .storage_identity_utf8 = {},
             .original_security_descriptor_sddl_utf8 = {},
             .restore_path = {}});
    }
    const QPointer<Impl> window(this);
    const auto submitted = trashCoordinator_.start(
        std::move(sources),
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
        [window](fileops::TrashResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_result(result, TrashOperationIntent::move);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
        resume_preview_dispatch(0);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
    }
    return submitted;
}

void FileCommandController::Impl::handle_trash_progress(const fileops::TrashProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::TrashPhase::prepared:
    case fileops::TrashPhase::restore_prepared:
    case fileops::TrashPhase::purge_prepared:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::TrashPhase::moving:
        phase = QCoreApplication::translate("MainWindow", "Moving");
        break;
    case fileops::TrashPhase::rollback:
    case fileops::TrashPhase::rollback_container_remove_intent:
        phase = QCoreApplication::translate("MainWindow", "rolling back move");
        break;
    case fileops::TrashPhase::manifest_intent:
    case fileops::TrashPhase::cancel_remaining_intent:
        phase = QCoreApplication::translate("MainWindow", "saving operation");
        break;
    case fileops::TrashPhase::published:
        phase = QCoreApplication::translate("MainWindow", "done");
        break;
    case fileops::TrashPhase::restoring:
        phase = QCoreApplication::translate("MainWindow", "restoring");
        break;
    case fileops::TrashPhase::restore_rollback:
        phase = QCoreApplication::translate("MainWindow", "rolling back restore");
        break;
    case fileops::TrashPhase::manifest_remove_intent:
    case fileops::TrashPhase::restore_container_remove_intent:
        phase = QCoreApplication::translate("MainWindow", "finishing restore");
        break;
    case fileops::TrashPhase::restored:
        phase = QCoreApplication::translate("MainWindow", "restored");
        break;
    case fileops::TrashPhase::purging:
        phase = QCoreApplication::translate("MainWindow", "permanent deletion");
        break;
    case fileops::TrashPhase::purge_manifest_remove_intent:
    case fileops::TrashPhase::purge_container_remove_intent:
    case fileops::TrashPhase::cancel_container_remove_intent:
        phase = QCoreApplication::translate("MainWindow", "cleaning records");
        break;
    case fileops::TrashPhase::purged:
        phase = QCoreApplication::translate("MainWindow", "deleted");
        break;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "VO-VE Trash: %1 · %2 of %3")
                           .arg(phase)
                           .arg(progress.completed)
                           .arg(progress.total);
    publish_status();
}

void FileCommandController::Impl::handle_trash_result(const fileops::TrashResult &result,
                                     const TrashOperationIntent intent) {
    auto &diagnostic = DiagnosticSink::instance();
    if (diagnostic.active()) {
        diagnostic.record("trash.result",
                          {{"intent", std::to_string(static_cast<int>(intent))},
                           {"status", std::to_string(static_cast<int>(result.status))},
                           {"operation_status", std::to_string(static_cast<int>(result.operation_status))},
                           {"completed", std::to_string(result.completed)},
                           {"total", std::to_string(result.total)},
                           {"detail", result.detail_utf8}});
    }
    notify_file_in_use(result.operation_status);
    const auto restoring = intent == TrashOperationIntent::restore;
    const auto purging = intent == TrashOperationIntent::purge;
    const auto owns_recovery = trashCoordinator_.owns_recovery();
    set_property("trashRecoveryPending", owns_recovery);
    if (result.ok()) {
        operationStatus_ =
            intent == TrashOperationIntent::cancel_move
                ? QCoreApplication::translate("TrashRecoveryDialog",
                      "Remaining moves cancelled. Kept in Trash: %1").arg(result.completed)
            : purging     ? QCoreApplication::translate("MainWindow", "Files permanently deleted: %1")
                              .arg(result.completed)
            : restoring ? QCoreApplication::translate("MainWindow", "Files restored: %1")
                              .arg(result.completed)
                        : QCoreApplication::translate("MainWindow", "Moved to VO-VE Trash: %1")
                              .arg(result.completed);
        ports_.clear_selection();
        if (purging && trashPurgeTotal_ != 0) {
            ++trashPurgeCompleted_;
            set_delete_in_flight(false);
            if (!submit_next_trash_purge()) {
                trashPurgeQueue_.clear();
                trashPurgeTotal_ = 0;
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "Failed to continue emptying VO-VE Trash");
                publish_status();
            }
            return;
        }
        if (catalog().trash_view) {
            start_trash_catalog_load(true);
        } else {
            begin_preview_generation();
            refresh_directory_sources();
            poll_catalog(16);
            start_trash_catalog_load(false);
        }
    } else if (result.status == fileops::TrashRunStatus::rolled_back) {
        operationStatus_ =
            restoring ? QCoreApplication::translate("MainWindow",
                                                    "Restore cancelled; Trash was not changed")
                      : QCoreApplication::translate(
                            "MainWindow", "Deletion cancelled; original names were restored");
        refresh_directory_sources();
        poll_catalog();
    } else if (owns_recovery || result.status == fileops::TrashRunStatus::recovery_required ||
               result.status == fileops::TrashRunStatus::journal_error ||
               result.status == fileops::TrashRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash operation paused. Press F5 to recover");
        const auto reason = trash_operation_error_text(result.operation_status, result.detail_utf8);
        if (!reason.isEmpty()) {
            operationStatus_ += QStringLiteral(": ") + reason;
        }
        begin_preview_generation(false);
        ports_.mark_all_previews_unverified();
    } else if (result.status == fileops::TrashRunStatus::unsupported) {
#ifdef Q_OS_WIN
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash supports local files on a fixed NTFS drive");
#else
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash is unavailable for this file or filesystem");
#endif
    } else if (result.status == fileops::TrashRunStatus::insufficient_space) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Not enough free disk space for VO-VE Trash");
    } else if (result.status == fileops::TrashRunStatus::storage_unavailable) {
        operationStatus_ =
            result.completed == 0 && result.operation_status == fileops::OperationStatus::permission_denied
                ? QCoreApplication::translate("MainWindow",
                    "VO-VE Trash is unavailable on this resource. Originals were not changed")
                : QCoreApplication::translate("MainWindow", "Failed to check VO-VE Trash storage");
        if (!result.detail_utf8.empty()) {
            operationStatus_ += QStringLiteral(": ") +
                                QString::fromStdString(result.detail_utf8).left(512).simplified();
        }
    } else {
        operationStatus_ =
            purging ? QCoreApplication::translate("MainWindow", "Permanent deletion failed")
            : restoring
                ? QCoreApplication::translate("MainWindow", "Restore failed")
                : QCoreApplication::translate("MainWindow", "Files were not moved to Trash");
        const auto reason = trash_operation_error_text(result.operation_status, result.detail_utf8);
        if (!reason.isEmpty()) {
            operationStatus_ += QStringLiteral(": ") + reason;
        }
        const auto detail = QString::fromStdString(result.detail_utf8).left(512).simplified();
        if (!detail.isEmpty()) {
            operationStatus_ += QStringLiteral(": ") + detail;
        }
    }
    if (!result.ok() && trashPurgeTotal_ != 0) {
        trashPurgeQueue_.clear();
        trashPurgeCompleted_ = 0;
        trashPurgeTotal_ = 0;
    }
    set_delete_in_flight(false);
    set_property("trashOperationDetail", QString::fromStdString(result.detail_utf8));
    resume_preview_dispatch(0);
    publish_status();
}

bool FileCommandController::Impl::submit_next_trash_purge() {
    if (trashPurgeQueue_.empty()) {
        const auto completed = trashPurgeCompleted_;
        trashPurgeCompleted_ = 0;
        trashPurgeTotal_ = 0;
        operationStatus_ = QCoreApplication::translate("MainWindow", "Trash operations deleted: %1")
                               .arg(static_cast<qulonglong>(completed));
        start_trash_catalog_load();
        return true;
    }
    const auto manifest = trashPurgeQueue_.front();
    trashPurgeQueue_.erase(trashPurgeQueue_.begin());
    return start_trash_manifest_operation(manifest, TrashAction::purge, false);
}

bool FileCommandController::Impl::start_trash_manifest_operation(const std::filesystem::path &manifest,
                                                const TrashAction action,
                                                const bool confirm_permanent_delete) {
    if (action == TrashAction::none || manifest.empty() || renameInFlight_ || deleteInFlight_ ||
        transferInFlight_ || createDirectoryInFlight_ || fileTransferCoordinator_.busy() ||
        fileOperationService_.busy() || pendingRenameOperation_.has_value() ||
        batchRenameCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy() || directoryTransferService_.busy() ||
        pendingExternalDirectoryDrop_.has_value() || pendingExternalTransferDrop_.has_value() ||
        !dropProbeSource_.idle() ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return false;
    }
    const auto intent =
        action == TrashAction::purge ? TrashOperationIntent::purge : TrashOperationIntent::restore;
    std::size_t operation_items{1};
    operation_items = ports_.trash_operation_items(manifest);
    std::filesystem::path restore_destination;
    if (action == TrashAction::restore_to) {
        auto options = QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks;
#ifdef VOVE_UI_TEST_HOOKS
        options |= QFileDialog::DontUseNativeDialog;
#endif
        const auto chosen = QFileDialog::getExistingDirectory(
        dialog_owner(), QCoreApplication::translate("MainWindow", "Restore to…"), catalog().current_path, options);
        if (chosen.isEmpty()) {
            return false;
        }
        restore_destination = native_path(chosen);
    }
    if (intent == TrashOperationIntent::restore && operation_items > 1) {
        QMessageBox confirmation(
            QMessageBox::Question, QCoreApplication::translate("MainWindow", "Restore operation?"),
            QCoreApplication::translate(
                "MainWindow",
                "The selected object belongs to one deletion operation containing %1 objects. "
                "Restore the entire operation?")
                .arg(static_cast<qulonglong>(operation_items)),
            QMessageBox::Cancel, dialog_owner());
        confirmation.setObjectName(QStringLiteral("trashRestoreConfirmation"));
        auto *confirm = confirmation.addButton(QCoreApplication::translate("MainWindow", "Restore"),
                                               QMessageBox::AcceptRole);
        confirm->setObjectName(QStringLiteral("trashRestoreConfirmButton"));
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != confirm) {
            return false;
        }
    }
    if (intent == TrashOperationIntent::purge && confirm_permanent_delete) {
        QMessageBox confirmation(
            QMessageBox::Warning, QCoreApplication::translate("MainWindow", "Delete permanently?"),
            QCoreApplication::translate(
                "MainWindow",
                "%1 objects from the selected deletion operation will be deleted and cannot be "
                "restored.")
                .arg(static_cast<qulonglong>(operation_items)),
            QMessageBox::Cancel, dialog_owner());
        confirmation.setObjectName(QStringLiteral("trashPurgeConfirmation"));
        auto *confirm =
            confirmation.addButton(QCoreApplication::translate("MainWindow", "Delete permanently"),
                                   QMessageBox::DestructiveRole);
        confirm->setObjectName(QStringLiteral("trashPurgeConfirmButton"));
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != confirm) {
            return false;
        }
    }
    operationStatus_ =
        intent == TrashOperationIntent::purge
            ? QCoreApplication::translate("MainWindow",
                                          "VO-VE Trash: preparing permanent deletion…")
            : QCoreApplication::translate("MainWindow", "VO-VE Trash: preparing restore…");
    set_delete_in_flight(true);
    publish_status();
    // A decoder can still hold the file briefly after its generation was cancelled. Stop the
    // isolated helper before moving or deleting it, and gate new preview work until completion.
    ports_.suspend_preview_dispatch();
    begin_preview_generation();
    ports_.stop_preview_readers();
    const QPointer<Impl> window(this);
    const auto progress = [window](fileops::TrashProgress progress) mutable {
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
    };
    const auto completion = [window, intent](fileops::TrashResult result) mutable {
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
    };
    const auto submitted = intent == TrashOperationIntent::purge
                               ? trashCoordinator_.purge(manifest, progress, completion)
                           : action == TrashAction::restore_to
                               ? trashCoordinator_.restore_to(
                                     manifest, std::move(restore_destination), progress, completion)
                               : trashCoordinator_.restore(manifest, progress, completion);
    if (!submitted) {
        set_delete_in_flight(false);
        resume_preview_dispatch(0);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
    }
    return submitted;
}

} // namespace vove::ui
