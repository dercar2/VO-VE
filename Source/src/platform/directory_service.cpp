#include "vove/platform/directory_service.hpp"

#include "catalog_process.hpp"
#include "catalog_protocol.hpp"
#include "directory_recursive.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vove::platform {

namespace detail {

struct EnumerationAttempt {
    catalog::RequestGeneration generation{};
    std::atomic_bool done{false};
    std::atomic_bool timed_out{false};
    std::atomic_bool cancelled{false};
    std::atomic<std::int64_t> last_progress_ns{};
    bool recursive{false};
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
    // These fields are protected by DirectoryServiceState::mutex.
    bool terminal_published{false};
    bool received_files{false};
    std::uint32_t directories_visited{};
    std::optional<catalog::CatalogBatch> pending_batch;
    std::mutex process_mutex;
    std::shared_ptr<CatalogProcess> process;
};

struct DirectoryServiceState {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<catalog::CatalogBatch> batches;
    std::optional<catalog::CatalogBatch> terminal_batch;
    std::optional<catalog::CatalogBatch> progress_batch;
    std::vector<std::weak_ptr<EnumerationAttempt>> attempts;
    std::atomic<catalog::RequestGeneration> active_generation{0};
    std::atomic_bool stopping{false};
    std::size_t active_workers{};
    std::filesystem::path helper_path;
    std::chrono::milliseconds inactivity_timeout{catalog::kCatalogInactivityTimeout};
};

} // namespace detail

namespace {

std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool attempt_is_current(const std::shared_ptr<detail::DirectoryServiceState> &state,
                        const std::shared_ptr<detail::EnumerationAttempt> &attempt) {
    return !state->stopping.load(std::memory_order_acquire) &&
           !attempt->timed_out.load(std::memory_order_acquire) &&
           !attempt->cancelled.load(std::memory_order_acquire) &&
           state->active_generation.load(std::memory_order_acquire) == attempt->generation;
}

void terminate_attempt(const std::shared_ptr<detail::EnumerationAttempt> &attempt) {
    attempt->cancelled.store(true, std::memory_order_release);
    // Do not acquire a last owning reference on the caller/UI thread: destruction can wait.
    std::scoped_lock lock(attempt->process_mutex);
    if (attempt->process) {
        attempt->process->terminate();
    }
}

void attach_process(const std::shared_ptr<detail::DirectoryServiceState> &state,
                    const std::shared_ptr<detail::EnumerationAttempt> &attempt,
                    std::shared_ptr<detail::CatalogProcess> process) {
    {
        std::scoped_lock lock(attempt->process_mutex);
        attempt->process = process;
    }
    if (!attempt_is_current(state, attempt)) {
        process->terminate();
    }
}

std::vector<std::shared_ptr<detail::EnumerationAttempt>>
collect_attempts(const std::shared_ptr<detail::DirectoryServiceState> &state,
                 const std::optional<catalog::RequestGeneration> generation = std::nullopt) {
    std::vector<std::shared_ptr<detail::EnumerationAttempt>> result;
    std::scoped_lock lock(state->mutex);
    std::erase_if(state->attempts, [](const auto &candidate) { return candidate.expired(); });
    for (const auto &candidate : state->attempts) {
        if (auto attempt = candidate.lock();
            attempt && (!generation || attempt->generation == *generation)) {
            result.push_back(std::move(attempt));
        }
    }
    return result;
}

void publish_timeout(const std::shared_ptr<detail::DirectoryServiceState> &state,
                     const std::shared_ptr<detail::EnumerationAttempt> &attempt, bool total_timeout);

bool publish_batch(const std::shared_ptr<detail::DirectoryServiceState> &state,
                   const std::shared_ptr<detail::EnumerationAttempt> &attempt,
                   catalog::CatalogBatch batch) {
    std::unique_lock lock(state->mutex);
    if (!attempt_is_current(state, attempt) || attempt->terminal_published) {
        return false;
    }
    if (attempt->recursive && std::chrono::steady_clock::now() >= attempt->deadline) {
        attempt->pending_batch = std::move(batch);
        attempt->timed_out.store(true, std::memory_order_release);
        publish_timeout(state, attempt, true);
        return false;
    }
    attempt->last_progress_ns.store(steady_now_ns(), std::memory_order_release);
    attempt->received_files = attempt->received_files || !batch.entries.empty();
    attempt->directories_visited = std::max(attempt->directories_visited, batch.directories_visited);
    batch.directories_visited = attempt->directories_visited;
    state->changed.notify_all();
    if (attempt->recursive && batch.entries.empty() && !batch.is_final && !batch.error &&
        !batch.truncated) {
        state->progress_batch = std::move(batch);
        return true;
    }
    attempt->pending_batch = std::move(batch);
    state->progress_batch.reset();
    state->changed.wait(lock, [&] {
        return !attempt_is_current(state, attempt) ||
               state->batches.size() < catalog::kCatalogQueueCapacity;
    });
    if (!attempt_is_current(state, attempt)) {
        attempt->pending_batch.reset();
        return false;
    }
    if (attempt->pending_batch->is_final) {
        attempt->terminal_published = true;
        state->progress_batch.reset();
    }
    state->batches.push_back(std::move(*attempt->pending_batch));
    attempt->pending_batch.reset();
    lock.unlock();
    state->changed.notify_all();
    return true;
}

void publish_timeout(const std::shared_ptr<detail::DirectoryServiceState> &state,
                     const std::shared_ptr<detail::EnumerationAttempt> &attempt,
                     const bool total_timeout) {
    // Caller holds state->mutex, making timeout publication atomic with queue and generation changes.
    if (state->active_generation.load(std::memory_order_acquire) != attempt->generation ||
        state->stopping.load(std::memory_order_acquire)) {
        return;
    }

    auto batch = attempt->pending_batch ? std::move(*attempt->pending_batch) : catalog::CatalogBatch{};
    attempt->pending_batch.reset();
    batch.generation = attempt->generation;
    batch.is_final = true;
    batch.truncated = attempt->recursive;
    batch.root_failed = false;
    batch.directories_visited = attempt->directories_visited;
    batch.error = {.kind = catalog::CatalogErrorKind::timed_out,
                   .message_utf8 = total_timeout ? "Recursive catalog total time limit reached" :
                                   "Directory helper timed out after " +
                                   std::to_string(state->inactivity_timeout.count()) +
                                   " milliseconds without progress"};

    // One reserved terminal slot preserves every accepted file even under full backpressure.
    state->terminal_batch = std::move(batch);
    state->progress_batch.reset();
    attempt->terminal_published = true;
    state->changed.notify_all();
}

void monitor_attempt(const std::shared_ptr<detail::DirectoryServiceState> &state,
                     const std::shared_ptr<detail::EnumerationAttempt> &attempt) {
    std::unique_lock lock(state->mutex);
    while (attempt_is_current(state, attempt) && !attempt->done.load(std::memory_order_acquire) &&
           !attempt->terminal_published) {
        const auto observed = attempt->last_progress_ns.load(std::memory_order_acquire);
        const auto inactivity_deadline = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(observed) + state->inactivity_timeout);
        const auto deadline = std::min(inactivity_deadline, attempt->deadline);
        state->changed.wait_until(lock, deadline, [&] {
            return !attempt_is_current(state, attempt) || attempt->terminal_published ||
                   attempt->done.load(std::memory_order_acquire) ||
                   attempt->last_progress_ns.load(std::memory_order_acquire) != observed;
        });
        if (!attempt_is_current(state, attempt) || attempt->terminal_published ||
            attempt->done.load(std::memory_order_acquire)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto total_timeout = now >= attempt->deadline;
        if (!total_timeout && (now < inactivity_deadline ||
            attempt->last_progress_ns.load(std::memory_order_acquire) != observed)) {
            continue;
        }

        bool expected = false;
        if (attempt->timed_out.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            publish_timeout(state, attempt, total_timeout);
            lock.unlock();
            std::shared_ptr<detail::CatalogProcess> process;
            {
                std::scoped_lock process_lock(attempt->process_mutex);
                process = attempt->process;
            }
            if (process) {
                process->terminate();
            }
        }
        return;
    }
}

void finish_worker(const std::shared_ptr<detail::DirectoryServiceState> &state,
                   const std::shared_ptr<detail::EnumerationAttempt> &attempt) {
    attempt->done.store(true, std::memory_order_release);
    std::shared_ptr<detail::CatalogProcess> retired;
    {
        std::scoped_lock process_lock(attempt->process_mutex);
        retired = std::move(attempt->process);
    }
    retired.reset();
    {
        std::scoped_lock lock(state->mutex);
        if (state->active_workers != 0) {
            --state->active_workers;
        }
        std::erase_if(state->attempts, [&](const auto &candidate) {
            const auto value = candidate.lock();
            return !value || value == attempt;
        });
    }
    state->changed.notify_all();
}

void run_request(const std::shared_ptr<detail::DirectoryServiceState> &state,
                 const std::shared_ptr<detail::EnumerationAttempt> &attempt,
                 const catalog::CatalogRequest request) {
    const auto publish_failure = [&](catalog::CatalogError error) noexcept {
        try {
            if (!attempt_is_current(state, attempt)) {
                return;
            }
            catalog::CatalogBatch batch;
            batch.generation = request.generation;
            batch.is_final = true;
            batch.truncated = request.recursive;
            {
                std::scoped_lock lock(state->mutex);
                batch.root_failed = request.recursive && attempt->directories_visited == 0 &&
                                    !attempt->received_files;
                batch.directories_visited = attempt->directories_visited;
            }
            batch.error = std::move(error);
            static_cast<void>(publish_batch(state, attempt, std::move(batch)));
        } catch (...) {
            // Allocation failure must not escape a detached transport thread.
        }
    };

    try {
        catalog::CatalogError process_error;
        auto process = detail::start_catalog_process(state->helper_path, process_error);
        if (!process) {
            publish_failure(std::move(process_error));
            finish_worker(state, attempt);
            return;
        }
        attach_process(state, attempt, process);
        if (!attempt_is_current(state, attempt)) {
            process->terminate();
            std::string ignored;
            static_cast<void>(process->wait(ignored));
            finish_worker(state, attempt);
            return;
        }

        std::string transport_error;
        const auto request_payload = detail::encode_request_payload(request);
        if (!process->write_frame(request_payload, transport_error)) {
            publish_failure({.kind = catalog::CatalogErrorKind::io_error,
                             .message_utf8 = std::move(transport_error)});
            process->terminate();
            std::string ignored;
            static_cast<void>(process->wait(ignored));
            finish_worker(state, attempt);
            return;
        }
        process->close_input();

        bool received_final = false;
        std::size_t received_entries{};
        std::size_t received_metadata_bytes{};
        while (attempt_is_current(state, attempt)) {
            std::vector<std::byte> payload;
            if (!process->read_frame(payload, transport_error,
                    request.recursive ? catalog::kRecursiveCatalogMaximumFrameBytes
                                      : detail::kCatalogMaximumFrameBytes)) {
                publish_failure({.kind = catalog::CatalogErrorKind::io_error,
                                 .message_utf8 = std::move(transport_error)});
                break;
            }
            catalog::CatalogBatch batch;
            if (!detail::decode_batch_payload(payload, batch, transport_error) ||
                batch.generation != request.generation) {
                if (transport_error.empty()) {
                    transport_error = "catalog helper returned the wrong generation";
                }
                publish_failure({.kind = catalog::CatalogErrorKind::io_error,
                                 .message_utf8 = std::move(transport_error)});
                break;
            }
            if (request.recursive) {
                std::size_t accepted{};
                for (const auto &entry : batch.entries) {
                    const auto cost = detail::recursive_metadata_cost(entry);
                    if (entry.kind != core::EntryKind::file ||
                        received_entries >= catalog::recursive_entry_limit(request) ||
                        cost > catalog::kRecursiveCatalogMaximumMetadataBytes - received_metadata_bytes) {
                        break;
                    }
                    ++accepted;
                    ++received_entries;
                    received_metadata_bytes += cost;
                }
                if (accepted != batch.entries.size()) {
                    batch.entries.resize(accepted);
                    batch.is_final = true;
                    batch.truncated = true;
                }
                batch.truncated = batch.truncated || bool(batch.error);
            }
            const auto is_final = batch.is_final;
            if (!publish_batch(state, attempt, std::move(batch))) {
                break;
            }
            if (is_final) {
                received_final = true;
                break;
            }
        }

        process->terminate();
        std::string wait_error;
        const auto exit_code = process->wait(wait_error);
        if (!received_final && attempt_is_current(state, attempt) && exit_code != 0 &&
            !wait_error.empty()) {
            publish_failure({.kind = catalog::CatalogErrorKind::io_error,
                             .message_utf8 = std::move(wait_error),
                             .platform_code = exit_code});
        }
    } catch (const std::exception &exception) {
        publish_failure(
            {.kind = catalog::CatalogErrorKind::io_error, .message_utf8 = exception.what()});
    } catch (...) {
        publish_failure({.kind = catalog::CatalogErrorKind::io_error,
                         .message_utf8 = "Directory helper transport failed"});
    }

    finish_worker(state, attempt);
}

} // namespace

DirectoryService::DirectoryService() : DirectoryService(DirectoryServiceOptions{}) {}

DirectoryService::DirectoryService(DirectoryServiceOptions options)
    : state_(std::make_shared<detail::DirectoryServiceState>()) {
    state_->helper_path = options.helper_path.empty() ? detail::default_catalog_helper_path()
                                                      : std::move(options.helper_path);
    if (options.inactivity_timeout.count() > 0) {
        state_->inactivity_timeout = options.inactivity_timeout;
    }
}

DirectoryService::~DirectoryService() {
    state_->stopping.store(true, std::memory_order_release);
    state_->active_generation.store(0, std::memory_order_release);
    const auto attempts = collect_attempts(state_);
    for (const auto &attempt : attempts) {
        terminate_attempt(attempt);
    }
    state_->changed.notify_all();
    {
        std::unique_lock lock(state_->mutex);
        state_->batches.clear();
        state_->terminal_batch.reset();
        state_->progress_batch.reset();
        static_cast<void>(state_->changed.wait_for(lock, std::chrono::milliseconds(750), [&] {
            return state_->active_workers == 0;
        }));
    }
    state_->changed.notify_all();
}

void DirectoryService::submit(catalog::CatalogRequest request) {
    const auto submitted_at = std::chrono::steady_clock::now();
    state_->active_generation.store(request.generation, std::memory_order_release);
    const auto previous = collect_attempts(state_);
    for (const auto &attempt : previous) {
        terminate_attempt(attempt);
    }

    auto attempt = std::make_shared<detail::EnumerationAttempt>();
    attempt->generation = request.generation;
    attempt->recursive = request.recursive;
    if (request.recursive && catalog::valid_recursive_request(request)) {
        attempt->deadline = submitted_at + request.total_timeout;
        request.maximum_entries = catalog::recursive_entry_limit(request);
    }
    attempt->last_progress_ns.store(steady_now_ns(), std::memory_order_release);

    {
        std::scoped_lock lock(state_->mutex);
        state_->batches.clear();
        state_->terminal_batch.reset();
        state_->progress_batch.reset();
        const auto invalid = !catalog::valid_recursive_request(request);
        if (invalid || state_->active_workers >= catalog::kCatalogMaxConcurrentEnumerations) {
            catalog::CatalogBatch batch;
            batch.generation = request.generation;
            batch.is_final = true;
            batch.truncated = request.recursive;
            batch.root_failed = request.recursive;
            batch.error = {.kind = invalid ? catalog::CatalogErrorKind::io_error
                                          : catalog::CatalogErrorKind::network_disconnected,
                           .message_utf8 = invalid ? "Invalid recursive catalog bounds"
                                                   : "Directory helper cleanup is still in progress"};
            state_->batches.push_back(std::move(batch));
            state_->changed.notify_all();
            return;
        }
        ++state_->active_workers;
        state_->attempts.push_back(attempt);
    }

    try {
        std::thread(monitor_attempt, state_, attempt).detach();
        std::thread(run_request, state_, attempt, std::move(request)).detach();
    } catch (...) {
        attempt->cancelled.store(true, std::memory_order_release);
        catalog::CatalogBatch batch;
        batch.generation = attempt->generation;
        batch.is_final = true;
        batch.truncated = attempt->recursive;
        batch.root_failed = attempt->recursive;
        batch.error = {.kind = catalog::CatalogErrorKind::io_error,
                       .message_utf8 = "Cannot start directory helper transport"};
        {
            std::scoped_lock lock(state_->mutex);
            state_->batches.push_back(std::move(batch));
        }
        finish_worker(state_, attempt);
    }
}

void DirectoryService::cancel(const catalog::RequestGeneration generation) {
    auto expected = generation;
    if (!state_->active_generation.compare_exchange_strong(expected, 0,
                                                           std::memory_order_acq_rel)) {
        return;
    }
    const auto attempts = collect_attempts(state_, generation);
    for (const auto &attempt : attempts) {
        terminate_attempt(attempt);
    }
    {
        std::scoped_lock lock(state_->mutex);
        std::erase_if(state_->batches, [generation](const catalog::CatalogBatch &batch) {
            return batch.generation == generation;
        });
        if (state_->terminal_batch && state_->terminal_batch->generation == generation) {
            state_->terminal_batch.reset();
        }
        if (state_->progress_batch && state_->progress_batch->generation == generation) {
            state_->progress_batch.reset();
        }
    }
    state_->changed.notify_all();
}

std::optional<catalog::CatalogBatch> DirectoryService::poll() {
    std::scoped_lock lock(state_->mutex);
    if (state_->batches.empty()) {
        if (state_->terminal_batch) {
            auto batch = std::move(state_->terminal_batch);
            state_->terminal_batch.reset();
            return batch;
        }
        if (state_->progress_batch) {
            auto batch = std::move(state_->progress_batch);
            state_->progress_batch.reset();
            return batch;
        }
        return std::nullopt;
    }
    auto batch = std::move(state_->batches.front());
    state_->batches.pop_front();
    state_->changed.notify_all();
    return batch;
}

bool DirectoryService::idle() const noexcept {
    std::scoped_lock lock(state_->mutex);
    return state_->active_workers == 0;
}

} // namespace vove::platform
