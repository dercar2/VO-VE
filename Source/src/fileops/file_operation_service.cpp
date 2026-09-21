#include "vove/fileops/file_operation_service.hpp"

#include "operation_evidence.hpp"
#include "catalog_process.hpp"
#include "vove/fileops/file_operation_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vove::fileops {

struct FileOperationService::State {
    std::mutex mutex;
    std::mutex completion_mutex;
    std::condition_variable completed;
    std::shared_ptr<platform::detail::CatalogProcess> process;
    std::filesystem::path helper_path;
    std::chrono::milliseconds timeout{std::chrono::seconds(15)};
    std::function<void()> after_operation_slot_released;
    std::atomic_bool complete_accepted_requests_during_stop{false};
    std::atomic_bool busy{false};
    std::atomic_bool stopping{false};
    std::atomic_size_t active_workers{0};
};

namespace {

class WorkerGuard final {
  public:
    explicit WorkerGuard(std::shared_ptr<FileOperationService::State> state)
        : state_(std::move(state)) {}
    ~WorkerGuard() {
        if (!operation_slot_released_) {
            state_->busy.store(false, std::memory_order_release);
        }
        state_->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        state_->completed.notify_all();
    }

    WorkerGuard(const WorkerGuard &) = delete;
    WorkerGuard &operator=(const WorkerGuard &) = delete;

    void release_operation_slot() noexcept {
        state_->busy.store(false, std::memory_order_release);
        operation_slot_released_ = true;
        state_->completed.notify_all();
    }

  private:
    std::shared_ptr<FileOperationService::State> state_;
    bool operation_slot_released_{};
};

std::filesystem::path default_helper_path() {
    const auto catalog_helper = platform::detail::default_catalog_helper_path();
    if (catalog_helper.empty()) {
        return {};
    }
#ifdef _WIN32
    return catalog_helper.parent_path() / L"vove-fileop-helper.exe";
#else
    return catalog_helper.parent_path() / "vove-fileop-helper";
#endif
}

OperationStatus transport_status(const catalog::CatalogErrorKind kind) {
    switch (kind) {
    case catalog::CatalogErrorKind::permission_denied:
        return OperationStatus::permission_denied;
    case catalog::CatalogErrorKind::authentication_required:
        return OperationStatus::authentication_required;
    case catalog::CatalogErrorKind::network_disconnected:
        return OperationStatus::disconnected;
    case catalog::CatalogErrorKind::timed_out:
        return OperationStatus::timed_out;
    case catalog::CatalogErrorKind::not_found:
        return OperationStatus::not_found;
    case catalog::CatalogErrorKind::cancelled:
    case catalog::CatalogErrorKind::io_error:
    case catalog::CatalogErrorKind::none:
        return OperationStatus::io_error;
    }
    return OperationStatus::io_error;
}

std::vector<std::byte> encode_request(const RenameRequest &request) {
    return encode_rename_request(request);
}

std::vector<std::byte> encode_request(const DeleteRequest &request) {
    return encode_delete_request(request);
}

std::vector<std::byte> encode_request(const CreateDirectoryRequest &request) {
    return encode_create_directory_request(request);
}

bool valid_request(const RenameRequest &request, std::string &detail) {
    return valid_rename_request(request, detail);
}

bool valid_request(const DeleteRequest &request, std::string &detail) {
    return valid_delete_request(request, detail);
}

bool valid_request(const CreateDirectoryRequest &request, std::string &detail) {
    return valid_create_directory_request(request, detail);
}

std::chrono::milliseconds helper_timeout(const std::shared_ptr<FileOperationService::State> &state,
                                         const RenameRequest &request) noexcept {
    if (request.action != RenameAction::execute ||
        request.mode != RenameMode::transfer_overwrite_stage) {
        return state->timeout;
    }
    constexpr std::uint64_t assumed_bytes_per_second = 1024U * 1024U;
    constexpr std::uint64_t base_seconds = 15U;
    constexpr std::uint64_t maximum_seconds = 2U * 60U * 60U;
    const auto blocks = request.expected_source.size_bytes / assumed_bytes_per_second +
                        (request.expected_source.size_bytes % assumed_bytes_per_second != 0U);
    const auto estimated_seconds =
        std::min(maximum_seconds, base_seconds + std::min(blocks, maximum_seconds) * 2U);
    return std::max(state->timeout, std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::seconds(estimated_seconds)));
}

template <typename Request>
std::chrono::milliseconds helper_timeout(const std::shared_ptr<FileOperationService::State> &state,
                                         const Request &) noexcept {
    return state->timeout;
}

template <typename Request>
OperationResult run_helper(const std::shared_ptr<FileOperationService::State> &state,
                           const Request &request, const bool mutating_attempt) {
    if (state->stopping.load(std::memory_order_acquire)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::io_error,
                .confirmed_snapshot = {},
                .detail_utf8 = "file operation service is stopping"};
    }
    catalog::CatalogError launch_error;
    auto process = platform::detail::start_catalog_process(state->helper_path, launch_error);
    if (!process) {
        return {.operation_id = request.operation_id,
                .status = mutating_attempt ? OperationStatus::unknown_outcome
                                           : transport_status(launch_error.kind),
                .platform_code = launch_error.platform_code,
                .confirmed_snapshot = {},
                .detail_utf8 = launch_error.message_utf8};
    }
    bool stop_after_launch{};
    {
        std::scoped_lock lock(state->mutex);
        state->process = process;
        stop_after_launch = state->stopping.load(std::memory_order_acquire);
    }
    if (stop_after_launch) {
        process->terminate();
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        std::scoped_lock lock(state->mutex);
        if (state->process == process) {
            state->process.reset();
        }
        return {.operation_id = request.operation_id,
                .status = OperationStatus::io_error,
                .confirmed_snapshot = {},
                .detail_utf8 = "file operation service stopped during helper launch"};
    }
    std::atomic_bool timed_out{false};
    const auto timeout = helper_timeout(state, request);
    std::jthread watchdog([&](const std::stop_token &stop) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!stop.stop_requested()) {
            timed_out.store(true, std::memory_order_release);
            process->terminate();
        }
    });
    auto clear_process = [&] {
        watchdog.request_stop();
        std::scoped_lock lock(state->mutex);
        if (state->process == process) {
            state->process.reset();
        }
    };

    std::string transport_error;
    try {
        const auto payload = encode_request(request);
        if (!process->write_frame(payload, transport_error)) {
            process->terminate();
            std::string ignored;
            static_cast<void>(process->wait(ignored));
            clear_process();
            return {.operation_id = request.operation_id,
                    .status = timed_out.load(std::memory_order_acquire)
                                  ? OperationStatus::timed_out
                                  : (mutating_attempt ? OperationStatus::unknown_outcome
                                                      : OperationStatus::io_error),
                    .confirmed_snapshot = {},
                    .detail_utf8 = std::move(transport_error)};
        }
        process->close_input();
    } catch (const std::exception &error) {
        process->terminate();
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        clear_process();
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .confirmed_snapshot = {},
                .detail_utf8 = error.what()};
    }

    std::vector<std::byte> response;
    if (!process->read_frame(response, transport_error, kMaximumOperationPayloadBytes)) {
        process->terminate();
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        clear_process();
        return {.operation_id = request.operation_id,
                .status = timed_out.load(std::memory_order_acquire)
                              ? OperationStatus::timed_out
                              : (mutating_attempt ? OperationStatus::unknown_outcome
                                                  : OperationStatus::io_error),
                .confirmed_snapshot = {},
                .detail_utf8 = std::move(transport_error)};
    }
    OperationResult result;
    if (!decode_operation_result(response, result, transport_error) ||
        result.operation_id != request.operation_id) {
        process->terminate();
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        clear_process();
        return {.operation_id = request.operation_id,
                .status =
                    mutating_attempt ? OperationStatus::unknown_outcome : OperationStatus::io_error,
                .confirmed_snapshot = {},
                .detail_utf8 = transport_error.empty() ? "file operation helper reply is invalid"
                                                       : std::move(transport_error)};
    }
    std::string wait_error;
    static_cast<void>(process->wait(wait_error));
    clear_process();
    return result;
}

template <typename Request, typename Action>
bool submit_request(const std::shared_ptr<FileOperationService::State> &state, Request request,
                    FileOperationService::Completion completion, const Action action) {
    bool expected = false;
    if (!completion || state->helper_path.empty() ||
        state->stopping.load(std::memory_order_acquire) ||
        !state->busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    request.action = action;
    state->active_workers.fetch_add(1, std::memory_order_acq_rel);
    try {
        std::thread([state, request = std::move(request),
                     completion = std::move(completion)]() mutable {
            WorkerGuard worker(state);
            OperationResult result{.operation_id = request.operation_id,
                                   .status = OperationStatus::io_error,
                                   .confirmed_snapshot = {},
                                   .detail_utf8 = "file operation failed unexpectedly"};
            try {
                std::string validation;
                const auto mutating_attempt = request.action == Action::execute;
                result = valid_request(request, validation)
                             ? run_helper(state, request, mutating_attempt)
                             : OperationResult{.operation_id = request.operation_id,
                                               .status = OperationStatus::invalid_request,
                                               .evidence = OperationEvidence::no_commit,
                                               .confirmed_snapshot = {},
                                               .detail_utf8 = std::move(validation)};
                if (mutating_attempt && !state->stopping.load(std::memory_order_acquire) &&
                    (result.status == OperationStatus::unknown_outcome ||
                     result.status == OperationStatus::timed_out ||
                     result.status == OperationStatus::disconnected ||
                     result.status == OperationStatus::io_error)) {
                    auto reconcile = request;
                    reconcile.action = Action::reconcile_only;
                    const auto observed = run_helper(state, reconcile, false);
                    const auto timed_out = result.status == OperationStatus::timed_out ||
                                           result.status == OperationStatus::disconnected;
                    result = detail::merge_reconciliation(std::move(result), observed);
                    if (timed_out && result.evidence == OperationEvidence::no_commit &&
                        observed.status == OperationStatus::unknown_outcome &&
                        observed.source_present && !observed.destination_present) {
                        result.detail_utf8 =
                            "operation timed out; reconciliation confirmed no committed mutation";
                    }
                }
            } catch (const std::exception &error) {
                result = {.operation_id = request.operation_id,
                          .status = OperationStatus::io_error,
                          .confirmed_snapshot = {},
                          .detail_utf8 = error.what()};
            } catch (...) {
                result = {.operation_id = request.operation_id,
                          .status = OperationStatus::io_error,
                          .confirmed_snapshot = {},
                          .detail_utf8 = "file operation failed with an unknown exception"};
            }
            worker.release_operation_slot();
            if (state->after_operation_slot_released) {
                try {
                    state->after_operation_slot_released();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Diagnostic hooks cannot change operation delivery.
                }
            }
            if (state->complete_accepted_requests_during_stop.load(std::memory_order_acquire) ||
                !state->stopping.load(std::memory_order_acquire)) {
                try {
                    completion(std::move(result));
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Completion is application code; it cannot terminate the worker.
                }
            }
        }).detach();
    } catch (...) {
        state->busy.store(false, std::memory_order_release);
        state->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        state->completed.notify_all();
        return false;
    }
    return true;
}

} // namespace

FileOperationService::FileOperationService(FileOperationServiceOptions options)
    : state_(std::make_shared<State>()) {
    state_->helper_path =
        options.helper_path.empty() ? default_helper_path() : std::move(options.helper_path);
    if (options.timeout.count() > 0) {
        state_->timeout = options.timeout;
    }
    state_->after_operation_slot_released = std::move(options.after_operation_slot_released);
}

FileOperationService::~FileOperationService() {
    stop();
}

void FileOperationService::stop() noexcept {
    state_->stopping.store(true, std::memory_order_release);
    std::shared_ptr<platform::detail::CatalogProcess> process;
    {
        std::scoped_lock lock(state_->mutex);
        process = state_->process;
    }
    if (process) {
        process->terminate();
    }
    std::unique_lock lock(state_->completion_mutex);
    const auto idle = [state = state_] {
        return !state->busy.load(std::memory_order_acquire) &&
               state->active_workers.load(std::memory_order_acquire) == 0;
    };
    if (state_->complete_accepted_requests_during_stop.load(std::memory_order_acquire)) {
        // The batch coordinator cannot safely abandon an accepted mutation result. Its helper is
        // already terminated above, and the retained completion owns no application callback.
        state_->completed.wait(lock, idle);
    } else {
        static_cast<void>(
            state_->completed.wait_for(lock, state_->timeout + std::chrono::seconds(1), idle));
    }
}

bool FileOperationService::submit_rename(RenameRequest request, Completion completion) {
    return submit(std::move(request), std::move(completion), RenameAction::execute);
}

bool FileOperationService::submit_reconciliation(RenameRequest request, Completion completion) {
    return submit(std::move(request), std::move(completion), RenameAction::reconcile_only);
}

bool FileOperationService::submit_delete(DeleteRequest request, Completion completion) {
    return submit(std::move(request), std::move(completion), DeleteAction::execute);
}

bool FileOperationService::submit_delete_reconciliation(DeleteRequest request,
                                                        Completion completion) {
    return submit(std::move(request), std::move(completion), DeleteAction::reconcile_only);
}

bool FileOperationService::submit_create_directory(CreateDirectoryRequest request,
                                                   Completion completion) {
    return submit(std::move(request), std::move(completion), CreateDirectoryAction::execute);
}

bool FileOperationService::submit_create_directory_reconciliation(CreateDirectoryRequest request,
                                                                  Completion completion) {
    return submit(std::move(request), std::move(completion), CreateDirectoryAction::reconcile_only);
}

bool FileOperationService::submit(RenameRequest request, Completion completion,
                                  const RenameAction action) {
    return submit_request(state_, std::move(request), std::move(completion), action);
}

bool FileOperationService::submit(DeleteRequest request, Completion completion,
                                  const DeleteAction action) {
    return submit_request(state_, std::move(request), std::move(completion), action);
}

bool FileOperationService::submit(CreateDirectoryRequest request, Completion completion,
                                  const CreateDirectoryAction action) {
    return submit_request(state_, std::move(request), std::move(completion), action);
}

bool FileOperationService::busy() const noexcept {
    return state_->busy.load(std::memory_order_acquire);
}

bool FileOperationService::stopping() const noexcept {
    return state_->stopping.load(std::memory_order_acquire);
}

void FileOperationService::retain_accepted_completions_during_stop() noexcept {
    state_->complete_accepted_requests_during_stop.store(true, std::memory_order_release);
}

} // namespace vove::fileops
