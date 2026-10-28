#include "file_command_workflow.hpp"

namespace vove::ui {
using namespace file_command_detail;

void FileCommandController::Impl::prompt_create_directory() {
    if (createDirectoryInFlight_ || renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
        return;
    }
    const auto parent = native_path(catalog().current_path).lexically_normal();
    bool accepted{};
    const auto name =
        QInputDialog::getText(dialog_owner(), QCoreApplication::translate("MainWindow", "Create Folder"),
                              QCoreApplication::translate("MainWindow", "Folder name:"),
                              QLineEdit::Normal,
                              QCoreApplication::translate("MainWindow", "New folder"), &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    std::string validation;
    const auto filename = native_path(name);
    if (!fileops::valid_destination_filename(filename, validation)) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Invalid folder name");
        publish_status();
        return;
    }
    if (createDirectoryInFlight_ || renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        native_path(catalog().current_path).lexically_normal() != parent ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The folder changed. Retry creating the folder");
        publish_status();
        return;
    }

    const auto generation = nextTransferProbeGeneration_++;
    pendingCreateDirectoryPreparation_ = {.generation = generation,
                                          .destination = (parent / filename).lexically_normal()};
    set_create_directory_in_flight(true);
    operationStatus_ = QCoreApplication::translate("MainWindow", "Checking current folder…");
    publish_status();
    transferProbeSource_.submit({.generation = generation, .path = parent, .maximum_entries = 1});
    transferProbeTimer_->start();
}

void FileCommandController::Impl::prompt_rename_selected() {
    const auto entries = selected_entries_in_view_order();
    if (entries.isEmpty() || !std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        })) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return;
    }
    if (entries.size() > 1) {
        prompt_batch_rename_selected(entries);
        return;
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        batchRenameCoordinator_.recovery_pending() || fileTransferCoordinator_.recovery_pending() ||
        trashCoordinator_.recovery_pending()) {
        return;
    }
    const auto &entry = entries.front();
    const auto source_path = QString::fromUtf8(entry.path_utf8);
    const QFileInfo source_info(source_path);
    const auto directory = entry.kind == core::EntryKind::directory;
    RenameDialog dialog(source_info.fileName(), dialog_owner(), directory);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto destination_name = dialog.file_name();
    if (destination_name.isEmpty() || destination_name == source_info.fileName()) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return;
    }
    const auto current_entries = selected_entries_in_view_order();
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        batchRenameCoordinator_.recovery_pending() || fileTransferCoordinator_.recovery_pending() ||
        trashCoordinator_.recovery_pending() || current_entries.size() != 1 ||
        current_entries.front().kind != entry.kind ||
        current_entries.front().path_utf8 != entry.path_utf8 ||
        current_entries.front().size_bytes != entry.size_bytes ||
        current_entries.front().modified_unix_ns != entry.modified_unix_ns ||
        current_entries.front().source_revision_utf8 != entry.source_revision_utf8) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file list changed. Retry renaming");
        publish_status();
        return;
    }
    prepare_directory_entries(entries, directory ? QList<qsizetype>{0} : QList<qsizetype>{},
                           [this, destination_name](QList<core::DirectoryEntry> prepared) {
                               submit_single_rename(prepared.front(), destination_name);
                           });
}

void FileCommandController::Impl::submit_single_rename(const core::DirectoryEntry &entry,
                                      const QString &destination_name) {
    const auto source_path = QString::fromUtf8(entry.path_utf8);
    const auto directory = entry.kind == core::EntryKind::directory;
    const auto destination_path = QFileInfo(source_path).dir().filePath(destination_name);
    static std::atomic_uint64_t next_operation{1};
    fileops::RenameRequest request{
        .operation_id = next_operation.fetch_add(1, std::memory_order_relaxed),
        .mode = !directory && native_path(source_path).extension() == native_path(destination_path).extension()
                    ? fileops::RenameMode::single_preserve_extension
                    : fileops::RenameMode::single_allow_extension_change,
        .object_kind = directory ? fileops::OperationObjectKind::directory
                                 : fileops::OperationObjectKind::regular_file,
        .source = native_path(source_path),
        .destination = native_path(destination_path),
        .expected_source = {.size_bytes = entry.size_bytes,
                            .modified_unix_ns = entry.modified_unix_ns,
                            .source_revision_utf8 = entry.source_revision_utf8},
        .source_parent_identity_utf8 = {},
        .destination_parent_identity_utf8 = {},
        .destination_anchor_path = {},
        .destination_anchor_identity_utf8 = {}};
    PendingRenameOperation operation{
        .request = request, .source_path = source_path, .destination_path = destination_path};
    auto submitted_request = operation.request;
    operationStatus_ = QCoreApplication::translate("MainWindow", "Renaming…");
    publish_status();
    const QPointer<Impl> window(this);
    set_rename_in_flight(true);
    if (!fileOperationService_.submit_rename(
            std::move(submitted_request),
            [window, operation = std::move(operation)](fileops::OperationResult result) mutable {
                if (window.isNull()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result),
                     operation = std::move(operation)]() mutable {
                        if (!window.isNull()) {
                            window->handle_rename_result(result, std::move(operation));
                        }
                    },
                    Qt::QueuedConnection);
            })) {
        set_rename_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
    }
}

void FileCommandController::Impl::prompt_batch_rename_selected(const QList<core::DirectoryEntry> &entries) {
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return;
    }
    if (entries.size() < 2 || renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
        createDirectoryInFlight_ || fileTransferCoordinator_.busy() ||
        fileOperationService_.busy() || pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy()) {
        return;
    }

    std::vector<fileops::BatchRenameSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back({.path = native_path(QString::fromUtf8(entry.path_utf8)),
                           .size_bytes = entry.size_bytes,
                           .modified_unix_ns = entry.modified_unix_ns,
                           .source_revision_utf8 = entry.source_revision_utf8,
                           .object_kind = entry.kind == core::EntryKind::directory
                                              ? fileops::OperationObjectKind::directory
                                              : fileops::OperationObjectKind::regular_file});
    }
    const auto parent = QFileInfo(QString::fromUtf8(entries.front().path_utf8)).absolutePath();
    if (!std::ranges::all_of(entries, [&](const auto &entry) {
            return same_directory_path(parent, QFileInfo(QString::fromUtf8(entry.path_utf8)).absolutePath());
        })) return;
    BatchRenameDialog dialog(sources, ports_.entry_names(catalog().recursive_view ? parent : QString{}), dialog_owner());
    if (dialog.exec() != QDialog::Accepted || !dialog.plan().valid()) {
        return;
    }
    if (std::ranges::all_of(dialog.plan().rows, [](const auto &row) {
            return row.source == row.destination;
        })) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Names unchanged");
        publish_status();
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return;
    }

    const auto current_entries = selected_entries_in_view_order();
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        batchRenameCoordinator_.recovery_pending() || fileTransferCoordinator_.recovery_pending() ||
        trashCoordinator_.recovery_pending() || !same_entry_snapshots(entries, current_entries)) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file list changed. Retry renaming");
        publish_status();
        return;
    }

    QList<qsizetype> directory_indices;
    for (qsizetype index{}; index < entries.size(); ++index) {
        const auto &row = dialog.plan().rows.at(static_cast<std::size_t>(index));
        if (entries.at(index).kind == core::EntryKind::directory && row.source != row.destination) {
            directory_indices.push_back(index);
        }
    }
    prepare_directory_entries(entries, std::move(directory_indices),
                           [this, plan = dialog.plan()](QList<core::DirectoryEntry> prepared) {
                               submit_batch_rename(plan, prepared);
                           });
}

void FileCommandController::Impl::submit_batch_rename(const fileops::BatchRenamePlan &plan,
                                     const QList<core::DirectoryEntry> &entries) {
    std::vector<fileops::BatchRenameSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back({.path = native_path(QString::fromUtf8(entry.path_utf8)),
                           .size_bytes = entry.size_bytes,
                           .modified_unix_ns = entry.modified_unix_ns,
                           .source_revision_utf8 = entry.source_revision_utf8,
                           .object_kind = entry.kind == core::EntryKind::directory
                                              ? fileops::OperationObjectKind::directory
                                              : fileops::OperationObjectKind::regular_file});
    }
    activeBatchSourcePaths_.clear();
    activeBatchSourcePaths_.reserve(entries.size());
    activeBatchUnchangedPaths_.clear();
    for (const auto &row : plan.rows) {
        if (row.source == row.destination) activeBatchUnchangedPaths_.push_back(display_path(row.source));
    }
    for (const auto &entry : entries) {
        activeBatchSourcePaths_.push_back(QString::fromUtf8(entry.path_utf8));
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Batch rename: preparing…");
    publish_status();
    set_rename_in_flight(true);
    const QPointer<Impl> window(this);
    const auto submitted = batchRenameCoordinator_.start(
        plan, std::move(sources),
        [window](fileops::BatchRenameProgress progress) mutable {
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, progress = std::move(progress)]() mutable {
                    if (!window.isNull()) {
                        window->handle_batch_rename_progress(progress);
                    }
                },
                Qt::QueuedConnection);
        },
        [window](fileops::BatchRenameResult result) mutable {
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, result = std::move(result)]() mutable {
                    if (!window.isNull()) {
                        window->handle_batch_rename_result(result);
                    }
                },
                Qt::QueuedConnection);
        });
    if (!submitted) {
        activeBatchSourcePaths_.clear();
        set_rename_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
    }
}

void FileCommandController::Impl::prompt_delete_selected() {
    const auto entries = selected_entries_in_view_order();
    const auto all_deletable_objects =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        });
    if (!all_deletable_objects) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return;
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy()) {
        return;
    }

#ifdef Q_OS_WIN
    const auto is_remote = [](const auto &entry) {
        return entry.source_revision_utf8.starts_with("win-smb64:") ||
               is_unc_path(QString::fromUtf8(entry.path_utf8));
    };
    const auto is_local = [](const auto &entry) {
        return entry.source_revision_utf8.starts_with("win-file128:") &&
               !is_unc_path(QString::fromUtf8(entry.path_utf8));
    };
    const auto all_remote = std::ranges::all_of(entries, is_remote);
    const auto any_remote = std::ranges::any_of(entries, is_remote);
    const auto all_local = std::ranges::all_of(entries, is_local);
    const auto any_directory = std::ranges::any_of(
        entries, [](const auto &entry) { return entry.kind == core::EntryKind::directory; });
#else
    // POSIX uses the private same-parent VO-VE Trash for local and mounted network files.
    // The helper verifies the supplied identity and performs the rename inside the source
    // filesystem, so a mounted SMB source remains recoverable without a network copy.
    constexpr bool all_remote = false;
    constexpr bool any_remote = false;
    constexpr bool all_local = true;
    constexpr bool any_directory = false;
#endif
    std::uint64_t total_bytes{};
    for (const auto &entry : entries) {
        total_bytes += entry.size_bytes;
    }
    if (!all_remote) {
        if (any_remote) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Local and network files must be deleted in separate operations");
            publish_status();
            return;
        }
        if (!all_local) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow",
                "The storage type could not be determined reliably. Deletion was not started");
            publish_status();
            return;
        }
        static_cast<void>(prompt_move_entries_to_trash(entries));
        return;
    }
    if (any_directory) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash is unavailable for network folders on Windows");
        publish_status();
        return;
    }

    QMessageBox confirmation(
        QMessageBox::Warning, QCoreApplication::translate("MainWindow", "Delete permanently?"),
        QCoreApplication::translate("MainWindow", "Delete files: %1").arg(entries.size()),
        QMessageBox::NoButton, dialog_owner());
    confirmation.setObjectName(QStringLiteral("permanentDeleteConfirmation"));
    confirmation.setInformativeText(
        QCoreApplication::translate(
            "MainWindow",
            "Path: %1\nTotal size: %2\nTrash is not guaranteed on a network resource. "
            "This deletion cannot be undone.")
            .arg(QDir::toNativeSeparators(catalog().current_path))
            .arg(QLocale().formattedDataSize(static_cast<qint64>(total_bytes))));
    auto *acknowledge =
        new QCheckBox(QCoreApplication::translate(
                          "MainWindow", "I understand that the files will be deleted permanently"),
                      &confirmation);
    acknowledge->setObjectName(QStringLiteral("permanentDeleteAcknowledge"));
    confirmation.setCheckBox(acknowledge);
    auto *remove =
        confirmation.addButton(QCoreApplication::translate("MainWindow", "Delete permanently"),
                               QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("permanentDeleteConfirmButton"));
    remove->setEnabled(false);
    confirmation.addButton(QCoreApplication::translate("MainWindow", "Cancel"),
                           QMessageBox::RejectRole);
    connect(acknowledge, &QCheckBox::toggled, remove, &QPushButton::setEnabled);
    confirmation.exec();
    if (confirmation.clickedButton() != remove || !acknowledge->isChecked()) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        publish_status();
        return;
    }

    const auto current_entries = selected_entries_in_view_order();
    if (!same_entry_snapshots(entries, current_entries) || renameInFlight_ || deleteInFlight_ ||
        transferInFlight_ || createDirectoryInFlight_ || fileTransferCoordinator_.busy() ||
        batchRenameCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy() || batchRenameCoordinator_.recovery_pending() ||
        fileTransferCoordinator_.recovery_pending()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file list changed. Retry deletion");
        publish_status();
        return;
    }

    std::vector<fileops::PermanentDeleteSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back({.path = native_path(QString::fromUtf8(entry.path_utf8)),
                           .snapshot = {.size_bytes = entry.size_bytes,
                                        .modified_unix_ns = entry.modified_unix_ns,
                                        .source_revision_utf8 = entry.source_revision_utf8}});
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Safe deletion: preparing…");
    set_delete_in_flight(true);
    publish_status();
    const QPointer<Impl> window(this);
    const auto submitted = permanentDeleteCoordinator_.start(
        std::move(sources),
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
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        publish_status();
    }
}

void FileCommandController::Impl::handle_rename_result(const fileops::OperationResult &result,
                                      PendingRenameOperation operation) {
    notify_file_in_use(result.status);
    operationStatus_ = operation_status_text(result.status);
    if (result.ok()) {
        refresh_after_rename(operation.destination_path);
    } else if (confirms_no_committed_rename(result)) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Rename failed");
        refresh_after_rename(operation.source_path);
    } else if (ambiguous_rename_status(result.status)) {
        const auto source_path = operation.source_path;
        pendingRenameOperation_ = std::move(operation);
        set_property("pendingRenameReconciliation", true);
        begin_preview_generation(false);
        ports_.mark_preview_unverified(source_path);
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Rename result is unverified. Press F5 to check");
    }
    set_rename_in_flight(false);
    publish_status();
}

void FileCommandController::Impl::handle_create_directory_result(const fileops::OperationResult &result,
                                                const QString &destination) {
    set_create_directory_in_flight(false);
    if (result.ok()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Folder created");
        if (transfer_path_key(native_path(destination).parent_path()) ==
            transfer_path_key(native_path(catalog().current_path))) {
            refresh_after_rename(destination);
        }
    } else if (result.status == fileops::OperationStatus::conflict) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "A folder with this name already exists");
        refresh_directory_sources();
        poll_catalog();
    } else if (result.status == fileops::OperationStatus::source_changed) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The current folder changed. Refresh it and try again");
        refresh_directory_sources();
        poll_catalog();
    } else if (result.status == fileops::OperationStatus::permission_denied) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "No permission to create a folder");
    } else if (result.status == fileops::OperationStatus::authentication_required) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Network folder sign-in required");
    } else if (result.status == fileops::OperationStatus::invalid_request) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Invalid folder name");
    } else if (result.status == fileops::OperationStatus::unsupported) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Safe folder creation is not available on this system yet");
    } else if (result.evidence == fileops::OperationEvidence::committed) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder created, but the directory is currently unavailable");
    } else if (result.evidence == fileops::OperationEvidence::no_commit) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Folder was not created");
    } else {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder creation could not be verified. Refresh the folder");
    }
    publish_status();
}

bool FileCommandController::Impl::submit_pending_rename_reconciliation() {
    const auto pending = pendingRenameOperation_;
    if (!pending) {
        return false;
    }
    if (renameInFlight_ || fileOperationService_.busy()) {
        return false;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Checking rename result…");
    publish_status();
    const QPointer<Impl> window(this);
    const auto request = pending->request;
    set_rename_in_flight(true);
    const auto submitted = fileOperationService_.submit_reconciliation(
        request, [window](fileops::OperationResult result) mutable {
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, result = std::move(result)]() mutable {
                    if (!window.isNull()) {
                        window->handle_rename_reconciliation_result(result);
                    }
                },
                Qt::QueuedConnection);
        });
    if (!submitted) {
        set_rename_in_flight(false);
    }
    return submitted;
}

void FileCommandController::Impl::handle_rename_reconciliation_result(const fileops::OperationResult &result) {
    if (!pendingRenameOperation_) {
        return;
    }
    if (result.ok()) {
        auto destination = std::move(pendingRenameOperation_->destination_path);
        pendingRenameOperation_.reset();
        set_property("pendingRenameReconciliation", false);
        operationStatus_ = operation_status_text(result.status);
        refresh_after_rename(destination);
    } else if (confirms_no_committed_rename(result)) {
        auto source = std::move(pendingRenameOperation_->source_path);
        pendingRenameOperation_.reset();
        set_property("pendingRenameReconciliation", false);
        operationStatus_ = QCoreApplication::translate("MainWindow", "Rename failed");
        refresh_after_rename(source);
    } else if (result.status == fileops::OperationStatus::conflict ||
               result.status == fileops::OperationStatus::source_changed) {
        auto source = std::move(pendingRenameOperation_->source_path);
        pendingRenameOperation_.reset();
        set_property("pendingRenameReconciliation", false);
        operationStatus_ = operation_status_text(result.status);
        refresh_after_rename(source);
    } else {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The result is still unverified. Check the network and press F5");
    }
    set_rename_in_flight(false);
    publish_status();
}

void FileCommandController::Impl::handle_batch_rename_progress(const fileops::BatchRenameProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::BatchTransactionPhase::prepared:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::BatchTransactionPhase::evacuating:
        phase = QCoreApplication::translate("MainWindow", "releasing names");
        break;
    case fileops::BatchTransactionPhase::rollback:
        phase = QCoreApplication::translate("MainWindow", "rolling back changes");
        break;
    case fileops::BatchTransactionPhase::commit_intent:
        phase = QCoreApplication::translate("MainWindow", "committing");
        break;
    case fileops::BatchTransactionPhase::publishing:
        phase = QCoreApplication::translate("MainWindow", "new names");
        break;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Batch rename: %1 · %2 of %3")
                           .arg(phase)
                           .arg(progress.completed)
                           .arg(progress.total);
    publish_status();
}

void FileCommandController::Impl::handle_batch_rename_result(const fileops::BatchRenameResult &result) {
    notify_file_in_use(result.operation_status);
#ifdef VOVE_UI_TEST_HOOKS
    set_property("batchRenameDetail", QString::fromStdString(result.detail_utf8));
#endif
    const auto needs_recovery = batchRenameCoordinator_.recovery_pending();
    set_property("batchRenameRecoveryPending", needs_recovery);
    if (result.ok()) {
        QStringList destinations;
        destinations.reserve(static_cast<qsizetype>(result.destinations.size()));
        for (const auto &path : result.destinations) {
            destinations.push_back(display_path(path));
        }
        operationStatus_ = QCoreApplication::translate("MainWindow", "Items renamed: %1")
                               .arg(result.destinations.size());
        destinations.append(activeBatchUnchangedPaths_);
        activeBatchSourcePaths_.clear();
        refresh_after_batch_rename(destinations);
    } else if (result.status == fileops::BatchRunStatus::rolled_back) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Rename cancelled; original names were restored");
        const auto sources = activeBatchSourcePaths_;
        activeBatchSourcePaths_.clear();
        refresh_after_batch_rename(sources);
    } else if (needs_recovery || result.status == fileops::BatchRunStatus::recovery_required ||
               result.status == fileops::BatchRunStatus::journal_error ||
               result.status == fileops::BatchRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Batch rename paused. Check the network and press F5");
        begin_preview_generation(false);
        ports_.mark_all_previews_unverified();
    } else if (result.status == fileops::BatchRunStatus::unsupported) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Batch rename is unavailable on this system");
        activeBatchSourcePaths_.clear();
    } else {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Batch rename failed");
        activeBatchSourcePaths_.clear();
    }
    if (!needs_recovery) activeBatchUnchangedPaths_.clear();
    set_rename_in_flight(false);
    publish_status();
}

void FileCommandController::Impl::handle_delete_progress(const fileops::PermanentDeleteProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::PermanentDeletePhase::prepared:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::PermanentDeletePhase::evacuating:
        phase = QCoreApplication::translate("MainWindow", "checking items");
        break;
    case fileops::PermanentDeletePhase::rollback:
        phase = QCoreApplication::translate("MainWindow", "restoring original names");
        break;
    case fileops::PermanentDeletePhase::permanent_delete_intent:
        phase = QCoreApplication::translate("MainWindow", "recording intent");
        break;
    case fileops::PermanentDeletePhase::deleting:
        phase = QCoreApplication::translate("MainWindow", "deleting");
        break;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Safe deletion: %1 · %2 of %3")
                           .arg(phase)
                           .arg(progress.completed)
                           .arg(progress.total);
    publish_status();
}

void FileCommandController::Impl::handle_delete_result(const fileops::PermanentDeleteResult &result) {
    notify_file_in_use(result.operation_status);
    const auto owns_recovery = permanentDeleteCoordinator_.owns_recovery();
    set_property("permanentDeleteRecoveryPending", owns_recovery);
    if (result.ok()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Files deleted: %1").arg(result.completed);
        ports_.clear_selection();
        begin_preview_generation();
        refresh_directory_sources();
        poll_catalog(16);
    } else if (result.status == fileops::PermanentDeleteRunStatus::rolled_back) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Deletion cancelled; original names were restored");
        refresh_directory_sources();
        poll_catalog();
    } else if (owns_recovery ||
               result.status == fileops::PermanentDeleteRunStatus::recovery_required ||
               result.status == fileops::PermanentDeleteRunStatus::journal_error ||
               result.status == fileops::PermanentDeleteRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Deletion paused. Check the network and press F5");
        begin_preview_generation(false);
        ports_.mark_all_previews_unverified();
    } else if (result.status == fileops::PermanentDeleteRunStatus::unsupported) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "This resource cannot be deleted safely using the selected method");
    } else {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Deletion failed");
    }
    set_delete_in_flight(false);
    publish_status();
}

} // namespace vove::ui
