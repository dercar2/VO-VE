#include "vove/fileops/file_transfer_coordinator.hpp"

#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/current_operation_lease.hpp"
#include "vove/fileops/directory_leaf_transfer.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace vove::fileops {
namespace {

enum class StepOutcome : std::uint8_t {
    committed,
    clean_failure,
    recovery_required,
    journal_error,
    stopped,
};

struct StepResult {
    StepOutcome outcome{StepOutcome::recovery_required};
    OperationStatus status{OperationStatus::io_error};
    OperationEvidence evidence{OperationEvidence::none};
    std::string detail_utf8;
};

struct PendingOperation {
    std::mutex mutex;
    std::condition_variable completed;
    OperationResult result;
    bool ready{};
};

struct PendingTransfer {
    std::mutex mutex;
    std::condition_variable completed;
    FileTransferStreamResult result;
    std::optional<FileTransferReservation> reservation;
    bool ready{};
};

struct PendingAudit {
    std::mutex mutex;
    std::condition_variable completed;
    FileTransferStreamResult result;
    bool ready{};
};

thread_local const void *active_coordinator_callback_state{};

class CallbackGuard final {
  public:
    explicit CallbackGuard(const void *state) noexcept
        : previous_(std::exchange(active_coordinator_callback_state, state)) {}

    ~CallbackGuard() {
        active_coordinator_callback_state = previous_;
    }

    CallbackGuard(const CallbackGuard &) = delete;
    CallbackGuard &operator=(const CallbackGuard &) = delete;

  private:
    const void *previous_{};
};

std::size_t completed_prefix(const FileTransferTransaction &transaction) {
    std::size_t completed{};
    for (const auto &item : transaction.items) {
        const auto final = item.destination_state == FileTransferDestinationState::published &&
                           item.overwrite_state == OverwriteDestinationState::none &&
                           ((transaction.kind == FileTransferKind::copy &&
                             item.source_location == FileTransferSourceLocation::original) ||
                            (transaction.kind == FileTransferKind::move &&
                             item.source_location == FileTransferSourceLocation::removed));
        if (!final) {
            break;
        }
        ++completed;
    }
    return completed;
}

std::vector<std::filesystem::path>
completed_destinations(const FileTransferTransaction &transaction) {
    std::vector<std::filesystem::path> paths;
    const auto count = completed_prefix(transaction);
    paths.reserve(count);
    for (std::size_t index{}; index < count; ++index) {
        paths.push_back(transaction.items[index].destination);
    }
    return paths;
}

std::uint64_t step_operation_id(const FileTransferTransaction &transaction,
                                const FileTransferStep step, const std::size_t index) noexcept {
    constexpr std::uint64_t mix = 0x9e3779b97f4a7c15ULL;
    const auto value =
        transaction.operation_id ^ (mix * (index + 1U)) ^ (static_cast<std::uint64_t>(step) << 56U);
    return value == 0 ? transaction.operation_id : value;
}

bool confirmed_no_commit(const OperationResult &result) noexcept {
    return result.evidence == OperationEvidence::no_commit;
}

bool mutation_committed(const OperationResult &result) noexcept {
    return result.status == OperationStatus::success &&
           result.evidence == OperationEvidence::committed;
}

bool snapshot_identity_matches(const SourceSnapshot &expected, const SourceSnapshot &confirmed) {
    const auto expected_identity = stable_object_identity(expected.source_revision_utf8);
    return !expected_identity.empty() &&
           (expected_identity == stable_object_identity(confirmed.source_revision_utf8) ||
            same_object_after_rename(expected, confirmed));
}

bool snapshot_exact_matches(const SourceSnapshot &expected, const SourceSnapshot &confirmed) {
    return expected.size_bytes == confirmed.size_bytes &&
           expected.modified_unix_ns == confirmed.modified_unix_ns &&
           same_source_revision(expected.source_revision_utf8, confirmed.source_revision_utf8);
}

std::string transfer_path_key(const std::filesystem::path &path) {
    const auto encoded = path.lexically_normal().u8string();
    std::string key(reinterpret_cast<const char *>(encoded.data()), encoded.size());
#ifdef _WIN32
    std::ranges::transform(key, key.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return key;
}

std::filesystem::path copy_destination_path(const std::filesystem::path &destination,
                                            const std::size_t ordinal) {
    auto filename = destination.stem();
    filename += std::filesystem::path(u8"_копия").native();
    if (ordinal > 1U) {
        filename += std::filesystem::path("_" + std::to_string(ordinal)).native();
    }
    filename += destination.extension().native();
    return destination.parent_path() / filename;
}

void clear_destination_proof(FileTransferItem &item) noexcept {
    item.destination_snapshot = {};
    item.content_sha256 = {};
    item.request_token = {};
    item.content_bytes = 0;
    item.content_proof_present = false;
    item.destination_state = FileTransferDestinationState::absent;
}

std::string journal_error_detail(const DurableJournalResult &result) {
    if (result.status == DurableJournalStatus::payload_too_large) {
        return "file-transfer journal exceeds its size limit";
    }
    if (result.status == DurableJournalStatus::payload_mismatch) {
        return "file-transfer journal changed after verification";
    }
    if (result.error) {
        return "file-transfer journal I/O failed: " + result.error.message();
    }
    return "file-transfer journal could not be updated";
}

struct ProbeResult {
    OperationStatus status{OperationStatus::io_error};
    SourceSnapshot snapshot;
    std::string detail_utf8;
};

struct ReservationMarkerProbe {
    ProbeResult file;
    bool owned{};
};

std::array<std::uint8_t, kFileTransferRequestTokenBytes>
new_request_token(const FileTransferTransaction &transaction, const std::size_t index) {
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> token{};
    std::random_device random;
    for (auto &value : token) {
        value = static_cast<std::uint8_t>(random());
    }
    for (std::size_t offset{}; offset < sizeof(transaction.operation_id); ++offset) {
        token[offset] ^= static_cast<std::uint8_t>(transaction.operation_id >> (offset * 8U));
    }
    token.back() ^= static_cast<std::uint8_t>(index);
    if (std::ranges::all_of(token, [](const std::uint8_t value) { return value == 0; })) {
        token.front() = 1;
    }
    return token;
}

} // namespace

struct FileTransferCoordinator::State : std::enable_shared_from_this<State> {
    explicit State(FileTransferCoordinatorOptions value)
        : options(std::move(value)), journal(options.journal_path),
          file_operations(options.file_operations), file_transfers(options.file_transfers) {
        file_operations.retain_accepted_completions_during_stop();
        file_transfers.retain_accepted_completions_during_stop();
        static_cast<void>(refresh_recovery_pending());
    }

    FileTransferCoordinatorOptions options;
    CurrentOperationJournalStore journal;
    FileOperationService file_operations;
    FileTransferService file_transfers;
    std::atomic_bool running{false};
    std::atomic_bool stopped{false};
    std::atomic_bool recovery_pending{false};
    std::mutex worker_mutex;
    std::condition_variable worker_completed;
    std::stop_source worker_stop;
    std::thread::id worker_id;
    std::optional<DirectoryLeafTransferRequest> active_directory_leaf_request;
    std::optional<DirectoryLeafCompletionProof> verified_directory_leaf_proof;
    bool directory_leaf_audit_journal_error{};
    Conflict active_conflict;
    std::mutex conflict_mutex;
    std::condition_variable_any conflict_decided;
    std::optional<FileTransferConflictDecision> conflict_decision;
    bool waiting_for_conflict{};

    void resolve_conflict(const FileTransferConflictDecision decision) noexcept {
        {
            std::scoped_lock lock(conflict_mutex);
            if (!waiting_for_conflict || conflict_decision.has_value()) {
                return;
            }
            conflict_decision = decision;
        }
        conflict_decided.notify_all();
    }

    [[nodiscard]] bool refresh_recovery_pending() noexcept {
        const auto pending = journal.read().status != DurableJournalStatus::not_found;
        recovery_pending.store(pending, std::memory_order_release);
        return pending;
    }

    [[nodiscard]] bool owns_recovery(const CurrentOperationKind kind) noexcept {
        const auto loaded = journal.read();
        const auto owns = loaded.ok() &&
                          loaded.encoding == CurrentOperationJournalEncoding::typed &&
                          loaded.kind == kind;
        recovery_pending.store(loaded.status != DurableJournalStatus::not_found,
                               std::memory_order_release);
        return owns;
    }

    [[nodiscard]] bool persist(const FileTransferTransaction &transaction,
                               std::string &detail_utf8) {
        try {
            std::vector<std::byte> payload;
            auto kind = CurrentOperationKind::file_transfer;
            if (active_directory_leaf_request) {
                payload = encode_directory_leaf_transfer(
                    {.binding = active_directory_leaf_request->binding,
                     .original_file = active_directory_leaf_request->file,
                     .transfer = transaction});
                kind = CurrentOperationKind::directory_leaf_transfer;
            } else {
                payload = encode_file_transfer_transaction(transaction);
            }
            const auto stored = journal.write(kind, payload);
            if (!stored.ok()) {
                detail_utf8 = journal_error_detail(stored);
                recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                                       std::memory_order_release);
                return false;
            }
            recovery_pending.store(true, std::memory_order_release);
            return true;
        } catch (const std::exception &error) {
            detail_utf8 = error.what();
            recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                                   std::memory_order_release);
            return false;
        }
    }

    [[nodiscard]] bool remove_journal(const FileTransferTransaction &transaction,
                                      std::string &detail_utf8) {
        if (options.before_journal_retirement &&
            !options.before_journal_retirement(transaction, detail_utf8)) {
            recovery_pending.store(true, std::memory_order_release);
            return false;
        }
        const auto removed = journal.remove();
        if (!removed.ok()) {
            detail_utf8 = journal_error_detail(removed);
            recovery_pending.store(true, std::memory_order_release);
            return false;
        }
        recovery_pending.store(false, std::memory_order_release);
        return true;
    }

    static void clear_active(FileTransferTransaction &transaction) noexcept {
        transaction.active_step = FileTransferStep::none;
        transaction.active_index = 0;
        transaction.active_evidence = OperationEvidence::none;
    }

    static void clear_failure(FileTransferTransaction &transaction) {
        transaction.failure_status = OperationStatus::success;
        transaction.failure_evidence = OperationEvidence::none;
        transaction.failure_detail_utf8.clear();
    }

    static void clear_overwrite(FileTransferItem &item) noexcept {
        item.overwrite_destination_snapshot = {};
        item.overwrite_backup_snapshot = {};
        item.overwrite_state = OverwriteDestinationState::none;
    }

    [[nodiscard]] FileTransferResult result_for(const FileTransferTransaction &transaction,
                                                const FileTransferRunStatus status,
                                                const OperationStatus operation_status,
                                                const OperationEvidence evidence,
                                                std::string detail_utf8) const {
        const auto completed = completed_prefix(transaction);
        std::optional<DirectoryLeafCompletionProof> proof;
        if (status == FileTransferRunStatus::success && evidence == OperationEvidence::committed &&
            active_directory_leaf_request && verified_directory_leaf_proof) {
            std::string binding_detail;
            if (directory_leaf_transfer_binding_matches(verified_directory_leaf_proof->binding,
                                                        active_directory_leaf_request->binding,
                                                        binding_detail)) {
                proof = verified_directory_leaf_proof;
            }
        }
        return {.status = status,
                .operation_status = operation_status,
                .evidence = evidence,
                .completed = completed,
                .total = transaction.items.size(),
                .failed_index = completed < transaction.items.size()
                                    ? std::optional<std::size_t>(completed)
                                    : std::nullopt,
                .destinations = completed_destinations(transaction),
                .directory_leaf_completion_proof = std::move(proof),
                .detail_utf8 = std::move(detail_utf8)};
    }

    void publish_progress(const Progress &progress, const FileTransferTransaction &transaction,
                          const std::size_t index, const FileTransferStep step,
                          const std::uint64_t bytes_written = 0) noexcept {
        if (!progress || index >= transaction.items.size()) {
            return;
        }
        const auto &item = transaction.items[index];
        try {
            CallbackGuard callback(this);
            progress({.phase = transaction.phase,
                      .step = step,
                      .completed = completed_prefix(transaction),
                      .total = transaction.items.size(),
                      .item_index = index,
                      .bytes_written = bytes_written,
                      .bytes_total = item.original_source_snapshot.size_bytes,
                      .source = item.source,
                      .destination = item.destination});
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // Progress is observational and cannot alter durable transfer state.
        }
    }

    [[nodiscard]] OperationResult submit_rename(const RenameRequest &request,
                                                const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
                pending->ready = true;
            }
            pending->completed.notify_all();
        };
        const auto accepted = reconcile ? file_operations.submit_reconciliation(request, completion)
                                        : file_operations.submit_rename(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .evidence = OperationEvidence::none,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected the transfer rename"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->ready; });
        return std::move(pending->result);
    }

    [[nodiscard]] OperationResult submit_delete(const DeleteRequest &request,
                                                const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
                pending->ready = true;
            }
            pending->completed.notify_all();
        };
        const auto accepted =
            reconcile ? file_operations.submit_delete_reconciliation(request, completion)
                      : file_operations.submit_delete(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .evidence = OperationEvidence::none,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected the transfer delete"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->ready; });
        return std::move(pending->result);
    }

    [[nodiscard]] RenameRequest rename_request(const FileTransferTransaction &transaction,
                                               const std::size_t index,
                                               const FileTransferStep step) const {
        const auto &item = transaction.items[index];
        RenameRequest request;
        request.operation_id = step_operation_id(transaction, step, index);
        request.action = RenameAction::execute;
        switch (step) {
        case FileTransferStep::atomic_move:
            request.mode = transaction.version < 6 &&
                                   item.overwrite_state == OverwriteDestinationState::authorized
                               ? RenameMode::transfer_atomic_replace
                               : RenameMode::transfer_atomic;
            if (transaction.version < 6) {
                request.expected_destination = item.overwrite_destination_snapshot;
            }
            request.source = item.source;
            request.destination = item.destination;
            request.expected_source = item.current_source_snapshot;
            request.source_parent_identity_utf8 = item.source_parent_identity_utf8;
            request.destination_parent_identity_utf8 = item.destination_parent_identity_utf8;
            break;
        case FileTransferStep::stage_source:
            request.mode = RenameMode::transfer_stage;
            request.source = item.source;
            request.destination = item.staged_source;
            request.expected_source = item.current_source_snapshot;
            request.source_parent_identity_utf8 = item.source_parent_identity_utf8;
            request.destination_parent_identity_utf8 = item.source_parent_identity_utf8;
            break;
        case FileTransferStep::publish_temp:
            request.mode = transaction.version < 6 &&
                                   item.overwrite_state == OverwriteDestinationState::authorized
                               ? RenameMode::transfer_publish_replace
                               : RenameMode::transfer_publish;
            if (transaction.version < 6) {
                request.expected_destination = item.overwrite_destination_snapshot;
            }
            request.source = item.temp_destination;
            request.destination = item.destination;
            request.expected_source = item.destination_snapshot;
            request.source_parent_identity_utf8 = item.destination_parent_identity_utf8;
            request.destination_parent_identity_utf8 = item.destination_parent_identity_utf8;
            break;
        case FileTransferStep::restore_staged_source:
            request.mode = RenameMode::transfer_restore;
            request.source = item.staged_source;
            request.destination = item.source;
            request.expected_source = item.current_source_snapshot;
            request.source_parent_identity_utf8 = item.source_parent_identity_utf8;
            request.destination_parent_identity_utf8 = item.source_parent_identity_utf8;
            break;
        case FileTransferStep::evacuate_overwrite_destination:
            request.mode = RenameMode::transfer_overwrite_stage;
            request.source = item.destination;
            request.destination = item.overwrite_backup;
            request.expected_source = item.overwrite_destination_snapshot;
            request.source_parent_identity_utf8 = item.destination_parent_identity_utf8;
            request.destination_parent_identity_utf8 = item.destination_parent_identity_utf8;
            break;
        case FileTransferStep::restore_overwrite_destination:
            request.mode = RenameMode::transfer_overwrite_restore;
            request.source = item.overwrite_backup;
            request.destination = item.destination;
            request.expected_source = item.overwrite_backup_snapshot;
            request.source_parent_identity_utf8 = item.destination_parent_identity_utf8;
            request.destination_parent_identity_utf8 = item.destination_parent_identity_utf8;
            break;
        case FileTransferStep::none:
        case FileTransferStep::reserve_temp:
        case FileTransferStep::stream_temp:
        case FileTransferStep::delete_source:
        case FileTransferStep::cleanup_temp:
        case FileTransferStep::cleanup_overwrite_destination:
            break;
        }
        if (active_directory_leaf_request) {
            request.destination_anchor_path = active_directory_leaf_request->binding.staging_root;
            request.destination_anchor_identity_utf8 =
                active_directory_leaf_request->binding.staging_root_identity_utf8;
        }
        return request;
    }

    [[nodiscard]] DeleteRequest delete_request(const FileTransferTransaction &transaction,
                                               const std::size_t index,
                                               const FileTransferStep step) const {
        const auto &item = transaction.items[index];
        DeleteRequest request;
        request.operation_id = step_operation_id(transaction, step, index);
        request.action = DeleteAction::execute;
        if (step == FileTransferStep::cleanup_temp) {
            request.mode = DeleteMode::transfer_temp_cleanup;
            request.source = item.temp_destination;
            request.expected_source = item.destination_snapshot;
            request.source_parent_identity_utf8 = item.destination_parent_identity_utf8;
        } else if (step == FileTransferStep::delete_source) {
            request.mode = DeleteMode::transfer_source_commit;
            request.source = item.staged_source;
            request.expected_source = item.current_source_snapshot;
            request.source_parent_identity_utf8 = item.source_parent_identity_utf8;
            request.guard_path = item.destination;
            request.expected_guard = item.destination_snapshot;
            request.guard_parent_identity_utf8 = item.destination_parent_identity_utf8;
        } else if (step == FileTransferStep::cleanup_overwrite_destination) {
            request.mode = DeleteMode::transfer_overwrite_cleanup;
            request.source = item.overwrite_backup;
            request.expected_source = item.overwrite_backup_snapshot;
            request.source_parent_identity_utf8 = item.destination_parent_identity_utf8;
        }
        return request;
    }

    [[nodiscard]] ProbeResult probe_temp(const FileTransferTransaction &transaction,
                                         const std::size_t index) {
        const auto result =
            submit_delete(delete_request(transaction, index, FileTransferStep::cleanup_temp), true);
        if (mutation_committed(result) && !result.source_present) {
            return {.status = OperationStatus::not_found, .snapshot = {}, .detail_utf8 = {}};
        }
        if (result.source_present &&
            !stable_object_identity(result.confirmed_snapshot.source_revision_utf8).empty()) {
            return {.status = OperationStatus::success,
                    .snapshot = result.confirmed_snapshot,
                    .detail_utf8 = {}};
        }
        return {.status = result.status, .snapshot = {}, .detail_utf8 = result.detail_utf8};
    }

    [[nodiscard]] ProbeResult probe_staged_source(const FileTransferTransaction &transaction,
                                                  const std::size_t index) {
        const auto result = submit_delete(
            delete_request(transaction, index, FileTransferStep::delete_source), true);
        if (result.source_present &&
            !stable_object_identity(result.confirmed_snapshot.source_revision_utf8).empty()) {
            return {.status = result.status,
                    .snapshot = result.confirmed_snapshot,
                    .detail_utf8 = result.detail_utf8};
        }
        return {.status = result.status, .snapshot = {}, .detail_utf8 = result.detail_utf8};
    }

    [[nodiscard]] ReservationMarkerProbe
    probe_reservation(const FileTransferTransaction &transaction, const std::size_t index) {
        const auto &item = transaction.items[index];
        FileTransferStreamRequest request{
            .operation_id = transaction.operation_id,
            .item_index = static_cast<std::uint32_t>(index),
            .request_token = item.request_token,
            .mode = FileTransferStreamMode::probe_reservation,
            .source = item.temp_destination,
            .temp_destination = {},
            .expected_source = {},
            .expected_temp = {},
            .source_parent_identity_utf8 = item.destination_parent_identity_utf8,
            .destination_parent_identity_utf8 = {},
            .destination_anchor_path = {},
            .destination_anchor_identity_utf8 = {},
        };
        auto pending = std::make_shared<PendingTransfer>();
        const auto accepted = file_transfers.submit(
            request,
            [pending](const FileTransferReservation &reservation, std::string &detail) {
                {
                    std::scoped_lock lock(pending->mutex);
                    pending->reservation = reservation;
                }
                if (reservation.temp_snapshot.size_bytes != kFileTransferReservationMarkerBytes) {
                    detail = "the reserved transfer path contains another object";
                    return false;
                }
                return true;
            },
            {},
            [pending](FileTransferStreamResult result) {
                {
                    std::scoped_lock lock(pending->mutex);
                    pending->result = std::move(result);
                    pending->ready = true;
                }
                pending->completed.notify_all();
            });
        if (!accepted) {
            return {.file = {.status = OperationStatus::io_error,
                             .snapshot = {},
                             .detail_utf8 = "file-transfer service rejected the reservation probe"},
                    .owned = false};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->ready; });
        const auto result = std::move(pending->result);
        const auto reservation = pending->reservation;
        lock.unlock();
        if (!reservation) {
            return {.file = {.status = result.status,
                             .snapshot = {},
                             .detail_utf8 = result.detail_utf8},
                    .owned = false};
        }
        ProbeResult file{.status = OperationStatus::success,
                         .snapshot = reservation->temp_snapshot,
                         .detail_utf8 = {}};
        if (reservation->temp_snapshot.size_bytes != kFileTransferReservationMarkerBytes) {
            return {.file = std::move(file), .owned = false};
        }
        if (!result.ok()) {
            return {.file = {.status = result.status,
                             .snapshot = reservation->temp_snapshot,
                             .detail_utf8 = result.detail_utf8},
                    .owned = false};
        }
        const auto marker =
            make_file_transfer_reservation_marker({.operation_id = transaction.operation_id,
                                                   .item_index = static_cast<std::uint32_t>(index),
                                                   .request_token = item.request_token});
        return {.file = std::move(file),
                .owned = result.bytes_written == marker.size() &&
                         result.content_sha256 == detail::sha256(marker)};
    }

    [[nodiscard]] bool begin_step(FileTransferTransaction &transaction, const std::size_t index,
                                  const FileTransferStep step, std::string &detail_utf8) {
        transaction.active_step = step;
        transaction.active_index = static_cast<std::uint32_t>(index);
        transaction.active_evidence = OperationEvidence::none;
        return persist(transaction, detail_utf8);
    }

    [[nodiscard]] bool apply_rename_success(FileTransferTransaction &transaction,
                                            const std::size_t index, const FileTransferStep step,
                                            const OperationResult &result,
                                            std::string &detail_utf8) {
        auto &item = transaction.items[index];
        const auto expected = step == FileTransferStep::publish_temp
                                  ? item.destination_snapshot
                                  : (step == FileTransferStep::evacuate_overwrite_destination
                                         ? item.overwrite_destination_snapshot
                                         : (step == FileTransferStep::restore_overwrite_destination
                                                ? item.overwrite_backup_snapshot
                                                : item.current_source_snapshot));
        if (!mutation_committed(result) ||
            !snapshot_identity_matches(expected, result.confirmed_snapshot)) {
            detail_utf8 = "transfer rename succeeded without the expected object identity";
            return false;
        }
        switch (step) {
        case FileTransferStep::atomic_move:
            item.strategy = FileTransferStrategy::move_atomic;
            item.source_location = FileTransferSourceLocation::removed;
            item.destination_state = FileTransferDestinationState::published;
            item.destination_snapshot = result.confirmed_snapshot;
            if (transaction.version < 6 &&
                item.overwrite_state == OverwriteDestinationState::authorized) {
                clear_overwrite(item);
            }
            break;
        case FileTransferStep::stage_source:
            item.source_location = FileTransferSourceLocation::staged;
            item.current_source_snapshot = result.confirmed_snapshot;
            break;
        case FileTransferStep::publish_temp:
            item.destination_state = FileTransferDestinationState::published;
            item.destination_snapshot = result.confirmed_snapshot;
            if (transaction.version < 6 &&
                item.overwrite_state == OverwriteDestinationState::authorized) {
                clear_overwrite(item);
            }
            break;
        case FileTransferStep::restore_staged_source:
            item.source_location = FileTransferSourceLocation::original;
            item.current_source_snapshot = result.confirmed_snapshot;
            break;
        case FileTransferStep::evacuate_overwrite_destination:
            item.overwrite_backup_snapshot = result.confirmed_snapshot;
            item.overwrite_state = OverwriteDestinationState::evacuated;
            break;
        case FileTransferStep::restore_overwrite_destination:
            clear_overwrite(item);
            break;
        case FileTransferStep::none:
        case FileTransferStep::reserve_temp:
        case FileTransferStep::stream_temp:
        case FileTransferStep::delete_source:
        case FileTransferStep::cleanup_temp:
        case FileTransferStep::cleanup_overwrite_destination:
            detail_utf8 = "transfer rename step is invalid";
            return false;
        }
        clear_active(transaction);
        if (transaction.phase != FileTransferPhase::prepublish_cleanup) {
            clear_failure(transaction);
            transaction.phase = completed_prefix(transaction) == transaction.items.size()
                                    ? FileTransferPhase::completed
                                    : FileTransferPhase::processing;
        }
        return true;
    }

    [[nodiscard]] bool record_unexpected_evacuation(FileTransferTransaction &transaction,
                                                    const std::size_t index,
                                                    const FileTransferStep step,
                                                    const OperationResult &result,
                                                    const bool recovered = false) {
        if (step != FileTransferStep::evacuate_overwrite_destination ||
            result.evidence != OperationEvidence::committed || !result.destination_present ||
            stable_object_identity(result.confirmed_snapshot.source_revision_utf8).empty()) {
            return false;
        }
        auto &item = transaction.items[index];
        if (!recovered && result.destination_matches_source) {
            return false;
        }
        item.overwrite_backup_snapshot = result.confirmed_snapshot;
        item.overwrite_state = OverwriteDestinationState::evacuated_unexpected;
        clear_active(transaction);
        return true;
    }

    [[nodiscard]] bool apply_delete_success(FileTransferTransaction &transaction,
                                            const std::size_t index, const FileTransferStep step,
                                            const OperationResult &result,
                                            std::string &detail_utf8) {
        if (!mutation_committed(result)) {
            detail_utf8 = "transfer delete succeeded without committed evidence";
            return false;
        }
        auto &item = transaction.items[index];
        if (step == FileTransferStep::cleanup_temp) {
            clear_destination_proof(item);
        } else if (step == FileTransferStep::delete_source) {
            item.source_location = FileTransferSourceLocation::removed;
        } else if (step == FileTransferStep::cleanup_overwrite_destination) {
            clear_overwrite(item);
        } else {
            detail_utf8 = "transfer delete step is invalid";
            return false;
        }
        clear_active(transaction);
        if (transaction.phase != FileTransferPhase::prepublish_cleanup) {
            clear_failure(transaction);
            transaction.phase = completed_prefix(transaction) == transaction.items.size()
                                    ? FileTransferPhase::completed
                                    : FileTransferPhase::processing;
        }
        return true;
    }

    [[nodiscard]] StepResult persist_committed(FileTransferTransaction &transaction,
                                               const std::stop_token &stop) {
        std::string detail;
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .evidence = OperationEvidence::committed,
                    .detail_utf8 = std::move(detail)};
        }
        if (stop.stop_requested()) {
            return {.outcome = StepOutcome::stopped,
                    .status = OperationStatus::success,
                    .evidence = OperationEvidence::committed,
                    .detail_utf8 = "file transfer stopped after recording a completed step"};
        }
        return {.outcome = StepOutcome::committed,
                .status = OperationStatus::success,
                .evidence = OperationEvidence::committed,
                .detail_utf8 = {}};
    }

    [[nodiscard]] StepResult preserve_uncertain(FileTransferTransaction &transaction,
                                                const OperationStatus status,
                                                const OperationEvidence evidence,
                                                std::string detail_utf8) {
        transaction.active_evidence = evidence;
        transaction.failure_status = status;
        transaction.failure_evidence = evidence;
        transaction.failure_detail_utf8 = detail_utf8;
        std::string journal_detail;
        if (!persist(transaction, journal_detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .evidence = evidence,
                    .detail_utf8 = std::move(journal_detail)};
        }
        return {.outcome = StepOutcome::recovery_required,
                .status = status,
                .evidence = evidence,
                .detail_utf8 = std::move(detail_utf8)};
    }

    [[nodiscard]] StepResult preserve_cleanup_uncertain(FileTransferTransaction &transaction,
                                                        const OperationStatus status,
                                                        const OperationEvidence evidence,
                                                        std::string detail_utf8) {
        transaction.active_evidence = evidence;
        std::string journal_detail;
        if (!persist(transaction, journal_detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .evidence = evidence,
                    .detail_utf8 = std::move(journal_detail)};
        }
        return {.outcome = StepOutcome::recovery_required,
                .status = status,
                .evidence = evidence,
                .detail_utf8 = std::move(detail_utf8)};
    }

    [[nodiscard]] StepResult enter_cleanup(FileTransferTransaction &transaction,
                                           const OperationStatus status,
                                           const OperationEvidence evidence,
                                           std::string detail_utf8) {
        transaction.phase = FileTransferPhase::prepublish_cleanup;
        clear_active(transaction);
        transaction.failure_status = status;
        transaction.failure_evidence = evidence;
        transaction.failure_detail_utf8 = detail_utf8;
        std::string journal_detail;
        if (!persist(transaction, journal_detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .evidence = evidence,
                    .detail_utf8 = std::move(journal_detail)};
        }
        return {.outcome = StepOutcome::clean_failure,
                .status = status,
                .evidence = evidence,
                .detail_utf8 = std::move(detail_utf8)};
    }

    [[nodiscard]] std::optional<FileTransferConflictDecision>
    await_conflict_decision(FileTransferConflict conflict, const std::stop_token &stop) {
        {
            std::scoped_lock lock(conflict_mutex);
            conflict_decision.reset();
            waiting_for_conflict = true;
        }
        try {
            if (!stop.stop_requested()) {
                CallbackGuard callback(this);
                active_conflict(std::move(conflict));
            }
        } catch (...) {
            resolve_conflict(FileTransferConflictDecision::cancel);
        }
        std::unique_lock lock(conflict_mutex);
        conflict_decided.wait(lock, stop, [this] { return conflict_decision.has_value(); });
        const auto decision = conflict_decision;
        waiting_for_conflict = false;
        conflict_decision.reset();
        return stop.stop_requested() ? std::nullopt : decision;
    }

    [[nodiscard]] bool assign_copy_destination(FileTransferTransaction &transaction,
                                               const std::size_t index,
                                               const std::stop_token &stop) {
        auto &item = transaction.items[index];
        std::unordered_set<std::string> occupied;
        occupied.reserve(transaction.items.size() * 2U);
        for (std::size_t other{}; other < transaction.items.size(); ++other) {
            if (other != index) {
                occupied.insert(transfer_path_key(transaction.items[other].destination));
            }
            occupied.insert(transfer_path_key(transaction.items[other].source));
        }
        for (std::size_t ordinal = 1U; ordinal <= kMaximumFileTransferItems; ++ordinal) {
            if (stop.stop_requested()) {
                return false;
            }
            const auto candidate = copy_destination_path(item.destination, ordinal);
            if (occupied.contains(transfer_path_key(candidate))) {
                continue;
            }
            auto request = rename_request(transaction, index, FileTransferStep::atomic_move);
            request.mode = RenameMode::transfer_atomic;
            request.destination = candidate;
            request.expected_destination = {};
            const auto observed = submit_rename(request, true);
            if (observed.status == OperationStatus::conflict) {
                continue;
            }
            item.destination = candidate;
            item.temp_destination =
                file_transfer_temp_destination_path(candidate, transaction.operation_id, index);
            item.overwrite_backup =
                file_transfer_overwrite_backup_path(candidate, transaction.operation_id, index);
            clear_overwrite(item);
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<FileTransferResult>
    request_initial_conflicts(FileTransferTransaction &transaction, const Progress &progress,
                              const std::stop_token &stop) {
        if (!active_conflict || active_directory_leaf_request ||
            transaction.phase != FileTransferPhase::prepared) {
            return std::nullopt;
        }
        struct ObservedConflict {
            std::size_t index{};
            SourceSnapshot destination;
        };
        std::vector<ObservedConflict> observed_conflicts;
        FileTransferConflict question;
        for (std::size_t index{}; index < transaction.items.size(); ++index) {
            auto &item = transaction.items[index];
            if (!item.overwrite_destination_snapshot.source_revision_utf8.empty()) {
                continue;
            }
            if (stop.stop_requested()) {
                return result_for(transaction, FileTransferRunStatus::stopped,
                                  OperationStatus::success, OperationEvidence::no_commit,
                                  "file transfer stopped before conflict check");
            }
            const auto observed = submit_rename(
                rename_request(transaction, index, FileTransferStep::atomic_move), true);
            if (observed.status != OperationStatus::conflict || !confirmed_no_commit(observed) ||
                stable_object_identity(observed.confirmed_snapshot.source_revision_utf8).empty()) {
                continue;
            }
            if (std::ranges::any_of(transaction.items, [&](const FileTransferItem &other) {
                    return same_object_identity(
                        observed.confirmed_snapshot.source_revision_utf8,
                        other.original_source_snapshot.source_revision_utf8);
                })) {
                continue;
            }
            observed_conflicts.push_back(
                {.index = index, .destination = observed.confirmed_snapshot});
            question.items.push_back({.source = item.source, .destination = item.destination});
            if (question.items.size() == 1U) {
                question.source = item.source;
                question.destination = item.destination;
            }
        }
        if (question.items.empty()) {
            return std::nullopt;
        }

        // The journal contains no authorization while the user is deciding. Recovery asks again.
        clear_active(transaction);
        clear_failure(transaction);
        std::string detail;
        if (!persist(transaction, detail)) {
            return result_for(transaction, FileTransferRunStatus::journal_error,
                              OperationStatus::io_error, OperationEvidence::no_commit,
                              std::move(detail));
        }
        const auto decision = await_conflict_decision(std::move(question), stop);
        if (!decision) {
            return result_for(transaction, FileTransferRunStatus::stopped, OperationStatus::success,
                              OperationEvidence::no_commit,
                              "file transfer stopped while awaiting conflict decision");
        }
        if (*decision == FileTransferConflictDecision::cancel) {
            transaction.cancelled = true;
            const auto cancelled =
                enter_cleanup(transaction, OperationStatus::conflict, OperationEvidence::no_commit,
                              "file transfer cancelled by user");
            if (cancelled.outcome == StepOutcome::journal_error) {
                return result_for(transaction, FileTransferRunStatus::journal_error,
                                  cancelled.status, cancelled.evidence,
                                  std::move(cancelled.detail_utf8));
            }
            return continue_cleanup(std::move(transaction), progress, stop);
        }
        if (*decision == FileTransferConflictDecision::overwrite) {
            transaction.version = kFileTransferTransactionVersion;
            for (auto &conflict : observed_conflicts) {
                auto &conflict_item = transaction.items[conflict.index];
                conflict_item.overwrite_destination_snapshot = std::move(conflict.destination);
                conflict_item.overwrite_state = OverwriteDestinationState::authorized;
            }
        } else {
            for (const auto &conflict : observed_conflicts) {
                if (!assign_copy_destination(transaction, conflict.index, stop)) {
                    const auto failed = enter_cleanup(
                        transaction, OperationStatus::conflict, OperationEvidence::no_commit,
                        "a unique copy destination could not be selected");
                    if (failed.outcome == StepOutcome::journal_error) {
                        return result_for(transaction, FileTransferRunStatus::journal_error,
                                          failed.status, failed.evidence,
                                          std::move(failed.detail_utf8));
                    }
                    return continue_cleanup(std::move(transaction), progress, stop);
                }
            }
        }
        if (!persist(transaction, detail)) {
            return result_for(transaction, FileTransferRunStatus::journal_error,
                              OperationStatus::io_error, OperationEvidence::no_commit,
                              std::move(detail));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<StepResult> request_overwrite(FileTransferTransaction &transaction,
                                                              const std::size_t index,
                                                              const FileTransferStep probe_step,
                                                              const std::stop_token &stop) {
        auto &item = transaction.items[index];
        if (stop.stop_requested()) {
            return StepResult{.outcome = StepOutcome::stopped,
                              .status = OperationStatus::success,
                              .detail_utf8 = "file transfer stopped before conflict check"};
        }
        if (!active_conflict || active_directory_leaf_request ||
            !item.overwrite_destination_snapshot.source_revision_utf8.empty()) {
            return std::nullopt;
        }
        const auto observed = submit_rename(rename_request(transaction, index, probe_step), true);
        if (observed.status != OperationStatus::conflict || !confirmed_no_commit(observed) ||
            stable_object_identity(observed.confirmed_snapshot.source_revision_utf8).empty()) {
            return std::nullopt;
        }
        auto destination = observed.confirmed_snapshot;
        if (std::ranges::any_of(transaction.items, [&](const FileTransferItem &other) {
                return same_object_identity(destination.source_revision_utf8,
                                            other.original_source_snapshot.source_revision_utf8);
            })) {
            return std::nullopt;
        }
        // A pending question is not authorization. Recovery may safely ask it again.
        clear_active(transaction);
        clear_failure(transaction);
        std::string detail;
        if (!persist(transaction, detail)) {
            return StepResult{.outcome = StepOutcome::journal_error,
                              .status = OperationStatus::io_error,
                              .evidence = OperationEvidence::no_commit,
                              .detail_utf8 = std::move(detail)};
        }
        const auto decision = await_conflict_decision(
            {.source = item.source,
             .destination = item.destination,
             .items = {{.source = item.source, .destination = item.destination}}},
            stop);
        if (!decision) {
            return StepResult{.outcome = StepOutcome::stopped,
                              .status = OperationStatus::success,
                              .evidence = OperationEvidence::no_commit,
                              .detail_utf8 =
                                  "file transfer stopped while awaiting conflict decision"};
        }
        transaction.version = kFileTransferTransactionVersion;
        if (*decision == FileTransferConflictDecision::cancel) {
            transaction.cancelled = true;
            return enter_cleanup(transaction, OperationStatus::conflict,
                                 OperationEvidence::no_commit, "file transfer cancelled by user");
        }
        if (*decision == FileTransferConflictDecision::create_copy) {
            if (!assign_copy_destination(transaction, index, stop)) {
                return enter_cleanup(transaction, OperationStatus::conflict,
                                     OperationEvidence::no_commit,
                                     "a unique copy destination could not be selected");
            }
        } else {
            item.overwrite_destination_snapshot = std::move(destination);
            item.overwrite_state = OverwriteDestinationState::authorized;
        }
        return persist_committed(transaction, stop);
    }

    [[nodiscard]] StepResult execute_rename_step(FileTransferTransaction &transaction,
                                                 const std::size_t index,
                                                 const FileTransferStep step,
                                                 const Progress &progress,
                                                 const std::stop_token &stop) {
        std::string detail;
        if (!begin_step(transaction, index, step, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .detail_utf8 = std::move(detail)};
        }
        publish_progress(progress, transaction, index, step);
        if (stop.stop_requested()) {
            return {.outcome = StepOutcome::stopped,
                    .status = OperationStatus::success,
                    .detail_utf8 = "file transfer stopped before rename"};
        }
        const auto result = submit_rename(rename_request(transaction, index, step), false);
        if (record_unexpected_evacuation(transaction, index, step, result)) {
            return enter_cleanup(transaction, OperationStatus::source_changed,
                                 OperationEvidence::committed,
                                 "overwrite destination changed during evacuation; the unexpected "
                                 "object will be restored");
        }
        if (mutation_committed(result)) {
            if (!apply_rename_success(transaction, index, step, result, detail)) {
                return preserve_uncertain(transaction, OperationStatus::unknown_outcome,
                                          OperationEvidence::committed, std::move(detail));
            }
            return persist_committed(transaction, stop);
        }
        if (step == FileTransferStep::atomic_move &&
            result.status == OperationStatus::cross_device && confirmed_no_commit(result)) {
            auto &item = transaction.items[index];
            item.strategy = FileTransferStrategy::move_stream;
            item.move_stream_reason = MoveStreamReason::cross_device_no_commit;
            clear_active(transaction);
            clear_failure(transaction);
            if (!persist(transaction, detail)) {
                return {.outcome = StepOutcome::journal_error,
                        .status = OperationStatus::io_error,
                        .evidence = OperationEvidence::no_commit,
                        .detail_utf8 = std::move(detail)};
            }
            return {.outcome = StepOutcome::committed,
                    .status = OperationStatus::cross_device,
                    .evidence = OperationEvidence::no_commit,
                    .detail_utf8 = {}};
        }
        if (transaction.phase == FileTransferPhase::prepublish_cleanup) {
            return preserve_cleanup_uncertain(transaction, result.status, result.evidence,
                                              result.detail_utf8);
        }
        if (confirmed_no_commit(result)) {
            if ((step == FileTransferStep::atomic_move || step == FileTransferStep::publish_temp) &&
                result.status == OperationStatus::conflict) {
                if (auto decision = request_overwrite(transaction, index, step, stop)) {
                    return *decision;
                }
            }
            return enter_cleanup(transaction, result.status, result.evidence, result.detail_utf8);
        }
        return preserve_uncertain(transaction, result.status, result.evidence, result.detail_utf8);
    }

    [[nodiscard]] StepResult recover_rename_step(FileTransferTransaction &transaction,
                                                 const std::size_t index,
                                                 const FileTransferStep step,
                                                 const Progress &progress,
                                                 const std::stop_token &stop) {
        publish_progress(progress, transaction, index, step);
        const auto result = submit_rename(rename_request(transaction, index, step), true);
        std::string detail;
        if (record_unexpected_evacuation(transaction, index, step, result,
                                         step ==
                                             FileTransferStep::evacuate_overwrite_destination)) {
            return enter_cleanup(transaction, OperationStatus::source_changed,
                                 OperationEvidence::committed,
                                 "overwrite destination changed during evacuation; the unexpected "
                                 "object will be restored");
        }
        if (mutation_committed(result)) {
            if (!apply_rename_success(transaction, index, step, result, detail)) {
                return preserve_uncertain(transaction, OperationStatus::unknown_outcome,
                                          OperationEvidence::committed, std::move(detail));
            }
            return persist_committed(transaction, stop);
        }
        if (!confirmed_no_commit(result)) {
            return transaction.phase == FileTransferPhase::prepublish_cleanup
                       ? preserve_cleanup_uncertain(transaction, result.status, result.evidence,
                                                    result.detail_utf8)
                       : preserve_uncertain(transaction, result.status, result.evidence,
                                            result.detail_utf8);
        }
        clear_active(transaction);
        if (transaction.phase != FileTransferPhase::prepublish_cleanup) {
            clear_failure(transaction);
        }
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .evidence = OperationEvidence::no_commit,
                    .detail_utf8 = std::move(detail)};
        }
        return execute_rename_step(transaction, index, step, progress, stop);
    }

    [[nodiscard]] StepResult execute_delete_step(FileTransferTransaction &transaction,
                                                 const std::size_t index,
                                                 const FileTransferStep step,
                                                 const Progress &progress,
                                                 const std::stop_token &stop) {
        if (step == FileTransferStep::delete_source) {
            if (const auto refresh = refresh_staged_source_for_delete(transaction, index, stop)) {
                return *refresh;
            }
            if (stop.stop_requested()) {
                return {.outcome = StepOutcome::stopped,
                        .status = OperationStatus::success,
                        .evidence = OperationEvidence::none,
                        .detail_utf8 = "file transfer stopped before source deletion"};
            }
        }
        std::string detail;
        if (!begin_step(transaction, index, step, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .detail_utf8 = std::move(detail)};
        }
        publish_progress(progress, transaction, index, step);
        const auto result = submit_delete(delete_request(transaction, index, step), false);
        if (mutation_committed(result)) {
            if (!apply_delete_success(transaction, index, step, result, detail)) {
                return preserve_uncertain(transaction, OperationStatus::unknown_outcome,
                                          OperationEvidence::committed, std::move(detail));
            }
            return persist_committed(transaction, stop);
        }
        if (transaction.phase == FileTransferPhase::prepublish_cleanup) {
            return preserve_cleanup_uncertain(transaction, result.status, result.evidence,
                                              result.detail_utf8);
        }
        return preserve_uncertain(transaction, result.status, result.evidence, result.detail_utf8);
    }

    [[nodiscard]] StepResult recover_delete_step(FileTransferTransaction &transaction,
                                                 const std::size_t index,
                                                 const FileTransferStep step,
                                                 const Progress &progress,
                                                 const std::stop_token &stop) {
        publish_progress(progress, transaction, index, step);
        const auto result = submit_delete(delete_request(transaction, index, step), true);
        std::string detail;
        if (mutation_committed(result)) {
            if (!apply_delete_success(transaction, index, step, result, detail)) {
                return preserve_uncertain(transaction, OperationStatus::unknown_outcome,
                                          OperationEvidence::committed, std::move(detail));
            }
            return persist_committed(transaction, stop);
        }
        if (!confirmed_no_commit(result)) {
            return transaction.phase == FileTransferPhase::prepublish_cleanup
                       ? preserve_cleanup_uncertain(transaction, result.status, result.evidence,
                                                    result.detail_utf8)
                       : preserve_uncertain(transaction, result.status, result.evidence,
                                            result.detail_utf8);
        }
        clear_active(transaction);
        transaction.active_evidence = OperationEvidence::none;
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .evidence = OperationEvidence::no_commit,
                    .detail_utf8 = std::move(detail)};
        }
        if (step == FileTransferStep::cleanup_temp ||
            step == FileTransferStep::cleanup_overwrite_destination) {
            return {.outcome = StepOutcome::committed,
                    .status = OperationStatus::success,
                    .evidence = OperationEvidence::no_commit,
                    .detail_utf8 = {}};
        }
        return execute_delete_step(transaction, index, step, progress, stop);
    }

    [[nodiscard]] FileTransferStreamRequest
    stream_request(const FileTransferTransaction &transaction, const std::size_t index,
                   const FileTransferStreamMode mode) const {
        const auto &item = transaction.items[index];
        return {.operation_id = transaction.operation_id,
                .item_index = static_cast<std::uint32_t>(index),
                .request_token = item.request_token,
                .mode = mode,
                .source = item.source_location == FileTransferSourceLocation::staged
                              ? item.staged_source
                              : item.source,
                .temp_destination = item.temp_destination,
                .expected_source = item.current_source_snapshot,
                .expected_temp = mode == FileTransferStreamMode::resume_existing
                                     ? item.destination_snapshot
                                     : SourceSnapshot{},
                .source_parent_identity_utf8 = item.source_parent_identity_utf8,
                .destination_parent_identity_utf8 = item.destination_parent_identity_utf8,
                .destination_anchor_path = active_directory_leaf_request
                                               ? active_directory_leaf_request->binding.staging_root
                                               : std::filesystem::path{},
                .destination_anchor_identity_utf8 =
                    active_directory_leaf_request
                        ? active_directory_leaf_request->binding.staging_root_identity_utf8
                        : std::string{}};
    }

    [[nodiscard]] FileTransferStreamRequest
    directory_leaf_audit_request(const FileTransferTransaction &transaction,
                                 const DirectoryLeafTransferRequest &leaf) const {
        const auto &item = transaction.items.front();
        return {.operation_id = transaction.operation_id,
                .item_index = 0U,
                .request_token = item.request_token,
                .mode = FileTransferStreamMode::audit_existing,
                .source = item.destination,
                .temp_destination = {},
                .expected_source = item.destination_snapshot,
                .expected_temp = {},
                .source_parent_identity_utf8 = item.destination_parent_identity_utf8,
                .destination_parent_identity_utf8 = {},
                .destination_anchor_path = leaf.binding.staging_root,
                .destination_anchor_identity_utf8 = leaf.binding.staging_root_identity_utf8};
    }

    [[nodiscard]] FileTransferStreamRequest
    staged_source_audit_request(const FileTransferTransaction &transaction, const std::size_t index,
                                const SourceSnapshot &observed) const {
        const auto &item = transaction.items[index];
        return {.operation_id = transaction.operation_id,
                .item_index = static_cast<std::uint32_t>(index),
                .request_token = item.request_token,
                .mode = FileTransferStreamMode::audit_existing,
                .source = item.staged_source,
                .temp_destination = {},
                .expected_source = observed,
                .expected_temp = {},
                .source_parent_identity_utf8 = item.source_parent_identity_utf8,
                .destination_parent_identity_utf8 = {},
                .destination_anchor_path = item.staged_source.parent_path(),
                .destination_anchor_identity_utf8 = item.source_parent_identity_utf8};
    }

    [[nodiscard]] std::optional<StepResult>
    refresh_staged_source_for_delete(FileTransferTransaction &transaction, const std::size_t index,
                                     const std::stop_token &stop) {
        if (stop.stop_requested()) {
            return StepResult{.outcome = StepOutcome::stopped,
                              .status = OperationStatus::unknown_outcome,
                              .evidence = OperationEvidence::none,
                              .detail_utf8 = "file transfer stopped before source verification"};
        }
        auto &item = transaction.items[index];
        const auto observed = probe_staged_source(transaction, index);
        if (snapshot_exact_matches(item.current_source_snapshot, observed.snapshot)) {
            return std::nullopt;
        }
        if (item.current_source_snapshot.size_bytes != observed.snapshot.size_bytes ||
            !same_object_identity(item.current_source_snapshot.source_revision_utf8,
                                  observed.snapshot.source_revision_utf8) ||
            !item.content_proof_present || item.content_bytes != observed.snapshot.size_bytes) {
            return preserve_uncertain(
                transaction,
                observed.status == OperationStatus::success ? OperationStatus::source_changed
                                                            : observed.status,
                OperationEvidence::none,
                observed.detail_utf8.empty() ? "staged source changed after destination publication"
                                             : observed.detail_utf8);
        }

        const auto expectedSnapshot = observed.snapshot;
        const auto expectedDigest = item.content_sha256;
        const auto expectedBytes = item.content_bytes;
        auto pending = std::make_shared<PendingAudit>();
        const auto accepted = file_transfers.submit_audit(
            staged_source_audit_request(transaction, index, expectedSnapshot),
            [this, &transaction, index, expectedSnapshot, expectedDigest,
             expectedBytes](const FileTransferStreamResult &result, std::string &detail) {
                if (!snapshot_exact_matches(expectedSnapshot, result.source_snapshot) ||
                    !snapshot_exact_matches(result.source_snapshot, result.temp_snapshot) ||
                    result.bytes_written != expectedBytes ||
                    result.content_sha256 != expectedDigest) {
                    detail = "staged source content changed after destination publication";
                    return FileTransferAuditCommitOutcome::rejected_no_commit;
                }
                auto candidate = transaction;
                candidate.items[index].original_source_snapshot = result.source_snapshot;
                candidate.items[index].current_source_snapshot = result.source_snapshot;
                if (!persist(candidate, detail)) {
                    return FileTransferAuditCommitOutcome::recovery_required;
                }
                transaction = std::move(candidate);
                return FileTransferAuditCommitOutcome::accepted;
            },
            {},
            [pending](FileTransferStreamResult result) {
                {
                    std::scoped_lock lock(pending->mutex);
                    pending->result = std::move(result);
                    pending->ready = true;
                }
                pending->completed.notify_all();
            });
        if (!accepted) {
            return preserve_uncertain(transaction, OperationStatus::io_error,
                                      OperationEvidence::none,
                                      "file-transfer service rejected source verification");
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->ready; });
        auto result = std::move(pending->result);
        lock.unlock();
        if (!result.ok()) {
            return preserve_uncertain(
                transaction,
                result.status == OperationStatus::success ? OperationStatus::source_changed
                                                          : result.status,
                OperationEvidence::none,
                result.detail_utf8.empty()
                    ? "staged source content changed after destination publication"
                    : result.detail_utf8);
        }
        if (stop.stop_requested()) {
            return StepResult{.outcome = StepOutcome::stopped,
                              .status = OperationStatus::success,
                              .evidence = OperationEvidence::none,
                              .detail_utf8 = "file transfer stopped after source verification"};
        }
        return std::nullopt;
    }

    [[nodiscard]] FileTransferStreamResult
    submit_directory_leaf_audit(const FileTransferTransaction &transaction,
                                const DirectoryLeafTransferRequest &leaf) {
        struct AuditExpectations {
            DirectoryLeafTransferBinding binding;
            std::filesystem::path destination;
            SourceSnapshot source_snapshot;
            SourceSnapshot destination_snapshot;
            std::array<std::uint8_t, kFileTransferDigestBytes> digest{};
            std::uint64_t bytes{};
        };
        static_assert(std::is_nothrow_move_constructible_v<DirectoryLeafCompletionProof>);
        auto pending = std::make_shared<PendingAudit>();
        const auto request = directory_leaf_audit_request(transaction, leaf);
        const auto expected = std::make_shared<const AuditExpectations>(AuditExpectations{
            .binding = leaf.binding,
            .destination = leaf.file.destination,
            .source_snapshot = transaction.items.front().original_source_snapshot,
            .destination_snapshot = transaction.items.front().destination_snapshot,
            .digest = transaction.items.front().content_sha256,
            .bytes = transaction.items.front().content_bytes,
        });
        verified_directory_leaf_proof.reset();
        directory_leaf_audit_journal_error = false;
        const auto committed = file_transfers.submit_audit(
            request,
            [this, expected, transaction](const FileTransferStreamResult &result, std::string &detail) {
                try {
                    if (!snapshot_exact_matches(expected->destination_snapshot,
                                                result.source_snapshot) ||
                        !snapshot_exact_matches(result.source_snapshot, result.temp_snapshot) ||
                        result.content_sha256 != expected->digest ||
                        result.bytes_written != expected->bytes) {
                        detail = "published directory leaf changed before its durable proof";
                        return FileTransferAuditCommitOutcome::rejected_no_commit;
                    }
                    auto proof = DirectoryLeafCompletionProof{
                        .binding = expected->binding,
                        .destination = expected->destination,
                        .source_snapshot = expected->source_snapshot,
                        .destination_snapshot = result.source_snapshot,
                        .content_sha256 = result.content_sha256,
                        .content_bytes = result.bytes_written,
                    };
                    if (!remove_journal(transaction, detail)) {
                        directory_leaf_audit_journal_error = true;
                        return FileTransferAuditCommitOutcome::rejected_no_commit;
                    }
                    verified_directory_leaf_proof.emplace(std::move(proof));
                    return FileTransferAuditCommitOutcome::accepted;
                } catch (const std::exception &error) {
                    detail = error.what();
                    return FileTransferAuditCommitOutcome::recovery_required;
                } catch (...) {
                    detail = "directory-leaf proof construction failed unexpectedly";
                    return FileTransferAuditCommitOutcome::recovery_required;
                }
            },
            {},
            [pending](FileTransferStreamResult result) {
                {
                    std::scoped_lock lock(pending->mutex);
                    pending->result = std::move(result);
                    pending->ready = true;
                }
                pending->completed.notify_all();
            });
        if (!committed) {
            return {.operation_id = request.operation_id,
                    .item_index = request.item_index,
                    .request_token = request.request_token,
                    .status = OperationStatus::io_error,
                    .evidence = OperationEvidence::none,
                    .platform_code = 0,
                    .source_snapshot = {},
                    .temp_snapshot = {},
                    .content_sha256 = {},
                    .bytes_written = 0,
                    .detail_utf8 = "file-transfer service rejected the directory-leaf audit"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->ready; });
        return std::move(pending->result);
    }

    [[nodiscard]] StepResult execute_stream_step(FileTransferTransaction &transaction,
                                                 const std::size_t index, const Progress &progress,
                                                 const std::stop_token &stop) {
        const auto mode = transaction.items[index].destination_state ==
                                  FileTransferDestinationState::temp_reserved
                              ? FileTransferStreamMode::resume_existing
                              : FileTransferStreamMode::reserve_new;
        if (mode == FileTransferStreamMode::reserve_new) {
            transaction.items[index].request_token = new_request_token(transaction, index);
            transaction.active_step = FileTransferStep::reserve_temp;
        } else {
            transaction.active_step = FileTransferStep::stream_temp;
        }
        transaction.active_index = static_cast<std::uint32_t>(index);
        transaction.active_evidence = OperationEvidence::none;
        std::string detail;
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .detail_utf8 = std::move(detail)};
        }
        publish_progress(progress, transaction, index, transaction.active_step);

        auto pending = std::make_shared<PendingTransfer>();
        const auto request = stream_request(transaction, index, mode);
        const auto accepted = file_transfers.submit(
            request,
            [this, &transaction, index, mode](const FileTransferReservation &reservation,
                                              std::string &commit_detail) {
                auto candidate = transaction;
                auto &candidate_item = candidate.items[index];
                if (candidate_item.request_token != reservation.request_token) {
                    commit_detail = "file-transfer reservation no longer matches durable intent";
                    return false;
                }
                if (mode == FileTransferStreamMode::resume_existing) {
                    if (candidate.active_step != FileTransferStep::stream_temp ||
                        candidate_item.destination_state !=
                            FileTransferDestinationState::temp_reserved ||
                        !same_source_revision(
                            candidate_item.destination_snapshot.source_revision_utf8,
                            reservation.temp_snapshot.source_revision_utf8)) {
                        commit_detail =
                            "resumed file-transfer reservation no longer matches durable intent";
                        return false;
                    }
                    return true;
                }
                if (candidate.active_step != FileTransferStep::reserve_temp ||
                    candidate_item.destination_state != FileTransferDestinationState::absent) {
                    commit_detail =
                        "new file-transfer reservation no longer matches durable intent";
                    return false;
                }
                candidate_item.destination_snapshot = reservation.temp_snapshot;
                candidate_item.destination_parent_revision_utf8 =
                    reservation.destination_parent_revision_utf8;
                candidate_item.destination_state = FileTransferDestinationState::temp_reserved;
                candidate.active_step = FileTransferStep::stream_temp;
                candidate.active_evidence = OperationEvidence::none;
                if (!persist(candidate, commit_detail)) {
                    return false;
                }
                transaction = std::move(candidate);
                return true;
            },
            [this, progress, &transaction, index](const FileTransferProgress &update) {
                publish_progress(progress, transaction, index, FileTransferStep::stream_temp,
                                 update.bytes_written);
            },
            [pending](FileTransferStreamResult result) {
                {
                    std::scoped_lock lock(pending->mutex);
                    pending->result = std::move(result);
                    pending->ready = true;
                }
                pending->completed.notify_all();
            });
        if (!accepted) {
            return preserve_uncertain(transaction, OperationStatus::io_error,
                                      OperationEvidence::none,
                                      "file-transfer service rejected the stream step");
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->ready; });
        auto result = std::move(pending->result);
        lock.unlock();

        auto &current_item = transaction.items[index];
        if (result.ok() && result.evidence == OperationEvidence::committed &&
            snapshot_identity_matches(current_item.current_source_snapshot,
                                      result.source_snapshot) &&
            snapshot_identity_matches(current_item.destination_snapshot, result.temp_snapshot) &&
            result.bytes_written == current_item.original_source_snapshot.size_bytes) {
            current_item.destination_snapshot = result.temp_snapshot;
            current_item.content_sha256 = result.content_sha256;
            current_item.content_bytes = result.bytes_written;
            current_item.content_proof_present = true;
            current_item.destination_state = FileTransferDestinationState::content_ready;
            clear_active(transaction);
            clear_failure(transaction);
            return persist_committed(transaction, stop);
        }
        if (stop.stop_requested()) {
            return preserve_uncertain(transaction, result.status, result.evidence,
                                      result.detail_utf8.empty()
                                          ? "file transfer stopped during streaming"
                                          : result.detail_utf8);
        }
        if (result.evidence == OperationEvidence::no_commit ||
            current_item.destination_state != FileTransferDestinationState::absent) {
            return enter_cleanup(transaction, result.status, result.evidence,
                                 result.detail_utf8.empty() ? "file streaming did not complete"
                                                            : result.detail_utf8);
        }
        return preserve_uncertain(transaction, result.status, result.evidence, result.detail_utf8);
    }

    [[nodiscard]] StepResult recover_stream_step(FileTransferTransaction &transaction,
                                                 const std::size_t index, const Progress &progress,
                                                 const std::stop_token &stop) {
        auto &item = transaction.items[index];
        std::string detail;
        if (transaction.active_step == FileTransferStep::reserve_temp) {
            const auto reservation = probe_reservation(transaction, index);
            if (reservation.file.status == OperationStatus::not_found) {
                clear_active(transaction);
                clear_failure(transaction);
                item.request_token = {};
                if (!persist(transaction, detail)) {
                    return {.outcome = StepOutcome::journal_error,
                            .status = OperationStatus::io_error,
                            .detail_utf8 = std::move(detail)};
                }
                return execute_stream_step(transaction, index, progress, stop);
            }
            if (reservation.file.status == OperationStatus::success && reservation.owned) {
                item.destination_snapshot = reservation.file.snapshot;
                item.destination_state = FileTransferDestinationState::temp_reserved;
                return enter_cleanup(transaction, OperationStatus::io_error,
                                     OperationEvidence::none,
                                     "an unacknowledged transfer reservation was recovered");
            }
            return preserve_uncertain(
                transaction,
                reservation.file.status == OperationStatus::success
                    ? OperationStatus::source_changed
                    : reservation.file.status,
                OperationEvidence::none,
                reservation.file.status == OperationStatus::success
                    ? "an unowned temporary object occupies the reserved transfer path"
                    : reservation.file.detail_utf8);
        }
        const auto observed = probe_temp(transaction, index);
        if (observed.status == OperationStatus::not_found) {
            clear_destination_proof(item);
            clear_active(transaction);
            clear_failure(transaction);
            if (!persist(transaction, detail)) {
                return {.outcome = StepOutcome::journal_error,
                        .status = OperationStatus::io_error,
                        .detail_utf8 = std::move(detail)};
            }
            return execute_stream_step(transaction, index, progress, stop);
        }
        if (observed.status != OperationStatus::success ||
            !same_object_identity(observed.snapshot.source_revision_utf8,
                                  item.destination_snapshot.source_revision_utf8)) {
            return preserve_uncertain(
                transaction,
                observed.status == OperationStatus::success ? OperationStatus::source_changed
                                                            : observed.status,
                OperationEvidence::none,
                observed.status == OperationStatus::success
                    ? "the reserved temporary object changed physical identity"
                    : observed.detail_utf8);
        }
        item.destination_snapshot = observed.snapshot;
        transaction.active_evidence = OperationEvidence::none;
        clear_failure(transaction);
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .status = OperationStatus::io_error,
                    .detail_utf8 = std::move(detail)};
        }
        return execute_stream_step(transaction, index, progress, stop);
    }

    [[nodiscard]] StepResult recover_active_step(FileTransferTransaction &transaction,
                                                 const Progress &progress,
                                                 const std::stop_token &stop) {
        const auto index = static_cast<std::size_t>(transaction.active_index);
        switch (transaction.active_step) {
        case FileTransferStep::atomic_move:
        case FileTransferStep::stage_source:
        case FileTransferStep::publish_temp:
        case FileTransferStep::restore_staged_source:
        case FileTransferStep::evacuate_overwrite_destination:
        case FileTransferStep::restore_overwrite_destination:
            return recover_rename_step(transaction, index, transaction.active_step, progress, stop);
        case FileTransferStep::reserve_temp:
        case FileTransferStep::stream_temp:
            return recover_stream_step(transaction, index, progress, stop);
        case FileTransferStep::delete_source:
        case FileTransferStep::cleanup_temp:
        case FileTransferStep::cleanup_overwrite_destination:
            return recover_delete_step(transaction, index, transaction.active_step, progress, stop);
        case FileTransferStep::none:
            break;
        }
        return {.outcome = StepOutcome::recovery_required,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::none,
                .detail_utf8 = "file-transfer recovery has no active step"};
    }

    [[nodiscard]] StepResult next_processing_step(FileTransferTransaction &transaction,
                                                  const Progress &progress,
                                                  const std::stop_token &stop) {
        const auto index = completed_prefix(transaction);
        if (index >= transaction.items.size()) {
            transaction.phase = FileTransferPhase::completed;
            clear_active(transaction);
            clear_failure(transaction);
            return persist_committed(transaction, stop);
        }
        auto &item = transaction.items[index];
        if (item.overwrite_state == OverwriteDestinationState::evacuated_unexpected) {
            return enter_cleanup(transaction, OperationStatus::source_changed,
                                 OperationEvidence::committed,
                                 "an unexpected overwrite destination is awaiting restoration");
        }
        const auto overwrite_ready_for_evacuation =
            item.overwrite_state == OverwriteDestinationState::authorized &&
            ((transaction.kind == FileTransferKind::move &&
              item.strategy == FileTransferStrategy::move_undecided) ||
             item.destination_state == FileTransferDestinationState::content_ready);
        if (overwrite_ready_for_evacuation) {
            transaction.version = kFileTransferTransactionVersion;
            return execute_rename_step(transaction, index,
                                       FileTransferStep::evacuate_overwrite_destination, progress,
                                       stop);
        }
        if (transaction.kind == FileTransferKind::move &&
            item.strategy == FileTransferStrategy::move_undecided) {
            return execute_rename_step(transaction, index, FileTransferStep::atomic_move, progress,
                                       stop);
        }
        if (transaction.kind == FileTransferKind::move &&
            item.strategy == FileTransferStrategy::move_stream &&
            item.source_location == FileTransferSourceLocation::original) {
            return execute_rename_step(transaction, index, FileTransferStep::stage_source, progress,
                                       stop);
        }
        if (item.destination_state == FileTransferDestinationState::absent ||
            item.destination_state == FileTransferDestinationState::temp_reserved) {
            return execute_stream_step(transaction, index, progress, stop);
        }
        if (item.destination_state == FileTransferDestinationState::content_ready) {
            return execute_rename_step(transaction, index, FileTransferStep::publish_temp, progress,
                                       stop);
        }
        if (transaction.kind == FileTransferKind::move &&
            item.destination_state == FileTransferDestinationState::published &&
            item.source_location == FileTransferSourceLocation::staged) {
            return execute_delete_step(transaction, index, FileTransferStep::delete_source,
                                       progress, stop);
        }
        const auto source_final = (transaction.kind == FileTransferKind::copy &&
                                   item.source_location == FileTransferSourceLocation::original) ||
                                  (transaction.kind == FileTransferKind::move &&
                                   item.source_location == FileTransferSourceLocation::removed);
        if (item.destination_state == FileTransferDestinationState::published && source_final &&
            item.overwrite_state == OverwriteDestinationState::evacuated) {
            return execute_delete_step(transaction, index,
                                       FileTransferStep::cleanup_overwrite_destination, progress,
                                       stop);
        }
        return preserve_uncertain(transaction, OperationStatus::unknown_outcome,
                                  OperationEvidence::none,
                                  "file-transfer item has no deterministic next step");
    }

    [[nodiscard]] FileTransferResult continue_cleanup(FileTransferTransaction transaction,
                                                      const Progress &progress,
                                                      const std::stop_token &stop) {
        const auto failure_status = transaction.failure_status;
        const auto failure_detail = transaction.failure_detail_utf8;
        const auto failure_evidence = transaction.failure_evidence;
        const auto index = completed_prefix(transaction);
        if (index >= transaction.items.size()) {
            return result_for(transaction, FileTransferRunStatus::recovery_required,
                              OperationStatus::unknown_outcome, OperationEvidence::none,
                              "file-transfer cleanup has no unfinished item");
        }
        for (;;) {
            if (stop.stop_requested()) {
                return result_for(transaction, FileTransferRunStatus::stopped, failure_status,
                                  failure_evidence,
                                  "file transfer stopped before cleanup completed");
            }
            StepResult step;
            if (transaction.active_step != FileTransferStep::none) {
                step = recover_active_step(transaction, progress, stop);
            } else {
                auto &item = transaction.items[index];
                if (item.destination_state == FileTransferDestinationState::absent &&
                    std::ranges::any_of(item.request_token,
                                        [](const std::uint8_t byte) { return byte != 0; })) {
                    const auto reservation = probe_reservation(transaction, index);
                    if (reservation.file.status == OperationStatus::success && reservation.owned) {
                        item.destination_snapshot = reservation.file.snapshot;
                        item.destination_state = FileTransferDestinationState::temp_reserved;
                        std::string detail;
                        if (!persist(transaction, detail)) {
                            return result_for(transaction, FileTransferRunStatus::journal_error,
                                              OperationStatus::io_error, failure_evidence,
                                              std::move(detail));
                        }
                        continue;
                    }
                    if (reservation.file.status != OperationStatus::not_found) {
                        return result_for(
                            transaction, FileTransferRunStatus::recovery_required,
                            reservation.file.status == OperationStatus::success
                                ? OperationStatus::source_changed
                                : reservation.file.status,
                            OperationEvidence::none,
                            reservation.file.status == OperationStatus::success
                                ? "an unowned object occupies the unacknowledged transfer path"
                                : reservation.file.detail_utf8);
                    }
                }
                if (item.destination_state == FileTransferDestinationState::temp_reserved ||
                    item.destination_state == FileTransferDestinationState::content_ready) {
                    const auto observed = probe_temp(transaction, index);
                    if (observed.status == OperationStatus::not_found) {
                        clear_destination_proof(item);
                        std::string detail;
                        if (!persist(transaction, detail)) {
                            return result_for(transaction, FileTransferRunStatus::journal_error,
                                              OperationStatus::io_error, failure_evidence,
                                              std::move(detail));
                        }
                        continue;
                    }
                    if (observed.status != OperationStatus::success ||
                        !same_object_identity(observed.snapshot.source_revision_utf8,
                                              item.destination_snapshot.source_revision_utf8)) {
                        return result_for(
                            transaction, FileTransferRunStatus::recovery_required,
                            observed.status == OperationStatus::success
                                ? OperationStatus::source_changed
                                : observed.status,
                            OperationEvidence::none,
                            observed.status == OperationStatus::success
                                ? "the owned transfer temporary object changed physical identity"
                                : observed.detail_utf8);
                    }
                    item.destination_snapshot = observed.snapshot;
                    item.content_sha256 = {};
                    item.content_bytes = 0;
                    item.content_proof_present = false;
                    item.destination_state = FileTransferDestinationState::temp_reserved;
                    std::string detail;
                    if (!persist(transaction, detail)) {
                        return result_for(transaction, FileTransferRunStatus::journal_error,
                                          OperationStatus::io_error, failure_evidence,
                                          std::move(detail));
                    }
                    if (stop.stop_requested()) {
                        return result_for(
                            transaction, FileTransferRunStatus::stopped, failure_status,
                            failure_evidence,
                            "file transfer stopped after recording the current temporary file");
                    }
                    step = execute_delete_step(transaction, index, FileTransferStep::cleanup_temp,
                                               progress, stop);
                } else if (transaction.kind == FileTransferKind::move &&
                           item.source_location == FileTransferSourceLocation::staged) {
                    step = execute_rename_step(transaction, index,
                                               FileTransferStep::restore_staged_source, progress,
                                               stop);
                } else if (item.overwrite_state == OverwriteDestinationState::evacuated ||
                           item.overwrite_state ==
                               OverwriteDestinationState::evacuated_unexpected) {
                    step = execute_rename_step(transaction, index,
                                               FileTransferStep::restore_overwrite_destination,
                                               progress, stop);
                } else {
                    std::string detail;
                    if (!remove_journal(transaction, detail)) {
                        return result_for(transaction, FileTransferRunStatus::journal_error,
                                          OperationStatus::io_error, failure_evidence,
                                          std::move(detail));
                    }
                    return result_for(transaction,
                                      transaction.cancelled
                                          ? FileTransferRunStatus::cancelled
                                          : FileTransferRunStatus::partial_failure,
                                      failure_status, failure_evidence, failure_detail);
                }
            }
            if (step.outcome == StepOutcome::committed) {
                continue;
            }
            if (step.outcome == StepOutcome::journal_error) {
                return result_for(transaction, FileTransferRunStatus::journal_error, step.status,
                                  step.evidence, std::move(step.detail_utf8));
            }
            if (step.outcome == StepOutcome::stopped) {
                return result_for(transaction, FileTransferRunStatus::stopped, step.status,
                                  step.evidence, std::move(step.detail_utf8));
            }
            return result_for(transaction, FileTransferRunStatus::recovery_required, step.status,
                              step.evidence, std::move(step.detail_utf8));
        }
    }

    [[nodiscard]] FileTransferResult continue_transaction(FileTransferTransaction transaction,
                                                          const Progress &progress,
                                                          const std::stop_token &stop) {
        if (transaction.phase == FileTransferPhase::prepared) {
            transaction.phase = FileTransferPhase::processing;
            clear_failure(transaction);
            std::string detail;
            if (!persist(transaction, detail)) {
                return result_for(transaction, FileTransferRunStatus::journal_error,
                                  OperationStatus::io_error, OperationEvidence::none,
                                  std::move(detail));
            }
        }
        if (transaction.phase == FileTransferPhase::prepublish_cleanup) {
            return continue_cleanup(std::move(transaction), progress, stop);
        }
        for (;;) {
            if (transaction.phase == FileTransferPhase::completed) {
                if (active_directory_leaf_request) {
                    const auto &leaf = *active_directory_leaf_request;
                    const auto verified = submit_rename(
                        rename_request(transaction, 0U, FileTransferStep::publish_temp), true);
                    if (verified.status != OperationStatus::success ||
                        verified.evidence != OperationEvidence::committed ||
                        !snapshot_exact_matches(transaction.items.front().destination_snapshot,
                                                verified.confirmed_snapshot)) {
                        return result_for(
                            transaction, FileTransferRunStatus::recovery_required, verified.status,
                            verified.evidence,
                            verified.detail_utf8.empty()
                                ? "directory-leaf publication proof could not be verified"
                                : verified.detail_utf8);
                    }
                    const auto audited = submit_directory_leaf_audit(transaction, leaf);
                    if (!audited.ok() || audited.evidence != OperationEvidence::committed ||
                        !verified_directory_leaf_proof) {
                        return result_for(
                            transaction,
                            directory_leaf_audit_journal_error
                                ? FileTransferRunStatus::journal_error
                                : FileTransferRunStatus::recovery_required,
                            audited.ok() ? OperationStatus::io_error : audited.status,
                            audited.evidence,
                            audited.detail_utf8.empty()
                                ? "directory-leaf content proof could not be committed"
                                : audited.detail_utf8);
                    }
                    return result_for(transaction, FileTransferRunStatus::success,
                                      OperationStatus::success, OperationEvidence::committed, {});
                }
                std::string detail;
                if (!remove_journal(transaction, detail)) {
                    return result_for(transaction, FileTransferRunStatus::journal_error,
                                      OperationStatus::io_error, OperationEvidence::committed,
                                      std::move(detail));
                }
                return result_for(transaction, FileTransferRunStatus::success,
                                  OperationStatus::success, OperationEvidence::committed, {});
            }
            if (stop.stop_requested()) {
                return result_for(transaction, FileTransferRunStatus::stopped,
                                  OperationStatus::success, OperationEvidence::none,
                                  "file transfer stopped at a durable boundary");
            }
            const auto step = transaction.active_step == FileTransferStep::none
                                  ? next_processing_step(transaction, progress, stop)
                                  : recover_active_step(transaction, progress, stop);
            if (step.outcome == StepOutcome::committed) {
                continue;
            }
            if (step.outcome == StepOutcome::clean_failure) {
                return continue_cleanup(std::move(transaction), progress, stop);
            }
            if (step.outcome == StepOutcome::journal_error) {
                return result_for(transaction, FileTransferRunStatus::journal_error, step.status,
                                  step.evidence, step.detail_utf8);
            }
            if (step.outcome == StepOutcome::stopped) {
                return result_for(transaction, FileTransferRunStatus::stopped, step.status,
                                  step.evidence, step.detail_utf8);
            }
            return result_for(transaction, FileTransferRunStatus::recovery_required, step.status,
                              step.evidence, step.detail_utf8);
        }
    }

    [[nodiscard]] FileTransferResult
    run_new(const FileTransferKind kind, const std::vector<FileTransferSource> &sources,
            const Progress &progress, const std::stop_token &stop,
            std::optional<DirectoryLeafTransferBinding> binding = std::nullopt) {
        active_directory_leaf_request.reset();
        verified_directory_leaf_proof.reset();
        if (binding) {
            if (sources.size() != 1U) {
                return {.status = FileTransferRunStatus::invalid_request,
                        .operation_status = OperationStatus::invalid_request,
                        .evidence = OperationEvidence::no_commit,
                        .completed = 0,
                        .total = sources.size(),
                        .failed_index =
                            sources.empty() ? std::nullopt : std::optional<std::size_t>(0),
                        .destinations = {},
                        .directory_leaf_completion_proof = std::nullopt,
                        .detail_utf8 = "directory-leaf transfer must contain exactly one copy"};
            }
            active_directory_leaf_request = DirectoryLeafTransferRequest{
                .binding = std::move(*binding), .file = sources.front()};
        }
        if (options.journal_path.empty()) {
            return {.status = FileTransferRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .evidence = OperationEvidence::no_commit,
                    .completed = 0,
                    .total = sources.size(),
                    .failed_index = sources.empty() ? std::nullopt : std::optional<std::size_t>(0),
                    .destinations = {},
                    .directory_leaf_completion_proof = std::nullopt,
                    .detail_utf8 = "file-transfer journal path is empty"};
        }
        if (journal.read().status != DurableJournalStatus::not_found) {
            recovery_pending.store(true, std::memory_order_release);
            return {.status = FileTransferRunStatus::recovery_required,
                    .operation_status = OperationStatus::unknown_outcome,
                    .evidence = OperationEvidence::none,
                    .completed = 0,
                    .total = sources.size(),
                    .failed_index = sources.empty() ? std::nullopt : std::optional<std::size_t>(0),
                    .destinations = {},
                    .directory_leaf_completion_proof = std::nullopt,
                    .detail_utf8 = "an unfinished operation must be recovered first"};
        }
        static std::atomic<std::uint64_t> sequence{1};
        auto operation_id = static_cast<std::uint64_t>(
                                std::chrono::system_clock::now().time_since_epoch().count()) ^
                            sequence.fetch_add(1, std::memory_order_relaxed);
        if (operation_id == 0) {
            operation_id = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
        }
        FileTransferTransaction transaction;
        try {
            if (active_directory_leaf_request) {
                if (kind != FileTransferKind::copy || sources.size() != 1U) {
                    throw std::invalid_argument(
                        "directory-leaf transfer must contain exactly one copy");
                }
                transaction =
                    prepare_directory_leaf_transfer(*active_directory_leaf_request, operation_id)
                        .transfer;
            } else {
                transaction = prepare_file_transfer_transaction(kind, sources, operation_id);
                for (std::size_t index = 0; index < sources.size(); ++index) {
                    const auto &approved = sources[index].authorized_overwrite_destination;
                    if (!approved.source_revision_utf8.empty()) {
                        transaction.items[index].overwrite_destination_snapshot = approved;
                        transaction.items[index].overwrite_state = OverwriteDestinationState::authorized;
                    }
                }
            }
        } catch (const std::exception &error) {
            return {.status = FileTransferRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .evidence = OperationEvidence::no_commit,
                    .completed = 0,
                    .total = sources.size(),
                    .failed_index = sources.empty() ? std::nullopt : std::optional<std::size_t>(0),
                    .destinations = {},
                    .directory_leaf_completion_proof = std::nullopt,
                    .detail_utf8 = error.what()};
        }
        std::string detail;
        if (!persist(transaction, detail)) {
            return result_for(transaction, FileTransferRunStatus::journal_error,
                              OperationStatus::io_error, OperationEvidence::no_commit,
                              std::move(detail));
        }
        if (auto conflict = request_initial_conflicts(transaction, progress, stop)) {
            return std::move(*conflict);
        }
        return continue_transaction(std::move(transaction), progress, stop);
    }

    [[nodiscard]] FileTransferResult
    run_resume(const Progress &progress, const std::stop_token &stop,
               std::optional<DirectoryLeafTransferBinding> expected_leaf_binding = std::nullopt,
               const DirectoryLeafRecoveryAdmission &admit_leaf_recovery = {}) {
        active_directory_leaf_request.reset();
        verified_directory_leaf_proof.reset();
        const auto loaded = journal.read();
        const auto expected_kind = expected_leaf_binding
                                       ? CurrentOperationKind::directory_leaf_transfer
                                       : CurrentOperationKind::file_transfer;
        if (!loaded.ok() || loaded.encoding != CurrentOperationJournalEncoding::typed ||
            loaded.kind != expected_kind) {
            return {.status = FileTransferRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .evidence = OperationEvidence::none,
                    .completed = 0,
                    .total = 0,
                    .failed_index = std::nullopt,
                    .destinations = {},
                    .directory_leaf_completion_proof = std::nullopt,
                    .detail_utf8 =
                        loaded.status == DurableJournalStatus::not_found
                            ? "there is no file transfer to recover"
                            : "current operation journal is not the expected file transfer"};
        }
        FileTransferTransaction transaction;
        std::string detail;
        if (expected_leaf_binding) {
            DirectoryLeafTransferJournal leaf;
            if (!decode_directory_leaf_transfer(loaded.payload, leaf, detail) ||
                !directory_leaf_transfer_binding_matches(leaf.binding, *expected_leaf_binding,
                                                         detail)) {
                return {.status = FileTransferRunStatus::journal_error,
                        .operation_status = OperationStatus::invalid_request,
                        .evidence = OperationEvidence::none,
                        .completed = 0,
                        .total = 1,
                        .failed_index = std::optional<std::size_t>(0),
                        .destinations = {},
                        .directory_leaf_completion_proof = std::nullopt,
                        .detail_utf8 = std::move(detail)};
            }
            DirectoryLeafTransferRequest candidate{
                .binding = leaf.binding,
                .file = leaf.original_file,
            };
            bool admitted{};
            try {
                admitted = admit_leaf_recovery && admit_leaf_recovery(candidate, detail);
            } catch (const std::exception &error) {
                detail = error.what();
            } catch (...) {
                detail = "directory-leaf recovery admission failed unexpectedly";
            }
            if (!admitted) {
                if (detail.empty()) {
                    detail = "directory-leaf recovery was rejected by its parent operation";
                }
                return {.status = FileTransferRunStatus::journal_error,
                        .operation_status = OperationStatus::invalid_request,
                        .evidence = OperationEvidence::none,
                        .completed = 0,
                        .total = 1,
                        .failed_index = std::optional<std::size_t>(0),
                        .destinations = {},
                        .directory_leaf_completion_proof = std::nullopt,
                        .detail_utf8 = std::move(detail)};
            }
            active_directory_leaf_request = std::move(candidate);
            transaction = std::move(leaf.transfer);
        } else if (!decode_file_transfer_transaction(loaded.payload, transaction, detail)) {
            return {.status = FileTransferRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .evidence = OperationEvidence::none,
                    .completed = 0,
                    .total = 0,
                    .failed_index = std::nullopt,
                    .destinations = {},
                    .directory_leaf_completion_proof = std::nullopt,
                    .detail_utf8 = std::move(detail)};
        }
        if (auto conflict = request_initial_conflicts(transaction, progress, stop)) {
            return std::move(*conflict);
        }
        return continue_transaction(std::move(transaction), progress, stop);
    }

    template <typename Job>
    [[nodiscard]] bool launch(CurrentOperationLease lease, Job job, Progress progress,
                              Completion completion) {
        if (!completion) {
            return false;
        }
        const auto self = shared_from_this();
        {
            std::scoped_lock lock(worker_mutex);
            if (stopped.load(std::memory_order_acquire) ||
                running.load(std::memory_order_acquire)) {
                return false;
            }
            worker_stop = std::stop_source{};
            running.store(true, std::memory_order_release);
            const auto stop = worker_stop.get_token();
            try {
                std::thread([self, lease = std::move(lease), job = std::move(job),
                             progress = std::move(progress), completion = std::move(completion),
                             stop]() mutable {
                    {
                        std::scoped_lock lock(self->worker_mutex);
                        self->worker_id = std::this_thread::get_id();
                    }
                    FileTransferResult result;
                    try {
                        result = job(progress, stop);
                    } catch (const std::exception &error) {
                        result = {.status = FileTransferRunStatus::recovery_required,
                                  .operation_status = OperationStatus::io_error,
                                  .evidence = OperationEvidence::none,
                                  .completed = 0,
                                  .total = 0,
                                  .failed_index = std::nullopt,
                                  .destinations = {},
                                  .directory_leaf_completion_proof = std::nullopt,
                                  .detail_utf8 = error.what()};
                    } catch (...) {
                        result = {.status = FileTransferRunStatus::recovery_required,
                                  .operation_status = OperationStatus::io_error,
                                  .evidence = OperationEvidence::none,
                                  .completed = 0,
                                  .total = 0,
                                  .failed_index = std::nullopt,
                                  .destinations = {},
                                  .directory_leaf_completion_proof = std::nullopt,
                                  .detail_utf8 = "file transfer failed unexpectedly"};
                    }
                    self->active_directory_leaf_request.reset();
                    self->active_conflict = {};
                    lease.release();
                    if (!stop.stop_requested()) {
                        try {
                            completion(std::move(result));
                        } catch (...) { // NOLINT(bugprone-empty-catch)
                            // Application callbacks cannot terminate the coordinator worker.
                        }
                    }
                    {
                        std::scoped_lock lock(self->worker_mutex);
                        self->worker_id = {};
                        self->running.store(false, std::memory_order_release);
                    }
                    self->worker_completed.notify_all();
                }).detach();
            } catch (...) {
                running.store(false, std::memory_order_release);
                worker_completed.notify_all();
                return false;
            }
        }
        return true;
    }

    void stop() noexcept {
        bool called_from_worker{};
        {
            std::scoped_lock lock(worker_mutex);
            stopped.store(true, std::memory_order_release);
            static_cast<void>(worker_stop.request_stop());
            called_from_worker =
                running.load(std::memory_order_acquire) && worker_id == std::this_thread::get_id();
        }
        file_transfers.stop();
        file_operations.stop();
        if (!called_from_worker && active_coordinator_callback_state != this) {
            std::unique_lock lock(worker_mutex);
            worker_completed.wait(lock,
                                  [this] { return !running.load(std::memory_order_acquire); });
        }
    }
};

FileTransferCoordinator::FileTransferCoordinator(FileTransferCoordinatorOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}

FileTransferCoordinator::~FileTransferCoordinator() {
    stop();
}

bool FileTransferCoordinator::start(const FileTransferKind kind,
                                    std::vector<FileTransferSource> sources, Progress progress,
                                    Completion completion, Conflict conflict) {
    if (sources.empty() || sources.size() > kMaximumFileTransferItems) {
        return false;
    }
    const auto state = state_;
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, kind, sources = std::move(sources),
         conflict = std::move(conflict)](const Progress &callback, const std::stop_token &stop) {
            state->active_conflict = conflict;
            return state->run_new(kind, sources, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool FileTransferCoordinator::resume(Progress progress, Completion completion, Conflict conflict) {
    const auto state = state_;
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || !state->owns_recovery(CurrentOperationKind::file_transfer)) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, conflict = std::move(conflict)](const Progress &callback,
                                                const std::stop_token &stop) {
            state->active_conflict = conflict;
            return state->run_resume(callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool FileTransferCoordinator::start_directory_leaf_copy(DirectoryLeafTransferRequest request,
                                                        Progress progress, Completion completion) {
    std::string detail;
    if (!valid_directory_leaf_transfer_request(request, detail)) {
        return false;
    }
    const auto state = state_;
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, request = std::move(request)](const Progress &callback,
                                              const std::stop_token &stop) {
            return state->run_new(FileTransferKind::copy, {request.file}, callback, stop,
                                  request.binding);
        },
        std::move(progress), std::move(completion));
}

bool FileTransferCoordinator::resume_directory_leaf_copy(
    DirectoryLeafTransferBinding expected_binding, DirectoryLeafRecoveryAdmission admit_recovery,
    Progress progress, Completion completion) {
    std::string detail;
    if (!valid_directory_leaf_transfer_binding(expected_binding, detail) || !admit_recovery) {
        return false;
    }
    const auto state = state_;
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() ||
        !state->owns_recovery(CurrentOperationKind::directory_leaf_transfer)) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, expected_binding = std::move(expected_binding),
         admit_recovery = std::move(admit_recovery)](const Progress &callback,
                                                     const std::stop_token &stop) {
            return state->run_resume(callback, stop, expected_binding, admit_recovery);
        },
        std::move(progress), std::move(completion));
}

bool FileTransferCoordinator::busy() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

bool FileTransferCoordinator::stopping() const noexcept {
    return state_->stopped.load(std::memory_order_acquire);
}

bool FileTransferCoordinator::recovery_pending() const noexcept {
    return state_->refresh_recovery_pending();
}

bool FileTransferCoordinator::owns_recovery() const noexcept {
    return state_->owns_recovery(CurrentOperationKind::file_transfer);
}

bool FileTransferCoordinator::owns_directory_leaf_recovery() const noexcept {
    return state_->owns_recovery(CurrentOperationKind::directory_leaf_transfer);
}

void FileTransferCoordinator::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

void FileTransferCoordinator::resolve_conflict(
    const FileTransferConflictDecision decision) noexcept {
    state_->resolve_conflict(decision);
}

void FileTransferCoordinator::resolve_conflict(const bool overwrite) noexcept {
    resolve_conflict(overwrite ? FileTransferConflictDecision::overwrite
                               : FileTransferConflictDecision::cancel);
}

} // namespace vove::fileops
