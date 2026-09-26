#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vove::worker {
namespace {

constexpr std::size_t kIoChunkBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumRememberedCancellations = 256;
constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

struct NativeReadResult {
    std::size_t bytes{};
    bool end_of_stream{};
    bool failed{};
    std::string diagnostic;
};

#ifdef _WIN32
[[nodiscard]] bool valid_native_handle(const NativeIoHandle handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

[[nodiscard]] NativeReadResult read_native(const NativeIoHandle input,
                                           const std::span<std::byte> buffer) {
    if (!valid_native_handle(input)) {
        return {.bytes = 0,
                .end_of_stream = false,
                .failed = true,
                .diagnostic = "invalid inherited handle"};
    }
    const auto request = static_cast<DWORD>(std::min<std::size_t>(
        buffer.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD received{};
    if (ReadFile(input, buffer.data(), request, &received, nullptr) == FALSE) {
        const auto error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
            return {.bytes = 0, .end_of_stream = true, .failed = false, .diagnostic = {}};
        }
        return {.bytes = 0,
                .end_of_stream = false,
                .failed = true,
                .diagnostic = "read from inherited handle failed: " +
                              std::to_string(static_cast<unsigned long>(error))};
    }
    return {.bytes = static_cast<std::size_t>(received),
            .end_of_stream = received == 0,
            .failed = false,
            .diagnostic = {}};
}

[[nodiscard]] bool write_native(const NativeIoHandle output, const std::span<const std::byte> bytes,
                                std::string &diagnostic) {
    if (!valid_native_handle(output)) {
        diagnostic = "invalid inherited handle";
        return false;
    }
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto request_size = std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        const auto request = static_cast<DWORD>(request_size);
        DWORD written{};
        if (WriteFile(output, bytes.data() + offset, request, &written, nullptr) == FALSE) {
            diagnostic = "write to inherited handle failed: " +
                         std::to_string(static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (written == 0) {
            diagnostic = "write to inherited handle made no progress";
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] NativeIoHandle token_to_native_handle(const OpaqueToken token) noexcept {
    if (token == 0 ||
        token > static_cast<OpaqueToken>(std::numeric_limits<std::uintptr_t>::max())) {
        return nullptr;
    }
    const auto value = static_cast<std::uintptr_t>(token);
    const auto handle = reinterpret_cast<NativeIoHandle>(value);
    return valid_native_handle(handle) ? handle : nullptr;
}
#else
[[nodiscard]] bool valid_native_handle(const NativeIoHandle handle) noexcept {
    return handle >= 0;
}

[[nodiscard]] NativeReadResult read_native(const NativeIoHandle input,
                                           const std::span<std::byte> buffer) {
    if (!valid_native_handle(input)) {
        return {.bytes = 0,
                .end_of_stream = false,
                .failed = true,
                .diagnostic = "invalid inherited file descriptor"};
    }
    for (;;) {
        const auto received = ::read(input, buffer.data(), buffer.size());
        if (received > 0) {
            return {.bytes = static_cast<std::size_t>(received),
                    .end_of_stream = false,
                    .failed = false,
                    .diagnostic = {}};
        }
        if (received == 0) {
            return {.bytes = 0, .end_of_stream = true, .failed = false, .diagnostic = {}};
        }
        if (errno != EINTR) {
            return {.bytes = 0,
                    .end_of_stream = false,
                    .failed = true,
                    .diagnostic =
                        "read from inherited file descriptor failed: " + std::to_string(errno)};
        }
    }
}

[[nodiscard]] bool write_native(const NativeIoHandle output, const std::span<const std::byte> bytes,
                                std::string &diagnostic) {
    if (!valid_native_handle(output)) {
        diagnostic = "invalid inherited file descriptor";
        return false;
    }
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto written = ::write(output, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        diagnostic = written == 0
                         ? "write to inherited file descriptor made no progress"
                         : "write to inherited file descriptor failed: " + std::to_string(errno);
        return false;
    }
    return true;
}

[[nodiscard]] NativeIoHandle token_to_native_handle(const OpaqueToken token) noexcept {
    if (token == 0 || token > static_cast<OpaqueToken>(std::numeric_limits<int>::max())) {
        return -1;
    }
    return static_cast<int>(token);
}
#endif

[[nodiscard]] std::uint32_t decode_u32_le(const std::span<const std::byte> bytes,
                                          const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

enum class ExactReadStatus : std::uint8_t { success, clean_end_of_stream, truncated, io_error };

[[nodiscard]] ExactReadStatus read_exact(const NativeIoHandle input,
                                         const std::span<std::byte> bytes,
                                         const bool allow_clean_end_of_stream,
                                         std::string &diagnostic) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto result = read_native(input, bytes.subspan(offset));
        if (result.failed) {
            diagnostic = result.diagnostic;
            return ExactReadStatus::io_error;
        }
        if (result.end_of_stream) {
            if (offset == 0 && allow_clean_end_of_stream) {
                return ExactReadStatus::clean_end_of_stream;
            }
            diagnostic = "framed message ended before all declared bytes arrived";
            return ExactReadStatus::truncated;
        }
        offset += result.bytes;
    }
    return ExactReadStatus::success;
}

[[nodiscard]] WorkerResult failure_result(const JobId job_id, const ResultStatus status,
                                          std::string diagnostic) {
    if (diagnostic.size() > kMaximumDiagnosticBytes) {
        diagnostic.resize(kMaximumDiagnosticBytes);
    }
    return {.job_id = job_id,
            .status = status,
            .width = 0,
            .height = 0,
            .color_model = ColorModel::unknown,
            .color_profile_utf8 = {},
            .source_profile_fingerprint = {},
            .page_index = 0,
            .page_count = 0,
            .bytes_written = 0,
            .diagnostic_utf8 = std::move(diagnostic)};
}

[[nodiscard]] bool valid_result_status(const ResultStatus status) noexcept {
    switch (status) {
    case ResultStatus::success:
    case ResultStatus::unsupported:
    case ResultStatus::malformed_source:
    case ResultStatus::timed_out:
    case ResultStatus::cancelled:
    case ResultStatus::resource_limit:
    case ResultStatus::internal_error:
    case ResultStatus::source_unavailable:
    case ResultStatus::disconnected:
    case ResultStatus::color_profile_required:
    case ResultStatus::password_required:
    case ResultStatus::ghostscript_required:
    case ResultStatus::embedded_preview_unavailable:
    case ResultStatus::source_changed:
    case ResultStatus::document_password_incorrect:
    case ResultStatus::memory_limit:
        return true;
    case ResultStatus::reserved_source_authentication_failure:
        return false;
    }
    return false;
}

[[nodiscard]] bool valid_color_model(const ColorModel model) noexcept {
    switch (model) {
    case ColorModel::unknown:
    case ColorModel::grayscale:
    case ColorModel::rgb:
    case ColorModel::cmyk:
    case ColorModel::lab:
    case ColorModel::mixed:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1;
            code_point = first & 0x1FU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] bool valid_executor_result(const WorkerResult &result,
                                         const WorkerJob &job) noexcept {
    const auto valid_fingerprint =
        result.source_profile_fingerprint.empty() ||
        (result.source_profile_fingerprint.size() == 64U &&
         std::ranges::all_of(result.source_profile_fingerprint, [](const char value) {
             return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
         }));
    if (result.job_id != job.job_id || !valid_result_status(result.status) ||
        !valid_color_model(result.color_model) ||
        result.provenance > PreviewProvenance::embedded_preview || !valid_fingerprint ||
        result.color_profile_utf8.size() > kMaximumProfileNameBytes ||
        result.diagnostic_utf8.size() > kMaximumDiagnosticBytes ||
        !valid_utf8(result.color_profile_utf8) || !valid_utf8(result.diagnostic_utf8) ||
        result.width > job.limits.canonical_edge || result.height > job.limits.canonical_edge ||
        result.bytes_written > job.limits.maximum_output_bytes ||
        result.page_count > kMaximumPageCount) {
        return false;
    }

    const auto pixel_count = static_cast<std::uint64_t>(result.width) * result.height;
    const auto maximum_pixels =
        static_cast<std::uint64_t>(job.limits.canonical_edge) * job.limits.canonical_edge;
    if (pixel_count > maximum_pixels || (result.page_count == 0 && result.page_index != 0) ||
        (result.page_count != 0 && result.page_index >= result.page_count)) {
        return false;
    }

    if (result.status == ResultStatus::success) {
        return result.width != 0 && result.height != 0 && result.bytes_written != 0 &&
               result.page_count != 0;
    }

    return result.width == 0 && result.height == 0 && result.color_model == ColorModel::unknown &&
           result.provenance == PreviewProvenance::primary_render &&
           result.color_profile_utf8.empty() && result.source_profile_fingerprint.empty() &&
           result.page_index == 0 && result.page_count == 0 && result.bytes_written == 0;
}

[[nodiscard]] bool deadline_expired(const std::chrono::steady_clock::time_point deadline) noexcept {
    return std::chrono::steady_clock::now() >= deadline;
}

[[nodiscard]] std::string hexadecimal(const std::uint64_t value) {
    constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string output(16, '0');
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned int>((output.size() - index - 1U) * 4U);
        output[index] = digits[static_cast<std::size_t>((value >> shift) & 0x0FULL)];
    }
    return output;
}

[[nodiscard]] bool generation_cancelled(const std::vector<RequestGeneration> &cancelled,
                                        const RequestGeneration generation) {
    return std::find(cancelled.begin(), cancelled.end(), generation) != cancelled.end();
}

void remember_cancellation(std::vector<RequestGeneration> &cancelled,
                           const RequestGeneration generation) {
    if (generation_cancelled(cancelled, generation)) {
        return;
    }
    if (cancelled.size() == kMaximumRememberedCancellations) {
        cancelled.erase(cancelled.begin());
    }
    cancelled.push_back(generation);
}

[[nodiscard]] WorkerResult dispatch_job(const WorkerJob &job, const RuntimeExecutor executor,
                                        const AnimationExecutor animation_executor = nullptr,
                                        const AnimationEmitter &emit = {}) {
    const auto source = token_to_native_handle(job.source_token);
    const auto output = token_to_native_handle(job.output_token);
    const auto profile = token_to_native_handle(job.profile_token);
    if (!valid_native_handle(source)) {
        return failure_result(job.job_id, ResultStatus::malformed_source,
                              "source token is not a valid inherited object");
    }
    if (!valid_native_handle(output)) {
        return failure_result(job.job_id, ResultStatus::internal_error,
                              "output token is not a valid inherited object");
    }
    if (job.profile_token != 0 && !valid_native_handle(profile)) {
        return failure_result(job.job_id, ResultStatus::internal_error,
                              "profile token is not a valid inherited object");
    }
    if (executor == nullptr) {
        return failure_result(job.job_id, ResultStatus::internal_error,
                              "worker runtime has no job executor");
    }
    try {
        const NativeJobIo io{.source = source, .output = output, .profile = profile};
        auto result = job.source_format_hint == SourceFormatHint::gif_animation
                          ? (animation_executor != nullptr && emit
                                 ? animation_executor(io, job, emit)
                                 : failure_result(job.job_id, ResultStatus::unsupported,
                                                  "animation executor unavailable"))
                          : executor(io, job);
        if (!valid_executor_result(result, job)) {
            return failure_result(job.job_id, ResultStatus::internal_error,
                                  "job executor returned invalid result metadata");
        }
        return result;
    } catch (const std::exception &) {
        return failure_result(job.job_id, ResultStatus::internal_error,
                              "job executor threw an exception");
    } catch (...) {
        return failure_result(job.job_id, ResultStatus::internal_error,
                              "job executor threw an unknown exception");
    }
}

#ifndef _WIN32
[[nodiscard]] bool receive_posix_objects(const NativeIoHandle transport, std::string &diagnostic) {
    std::array<std::byte, CMSG_SPACE(sizeof(int) * 3U)> control{};
    std::byte marker{};
    iovec vector{.iov_base = &marker, .iov_len = 1};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    ssize_t received{};
    do {
        received = ::recvmsg(transport, &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received != 1 || marker != std::byte{0x56} || (message.msg_flags & MSG_CTRUNC) != 0) {
        diagnostic = received < 0 ? "receiving worker objects failed: " + std::to_string(errno)
                                  : "worker object message is malformed";
        return false;
    }

    const auto *header = CMSG_FIRSTHDR(&message);
    if (header == nullptr || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        (header->cmsg_len != CMSG_LEN(sizeof(int) * 2U) &&
         header->cmsg_len != CMSG_LEN(sizeof(int) * 3U))) {
        diagnostic = "worker object message does not contain two or three descriptors";
        return false;
    }
    const auto object_count =
        header->cmsg_len == CMSG_LEN(sizeof(int) * 3U) ? std::size_t{3} : std::size_t{2};
    std::array<int, 3> objects{-1, -1, -1};
    std::memcpy(objects.data(), CMSG_DATA(header), sizeof(int) * object_count);

    std::array<int, 3> temporary{-1, -1, -1};
    for (std::size_t index = 0; index < object_count; ++index) {
        temporary[index] = ::fcntl(objects[index], F_DUPFD_CLOEXEC, 16);
        if (temporary[index] < 0) {
            diagnostic = "duplicating received worker object failed: " + std::to_string(errno);
            for (std::size_t object_index = 0; object_index < object_count; ++object_index) {
                static_cast<void>(::close(objects[object_index]));
            }
            for (const auto descriptor : temporary) {
                if (descriptor >= 0) {
                    static_cast<void>(::close(descriptor));
                }
            }
            return false;
        }
    }
    for (std::size_t index = 0; index < object_count; ++index) {
        static_cast<void>(::close(objects[index]));
    }
    if (::dup2(temporary[0], 3) < 0 || ::dup2(temporary[1], 4) < 0 ||
        (object_count == 3 && ::dup2(temporary[2], 5) < 0)) {
        diagnostic = "installing received worker objects failed: " + std::to_string(errno);
        for (const auto descriptor : temporary) {
            static_cast<void>(::close(descriptor));
        }
        return false;
    }
    for (const auto descriptor : temporary) {
        static_cast<void>(::close(descriptor));
    }
    return true;
}
#endif

} // namespace

FrameReadResult read_framed_message(const NativeIoHandle input) {
    FrameReadResult result;
    result.bytes.resize(kProtocolHeaderBytes);
    const auto header_status = read_exact(input, result.bytes, true, result.diagnostic);
    if (header_status != ExactReadStatus::success) {
        result.bytes.clear();
        switch (header_status) {
        case ExactReadStatus::clean_end_of_stream:
            result.status = FrameReadStatus::clean_end_of_stream;
            break;
        case ExactReadStatus::truncated:
            result.status = FrameReadStatus::truncated;
            break;
        case ExactReadStatus::io_error:
            result.status = FrameReadStatus::io_error;
            break;
        case ExactReadStatus::success:
            break;
        }
        return result;
    }

    constexpr std::size_t frame_size_offset = 12;
    const auto frame_size = decode_u32_le(result.bytes, frame_size_offset);
    if (frame_size < kProtocolHeaderBytes || frame_size > kMaximumFrameBytes) {
        result.status = FrameReadStatus::invalid;
        result.diagnostic = "framed message declares an invalid size";
        result.bytes.clear();
        return result;
    }
    result.bytes.resize(frame_size);
    const auto payload = std::span<std::byte>{result.bytes}.subspan(kProtocolHeaderBytes);
    const auto payload_status = read_exact(input, payload, false, result.diagnostic);
    if (payload_status != ExactReadStatus::success) {
        result.status = payload_status == ExactReadStatus::io_error ? FrameReadStatus::io_error
                                                                    : FrameReadStatus::truncated;
        result.bytes.clear();
        return result;
    }

    DecodedFrame decoded;
    DecodeError error;
    if (!decode_frame(result.bytes, decoded, error)) {
        result.status = FrameReadStatus::invalid;
        result.diagnostic = error.message;
        result.bytes.clear();
        return result;
    }
    result.status = FrameReadStatus::success;
    return result;
}

bool write_framed_message(const NativeIoHandle output, const std::span<const std::byte> bytes,
                          std::string &diagnostic) {
    DecodedFrame decoded;
    DecodeError error;
    if (!decode_frame(bytes, decoded, error)) {
        diagnostic = "refusing to write an invalid frame: " + error.message;
        return false;
    }
    diagnostic.clear();
    return write_native(output, bytes, diagnostic);
}

WorkerResult execute_synthetic_job_native(const NativeJobIo io, const WorkerJob &job) {
    const auto timeout = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(job.limits.wall_timeout_ms)};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<std::byte, kIoChunkBytes> buffer{};
    std::uint64_t input_bytes{};
    std::uint64_t hash = kFnvOffsetBasis;

    for (;;) {
        if (deadline_expired(deadline)) {
            return failure_result(job.job_id, ResultStatus::timed_out,
                                  "synthetic source read exceeded its wall-time limit");
        }
        const auto remaining = job.limits.maximum_input_bytes - input_bytes;
        const auto probe_bytes =
            std::min<std::uint64_t>(static_cast<std::uint64_t>(buffer.size()), remaining + 1U);
        const auto read = read_native(
            io.source, std::span<std::byte>{buffer}.first(static_cast<std::size_t>(probe_bytes)));
        if (read.failed) {
            return failure_result(job.job_id, ResultStatus::malformed_source, read.diagnostic);
        }
        if (read.end_of_stream) {
            break;
        }
        const auto chunk_bytes = static_cast<std::uint64_t>(read.bytes);
        if (input_bytes > job.limits.maximum_input_bytes ||
            chunk_bytes > job.limits.maximum_input_bytes - input_bytes) {
            return failure_result(job.job_id, ResultStatus::resource_limit,
                                  "source exceeds the configured input byte limit");
        }
        for (const auto byte : std::span<const std::byte>{buffer}.first(read.bytes)) {
            hash ^= std::to_integer<unsigned char>(byte);
            hash *= kFnvPrime;
        }
        input_bytes += chunk_bytes;
    }

    if (deadline_expired(deadline)) {
        return failure_result(job.job_id, ResultStatus::timed_out,
                              "synthetic processing exceeded its wall-time limit");
    }

    const auto payload = std::string{"VOVE-SYNTHETIC-1\ninput-bytes="} +
                         std::to_string(input_bytes) + "\nfnv1a64=" + hexadecimal(hash) +
                         "\nedge=" + std::to_string(job.limits.canonical_edge) + "\n";
    if (payload.size() > job.limits.maximum_output_bytes) {
        return failure_result(job.job_id, ResultStatus::resource_limit,
                              "synthetic result exceeds the configured output byte limit");
    }
    const auto payload_bytes = std::as_bytes(std::span<const char>{payload.data(), payload.size()});
    std::string diagnostic;
    if (!write_native(io.output, payload_bytes, diagnostic)) {
        return failure_result(job.job_id, ResultStatus::internal_error, std::move(diagnostic));
    }
    if (deadline_expired(deadline)) {
        return failure_result(job.job_id, ResultStatus::timed_out,
                              "synthetic output exceeded its wall-time limit");
    }

    return {.job_id = job.job_id,
            .status = ResultStatus::success,
            .width = job.limits.canonical_edge,
            .height = job.limits.canonical_edge,
            .color_model = ColorModel::rgb,
            .color_profile_utf8 = "sRGB",
            .source_profile_fingerprint = {},
            .page_index = 0,
            .page_count = 1,
            .bytes_written = static_cast<std::uint64_t>(payload.size()),
            .diagnostic_utf8 = {}};
}

WorkerResult execute_synthetic_job(const WorkerJob &job) {
    return dispatch_job(job, execute_synthetic_job_native);
}

RuntimeExitCode run_worker_runtime(const NativeIoHandle input, const NativeIoHandle output,
                                   const RuntimeOptions &options) {
    if (!valid_native_handle(input) || !valid_native_handle(output)) {
        return RuntimeExitCode::invalid_standard_handle;
    }

    auto frame = read_framed_message(input);
    if (frame.status != FrameReadStatus::success) {
        return frame.status == FrameReadStatus::io_error ? RuntimeExitCode::io_error
                                                         : RuntimeExitCode::handshake_rejected;
    }
    Handshake handshake;
    DecodeError decode_error;
    if (!decode_handshake(frame.bytes, handshake, decode_error) ||
        handshake.build_id != options.build_id) {
        return RuntimeExitCode::handshake_rejected;
    }

    const Handshake acknowledgement{.build_id = options.build_id,
                                    .capabilities = options.capabilities};
    std::vector<std::byte> acknowledgement_bytes;
    try {
        acknowledgement_bytes =
            encode_handshake(acknowledgement, MessageKind::handshake_acknowledgement);
    } catch (const std::exception &) {
        return RuntimeExitCode::handshake_rejected;
    }
    std::string diagnostic;
    if (!write_framed_message(output, acknowledgement_bytes, diagnostic)) {
        return RuntimeExitCode::io_error;
    }

#ifndef _WIN32
    if (options.receive_posix_objects && !receive_posix_objects(input, diagnostic)) {
        return RuntimeExitCode::io_error;
    }
#endif

    std::vector<RequestGeneration> cancelled_generations;
    cancelled_generations.reserve(kMaximumRememberedCancellations);
    for (;;) {
        frame = read_framed_message(input);
        if (frame.status == FrameReadStatus::clean_end_of_stream) {
            return RuntimeExitCode::success;
        }
        if (frame.status != FrameReadStatus::success) {
            return frame.status == FrameReadStatus::io_error ? RuntimeExitCode::io_error
                                                             : RuntimeExitCode::malformed_frame;
        }

        DecodedFrame envelope;
        if (!decode_frame(frame.bytes, envelope, decode_error)) {
            return RuntimeExitCode::malformed_frame;
        }
        if (envelope.header.kind == MessageKind::cancel_generation) {
            CancelGeneration cancel;
            if (!decode_cancel_generation(frame.bytes, cancel, decode_error)) {
                return RuntimeExitCode::malformed_frame;
            }
            remember_cancellation(cancelled_generations, cancel.generation);
            continue;
        }
        if (envelope.header.kind != MessageKind::worker_job) {
            return RuntimeExitCode::malformed_frame;
        }

        WorkerJob job;
        if (!decode_worker_job(frame.bytes, job, decode_error)) {
            return RuntimeExitCode::malformed_frame;
        }
        auto result = generation_cancelled(cancelled_generations, job.generation)
                          ? failure_result(job.job_id, ResultStatus::cancelled,
                                           "request generation was cancelled before execution")
                          : dispatch_job(job, options.executor, options.animation_executor,
                              [&](const AnimationFrame &animation) {
                                  if (animation.image.job_id != job.job_id ||
                                      !valid_executor_result(animation.image, job))
                                      return false;
                                  if (!write_framed_message(output, encode_animation_frame(animation),
                                                            diagnostic))
                                      return false;
                                  const auto reply = read_framed_message(input);
                                  AnimationAdvance advance;
                                  return reply.status == FrameReadStatus::success &&
                                         decode_animation_advance(reply.bytes, advance, decode_error) &&
                                         advance.job_id == job.job_id && advance.proceed;
                              });
        const auto result_bytes = encode_worker_result(result);
        if (!write_framed_message(output, result_bytes, diagnostic)) {
            return RuntimeExitCode::io_error;
        }
    }
}

} // namespace vove::worker
