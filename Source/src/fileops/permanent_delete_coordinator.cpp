#include "vove/fileops/permanent_delete_coordinator.hpp"

#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/current_operation_lease.hpp"

#include "operation_evidence.hpp"

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
    OperationStatus status{OperationStatus::io_error};
    std::string detail;
};

struct PendingOperation {
    std::mutex mutex;
    std::condition_variable completed;
    std::optional<OperationResult> result;
};

std::size_t count_location(const PermanentDeleteTransaction &transaction,
                           const PermanentDeleteItemLocation location) {
    std::size_t count{};
    for (const auto &item : transaction.items) {
        count += item.location == location ? 1U : 0U;
    }
    return count;
}

std::uint64_t step_operation_id(const PermanentDeleteTransaction &transaction,
                                const PermanentDeleteStep step, const std::size_t index) noexcept {
    constexpr std::uint64_t mix = 0x9e3779b97f4a7c15ULL;
    auto value =
        transaction.operation_id ^ (mix * (index + 1U)) ^ (static_cast<std::uint64_t>(step) << 56U);
    return value == 0 ? transaction.operation_id : value;
}

std::string journal_error_detail(const DurableJournalResult &result) {
    if (result.status == DurableJournalStatus::payload_too_large) {
        return "permanent-delete journal exceeds its size limit";
    }
    if (result.error) {
        return "permanent-delete journal I/O failed: " + result.error.message();
    }
    return "permanent-delete journal could not be updated";
}

} // namespace

struct PermanentDeleteCoordinator::State : std::enable_shared_from_this<State> {
    explicit State(PermanentDeleteCoordinatorOptions value)
        : options(std::move(value)), journal(options.journal_path),
          file_operations(options.file_operations) {
        file_operations.retain_accepted_completions_during_stop();
        static_cast<void>(refresh_recovery_pending());
    }

    PermanentDeleteCoordinatorOptions options;
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
        const auto owns = loaded.ok() &&
                          loaded.encoding == CurrentOperationJournalEncoding::typed &&
                          loaded.kind == CurrentOperationKind::permanent_delete;
        recovery_pending.store(loaded.status != DurableJournalStatus::not_found,
                               std::memory_order_release);
        return owns;
    }

    [[nodiscard]] bool persist(const PermanentDeleteTransaction &transaction, std::string &detail) {
        try {
            const auto payload = encode_permanent_delete_transaction(transaction);
            const auto stored = journal.write(CurrentOperationKind::permanent_delete, payload);
            if (!stored.ok()) {
                detail = journal_error_detail(stored);
                recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                                       std::memory_order_release);
                return false;
            }
            recovery_pending.store(true, std::memory_order_release);
            return true;
        } catch (const std::exception &error) {
            detail = error.what();
            recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                                   std::memory_order_release);
            return false;
        }
    }

    [[nodiscard]] bool remove_journal(std::string &detail) {
        const auto removed = journal.remove();
        if (!removed.ok()) {
            detail = journal_error_detail(removed);
            recovery_pending.store(true, std::memory_order_release);
            return false;
        }
        recovery_pending.store(false, std::memory_order_release);
        return true;
    }

    [[nodiscard]] OperationResult wait_for_rename(const RenameRequest &request,
                                                  const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
            }
            pending->completed.notify_all();
        };
        const auto accepted = reconcile ? file_operations.submit_reconciliation(request, completion)
                                        : file_operations.submit_rename(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected a delete rename step"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->result.has_value(); });
        if (!pending->result) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "delete rename step completed without a result"};
        }
        return std::move(*pending->result);
    }

    [[nodiscard]] OperationResult wait_for_delete(const DeleteRequest &request,
                                                  const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
            }
            pending->completed.notify_all();
        };
        const auto accepted =
            reconcile ? file_operations.submit_delete_reconciliation(request, completion)
                      : file_operations.submit_delete(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected a permanent-delete step"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->result.has_value(); });
        if (!pending->result) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "permanent-delete step completed without a result"};
        }
        return std::move(*pending->result);
    }

    [[nodiscard]] RenameRequest rename_request(const PermanentDeleteTransaction &transaction,
                                               const std::size_t index,
                                               const PermanentDeleteStep step) const {
        const auto &item = transaction.items[index];
        return {.operation_id = step_operation_id(transaction, step, index),
                .action = RenameAction::execute,
                .mode = RenameMode::batch_internal,
                .source = item.current,
                .destination = step == PermanentDeleteStep::evacuate ? item.temporary : item.source,
                .expected_source = item.current_snapshot,
                .source_parent_identity_utf8 = {},
                .destination_parent_identity_utf8 = {},
                .destination_anchor_path = {},
                .destination_anchor_identity_utf8 = {}};
    }

    [[nodiscard]] DeleteRequest delete_request(const PermanentDeleteTransaction &transaction,
                                               const std::size_t index) const {
        const auto &item = transaction.items[index];
        return {.operation_id = step_operation_id(transaction, PermanentDeleteStep::erase, index),
                .action = DeleteAction::execute,
                .mode = DeleteMode::permanent_remote,
                .source = item.current,
                .expected_source = item.current_snapshot,
                .source_parent_identity_utf8 = {},
                .guard_path = {},
                .expected_guard = {},
                .guard_parent_identity_utf8 = {}};
    }

    [[nodiscard]] bool apply_rename_success(PermanentDeleteTransaction &transaction,
                                            const std::size_t index, const PermanentDeleteStep step,
                                            const OperationResult &result, std::string &detail) {
        auto &item = transaction.items[index];
        const auto expected = stable_object_identity(item.current_snapshot.source_revision_utf8);
        const auto confirmed =
            stable_object_identity(result.confirmed_snapshot.source_revision_utf8);
        if (!result.ok() || expected.empty() || confirmed != expected) {
            detail = "delete rename step succeeded without a trustworthy snapshot";
            return false;
        }
        item.current = step == PermanentDeleteStep::evacuate ? item.temporary : item.source;
        item.current_snapshot = result.confirmed_snapshot;
        item.location = step == PermanentDeleteStep::evacuate
                            ? PermanentDeleteItemLocation::temporary
                            : PermanentDeleteItemLocation::source;
        return true;
    }

    static void clear_active(PermanentDeleteTransaction &transaction) {
        transaction.active_step = PermanentDeleteStep::none;
        transaction.active_index = 0;
        transaction.active_evidence = OperationEvidence::none;
        if (transaction.phase != PermanentDeletePhase::rollback) {
            transaction.failure_status = OperationStatus::success;
            transaction.failure_detail_utf8.clear();
        }
    }

    [[nodiscard]] bool apply_success(PermanentDeleteTransaction &transaction,
                                     const std::size_t index, const PermanentDeleteStep step,
                                     const OperationResult &result, std::string &detail) {
        if (step == PermanentDeleteStep::erase) {
            if (!result.ok() || result.evidence != OperationEvidence::committed) {
                detail = "delete step succeeded without committed evidence";
                return false;
            }
            auto &item = transaction.items[index];
            item.current.clear();
            item.location = PermanentDeleteItemLocation::deleted;
        } else if (!apply_rename_success(transaction, index, step, result, detail)) {
            return false;
        }
        clear_active(transaction);
        return true;
    }

    [[nodiscard]] OperationResult submit_step(const PermanentDeleteTransaction &transaction,
                                              const std::size_t index,
                                              const PermanentDeleteStep step,
                                              const bool reconcile) {
        return step == PermanentDeleteStep::erase
                   ? wait_for_delete(delete_request(transaction, index), reconcile)
                   : wait_for_rename(rename_request(transaction, index, step), reconcile);
    }

    [[nodiscard]] StepResult execute_step(PermanentDeleteTransaction &transaction,
                                          const std::size_t index, const PermanentDeleteStep step,
                                          const Progress &progress, const std::stop_token &stop) {
        transaction.active_step = step;
        transaction.active_index = static_cast<std::uint32_t>(index);
        transaction.active_evidence = OperationEvidence::none;
        std::string detail;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        if (progress) {
            progress({.phase = transaction.phase,
                      .completed =
                          count_location(transaction, step == PermanentDeleteStep::erase
                                                          ? PermanentDeleteItemLocation::deleted
                                                          : PermanentDeleteItemLocation::temporary),
                      .total = transaction.items.size(),
                      .source = transaction.items[index].source});
        }
        auto result = submit_step(transaction, index, step, false);
        if (result.status == OperationStatus::not_found && !stop.stop_requested()) {
            result = submit_step(transaction, index, step, true);
        }
        if (result.ok()) {
            transaction.active_evidence = result.evidence;
            if (!apply_success(transaction, index, step, result, detail)) {
                transaction.failure_status = OperationStatus::unknown_outcome;
                transaction.failure_detail_utf8 = detail;
                if (!persist(transaction, detail)) {
                    return {StepOutcome::journal_error, OperationStatus::io_error,
                            std::move(detail)};
                }
                return {StepOutcome::recovery_required, OperationStatus::unknown_outcome,
                        transaction.failure_detail_utf8};
            }
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return stop.stop_requested()
                       ? StepResult{StepOutcome::stopped, OperationStatus::success,
                                    "permanent delete stopped after recording a completed step"}
                       : StepResult{StepOutcome::committed, OperationStatus::success, {}};
        }

        transaction.failure_status = result.status;
        transaction.active_evidence = result.evidence;
        transaction.failure_detail_utf8 = result.detail_utf8;
        if (result.evidence == OperationEvidence::no_commit) {
            if (step == PermanentDeleteStep::evacuate) {
                transaction.phase = PermanentDeletePhase::rollback;
            }
            transaction.active_step = PermanentDeleteStep::none;
            transaction.active_index = 0;
            transaction.active_evidence = OperationEvidence::none;
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return stop.stop_requested()
                       ? StepResult{StepOutcome::stopped, result.status,
                                    "permanent delete stopped after recording a non-commit"}
                       : StepResult{StepOutcome::no_commit, result.status, result.detail_utf8};
        }
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error,
                    transaction.failure_detail_utf8 + "; " + detail};
        }
        return stop.stop_requested()
                   ? StepResult{StepOutcome::stopped, result.status,
                                "permanent delete stopped after recording operation evidence"}
                   : StepResult{StepOutcome::recovery_required, result.status, result.detail_utf8};
    }

    [[nodiscard]] StepResult reconcile_active(PermanentDeleteTransaction &transaction,
                                              const std::stop_token &stop) {
        const auto index = static_cast<std::size_t>(transaction.active_index);
        const auto step = transaction.active_step;
        auto result = submit_step(transaction, index, step, true);
        if (transaction.active_evidence != OperationEvidence::none) {
            const auto operation_id = result.operation_id;
            result = detail::merge_reconciliation({.operation_id = operation_id,
                                                   .status = transaction.failure_status,
                                                   .evidence = transaction.active_evidence,
                                                   .confirmed_snapshot = {},
                                                   .detail_utf8 = transaction.failure_detail_utf8},
                                                  std::move(result));
        }
        std::string detail;
        if (result.ok()) {
            if (!apply_success(transaction, index, step, result, detail) ||
                !persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return stop.stop_requested()
                       ? StepResult{StepOutcome::stopped, OperationStatus::success,
                                    "delete recovery stopped after recording reconciliation"}
                       : StepResult{StepOutcome::committed, OperationStatus::success, {}};
        }
        if (result.evidence == OperationEvidence::no_commit) {
            if (step == PermanentDeleteStep::evacuate) {
                transaction.phase = PermanentDeletePhase::rollback;
            }
            transaction.failure_status = result.status;
            transaction.failure_detail_utf8 = result.detail_utf8;
            transaction.active_step = PermanentDeleteStep::none;
            transaction.active_index = 0;
            transaction.active_evidence = OperationEvidence::none;
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return {StepOutcome::no_commit, result.status, result.detail_utf8};
        }
        transaction.failure_status = result.status;
        transaction.active_evidence = result.evidence;
        transaction.failure_detail_utf8 = result.detail_utf8;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error,
                    transaction.failure_detail_utf8 + "; " + detail};
        }
        return {StepOutcome::recovery_required, result.status, result.detail_utf8};
    }

    [[nodiscard]] PermanentDeleteResult
    recovery_result(const PermanentDeleteTransaction &transaction, const StepResult &step) const {
        return {.status = step.outcome == StepOutcome::journal_error
                              ? PermanentDeleteRunStatus::journal_error
                              : (step.outcome == StepOutcome::stopped
                                     ? PermanentDeleteRunStatus::stopped
                                     : PermanentDeleteRunStatus::recovery_required),
                .operation_status = step.status,
                .completed = count_location(transaction, PermanentDeleteItemLocation::deleted),
                .total = transaction.items.size(),
                .detail_utf8 = step.detail};
    }

    [[nodiscard]] PermanentDeleteResult rollback(PermanentDeleteTransaction &transaction,
                                                 const Progress &progress,
                                                 const std::stop_token &stop) {
        const auto original_status = transaction.failure_status;
        const auto original_detail = transaction.failure_detail_utf8;
        for (std::size_t offset{}; offset < transaction.items.size(); ++offset) {
            const auto index = transaction.items.size() - 1U - offset;
            if (transaction.items[index].location != PermanentDeleteItemLocation::temporary) {
                continue;
            }
            const auto step =
                execute_step(transaction, index, PermanentDeleteStep::rollback, progress, stop);
            if (step.outcome != StepOutcome::committed) {
                return recovery_result(transaction, step);
            }
        }
        std::string detail;
        if (!remove_journal(detail)) {
            return {.status = PermanentDeleteRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .detail_utf8 = std::move(detail)};
        }
        return {.status = PermanentDeleteRunStatus::rolled_back,
                .operation_status = original_status,
                .completed = 0,
                .total = transaction.items.size(),
                .detail_utf8 =
                    original_detail.empty() ? "permanent delete was rolled back" : original_detail};
    }

    [[nodiscard]] PermanentDeleteResult continue_transaction(PermanentDeleteTransaction transaction,
                                                             const Progress &progress,
                                                             const std::stop_token &stop) {
        std::string detail;
        if (transaction.active_step != PermanentDeleteStep::none) {
            const auto active_step = transaction.active_step;
            const auto reconciled = reconcile_active(transaction, stop);
            if (reconciled.outcome == StepOutcome::recovery_required ||
                reconciled.outcome == StepOutcome::journal_error ||
                reconciled.outcome == StepOutcome::stopped) {
                return recovery_result(transaction, reconciled);
            }
            if (reconciled.outcome == StepOutcome::no_commit &&
                active_step != PermanentDeleteStep::evacuate &&
                !(active_step == PermanentDeleteStep::erase &&
                  reconciled.status == OperationStatus::conflict)) {
                return recovery_result(transaction, {StepOutcome::recovery_required,
                                                     reconciled.status, reconciled.detail});
            }
        }
        if (transaction.phase == PermanentDeletePhase::prepared) {
            transaction.phase = PermanentDeletePhase::evacuating;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == PermanentDeletePhase::evacuating) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                if (transaction.items[index].location != PermanentDeleteItemLocation::source) {
                    continue;
                }
                const auto step =
                    execute_step(transaction, index, PermanentDeleteStep::evacuate, progress, stop);
                if (step.outcome == StepOutcome::committed) {
                    continue;
                }
                if (step.outcome == StepOutcome::no_commit) {
                    return rollback(transaction, progress, stop);
                }
                return recovery_result(transaction, step);
            }
            transaction.phase = PermanentDeletePhase::permanent_delete_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == PermanentDeletePhase::permanent_delete_intent) {
            transaction.phase = PermanentDeletePhase::deleting;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == PermanentDeletePhase::deleting) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                if (transaction.items[index].location == PermanentDeleteItemLocation::deleted) {
                    continue;
                }
                if (transaction.items[index].location != PermanentDeleteItemLocation::temporary) {
                    return {.status = PermanentDeleteRunStatus::recovery_required,
                            .operation_status = OperationStatus::source_changed,
                            .completed =
                                count_location(transaction, PermanentDeleteItemLocation::deleted),
                            .total = transaction.items.size(),
                            .detail_utf8 = "delete journal contains an impossible item location"};
                }
                const auto step =
                    execute_step(transaction, index, PermanentDeleteStep::erase, progress, stop);
                if (step.outcome != StepOutcome::committed) {
                    return recovery_result(
                        transaction,
                        step.outcome == StepOutcome::no_commit
                            ? StepResult{StepOutcome::recovery_required, step.status, step.detail}
                            : step);
                }
            }
            if (!remove_journal(detail)) {
                return {.status = PermanentDeleteRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = transaction.items.size(),
                        .total = transaction.items.size(),
                        .detail_utf8 = std::move(detail)};
            }
            return {.status = PermanentDeleteRunStatus::success,
                    .operation_status = OperationStatus::success,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .detail_utf8 = {}};
        }
        if (transaction.phase == PermanentDeletePhase::rollback) {
            return rollback(transaction, progress, stop);
        }
        return {.status = PermanentDeleteRunStatus::recovery_required,
                .operation_status = OperationStatus::io_error,
                .completed = count_location(transaction, PermanentDeleteItemLocation::deleted),
                .total = transaction.items.size(),
                .detail_utf8 = "permanent-delete phase is not recoverable"};
    }

    [[nodiscard]] PermanentDeleteResult run_new(const std::vector<PermanentDeleteSource> &sources,
                                                const Progress &progress,
                                                const std::stop_token &stop) {
        if (options.journal_path.empty() || sources.empty()) {
            return {.status = PermanentDeleteRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .total = sources.size(),
                    .detail_utf8 = "permanent-delete request or journal path is empty"};
        }
        if (journal.read().status != DurableJournalStatus::not_found) {
            recovery_pending.store(true, std::memory_order_release);
            return {.status = PermanentDeleteRunStatus::recovery_required,
                    .operation_status = OperationStatus::unknown_outcome,
                    .total = sources.size(),
                    .detail_utf8 = "an unfinished file operation must be recovered first"};
        }
        for (const auto &source : sources) {
            const auto classification = inspect_delete_target(source.path);
            if (classification.kind == DeleteTargetKind::remote) {
                continue;
            }
            if (classification.kind == DeleteTargetKind::unknown &&
                classification.failure_status != OperationStatus::success &&
                classification.failure_status != OperationStatus::unsupported) {
                return {.status = PermanentDeleteRunStatus::failed,
                        .operation_status = classification.failure_status,
                        .total = sources.size(),
                        .detail_utf8 = "remote filesystem could not be verified"};
            }
            return {.status = PermanentDeleteRunStatus::unsupported,
                    .operation_status = OperationStatus::unsupported,
                    .total = sources.size(),
                    .detail_utf8 =
                        "permanent delete is allowed only on a verified remote filesystem"};
        }
        static std::atomic<std::uint64_t> sequence{1};
        auto operation_id = static_cast<std::uint64_t>(
                                std::chrono::system_clock::now().time_since_epoch().count()) ^
                            sequence.fetch_add(1, std::memory_order_relaxed);
        if (operation_id == 0) {
            operation_id = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
        }
        PermanentDeleteTransaction transaction;
        try {
            transaction = prepare_permanent_delete_transaction(sources, operation_id);
        } catch (const std::exception &error) {
            return {.status = PermanentDeleteRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .total = sources.size(),
                    .detail_utf8 = error.what()};
        }
        std::error_code directory_error;
        const auto parent = options.journal_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error) {
            return {.status = PermanentDeleteRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .total = sources.size(),
                    .detail_utf8 = "delete journal directory could not be created: " +
                                   directory_error.message()};
        }
        std::string detail;
        if (!persist(transaction, detail)) {
            return {.status = PermanentDeleteRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .total = transaction.items.size(),
                    .detail_utf8 = std::move(detail)};
        }
        return continue_transaction(std::move(transaction), progress, stop);
    }

    [[nodiscard]] PermanentDeleteResult run_resume(const Progress &progress,
                                                   const std::stop_token &stop) {
        const auto loaded = journal.read();
        if (!loaded.ok() || loaded.encoding != CurrentOperationJournalEncoding::typed ||
            loaded.kind != CurrentOperationKind::permanent_delete) {
            return {.status = PermanentDeleteRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .detail_utf8 = loaded.status == DurableJournalStatus::not_found
                                       ? "there is no permanent delete to recover"
                                       : "current operation journal is not a permanent delete"};
        }
        PermanentDeleteTransaction transaction;
        std::string detail;
        if (!decode_permanent_delete_transaction(loaded.payload, transaction, detail)) {
            return {.status = PermanentDeleteRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
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
        std::scoped_lock lock(worker_mutex);
        if (stopped.load(std::memory_order_acquire) || running.load(std::memory_order_acquire)) {
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
                PermanentDeleteResult result;
                try {
                    result = job(progress, stop);
                } catch (const std::exception &error) {
                    result.detail_utf8 = error.what();
                } catch (...) {
                    result.detail_utf8 = "permanent delete failed unexpectedly";
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

PermanentDeleteCoordinator::PermanentDeleteCoordinator(PermanentDeleteCoordinatorOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}

PermanentDeleteCoordinator::~PermanentDeleteCoordinator() {
    stop();
}

bool PermanentDeleteCoordinator::start(std::vector<PermanentDeleteSource> sources,
                                       Progress progress, Completion completion) {
    const auto state = state_;
    if (sources.empty()) {
        return false;
    }
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, sources = std::move(sources)](const Progress &callback,
                                              const std::stop_token &stop) mutable {
            return state->run_new(sources, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool PermanentDeleteCoordinator::resume(Progress progress, Completion completion) {
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

bool PermanentDeleteCoordinator::busy() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

bool PermanentDeleteCoordinator::stopping() const noexcept {
    return state_->stopped.load(std::memory_order_acquire);
}

bool PermanentDeleteCoordinator::recovery_pending() const noexcept {
    return state_->refresh_recovery_pending();
}

bool PermanentDeleteCoordinator::owns_recovery() const noexcept {
    return state_->owns_recovery();
}

void PermanentDeleteCoordinator::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

} // namespace vove::fileops
