#include "catalog_protocol.hpp"
#include "file_operation_executor.hpp"
#include "file_transfer_executor.hpp"
#include "vove/core/reserved_names.hpp"
#include "vove/fileops/basic_directory_transfer.hpp"
#include "vove/fileops/basic_directory_transfer_protocol.hpp"
#include "vove/fileops/file_operation_protocol.hpp"
#include "vove/fileops/file_transfer_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace {

void use_binary_stdio() {
#ifdef _WIN32
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
#endif
}

bool arm_parent_death() {
#ifdef __linux__
    return prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() != 1;
#else
    return true;
#endif
}

class DirectoryProgressChannel final {
  public:
    explicit DirectoryProgressChannel(
        const vove::fileops::BasicDirectoryTransferStreamRequest &request)
        : request_(request),
          latest_(
              {.phase = vove::fileops::BasicDirectoryTransferPhase::enumerating,
               .current_path = request.source.empty() ? request.manifest_path : request.source}),
          heartbeat_([this] { run_heartbeat(); }) {}

    ~DirectoryProgressChannel() {
        stop_heartbeat();
    }

    DirectoryProgressChannel(const DirectoryProgressChannel &) = delete;
    DirectoryProgressChannel &operator=(const DirectoryProgressChannel &) = delete;

    void publish(const vove::fileops::BasicDirectoryTransferProgress &value) {
        if (value.phase != vove::fileops::BasicDirectoryTransferPhase::completed) {
            std::scoped_lock lock(mutex_);
            latest_ = value;
            write_progress(value);
            return;
        }
        stop_heartbeat();
        write_progress(value);
        std::vector<std::byte> acknowledgement_payload;
        std::string detail;
        vove::fileops::BasicDirectoryTransferCompletionAck acknowledgement;
        if (!vove::platform::detail::read_stream_frame(
                std::cin, acknowledgement_payload, detail,
                vove::fileops::kMaximumBasicDirectoryTransferProtocolBytes) ||
            !vove::fileops::decode_basic_directory_transfer_completion_ack(
                acknowledgement_payload, acknowledgement, detail) ||
            acknowledgement.request_id != request_.request_id) {
            std::_Exit(3);
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        if (const auto *exit_after_ack =
                std::getenv("VOVE_BASIC_DIRECTORY_TRANSFER_TEST_EXIT_AFTER_COMPLETION_ACK");
            exit_after_ack != nullptr && *exit_after_ack != '\0') {
            std::_Exit(88);
        }
#endif
    }

    void finish() {
        stop_heartbeat();
    }

  private:
    void write_progress(const vove::fileops::BasicDirectoryTransferProgress &value) const {
        const auto payload = vove::fileops::encode_basic_directory_transfer_progress_frame(
            {.request_id = request_.request_id, .progress = value});
        if (!vove::platform::detail::write_stream_frame(std::cout, payload)) {
            std::_Exit(3);
        }
    }

    void run_heartbeat() {
        std::unique_lock lock(mutex_);
        write_progress(latest_);
        while (!stopping_) {
            if (condition_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; })) {
                break;
            }
            write_progress(latest_);
        }
    }

    void stop_heartbeat() {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (heartbeat_.joinable()) {
            heartbeat_.join();
        }
    }

    const vove::fileops::BasicDirectoryTransferStreamRequest &request_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{};
    vove::fileops::BasicDirectoryTransferProgress latest_;
    std::thread heartbeat_;
};

bool control_name_matches_operation(const std::filesystem::path &manifest_path,
                                    const std::uint64_t operation_id) {
    std::array<char, 32> hexadecimal{};
    const auto [end, error] = std::to_chars(
        hexadecimal.data(), hexadecimal.data() + hexadecimal.size(), operation_id, 16);
    if (error != std::errc{}) {
        return false;
    }
    const auto prefix_view = vove::core::kTransferDestinationFilenamePrefix;
    std::u8string expected(prefix_view);
    const auto *first = reinterpret_cast<const char8_t *>(hexadecimal.data());
    expected.append(first, first + (end - hexadecimal.data()));
    expected.push_back(u8'-');
    const auto name = manifest_path.parent_path().filename().u8string();
    return vove::core::ascii_istarts_with(std::u8string_view{name}, std::u8string_view{expected});
}

vove::fileops::BasicDirectoryTransferRecoveryIdentity
recovery_identity(const vove::fileops::BasicDirectoryTransferStreamRequest &request) {
    return {.operation_id = request.operation_id,
            .source = request.source,
            .destination = request.destination,
            .source_revision_utf8 = request.expected_source_revision_utf8};
}

bool same_transfer_address(
    const vove::fileops::FileTransferStreamRequest &request, const std::uint64_t operation_id,
    const std::uint32_t item_index,
    const std::array<std::uint8_t, vove::fileops::kFileTransferRequestTokenBytes> &token) {
    return operation_id == request.operation_id && item_index == request.item_index &&
           token == request.request_token;
}

int run_transfer(const vove::fileops::FileTransferStreamRequest &request) {
    auto begun = vove::fileops::detail::begin_file_transfer_stream(request);
    if (!begun.session) {
        const auto response = vove::fileops::encode_file_transfer_stream_result(begun.failure);
        return vove::platform::detail::write_stream_frame(std::cout, response) ? EXIT_SUCCESS : 3;
    }

    const auto reservation =
        vove::fileops::encode_file_transfer_reservation(begun.session->reservation());
    if (!vove::platform::detail::write_stream_frame(std::cout, reservation)) {
        return 3;
    }
    std::vector<std::byte> acknowledgement_payload;
    std::string error;
    if (!vove::platform::detail::read_stream_frame(
            std::cin, acknowledgement_payload, error,
            vove::fileops::kMaximumFileTransferProtocolBytes)) {
        return 1;
    }
    vove::fileops::FileTransferReservationAck acknowledgement;
    if (!vove::fileops::decode_file_transfer_reservation_ack(acknowledgement_payload,
                                                             acknowledgement, error) ||
        !same_transfer_address(request, acknowledgement.operation_id, acknowledgement.item_index,
                               acknowledgement.request_token) ||
        !acknowledgement.accepted) {
        return 2;
    }

    const auto result =
        begun.session->stream([&](const vove::fileops::FileTransferProgress &value) {
            const auto progress = vove::fileops::encode_file_transfer_progress(value);
            return vove::platform::detail::write_stream_frame(std::cout, progress);
        });
    const auto response = vove::fileops::encode_file_transfer_stream_result(result);
    if (!vove::platform::detail::write_stream_frame(std::cout, response)) {
        return 3;
    }
    if (request.mode != vove::fileops::FileTransferStreamMode::audit_existing) {
        return EXIT_SUCCESS;
    }
    acknowledgement_payload.clear();
    if (!vove::platform::detail::read_stream_frame(
            std::cin, acknowledgement_payload, error,
            vove::fileops::kMaximumFileTransferProtocolBytes)) {
        return 1;
    }
    if (!vove::fileops::decode_file_transfer_reservation_ack(acknowledgement_payload,
                                                             acknowledgement, error) ||
        !same_transfer_address(request, acknowledgement.operation_id, acknowledgement.item_index,
                               acknowledgement.request_token)) {
        return 2;
    }
    return acknowledgement.accepted ? EXIT_SUCCESS : 7;
}

int run_basic_directory_transfer(
    const vove::fileops::BasicDirectoryTransferStreamRequest &request) {
    DirectoryProgressChannel progress_channel(request);
    const auto publish_progress =
        [&progress_channel](const vove::fileops::BasicDirectoryTransferProgress &value) {
            progress_channel.publish(value);
        };

    vove::fileops::BasicDirectoryTransferResult result;
    switch (request.command) {
    case vove::fileops::BasicDirectoryTransferCommand::copy: {
        const auto planned =
            vove::fileops::plan_basic_directory_copy({.source = request.source,
                                                      .destination = request.destination,
                                                      .operation_id = request.operation_id});
        if (!planned.ok() ||
            planned.manifest.source_revision_utf8 != request.expected_source_revision_utf8) {
            result = planned;
            if (planned.ok()) {
                result.status = vove::fileops::BasicDirectoryTransferStatus::source_changed;
                result.detail_utf8 = "source directory changed after the transfer was requested";
            }
        } else {
            result =
                vove::fileops::execute_basic_directory_copy(planned.manifest, publish_progress);
        }
        break;
    }
    case vove::fileops::BasicDirectoryTransferCommand::move: {
        const auto planned =
            vove::fileops::plan_basic_directory_move({.source = request.source,
                                                      .destination = request.destination,
                                                      .operation_id = request.operation_id});
        if (!planned.ok() ||
            planned.manifest.source_revision_utf8 != request.expected_source_revision_utf8) {
            result = planned;
            if (planned.ok()) {
                result.status = vove::fileops::BasicDirectoryTransferStatus::source_changed;
                result.detail_utf8 = "source directory changed after the transfer was requested";
            }
        } else {
            result =
                vove::fileops::execute_basic_directory_move(planned.manifest, publish_progress);
        }
        break;
    }
    case vove::fileops::BasicDirectoryTransferCommand::resume:
        result = vove::fileops::resume_basic_directory_transfer(
            request.manifest_path, recovery_identity(request), publish_progress);
        break;
    case vove::fileops::BasicDirectoryTransferCommand::discover_and_resume:
    case vove::fileops::BasicDirectoryTransferCommand::discover_and_discard: {
        const auto scanned =
            vove::fileops::scan_basic_directory_recoveries(request.destination.parent_path());
        if (!scanned.ok()) {
            result.status = scanned.status;
            result.error = scanned.error;
            result.platform_code = scanned.platform_code;
            result.detail_utf8 = scanned.detail_utf8;
            result.recovery_available = true;
            result.destination = request.destination;
            break;
        }
        std::vector<const vove::fileops::BasicDirectoryTransferPlanResult *> named_candidates;
        std::vector<const vove::fileops::BasicDirectoryTransferPlanResult *> exact_candidates;
        for (const auto &candidate : scanned.candidates) {
            if (!control_name_matches_operation(candidate.manifest_path, request.operation_id)) {
                continue;
            }
            named_candidates.push_back(&candidate);
            if (candidate.ok() && candidate.manifest.operation_id == request.operation_id &&
                candidate.manifest.source == request.source &&
                candidate.manifest.destination == request.destination &&
                candidate.manifest.source_revision_utf8 == request.expected_source_revision_utf8) {
                exact_candidates.push_back(&candidate);
            }
        }
        if (named_candidates.size() == 1U && exact_candidates.size() == 1U) {
            result = request.command ==
                             vove::fileops::BasicDirectoryTransferCommand::discover_and_discard
                         ? vove::fileops::discard_basic_directory_transfer(
                               exact_candidates.front()->manifest_path, recovery_identity(request),
                               publish_progress)
                         : vove::fileops::resume_basic_directory_transfer(
                               exact_candidates.front()->manifest_path, recovery_identity(request),
                               publish_progress);
        } else if (!named_candidates.empty()) {
            result.status = vove::fileops::BasicDirectoryTransferStatus::recovery_required;
            result.recovery_available = true;
            result.destination = request.destination;
            result.detail_utf8 = exact_candidates.empty()
                                     ? "directory recovery control is damaged or mismatched"
                                     : "directory recovery operation is ambiguous";
        } else if (scanned.truncated) {
            result.status = vove::fileops::BasicDirectoryTransferStatus::recovery_required;
            result.recovery_available = true;
            result.destination = request.destination;
            result.detail_utf8 = "directory recovery scan reached its safety budget";
        } else {
            result.status = vove::fileops::BasicDirectoryTransferStatus::not_found;
            result.destination = request.destination;
            result.detail_utf8 = "directory recovery operation was not found";
        }
        break;
    }
    case vove::fileops::BasicDirectoryTransferCommand::discard:
        result = vove::fileops::discard_basic_directory_transfer(
            request.manifest_path, recovery_identity(request), publish_progress);
        break;
    }
    progress_channel.finish();
    const auto response = vove::fileops::encode_basic_directory_transfer_result_frame(
        {.request_id = request.request_id, .result = std::move(result)});
    return vove::platform::detail::write_stream_frame(std::cout, response) ? EXIT_SUCCESS : 3;
}

} // namespace

int main() {
    use_binary_stdio();
    if (!arm_parent_death()) {
        return 6;
    }
    try {
        std::vector<std::byte> payload;
        std::string error;
        if (!vove::platform::detail::read_stream_frame(
                std::cin, payload, error, vove::fileops::kMaximumOperationPayloadBytes)) {
            return 1;
        }
        vove::fileops::OperationRequestKind kind{};
        if (!vove::fileops::decode_operation_request_kind(payload, kind, error)) {
            vove::fileops::BasicDirectoryTransferStreamRequest directory_transfer;
            if (vove::fileops::decode_basic_directory_transfer_stream_request(
                    payload, directory_transfer, error)) {
                return run_basic_directory_transfer(directory_transfer);
            }
            vove::fileops::FileTransferStreamRequest transfer;
            if (!vove::fileops::decode_file_transfer_stream_request(payload, transfer, error)) {
                return 2;
            }
            return run_transfer(transfer);
        }
        vove::fileops::OperationResult result;
        if (kind == vove::fileops::OperationRequestKind::rename) {
            vove::fileops::RenameRequest request;
            if (!vove::fileops::decode_rename_request(payload, request, error)) {
                return 2;
            }
            result = request.action == vove::fileops::RenameAction::execute
                         ? vove::fileops::detail::execute_rename(request)
                         : vove::fileops::detail::reconcile_rename(request);
        } else if (kind == vove::fileops::OperationRequestKind::permanent_delete) {
            vove::fileops::DeleteRequest request;
            if (!vove::fileops::decode_delete_request(payload, request, error)) {
                return 2;
            }
            result = request.action == vove::fileops::DeleteAction::execute
                         ? vove::fileops::detail::execute_delete(request)
                         : vove::fileops::detail::reconcile_delete(request);
        } else {
            vove::fileops::CreateDirectoryRequest request;
            if (!vove::fileops::decode_create_directory_request(payload, request, error)) {
                return 2;
            }
            result = request.action == vove::fileops::CreateDirectoryAction::execute
                         ? vove::fileops::detail::execute_create_directory(request)
                         : vove::fileops::detail::reconcile_create_directory(request);
        }
        const auto response = vove::fileops::encode_operation_result(result);
        return vove::platform::detail::write_stream_frame(std::cout, response) ? EXIT_SUCCESS : 3;
    } catch (const std::exception &) {
        return 4;
    } catch (...) {
        return 5;
    }
}
