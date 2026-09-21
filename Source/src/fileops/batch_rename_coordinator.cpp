#include "vove/fileops/batch_rename_coordinator.hpp"

#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/current_operation_lease.hpp"

#include "filename_component_limit.hpp"
#include "operation_evidence.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace vove::fileops {
namespace {

enum class StepOutcome : std::uint8_t {
    committed,
    no_commit,
    recovery_required,
    journal_error,
    stopped,
};

struct StepResult {
    StepOutcome outcome{StepOutcome::recovery_required};
    OperationStatus operation_status{OperationStatus::io_error};
    std::string detail_utf8;
};

struct PendingOperation {
    std::mutex mutex;
    std::condition_variable completed;
    std::optional<OperationResult> result;
};

bool confirmed_no_commit(const OperationResult &result) noexcept {
    return result.evidence == OperationEvidence::no_commit;
}

std::filesystem::path step_destination(const BatchTransactionItem &item, const BatchStepKind step) {
    switch (step) {
    case BatchStepKind::evacuate:
        return item.temporary;
    case BatchStepKind::rollback:
        return item.source;
    case BatchStepKind::publish:
        return item.destination;
    case BatchStepKind::none:
        break;
    }
    return {};
}

BatchItemLocation step_location(const BatchStepKind step) {
    switch (step) {
    case BatchStepKind::evacuate:
        return BatchItemLocation::temporary;
    case BatchStepKind::rollback:
        return BatchItemLocation::source;
    case BatchStepKind::publish:
        return BatchItemLocation::destination;
    case BatchStepKind::none:
        break;
    }
    return BatchItemLocation::source;
}

std::uint64_t step_operation_id(const BatchRenameTransaction &transaction, const BatchStepKind step,
                                const std::size_t index) noexcept {
    constexpr std::uint64_t mix = 0x9e3779b97f4a7c15ULL;
    auto value =
        transaction.operation_id ^ (mix * (index + 1U)) ^ (static_cast<std::uint64_t>(step) << 56U);
    return value == 0 ? transaction.operation_id : value;
}

std::size_t count_location(const BatchRenameTransaction &transaction,
                           const BatchItemLocation location) {
    std::size_t count{};
    for (const auto &item : transaction.items) {
        count += item.location == location ? 1U : 0U;
    }
    return count;
}

std::string journal_error_detail(const DurableJournalResult &result) {
    if (result.status == DurableJournalStatus::payload_too_large) {
        return "batch rename journal exceeds its size limit";
    }
    if (result.error) {
        return "batch rename journal I/O failed: " + result.error.message();
    }
    return "batch rename journal could not be updated";
}

bool destination_components_fit_volume(const BatchRenameTransaction &transaction,
                                       std::string &detail_utf8) {
    const auto &directory = transaction.items.front().source.parent_path();
    const auto limit = detail::query_filename_component_limit(directory);
    if (!limit.ok()) {
        detail_utf8 = "destination filename limit could not be verified: " + limit.error.message();
        return false;
    }
    for (const auto &item : transaction.items) {
        if (detail::filename_component_units(item.temporary.filename()) > limit.maximum_units ||
            detail::filename_component_units(item.destination.filename()) > limit.maximum_units) {
            detail_utf8 = "destination filename exceeds this filesystem's component limit";
            return false;
        }
    }
    return true;
}

} // namespace

struct BatchRenameCoordinator::State : std::enable_shared_from_this<State> {
    explicit State(BatchRenameCoordinatorOptions value)
        : options(std::move(value)), journal(options.journal_path),
          file_operations(options.file_operations) {
        file_operations.retain_accepted_completions_during_stop();
        static_cast<void>(refresh_recovery_pending());
    }

    BatchRenameCoordinatorOptions options;
    CurrentOperationJournalStore journal;
    FileOperationService file_operations;
    std::atomic_bool running{false};
    std::atomic_bool stopped{false};
    std::atomic_bool recovery_pending{false};
    std::mutex worker_mutex;
    std::condition_variable worker_completed;
    std::stop_source worker_stop;
    std::thread::id worker_id;

    [[nodiscard]] bool refresh_recovery_pending() noexcept {
        const auto pending = journal.read().status != DurableJournalStatus::not_found;
        recovery_pending.store(pending, std::memory_order_release);
        return pending;
    }

    [[nodiscard]] bool owns_recovery() noexcept {
        const auto loaded = journal.read();
        const auto owns =
            loaded.ok() && (loaded.encoding == CurrentOperationJournalEncoding::legacy_untyped ||
                            (loaded.encoding == CurrentOperationJournalEncoding::typed &&
                             loaded.kind == CurrentOperationKind::batch_rename));
        recovery_pending.store(loaded.status != DurableJournalStatus::not_found,
                               std::memory_order_release);
        return owns;
    }

    [[nodiscard]] bool persist(const BatchRenameTransaction &transaction,
                               std::string &detail_utf8) {
        try {
            const auto payload = encode_batch_transaction(transaction);
            const auto stored = journal.write(CurrentOperationKind::batch_rename, payload);
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

    [[nodiscard]] bool remove_journal(std::string &detail_utf8) {
        const auto removed = journal.remove();
        if (!removed.ok()) {
            detail_utf8 = journal_error_detail(removed);
            recovery_pending.store(true, std::memory_order_release);
            return false;
        }
        recovery_pending.store(false, std::memory_order_release);
        return true;
    }

    [[nodiscard]] OperationResult submit_and_wait(const RenameRequest &request,
                                                  const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto accepted =
            reconcile ? file_operations.submit_reconciliation(
                            request,
                            [pending](OperationResult result) {
                                {
                                    std::scoped_lock lock(pending->mutex);
                                    pending->result = std::move(result);
                                }
                                pending->completed.notify_all();
                            })
                      : file_operations.submit_rename(request, [pending](OperationResult result) {
                            {
                                std::scoped_lock lock(pending->mutex);
                                pending->result = std::move(result);
                            }
                            pending->completed.notify_all();
                        });
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service did not accept the batch step"};
        }

        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->result.has_value(); });
        if (!pending->result) {
            std::terminate();
        }
        return std::move(pending->result.value());
    }

    [[nodiscard]] RenameRequest request_for(const BatchRenameTransaction &transaction,
                                            const std::size_t index,
                                            const BatchStepKind step) const {
        const auto &item = transaction.items[index];
        return {.operation_id = step_operation_id(transaction, step, index),
                .action = RenameAction::execute,
                .mode = RenameMode::batch_internal,
                .object_kind = item.object_kind,
                .source = item.current,
                .destination = step_destination(item, step),
                .expected_source = item.current_snapshot,
                .source_parent_identity_utf8 = {},
                .destination_parent_identity_utf8 = {},
                .destination_anchor_path = {},
                .destination_anchor_identity_utf8 = {}};
    }

    [[nodiscard]] bool apply_success(BatchRenameTransaction &transaction, const std::size_t index,
                                     const BatchStepKind step, const OperationResult &result,
                                     std::string &detail_utf8) {
        auto &item = transaction.items[index];
        const auto expected_identity =
            stable_object_identity(item.current_snapshot.source_revision_utf8);
        const auto confirmed_identity =
            stable_object_identity(result.confirmed_snapshot.source_revision_utf8);
        if (!result.ok() || expected_identity.empty() || confirmed_identity != expected_identity) {
            detail_utf8 = "batch step succeeded without a trustworthy object snapshot";
            return false;
        }
        item.current = step_destination(item, step);
        item.current_snapshot = result.confirmed_snapshot;
        item.location = step_location(step);
        transaction.active_step = BatchStepKind::none;
        transaction.active_index = 0;
        transaction.active_evidence = OperationEvidence::none;
        if (transaction.phase != BatchTransactionPhase::rollback) {
            transaction.failure_status = OperationStatus::success;
            transaction.failure_detail_utf8.clear();
        }
        return true;
    }

    [[nodiscard]] StepResult execute_step(BatchRenameTransaction &transaction,
                                          const std::size_t index, const BatchStepKind step,
                                          const Progress &progress, const std::stop_token &stop) {
        transaction.active_step = step;
        transaction.active_index = static_cast<std::uint32_t>(index);
        transaction.active_evidence = OperationEvidence::none;
        std::string detail;
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .detail_utf8 = std::move(detail)};
        }

        const auto request = request_for(transaction, index, step);
        if (progress) {
            progress({.phase = transaction.phase,
                      .completed = step == BatchStepKind::publish
                                       ? count_location(transaction, BatchItemLocation::destination)
                                       : count_location(transaction, BatchItemLocation::temporary),
                      .total = transaction.items.size(),
                      .source = request.source,
                      .destination = request.destination});
        }
        auto result = submit_and_wait(request, false);
        if (result.status == OperationStatus::not_found && !stop.stop_requested()) {
            result = submit_and_wait(request, true);
        }
        if (!result.ok()) {
            const auto step_name = step == BatchStepKind::evacuate ? "evacuate"
                                   : step == BatchStepKind::rollback ? "rollback" : "publish";
            result.detail_utf8 = "batch " + std::string(step_name) + " item " +
                                 std::to_string(index + 1) + ": " + result.detail_utf8;
        }
        if (result.ok()) {
            transaction.active_evidence = result.evidence;
            if (!apply_success(transaction, index, step, result, detail)) {
                transaction.failure_status = OperationStatus::unknown_outcome;
                transaction.failure_detail_utf8 = detail;
                if (!persist(transaction, detail)) {
                    return {.outcome = StepOutcome::journal_error,
                            .operation_status = OperationStatus::io_error,
                            .detail_utf8 = std::move(detail)};
                }
                return {.outcome = StepOutcome::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .detail_utf8 = transaction.failure_detail_utf8};
            }
            if (!persist(transaction, detail)) {
                return {.outcome = StepOutcome::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .detail_utf8 = std::move(detail)};
            }
            if (stop.stop_requested()) {
                return {.outcome = StepOutcome::stopped,
                        .operation_status = OperationStatus::success,
                        .detail_utf8 = "batch rename stopped after recording the completed step"};
            }
            return {.outcome = StepOutcome::committed,
                    .operation_status = OperationStatus::success,
                    .detail_utf8 = {}};
        }

        transaction.failure_status = result.status;
        transaction.active_evidence = result.evidence;
        transaction.failure_detail_utf8 = result.detail_utf8;
        if (confirmed_no_commit(result)) {
            if (step == BatchStepKind::evacuate) {
                transaction.phase = BatchTransactionPhase::rollback;
            }
            transaction.active_step = BatchStepKind::none;
            transaction.active_index = 0;
            transaction.active_evidence = OperationEvidence::none;
            if (!persist(transaction, detail)) {
                return {.outcome = StepOutcome::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .detail_utf8 = std::move(detail)};
            }
            if (stop.stop_requested()) {
                return {.outcome = StepOutcome::stopped,
                        .operation_status = result.status,
                        .detail_utf8 = "batch rename stopped after recording the non-commit"};
            }
            return {.outcome = StepOutcome::no_commit,
                    .operation_status = result.status,
                    .detail_utf8 = result.detail_utf8};
        }
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .detail_utf8 = transaction.failure_detail_utf8 + "; " + detail};
        }
        if (stop.stop_requested()) {
            return {.outcome = StepOutcome::stopped,
                    .operation_status = result.status,
                    .detail_utf8 = "batch rename stopped after recording operation evidence"};
        }
        return {.outcome = StepOutcome::recovery_required,
                .operation_status = result.status,
                .detail_utf8 = detail.empty() ? result.detail_utf8 : std::move(detail)};
    }

    [[nodiscard]] StepResult reconcile_active(BatchRenameTransaction &transaction,
                                              const std::stop_token &stop) {
        const auto index = static_cast<std::size_t>(transaction.active_index);
        const auto step = transaction.active_step;
        const auto request = request_for(transaction, index, step);
        auto result = submit_and_wait(request, true);
        if (transaction.active_evidence != OperationEvidence::none) {
            result = detail::merge_reconciliation({.operation_id = request.operation_id,
                                                   .status = transaction.failure_status,
                                                   .evidence = transaction.active_evidence,
                                                   .confirmed_snapshot = {},
                                                   .detail_utf8 = transaction.failure_detail_utf8},
                                                  std::move(result));
        }
        std::string detail;
        if (result.ok()) {
            if (!apply_success(transaction, index, step, result, detail)) {
                transaction.failure_status = OperationStatus::unknown_outcome;
                transaction.active_evidence = result.evidence;
                transaction.failure_detail_utf8 = detail;
                if (!persist(transaction, detail)) {
                    return {.outcome = StepOutcome::journal_error,
                            .operation_status = OperationStatus::io_error,
                            .detail_utf8 = std::move(detail)};
                }
                return {.outcome = StepOutcome::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .detail_utf8 = transaction.failure_detail_utf8};
            }
            if (!persist(transaction, detail)) {
                return {.outcome = StepOutcome::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .detail_utf8 = std::move(detail)};
            }
            if (stop.stop_requested()) {
                return {.outcome = StepOutcome::stopped,
                        .operation_status = OperationStatus::success,
                        .detail_utf8 =
                            "batch recovery stopped after recording the reconciled step"};
            }
            return {.outcome = StepOutcome::committed,
                    .operation_status = OperationStatus::success,
                    .detail_utf8 = {}};
        }
        if (confirmed_no_commit(result)) {
            transaction.active_step = BatchStepKind::none;
            transaction.active_index = 0;
            transaction.active_evidence = OperationEvidence::none;
            if (transaction.phase != BatchTransactionPhase::rollback) {
                transaction.failure_status = OperationStatus::success;
                transaction.failure_detail_utf8.clear();
            }
            if (!persist(transaction, detail)) {
                return {.outcome = StepOutcome::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .detail_utf8 = std::move(detail)};
            }
            if (stop.stop_requested()) {
                return {.outcome = StepOutcome::stopped,
                        .operation_status = result.status,
                        .detail_utf8 =
                            "batch recovery stopped after recording the reconciled non-commit"};
            }
            return {.outcome = StepOutcome::no_commit,
                    .operation_status = result.status,
                    .detail_utf8 = result.detail_utf8};
        }

        transaction.failure_status = result.status;
        transaction.active_evidence = result.evidence;
        transaction.failure_detail_utf8 = result.detail_utf8;
        if (!persist(transaction, detail)) {
            return {.outcome = StepOutcome::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .detail_utf8 = transaction.failure_detail_utf8 + "; " + detail};
        }
        if (stop.stop_requested()) {
            return {.outcome = StepOutcome::stopped,
                    .operation_status = result.status,
                    .detail_utf8 =
                        "batch recovery stopped after recording reconciliation evidence"};
        }
        return {.outcome = StepOutcome::recovery_required,
                .operation_status = result.status,
                .detail_utf8 = result.detail_utf8.empty() ? std::move(detail) : result.detail_utf8};
    }

    [[nodiscard]] BatchRenameResult recovery_result(const BatchRenameTransaction &transaction,
                                                    const StepResult &step) const {
        return {.status = step.outcome == StepOutcome::journal_error
                              ? BatchRunStatus::journal_error
                              : (step.outcome == StepOutcome::stopped
                                     ? BatchRunStatus::stopped
                                     : BatchRunStatus::recovery_required),
                .operation_status = step.operation_status,
                .completed = count_location(transaction, BatchItemLocation::destination),
                .total = transaction.items.size(),
                .destinations = {},
                .detail_utf8 = step.detail_utf8};
    }

    [[nodiscard]] BatchRenameResult rollback(BatchRenameTransaction &transaction,
                                             const Progress &progress,
                                             const std::stop_token &stop) {
        const auto original_status = transaction.failure_status;
        const auto original_detail = transaction.failure_detail_utf8;
        for (std::size_t offset{}; offset < transaction.items.size(); ++offset) {
            const auto index = transaction.items.size() - 1U - offset;
            if (transaction.items[index].location != BatchItemLocation::temporary) {
                continue;
            }
            const auto step =
                execute_step(transaction, index, BatchStepKind::rollback, progress, stop);
            if (step.outcome != StepOutcome::committed) {
                return recovery_result(transaction, step);
            }
        }
        std::string detail;
        if (!remove_journal(detail)) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .destinations = {},
                    .detail_utf8 = std::move(detail)};
        }
        return {.status = BatchRunStatus::rolled_back,
                .operation_status = original_status,
                .completed = transaction.items.size(),
                .total = transaction.items.size(),
                .destinations = {},
                .detail_utf8 =
                    original_detail.empty() ? "batch rename was rolled back" : original_detail};
    }

    [[nodiscard]] BatchRenameResult continue_transaction(BatchRenameTransaction transaction,
                                                         const Progress &progress,
                                                         const std::stop_token &stop) {
        std::string detail;
        if (transaction.active_step == BatchStepKind::none &&
            transaction.phase == BatchTransactionPhase::evacuating &&
            transaction.failure_status != OperationStatus::success) {
            transaction.phase = BatchTransactionPhase::rollback;
            if (!persist(transaction, detail)) {
                return recovery_result(transaction, {.outcome = StepOutcome::journal_error,
                                                     .operation_status = OperationStatus::io_error,
                                                     .detail_utf8 = std::move(detail)});
            }
        }
        if (transaction.active_step != BatchStepKind::none) {
            const auto reconciled = reconcile_active(transaction, stop);
            if (reconciled.outcome == StepOutcome::recovery_required ||
                reconciled.outcome == StepOutcome::journal_error ||
                reconciled.outcome == StepOutcome::stopped) {
                return recovery_result(transaction, reconciled);
            }
        }

        if (transaction.phase == BatchTransactionPhase::prepared) {
            transaction.phase = BatchTransactionPhase::evacuating;
            if (!persist(transaction, detail)) {
                return recovery_result(transaction, {.outcome = StepOutcome::journal_error,
                                                     .operation_status = OperationStatus::io_error,
                                                     .detail_utf8 = std::move(detail)});
            }
        }

        if (transaction.phase == BatchTransactionPhase::evacuating) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                if (transaction.items[index].location != BatchItemLocation::source) {
                    continue;
                }
                const auto step =
                    execute_step(transaction, index, BatchStepKind::evacuate, progress, stop);
                if (step.outcome == StepOutcome::committed) {
                    continue;
                }
                if (step.outcome == StepOutcome::no_commit) {
                    return rollback(transaction, progress, stop);
                }
                return recovery_result(transaction, step);
            }
            transaction.phase = BatchTransactionPhase::commit_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(transaction, {.outcome = StepOutcome::journal_error,
                                                     .operation_status = OperationStatus::io_error,
                                                     .detail_utf8 = std::move(detail)});
            }
        }

        if (transaction.phase == BatchTransactionPhase::commit_intent) {
            transaction.phase = BatchTransactionPhase::publishing;
            if (!persist(transaction, detail)) {
                return recovery_result(transaction, {.outcome = StepOutcome::journal_error,
                                                     .operation_status = OperationStatus::io_error,
                                                     .detail_utf8 = std::move(detail)});
            }
        }

        if (transaction.phase == BatchTransactionPhase::publishing) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                if (transaction.items[index].location == BatchItemLocation::destination) {
                    continue;
                }
                if (transaction.items[index].location != BatchItemLocation::temporary) {
                    return {.status = BatchRunStatus::recovery_required,
                            .operation_status = OperationStatus::source_changed,
                            .completed =
                                count_location(transaction, BatchItemLocation::destination),
                            .total = transaction.items.size(),
                            .destinations = {},
                            .detail_utf8 =
                                "published batch journal contains an impossible item location"};
                }
                const auto step =
                    execute_step(transaction, index, BatchStepKind::publish, progress, stop);
                if (step.outcome != StepOutcome::committed) {
                    return recovery_result(transaction, step);
                }
            }
            std::vector<std::filesystem::path> destinations;
            destinations.reserve(transaction.items.size());
            for (const auto &item : transaction.items) {
                destinations.push_back(item.destination);
            }
            if (!remove_journal(detail)) {
                return {.status = BatchRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = transaction.items.size(),
                        .total = transaction.items.size(),
                        .destinations = std::move(destinations),
                        .detail_utf8 = std::move(detail)};
            }
            return {.status = BatchRunStatus::success,
                    .operation_status = OperationStatus::success,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .destinations = std::move(destinations),
                    .detail_utf8 = {}};
        }

        if (transaction.phase == BatchTransactionPhase::rollback) {
            return rollback(transaction, progress, stop);
        }

        return {.status = BatchRunStatus::recovery_required,
                .operation_status = OperationStatus::io_error,
                .completed = 0,
                .total = transaction.items.size(),
                .destinations = {},
                .detail_utf8 = "batch rename transaction phase is not recoverable"};
    }

    [[nodiscard]] BatchRenameResult run_new(const BatchRenamePlan &plan,
                                            const std::vector<BatchRenameSource> &sources,
                                            const Progress &progress, const std::stop_token &stop) {
        const auto stopped_result = [] {
            return BatchRenameResult{.status = BatchRunStatus::stopped,
                                     .operation_status = OperationStatus::success,
                                     .destinations = {},
                                     .detail_utf8 = "batch rename stopped before journaling"};
        };
        if (stop.stop_requested()) {
            return stopped_result();
        }
        if (options.journal_path.empty()) {
            return {.status = BatchRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = "batch rename journal path is empty"};
        }
        const auto existing = journal.read();
        if (existing.status != DurableJournalStatus::not_found) {
            recovery_pending.store(true, std::memory_order_release);
            return {.status = BatchRunStatus::recovery_required,
                    .operation_status = OperationStatus::unknown_outcome,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = "an unfinished batch rename must be recovered first"};
        }
        static std::atomic<std::uint64_t> sequence{1};
        auto operation_id = static_cast<std::uint64_t>(
                                std::chrono::system_clock::now().time_since_epoch().count()) ^
                            sequence.fetch_add(1, std::memory_order_relaxed);
        if (operation_id == 0) {
            operation_id = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
        }
        BatchRenameTransaction transaction;
        try {
            transaction = prepare_batch_transaction(plan, sources, operation_id);
        } catch (const std::exception &error) {
            return {.status = BatchRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = sources.size(),
                    .destinations = {},
                    .detail_utf8 = error.what()};
        }
        if (transaction.items.empty()) {
            return {.status = BatchRunStatus::success,
                    .operation_status = OperationStatus::success,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = {}};
        }
        std::string detail;
        if (!destination_components_fit_volume(transaction, detail)) {
            return {.status = BatchRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .destinations = {},
                    .detail_utf8 = std::move(detail)};
        }
        std::error_code directory_error;
        if (stop.stop_requested()) {
            return stopped_result();
        }
        const auto parent = options.journal_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = "batch journal directory could not be created: " +
                                   directory_error.message()};
        }
        if (stop.stop_requested()) {
            return stopped_result();
        }
        if (!persist(transaction, detail)) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .destinations = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_transaction(std::move(transaction), progress, stop);
    }

    [[nodiscard]] BatchRenameResult run_resume(const Progress &progress,
                                               const std::stop_token &stop) {
        const auto loaded = journal.read();
        if (!loaded.ok()) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = loaded.status == DurableJournalStatus::not_found
                                       ? "there is no batch rename to recover"
                                       : "batch rename journal is unreadable"};
        }
        BatchRenameTransaction transaction;
        std::string detail;
        if (loaded.encoding == CurrentOperationJournalEncoding::typed &&
            loaded.kind != CurrentOperationKind::batch_rename) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = "current operation journal belongs to another operation"};
        }
        if (!decode_batch_transaction(loaded.payload, transaction, detail)) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = 0,
                    .destinations = {},
                    .detail_utf8 = std::move(detail)};
        }
        if (loaded.encoding == CurrentOperationJournalEncoding::legacy_untyped &&
            !persist(transaction, detail)) {
            return {.status = BatchRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .destinations = {},
                    .detail_utf8 = std::move(detail)};
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
                        std::scoped_lock worker_lock(self->worker_mutex);
                        self->worker_id = std::this_thread::get_id();
                    }
                    BatchRenameResult result;
                    try {
                        result = job(progress, stop);
                    } catch (const std::exception &error) {
                        result = {.status = BatchRunStatus::recovery_required,
                                  .operation_status = OperationStatus::io_error,
                                  .completed = 0,
                                  .total = 0,
                                  .destinations = {},
                                  .detail_utf8 = error.what()};
                    } catch (...) {
                        result = {.status = BatchRunStatus::recovery_required,
                                  .operation_status = OperationStatus::io_error,
                                  .completed = 0,
                                  .total = 0,
                                  .destinations = {},
                                  .detail_utf8 = "batch rename failed unexpectedly"};
                    }
                    lease.release();
                    if (!stop.stop_requested()) {
                        try {
                            completion(std::move(result));
                        } catch (...) { // NOLINT(bugprone-empty-catch)
                            // Application callbacks cannot terminate the coordinator worker.
                        }
                    }
                    {
                        std::scoped_lock worker_lock(self->worker_mutex);
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
        file_operations.stop();
        if (!called_from_worker) {
            std::unique_lock lock(worker_mutex);
            worker_completed.wait(lock,
                                  [this] { return !running.load(std::memory_order_acquire); });
        }
    }
};

BatchRenameCoordinator::BatchRenameCoordinator(BatchRenameCoordinatorOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}

BatchRenameCoordinator::~BatchRenameCoordinator() {
    stop();
}

bool BatchRenameCoordinator::start(BatchRenamePlan plan, std::vector<BatchRenameSource> sources,
                                   Progress progress, Completion completion) {
    const auto state = state_;
    if (state->stopped.load(std::memory_order_acquire) ||
        !plan.valid() || plan.rows.size() != sources.size() || sources.empty()) {
        return false;
    }
    std::error_code lease_error;
    const auto unchanged = std::ranges::all_of(plan.rows, [](const auto &row) {
        return row.source == row.destination;
    });
    auto lease = unchanged ? CurrentOperationLease{}
                           : CurrentOperationLease::try_acquire(state->options.journal_path,
                                                                 lease_error);
    if ((!unchanged && !lease.owns_lock()) || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, plan = std::move(plan), sources = std::move(sources)](const Progress &callback,
                                                                      const std::stop_token &stop) {
            return state->run_new(plan, sources, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool BatchRenameCoordinator::resume(Progress progress, Completion completion) {
    const auto state = state_;
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || !state->owns_recovery()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state](const Progress &callback, const std::stop_token &stop) {
            return state->run_resume(callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool BatchRenameCoordinator::busy() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

bool BatchRenameCoordinator::stopping() const noexcept {
    return state_->stopped.load(std::memory_order_acquire);
}

bool BatchRenameCoordinator::recovery_pending() const noexcept {
    return state_->refresh_recovery_pending();
}

bool BatchRenameCoordinator::owns_recovery() const noexcept {
    return state_->owns_recovery();
}

void BatchRenameCoordinator::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

} // namespace vove::fileops
