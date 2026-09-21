#include "vove/fileops/file_transfer_service.hpp"

#include "catalog_process.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vove::fileops {

struct FileTransferService::State {
    std::mutex mutex;
    std::mutex worker_mutex;
    std::mutex completion_mutex;
    std::condition_variable workers_completed;
    std::shared_ptr<platform::detail::CatalogProcess> process;
    std::thread worker;
    std::filesystem::path helper_path;
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(15)};
    std::function<void()> after_operation_slot_released;
    std::atomic_bool complete_accepted_requests_during_stop{false};
    std::atomic_bool busy{false};
    std::atomic_bool stopping{false};
    std::atomic_size_t active_workers{0};
    bool audit_commit_active{};
    bool terminate_after_audit_commit{};
};

namespace {

thread_local FileTransferService::State *active_worker_state{};

constexpr std::uint64_t directoryProgressByteInterval = 64ULL * 1'024ULL * 1'024ULL;

bool valid_directory_progress_transition(const BasicDirectoryTransferPhase previous,
                                         const BasicDirectoryTransferPhase next) noexcept {
    if (previous == next) {
        return previous != BasicDirectoryTransferPhase::completed;
    }
    switch (previous) {
    case BasicDirectoryTransferPhase::enumerating:
        return next == BasicDirectoryTransferPhase::staging_ready ||
               next == BasicDirectoryTransferPhase::publishing ||
               next == BasicDirectoryTransferPhase::retiring_source ||
               next == BasicDirectoryTransferPhase::completed;
    case BasicDirectoryTransferPhase::staging_ready:
        return next == BasicDirectoryTransferPhase::copying ||
               next == BasicDirectoryTransferPhase::verifying;
    case BasicDirectoryTransferPhase::copying:
        return next == BasicDirectoryTransferPhase::verifying;
    case BasicDirectoryTransferPhase::verifying:
        return next == BasicDirectoryTransferPhase::retiring_source ||
               next == BasicDirectoryTransferPhase::publishing;
    case BasicDirectoryTransferPhase::publishing:
        return next == BasicDirectoryTransferPhase::staging_ready ||
               next == BasicDirectoryTransferPhase::completed;
    case BasicDirectoryTransferPhase::retiring_source:
        return next == BasicDirectoryTransferPhase::deleting_source ||
               next == BasicDirectoryTransferPhase::publishing;
    case BasicDirectoryTransferPhase::deleting_source:
        return next == BasicDirectoryTransferPhase::publishing;
    case BasicDirectoryTransferPhase::completed:
        return false;
    }
    return false;
}

std::size_t
directory_progress_frame_limit(const BasicDirectoryTransferProgress &progress) noexcept {
    constexpr auto reserve = std::size_t{36'000};
    const auto entry_frames =
        progress.total_entries > (std::numeric_limits<std::size_t>::max() - reserve) / 4U
            ? std::numeric_limits<std::size_t>::max()
            : progress.total_entries * 4U + reserve;
    const auto byte_frames = progress.total_bytes / directoryProgressByteInterval +
                             (progress.total_bytes % directoryProgressByteInterval != 0U ? 1U : 0U);
    if (byte_frames > std::numeric_limits<std::size_t>::max() - entry_frames) {
        return std::numeric_limits<std::size_t>::max();
    }
    return entry_frames + static_cast<std::size_t>(byte_frames);
}

class WorkerGuard final {
  public:
    explicit WorkerGuard(std::shared_ptr<FileTransferService::State> state)
        : state_(std::move(state)) {
        active_worker_state = state_.get();
    }

    ~WorkerGuard() {
        if (!slot_released_) {
            state_->busy.store(false, std::memory_order_release);
        }
        active_worker_state = nullptr;
        state_->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        state_->workers_completed.notify_all();
    }

    WorkerGuard(const WorkerGuard &) = delete;
    WorkerGuard &operator=(const WorkerGuard &) = delete;

    void release_slot() noexcept {
        state_->busy.store(false, std::memory_order_release);
        slot_released_ = true;
    }

  private:
    std::shared_ptr<FileTransferService::State> state_;
    bool slot_released_{};
};

class AuditCommitGuard final {
  public:
    AuditCommitGuard(std::shared_ptr<FileTransferService::State> state,
                     std::shared_ptr<platform::detail::CatalogProcess> process, const bool required)
        : state_(std::move(state)), process_(std::move(process)) {
        if (!required) {
            return;
        }
        std::scoped_lock lock(state_->mutex);
        if (state_->stopping.load(std::memory_order_acquire) || state_->process != process_) {
            return;
        }
        state_->audit_commit_active = true;
        active_ = true;
    }

    ~AuditCommitGuard() {
        release();
    }

    AuditCommitGuard(const AuditCommitGuard &) = delete;
    AuditCommitGuard &operator=(const AuditCommitGuard &) = delete;

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    void release() noexcept {
        if (!active_) {
            return;
        }
        std::shared_ptr<platform::detail::CatalogProcess> deferred_termination;
        {
            std::scoped_lock lock(state_->mutex);
            state_->audit_commit_active = false;
            if (state_->terminate_after_audit_commit && state_->process == process_) {
                deferred_termination = process_;
            }
            state_->terminate_after_audit_commit = false;
        }
        active_ = false;
        if (deferred_termination) {
            deferred_termination->terminate();
        }
    }

  private:
    std::shared_ptr<FileTransferService::State> state_;
    std::shared_ptr<platform::detail::CatalogProcess> process_;
    bool active_{};
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

bool same_address(const FileTransferStreamRequest &request, const std::uint64_t operation_id,
                  const std::uint32_t item_index,
                  const std::array<std::uint8_t, kFileTransferRequestTokenBytes> &token) noexcept {
    return operation_id == request.operation_id && item_index == request.item_index &&
           token == request.request_token;
}

FileTransferStreamResult make_result(const FileTransferStreamRequest &request,
                                     const OperationStatus status, const OperationEvidence evidence,
                                     std::string detail) {
    FileTransferStreamResult result;
    result.operation_id = request.operation_id;
    result.item_index = request.item_index;
    result.request_token = request.request_token;
    result.status = status;
    result.evidence = evidence;
    result.detail_utf8 = std::move(detail);
    return result;
}

bool reservation_matches_request(const FileTransferStreamRequest &request,
                                 const FileTransferReservation &reservation, std::string &detail) {
    if (request.mode == FileTransferStreamMode::probe_reservation) {
        if (request.source_parent_identity_utf8 != reservation.destination_parent_identity_utf8 ||
            stable_object_identity(reservation.destination_parent_revision_utf8) !=
                request.source_parent_identity_utf8) {
            detail = "reservation probe names another source directory";
            return false;
        }
        return true;
    }
    if (request.mode == FileTransferStreamMode::audit_existing) {
        if (request.source_parent_identity_utf8 != reservation.destination_parent_identity_utf8 ||
            reservation.temp_snapshot.size_bytes != request.expected_source.size_bytes ||
            reservation.temp_snapshot.modified_unix_ns !=
                request.expected_source.modified_unix_ns ||
            !same_source_revision(reservation.temp_snapshot.source_revision_utf8,
                                  request.expected_source.source_revision_utf8)) {
            detail = "file-audit reservation no longer matches the published file";
            return false;
        }
        return true;
    }
    if (request.destination_parent_identity_utf8 != reservation.destination_parent_identity_utf8 ||
        stable_object_identity(reservation.destination_parent_revision_utf8) !=
            request.destination_parent_identity_utf8) {
        detail = "file-transfer reservation names another destination directory";
        return false;
    }
    if (same_object_identity(request.expected_source.source_revision_utf8,
                             reservation.temp_snapshot.source_revision_utf8)) {
        detail = "file-transfer reservation aliases the source object";
        return false;
    }
    if (request.mode == FileTransferStreamMode::reserve_new) {
        if (reservation.temp_snapshot.size_bytes != kFileTransferReservationMarkerBytes) {
            detail = "new file-transfer reservation has no ownership marker";
            return false;
        }
        return true;
    }
    if (reservation.temp_snapshot.size_bytes != request.expected_temp.size_bytes ||
        reservation.temp_snapshot.modified_unix_ns != request.expected_temp.modified_unix_ns ||
        !same_source_revision(reservation.temp_snapshot.source_revision_utf8,
                              request.expected_temp.source_revision_utf8)) {
        detail = "resumed file-transfer reservation changed identity";
        return false;
    }
    return true;
}

bool successful_result_matches(const FileTransferStreamRequest &request,
                               const FileTransferReservation &reservation,
                               const FileTransferStreamResult &result) {
    if (request.mode == FileTransferStreamMode::probe_reservation) {
        return result.evidence == OperationEvidence::committed &&
               result.bytes_written == reservation.temp_snapshot.size_bytes &&
               result.source_snapshot.size_bytes == result.bytes_written &&
               result.source_snapshot.modified_unix_ns ==
                   reservation.temp_snapshot.modified_unix_ns &&
               same_source_revision(result.source_snapshot.source_revision_utf8,
                                    reservation.temp_snapshot.source_revision_utf8) &&
               result.temp_snapshot.size_bytes == result.source_snapshot.size_bytes &&
               result.temp_snapshot.modified_unix_ns == result.source_snapshot.modified_unix_ns &&
               same_source_revision(result.temp_snapshot.source_revision_utf8,
                                    result.source_snapshot.source_revision_utf8);
    }
    if (request.mode == FileTransferStreamMode::audit_existing) {
        return result.evidence == OperationEvidence::committed &&
               result.bytes_written == request.expected_source.size_bytes &&
               result.source_snapshot.size_bytes == request.expected_source.size_bytes &&
               result.source_snapshot.modified_unix_ns ==
                   request.expected_source.modified_unix_ns &&
               same_source_revision(result.source_snapshot.source_revision_utf8,
                                    request.expected_source.source_revision_utf8) &&
               result.temp_snapshot.size_bytes == result.source_snapshot.size_bytes &&
               result.temp_snapshot.modified_unix_ns == result.source_snapshot.modified_unix_ns &&
               same_source_revision(result.temp_snapshot.source_revision_utf8,
                                    result.source_snapshot.source_revision_utf8) &&
               reservation.temp_snapshot.size_bytes == result.source_snapshot.size_bytes &&
               same_source_revision(reservation.temp_snapshot.source_revision_utf8,
                                    result.source_snapshot.source_revision_utf8);
    }
    const auto source_matches_request =
        (result.source_snapshot.modified_unix_ns == request.expected_source.modified_unix_ns &&
         same_source_revision(result.source_snapshot.source_revision_utf8,
                              request.expected_source.source_revision_utf8)) ||
        same_object_after_rename(request.expected_source, result.source_snapshot);
    return result.evidence == OperationEvidence::committed &&
           result.bytes_written == request.expected_source.size_bytes &&
           result.source_snapshot.size_bytes == request.expected_source.size_bytes &&
           source_matches_request &&
           result.temp_snapshot.size_bytes == request.expected_source.size_bytes &&
           same_object_identity(result.temp_snapshot.source_revision_utf8,
                                reservation.temp_snapshot.source_revision_utf8);
}

bool valid_pre_reservation_failure(const FileTransferStreamResult &result) {
    const auto empty_snapshot = [](const SourceSnapshot &snapshot) {
        return snapshot.size_bytes == 0 && snapshot.modified_unix_ns == 0 &&
               snapshot.source_revision_utf8.empty();
    };
    return !result.ok() && result.evidence == OperationEvidence::no_commit &&
           result.bytes_written == 0 && empty_snapshot(result.source_snapshot) &&
           empty_snapshot(result.temp_snapshot) &&
           std::ranges::all_of(result.content_sha256,
                               [](const std::uint8_t byte) { return byte == 0; });
}

class IdleWatchdog final {
  public:
    IdleWatchdog(std::shared_ptr<platform::detail::CatalogProcess> process,
                 const std::chrono::milliseconds timeout, std::atomic_bool &timedOut)
        : process_(std::move(process)), timeout_(timeout), timed_out_(timedOut),
          deadline_(std::chrono::steady_clock::now() + timeout_), thread_([this] { run(); }) {}

    ~IdleWatchdog() {
        stop();
    }

    IdleWatchdog(const IdleWatchdog &) = delete;
    IdleWatchdog &operator=(const IdleWatchdog &) = delete;

    void touch() {
        std::scoped_lock lock(mutex_);
        deadline_ = std::chrono::steady_clock::now() + timeout_;
        ++generation_;
        changed_.notify_all();
    }

    void pause() {
        std::scoped_lock lock(mutex_);
        armed_ = false;
        ++generation_;
        changed_.notify_all();
    }

    void resume() {
        std::scoped_lock lock(mutex_);
        deadline_ = std::chrono::steady_clock::now() + timeout_;
        armed_ = true;
        ++generation_;
        changed_.notify_all();
    }

    void stop() noexcept {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        changed_.notify_all();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
    }

  private:
    void run() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            if (!armed_) {
                const auto observed = generation_;
                changed_.wait(lock,
                              [this, observed] { return stopping_ || generation_ != observed; });
                continue;
            }
            const auto observed = generation_;
            const auto deadline = deadline_;
            if (changed_.wait_until(lock, deadline, [this, observed] {
                    return stopping_ || !armed_ || generation_ != observed;
                })) {
                continue;
            }
            timed_out_.store(true, std::memory_order_release);
            armed_ = false;
            lock.unlock();
            process_->terminate();
            return;
        }
    }

    std::shared_ptr<platform::detail::CatalogProcess> process_;
    std::chrono::milliseconds timeout_;
    std::atomic_bool &timed_out_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t generation_{};
    bool armed_{true};
    bool stopping_{};
    std::thread thread_;
};

FileTransferStreamResult
run_helper(const std::shared_ptr<FileTransferService::State> &state,
           const FileTransferStreamRequest &request,
           const FileTransferService::ReservationCommit &commit_reservation,
           const FileTransferService::AuditCommit &commit_audit,
           const FileTransferService::Progress &publish_progress) {
    if (state->stopping.load(std::memory_order_acquire)) {
        return make_result(request, OperationStatus::io_error, OperationEvidence::no_commit,
                           "file-transfer service is stopping");
    }
    catalog::CatalogError launch_error;
    auto process = platform::detail::start_catalog_process(state->helper_path, launch_error);
    if (!process) {
        auto result = make_result(request, OperationStatus::unknown_outcome,
                                  OperationEvidence::none, launch_error.message_utf8);
        result.platform_code = launch_error.platform_code;
        return result;
    }
    {
        std::scoped_lock lock(state->mutex);
        state->process = process;
    }
    if (state->stopping.load(std::memory_order_acquire)) {
        process->terminate();
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        std::scoped_lock lock(state->mutex);
        if (state->process == process) {
            state->process.reset();
        }
        return make_result(request, OperationStatus::io_error, OperationEvidence::none,
                           "file-transfer service stopped during helper launch");
    }

    std::atomic_bool timed_out{false};
    IdleWatchdog watchdog(process, state->idle_timeout, timed_out);
    const auto clear_process = [&] {
        watchdog.stop();
        std::scoped_lock lock(state->mutex);
        if (state->process == process) {
            state->process.reset();
        }
    };
    const auto finish_process = [&] {
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        clear_process();
    };

    std::string transport_error;
    try {
        if (!process->write_frame(encode_file_transfer_stream_request(request), transport_error)) {
            process->terminate();
            finish_process();
            return make_result(request,
                               timed_out.load(std::memory_order_acquire)
                                   ? OperationStatus::timed_out
                                   : OperationStatus::unknown_outcome,
                               OperationEvidence::none, std::move(transport_error));
        }
        watchdog.touch();
    } catch (const std::exception &error) {
        process->terminate();
        finish_process();
        return make_result(request, OperationStatus::invalid_request, OperationEvidence::no_commit,
                           error.what());
    }

    std::vector<std::byte> response;
    if (!process->read_frame(response, transport_error, kMaximumFileTransferProtocolBytes)) {
        process->terminate();
        finish_process();
        return make_result(request,
                           timed_out.load(std::memory_order_acquire)
                               ? OperationStatus::timed_out
                               : OperationStatus::unknown_outcome,
                           OperationEvidence::none, std::move(transport_error));
    }
    watchdog.touch();
    FileTransferMessageKind first_kind{};
    if (!decode_file_transfer_message_kind(response, first_kind, transport_error)) {
        process->terminate();
        finish_process();
        return make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                           "file-transfer helper emitted an invalid first frame");
    }
    if (first_kind == FileTransferMessageKind::complete) {
        FileTransferStreamResult result;
        if (!decode_file_transfer_stream_result(response, result, transport_error) ||
            !same_address(request, result.operation_id, result.item_index, result.request_token) ||
            !valid_pre_reservation_failure(result)) {
            process->terminate();
            finish_process();
            return make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                               "file-transfer helper failure is misaddressed or invalid");
        }
        process->close_input();
        finish_process();
        return result;
    }
    if (first_kind != FileTransferMessageKind::reservation) {
        process->terminate();
        finish_process();
        return make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                           "file-transfer helper skipped temporary reservation");
    }

    FileTransferReservation reservation;
    if (!decode_file_transfer_reservation(response, reservation, transport_error) ||
        !same_address(request, reservation.operation_id, reservation.item_index,
                      reservation.request_token)) {
        process->terminate();
        finish_process();
        return make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                           "file-transfer reservation is misaddressed or invalid");
    }
    if (!reservation_matches_request(request, reservation, transport_error)) {
        process->terminate();
        finish_process();
        return make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                           std::move(transport_error));
    }

    watchdog.pause();
    bool accepted{};
    std::string commit_detail;
    try {
        accepted = commit_reservation(reservation, commit_detail);
    } catch (const std::exception &error) {
        commit_detail = error.what();
    } catch (...) {
        commit_detail = "reservation commit failed with an unknown exception";
    }
    watchdog.resume();

    FileTransferReservationAck acknowledgement;
    acknowledgement.operation_id = request.operation_id;
    acknowledgement.item_index = request.item_index;
    acknowledgement.request_token = request.request_token;
    acknowledgement.accepted = accepted;
    try {
        if (!process->write_frame(encode_file_transfer_reservation_ack(acknowledgement),
                                  transport_error)) {
            process->terminate();
            finish_process();
            auto result =
                make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                            "reservation acknowledgement was not delivered");
            result.temp_snapshot = reservation.temp_snapshot;
            return result;
        }
    } catch (const std::exception &error) {
        process->terminate();
        finish_process();
        auto result = make_result(request, OperationStatus::unknown_outcome,
                                  OperationEvidence::none, error.what());
        result.temp_snapshot = reservation.temp_snapshot;
        return result;
    }
    if (request.mode != FileTransferStreamMode::audit_existing) {
        process->close_input();
    }
    watchdog.touch();
    if (!accepted) {
        finish_process();
        return make_result(request, OperationStatus::io_error, OperationEvidence::no_commit,
                           commit_detail.empty() ? "temporary reservation was not committed"
                                                 : std::move(commit_detail));
    }

    std::uint64_t previous_progress{};
    std::uint64_t progress_frames{};
    const auto expected_stream_bytes = request.mode == FileTransferStreamMode::probe_reservation
                                           ? reservation.temp_snapshot.size_bytes
                                           : request.expected_source.size_bytes;
    const auto maximum_progress_frames =
        expected_stream_bytes == 0
            ? std::uint64_t{0}
            : ((expected_stream_bytes - 1U) / kFileTransferProgressChunkBytes) + 1U;
    for (;;) {
        response.clear();
        if (!process->read_frame(response, transport_error, kMaximumFileTransferProtocolBytes)) {
            process->terminate();
            finish_process();
            auto result = make_result(request,
                                      timed_out.load(std::memory_order_acquire)
                                          ? OperationStatus::timed_out
                                          : OperationStatus::unknown_outcome,
                                      OperationEvidence::none, std::move(transport_error));
            result.temp_snapshot = reservation.temp_snapshot;
            return result;
        }
        watchdog.touch();
        FileTransferMessageKind kind{};
        if (!decode_file_transfer_message_kind(response, kind, transport_error)) {
            process->terminate();
            finish_process();
            auto result =
                make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                            "file-transfer helper emitted an invalid frame");
            result.temp_snapshot = reservation.temp_snapshot;
            return result;
        }
        if (kind == FileTransferMessageKind::progress) {
            FileTransferProgress progress;
            if (!decode_file_transfer_progress(response, progress, transport_error) ||
                !same_address(request, progress.operation_id, progress.item_index,
                              progress.request_token) ||
                progress.bytes_written <= previous_progress ||
                progress.bytes_written > expected_stream_bytes ||
                ++progress_frames > maximum_progress_frames) {
                process->terminate();
                finish_process();
                auto result =
                    make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                                "file-transfer progress is invalid or regressed");
                result.temp_snapshot = reservation.temp_snapshot;
                return result;
            }
            previous_progress = progress.bytes_written;
            if (publish_progress) {
                watchdog.pause();
                try {
                    publish_progress(progress);
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // UI progress cannot change the durable transfer outcome.
                }
                watchdog.resume();
            }
            continue;
        }
        if (kind != FileTransferMessageKind::complete) {
            process->terminate();
            finish_process();
            auto result =
                make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                            "file-transfer helper emitted an unexpected frame");
            result.temp_snapshot = reservation.temp_snapshot;
            return result;
        }
        FileTransferStreamResult result;
        if (!decode_file_transfer_stream_result(response, result, transport_error) ||
            !same_address(request, result.operation_id, result.item_index, result.request_token) ||
            (result.ok() && !successful_result_matches(request, reservation, result))) {
            process->terminate();
            finish_process();
            auto invalid =
                make_result(request, OperationStatus::unknown_outcome, OperationEvidence::none,
                            "file-transfer result is invalid or misaddressed");
            invalid.temp_snapshot = reservation.temp_snapshot;
            return invalid;
        }
        if (request.mode == FileTransferStreamMode::audit_existing) {
            AuditCommitGuard commit_guard(state, process, result.ok());
            if (result.ok() && !commit_guard.active()) {
                process->terminate();
                finish_process();
                result.status = OperationStatus::unknown_outcome;
                result.evidence = OperationEvidence::none;
                result.detail_utf8 = "file-audit stopped before its durable commit";
                return result;
            }
            auto outcome = FileTransferAuditCommitOutcome::recovery_required;
            std::string commit_detail;
            if (result.ok()) {
                watchdog.pause();
                try {
                    outcome = commit_audit ? commit_audit(result, commit_detail)
                                           : FileTransferAuditCommitOutcome::recovery_required;
                } catch (const std::exception &error) {
                    commit_detail = error.what();
                } catch (...) {
                    commit_detail = "file-audit commit failed unexpectedly";
                }
                watchdog.resume();
            }
            const auto accepted = outcome == FileTransferAuditCommitOutcome::accepted;
            if (result.ok() && !accepted) {
                if (outcome == FileTransferAuditCommitOutcome::rejected_no_commit) {
                    result.status = OperationStatus::io_error;
                    result.evidence = OperationEvidence::no_commit;
                } else {
                    result.status = OperationStatus::unknown_outcome;
                    result.evidence = OperationEvidence::none;
                }
                result.detail_utf8 =
                    commit_detail.empty()
                        ? (outcome == FileTransferAuditCommitOutcome::rejected_no_commit
                               ? "file-audit durable commit was rejected"
                               : "file-audit durable commit requires recovery")
                        : commit_detail;
            }
            FileTransferReservationAck acknowledgement{
                .operation_id = request.operation_id,
                .item_index = request.item_index,
                .request_token = request.request_token,
                .accepted = accepted,
            };
            try {
                if (!process->write_frame(encode_file_transfer_reservation_ack(acknowledgement),
                                          transport_error)) {
                    process->terminate();
                    finish_process();
                    return result;
                }
            } catch (const std::exception &) {
                process->terminate();
                finish_process();
                return result;
            }
            process->close_input();
            commit_guard.release();
        }
        finish_process();
        return result;
    }
}

BasicDirectoryTransferResult
make_directory_result(const BasicDirectoryTransferStreamRequest &request,
                      const BasicDirectoryTransferStatus status, std::string detail) {
    BasicDirectoryTransferResult result;
    result.status = status;
    result.destination = request.destination;
    result.manifest_path = request.manifest_path;
    result.detail_utf8 = std::move(detail);
    return result;
}

BasicDirectoryTransferResult
run_directory_helper(const std::shared_ptr<FileTransferService::State> &state,
                     const BasicDirectoryTransferStreamRequest &request,
                     const FileTransferService::DirectoryProgress &publish_progress) {
    if (state->stopping.load(std::memory_order_acquire)) {
        auto result = make_directory_result(request, BasicDirectoryTransferStatus::io_error,
                                            "directory-transfer service is stopping");
        result.recovery_available = true;
        return result;
    }
    catalog::CatalogError launch_error;
    auto process = platform::detail::start_catalog_process(state->helper_path, launch_error);
    if (!process) {
        auto result = make_directory_result(request, BasicDirectoryTransferStatus::unknown_outcome,
                                            launch_error.message_utf8);
        result.platform_code = launch_error.platform_code;
        result.recovery_available = true;
        return result;
    }
    {
        std::scoped_lock lock(state->mutex);
        state->process = process;
    }
    if (state->stopping.load(std::memory_order_acquire)) {
        process->terminate();
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        std::scoped_lock lock(state->mutex);
        if (state->process == process) {
            state->process.reset();
        }
        auto result =
            make_directory_result(request, BasicDirectoryTransferStatus::io_error,
                                  "directory-transfer service stopped during helper launch");
        result.recovery_available = true;
        return result;
    }

    std::atomic_bool timed_out{false};
    IdleWatchdog watchdog(process, state->idle_timeout, timed_out);
    const auto clear_process = [&] {
        watchdog.stop();
        std::scoped_lock lock(state->mutex);
        if (state->process == process) {
            state->process.reset();
        }
    };
    const auto finish_process = [&] {
        std::string ignored;
        static_cast<void>(process->wait(ignored));
        clear_process();
    };
    bool completion_evidence_received{};
    const auto transport_failure = [&](std::string detail) {
        auto result = make_directory_result(request,
                                            timed_out.load(std::memory_order_acquire)
                                                ? BasicDirectoryTransferStatus::timed_out
                                                : BasicDirectoryTransferStatus::unknown_outcome,
                                            std::move(detail));
        result.recovery_available = true;
        if (completion_evidence_received) {
            result.detail_utf8 =
                "directory operation completed; hidden recovery cleanup still requires "
                "confirmation";
        }
        return result;
    };

    std::string transport_error;
    try {
        if (!process->write_frame(encode_basic_directory_transfer_stream_request(request),
                                  transport_error)) {
            process->terminate();
            finish_process();
            return transport_failure(std::move(transport_error));
        }
        watchdog.touch();
    } catch (const std::exception &error) {
        process->terminate();
        finish_process();
        return make_directory_result(request, BasicDirectoryTransferStatus::invalid_request,
                                     error.what());
    }

    std::size_t progress_frames{};
    BasicDirectoryTransferPhase previous_phase{BasicDirectoryTransferPhase::enumerating};
    std::size_t previous_entries{};
    std::uint64_t previous_bytes{};
    std::size_t expected_total_entries{};
    std::uint64_t expected_total_bytes{};
    std::optional<BasicDirectoryTransferProgress> previous_progress;
    for (;;) {
        std::vector<std::byte> response;
        if (!process->read_frame(response, transport_error,
                                 kMaximumBasicDirectoryTransferProtocolBytes)) {
            process->terminate();
            finish_process();
            return transport_failure(std::move(transport_error));
        }
        watchdog.touch();
        BasicDirectoryTransferMessageKind kind{};
        if (!decode_basic_directory_transfer_message_kind(response, kind, transport_error)) {
            process->terminate();
            finish_process();
            auto result =
                make_directory_result(request, BasicDirectoryTransferStatus::unknown_outcome,
                                      "directory-transfer helper emitted an invalid frame");
            result.recovery_available = true;
            return result;
        }
        if (kind == BasicDirectoryTransferMessageKind::progress) {
            BasicDirectoryTransferProgressFrame frame;
            const auto decoded =
                decode_basic_directory_transfer_progress_frame(response, frame, transport_error);
            if (decoded && expected_total_entries == 0U && frame.progress.total_entries != 0U) {
                expected_total_entries = frame.progress.total_entries;
                expected_total_bytes = frame.progress.total_bytes;
            }
            const auto totals_changed = decoded && expected_total_entries != 0U &&
                                        (frame.progress.total_entries != expected_total_entries ||
                                         frame.progress.total_bytes != expected_total_bytes);
            const auto maximum_frames =
                decoded ? directory_progress_frame_limit(frame.progress) : 0U;
            const auto heartbeat =
                decoded && previous_progress &&
                is_basic_directory_transfer_heartbeat(*previous_progress, frame.progress);
            if (!decoded || frame.request_id != request.request_id ||
                !valid_directory_progress_transition(previous_phase, frame.progress.phase) ||
                frame.progress.completed_entries < previous_entries ||
                frame.progress.completed_bytes < previous_bytes || totals_changed ||
                (!heartbeat && ++progress_frames > maximum_frames)) {
                process->terminate();
                finish_process();
                auto result = make_directory_result(
                    request, BasicDirectoryTransferStatus::unknown_outcome,
                    "directory-transfer helper progress is invalid or regressed");
                result.recovery_available = true;
                return result;
            }
            previous_phase = frame.progress.phase;
            previous_entries = frame.progress.completed_entries;
            previous_bytes = frame.progress.completed_bytes;
            previous_progress = frame.progress;
            bool progress_accepted = true;
            if (publish_progress) {
                watchdog.pause();
                try {
                    publish_progress(frame.progress);
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    progress_accepted = false;
                }
                watchdog.resume();
            }
            if (!progress_accepted &&
                frame.progress.phase == BasicDirectoryTransferPhase::completed) {
                process->terminate();
                finish_process();
                return transport_failure(
                    "directory completion evidence could not be persisted by the client");
            }
            if (frame.progress.phase == BasicDirectoryTransferPhase::completed) {
                completion_evidence_received = true;
                try {
                    if (!process->write_frame(encode_basic_directory_transfer_completion_ack(
                                                  {.request_id = request.request_id}),
                                              transport_error)) {
                        process->terminate();
                        finish_process();
                        return transport_failure(std::move(transport_error));
                    }
                    process->close_input();
                    watchdog.touch();
                } catch (const std::exception &error) {
                    process->terminate();
                    finish_process();
                    return transport_failure(error.what());
                }
            }
            continue;
        }
        BasicDirectoryTransferResultFrame frame;
        if (!decode_basic_directory_transfer_result_frame(response, frame, transport_error) ||
            frame.request_id != request.request_id ||
            frame.result.completed_entries < previous_entries ||
            frame.result.completed_bytes < previous_bytes ||
            (frame.result.ok() && !completion_evidence_received)) {
            process->terminate();
            finish_process();
            auto result = make_directory_result(
                request, BasicDirectoryTransferStatus::unknown_outcome,
                "directory-transfer helper result is invalid or misaddressed");
            result.recovery_available = true;
            return result;
        }
        finish_process();
        return std::move(frame.result);
    }
}

} // namespace

FileTransferService::FileTransferService(FileTransferServiceOptions options)
    : state_(std::make_shared<State>()) {
    state_->helper_path =
        options.helper_path.empty() ? default_helper_path() : std::move(options.helper_path);
    if (options.idle_timeout.count() > 0) {
        state_->idle_timeout = options.idle_timeout;
    }
    state_->after_operation_slot_released = std::move(options.after_operation_slot_released);
}

FileTransferService::~FileTransferService() {
    stop();
}

bool FileTransferService::submit(FileTransferStreamRequest request,
                                 ReservationCommit commit_reservation, Progress progress,
                                 Completion completion) {
    if (request.mode == FileTransferStreamMode::audit_existing) {
        return false;
    }
    return submit_impl(std::move(request), std::move(commit_reservation), {}, std::move(progress),
                       std::move(completion));
}

bool FileTransferService::submit_audit(FileTransferStreamRequest request, AuditCommit commit_audit,
                                       Progress progress, Completion completion) {
    if (request.mode != FileTransferStreamMode::audit_existing) {
        return false;
    }
    const auto accept_reservation = [](const FileTransferReservation &, std::string &) {
        return true;
    };
    return submit_impl(std::move(request), accept_reservation, std::move(commit_audit),
                       std::move(progress), std::move(completion));
}

bool FileTransferService::submit_directory(BasicDirectoryTransferStreamRequest request,
                                           DirectoryProgress progress,
                                           DirectoryCompletion completion) {
    bool expected{};
    std::string validation;
    if (!progress || !completion || state_->helper_path.empty() ||
        !valid_basic_directory_transfer_stream_request(request, validation) ||
        state_->stopping.load(std::memory_order_acquire) ||
        !state_->busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    std::scoped_lock worker_lock(state_->worker_mutex);
    if (state_->stopping.load(std::memory_order_acquire)) {
        state_->busy.store(false, std::memory_order_release);
        return false;
    }
    if (state_->worker.joinable()) {
        if (state_->worker.get_id() == std::this_thread::get_id()) {
            state_->worker.detach();
        } else {
            state_->worker.join();
        }
    }
    state_->active_workers.fetch_add(1, std::memory_order_acq_rel);
    try {
        state_->worker = std::thread([state = state_, request = std::move(request),
                                      progress = std::move(progress),
                                      completion = std::move(completion)]() mutable {
            WorkerGuard worker(state);
            auto result = make_directory_result(request, BasicDirectoryTransferStatus::io_error,
                                                "directory-transfer service failed unexpectedly");
            try {
                result = run_directory_helper(state, request, progress);
            } catch (const std::exception &error) {
                result = make_directory_result(request, BasicDirectoryTransferStatus::io_error,
                                               error.what());
            } catch (...) {
                result = make_directory_result(
                    request, BasicDirectoryTransferStatus::io_error,
                    "directory-transfer service failed with an unknown exception");
            }
            worker.release_slot();
            if (state->after_operation_slot_released) {
                try {
                    state->after_operation_slot_released();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Diagnostic hooks cannot change delivery.
                }
            }
            if (state->complete_accepted_requests_during_stop.load(std::memory_order_acquire) ||
                !state->stopping.load(std::memory_order_acquire)) {
                try {
                    completion(std::move(result));
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Application callbacks cannot terminate the transfer worker.
                }
            }
        });
    } catch (...) {
        state_->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        state_->workers_completed.notify_all();
        state_->busy.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool FileTransferService::submit_impl(FileTransferStreamRequest request,
                                      ReservationCommit commit_reservation,
                                      AuditCommit commit_audit, Progress progress,
                                      Completion completion) {
    bool expected{};
    const auto audit = request.mode == FileTransferStreamMode::audit_existing;
    if (!commit_reservation || (audit && !commit_audit) || !completion ||
        state_->helper_path.empty() || state_->stopping.load(std::memory_order_acquire) ||
        !state_->busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    std::scoped_lock worker_lock(state_->worker_mutex);
    if (state_->stopping.load(std::memory_order_acquire)) {
        state_->busy.store(false, std::memory_order_release);
        return false;
    }
    if (state_->worker.joinable()) {
        if (state_->worker.get_id() == std::this_thread::get_id()) {
            state_->worker.detach();
        } else {
            state_->worker.join();
        }
    }
    state_->active_workers.fetch_add(1, std::memory_order_acq_rel);
    try {
        state_->worker = std::thread([state = state_, request = std::move(request),
                                      commit_reservation = std::move(commit_reservation),
                                      commit_audit = std::move(commit_audit),
                                      progress = std::move(progress),
                                      completion = std::move(completion)]() mutable {
            WorkerGuard worker(state);
            auto result = make_result(request, OperationStatus::io_error, OperationEvidence::none,
                                      "file-transfer service failed unexpectedly");
            try {
                std::string validation;
                result =
                    valid_file_transfer_stream_request(request, validation)
                        ? run_helper(state, request, commit_reservation, commit_audit, progress)
                        : make_result(request, OperationStatus::invalid_request,
                                      OperationEvidence::no_commit, std::move(validation));
            } catch (const std::exception &error) {
                result = make_result(request, OperationStatus::io_error, OperationEvidence::none,
                                     error.what());
            } catch (...) {
                result = make_result(request, OperationStatus::io_error, OperationEvidence::none,
                                     "file-transfer service failed with an unknown exception");
            }
            worker.release_slot();
            if (state->after_operation_slot_released) {
                try {
                    state->after_operation_slot_released();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Diagnostic hooks cannot change delivery.
                }
            }
            if (state->complete_accepted_requests_during_stop.load(std::memory_order_acquire) ||
                !state->stopping.load(std::memory_order_acquire)) {
                try {
                    completion(std::move(result));
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Application callbacks cannot terminate the transfer worker.
                }
            }
        });
    } catch (...) {
        state_->busy.store(false, std::memory_order_release);
        state_->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        state_->workers_completed.notify_all();
        return false;
    }
    return true;
}

bool FileTransferService::busy() const noexcept {
    return state_->busy.load(std::memory_order_acquire);
}

bool FileTransferService::stopping() const noexcept {
    return state_->stopping.load(std::memory_order_acquire);
}

void FileTransferService::retain_accepted_completions_during_stop() noexcept {
    state_->complete_accepted_requests_during_stop.store(true, std::memory_order_release);
}

void FileTransferService::stop() noexcept {
    state_->stopping.store(true, std::memory_order_release);
    std::shared_ptr<platform::detail::CatalogProcess> process;
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->audit_commit_active) {
            state_->terminate_after_audit_commit = true;
        } else {
            process = state_->process;
        }
    }
    if (process) {
        process->terminate();
    }
    std::thread worker;
    {
        std::scoped_lock lock(state_->worker_mutex);
        worker = std::move(state_->worker);
    }
    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) {
            // A completion may release its owning service. The worker owns State independently and
            // touches no FileTransferService members after the callback returns.
            worker.detach();
        } else {
            worker.join();
        }
    }
    if (active_worker_state != state_.get()) {
        std::unique_lock lock(state_->completion_mutex);
        state_->workers_completed.wait(lock, [state = state_] {
            return state->active_workers.load(std::memory_order_acquire) == 0;
        });
    }
}

} // namespace vove::fileops
