#pragma once

#include "vove/worker/protocol.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vove::worker {

inline constexpr std::string_view kSyntheticWorkerBuildId = "vove-stage3-synthetic-1";

#ifdef _WIN32
using NativeIoHandle = void *;
#else
using NativeIoHandle = int;
#endif

struct NativeJobIo {
    NativeIoHandle source{};
    NativeIoHandle output{};
#ifdef _WIN32
    NativeIoHandle profile{};
#else
    NativeIoHandle profile{-1};
#endif
};

[[nodiscard]] WorkerResult execute_synthetic_job_native(NativeJobIo io, const WorkerJob &job);

// Runtime resolves and validates both tokens before invoking an executor; no paths are exposed.
using RuntimeExecutor = WorkerResult (*)(NativeJobIo io, const WorkerJob &job);

enum class FrameReadStatus {
    success,
    clean_end_of_stream,
    truncated,
    invalid,
    io_error,
};

struct FrameReadResult {
    FrameReadStatus status{FrameReadStatus::io_error};
    std::vector<std::byte> bytes;
    std::string diagnostic;
};

enum class RuntimeExitCode : int {
    success = 0,
    invalid_standard_handle = 2,
    handshake_rejected = 3,
    malformed_frame = 4,
    io_error = 5,
};

struct RuntimeOptions {
    std::string build_id{std::string{kSyntheticWorkerBuildId}};
    CapabilitySet capabilities{capability_bit(Capability::read_only_source_token) |
                               capability_bit(Capability::write_only_output_token) |
                               capability_bit(Capability::synthetic_handler)};
#ifndef _WIN32
    bool receive_posix_objects{};
#endif
    RuntimeExecutor executor{execute_synthetic_job_native};
};

[[nodiscard]] FrameReadResult read_framed_message(NativeIoHandle input);
[[nodiscard]] bool write_framed_message(NativeIoHandle output, std::span<const std::byte> bytes,
                                        std::string &diagnostic);

// Compatibility entry point: tokens are resolved to inherited native objects before execution.
[[nodiscard]] WorkerResult execute_synthetic_job(const WorkerJob &job);

[[nodiscard]] RuntimeExitCode run_worker_runtime(NativeIoHandle input, NativeIoHandle output,
                                                 const RuntimeOptions &options = {});

} // namespace vove::worker
