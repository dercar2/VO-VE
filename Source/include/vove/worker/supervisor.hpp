#pragma once

#include "vove/worker/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vove::worker {

using NativeObject = std::intptr_t;
inline constexpr NativeObject kInvalidNativeObject = static_cast<NativeObject>(-1);

enum class SandboxLevel : std::uint8_t {
    none,
    degraded,
    minimum,
    strict,
};

struct SandboxReport {
    SandboxLevel level{SandboxLevel::none};
    bool process_contained{};
    bool identity_restricted{};
    bool no_new_privileges{};
    bool network_isolated{};
    bool filesystem_isolated{};
    bool memory_limited{};
    bool cpu_limited{};
    std::string detail;
};

enum class SupervisorError : std::uint8_t {
    none,
    invalid_request,
    sandbox_unavailable,
    launch_failed,
    transport_error,
    handshake_failed,
    incompatible_worker,
    timed_out,
    resource_limit,
    worker_crashed,
};

struct SupervisorRequest {
    std::filesystem::path worker_executable;
    NativeObject source_object{kInvalidNativeObject};
    NativeObject output_object{kInvalidNativeObject};
    NativeObject profile_object{kInvalidNativeObject};
    WorkerJob job;
    std::string expected_build_id;
    std::chrono::milliseconds timeout{30'000};
    bool require_minimum_sandbox{true};
};

struct SupervisorResult {
    SupervisorError error{SupervisorError::none};
    std::string detail;
    WorkerResult worker_result;
    SandboxReport sandbox;
    int exit_code{};
    std::uint32_t system_error{};
    bool handshake_completed{};

    [[nodiscard]] bool ok() const noexcept {
        return error == SupervisorError::none && worker_result.status == ResultStatus::success;
    }
};

struct ExternalRendererRequest {
    std::filesystem::path executable;
    std::vector<std::string> arguments_utf8;
    NativeObject source_object{kInvalidNativeObject};
    NativeObject output_object{kInvalidNativeObject};
    JobLimits limits;
    std::chrono::milliseconds timeout{30'000};
    bool require_minimum_sandbox{true};
};

struct ExternalRendererResult {
    SupervisorError error{SupervisorError::none};
    std::string detail;
    SandboxReport sandbox;
    int exit_code{};
    std::uint64_t bytes_written{};

    [[nodiscard]] bool ok() const noexcept {
        return error == SupervisorError::none && exit_code == 0;
    }
};

// Executes one isolated job. The worker receives duplicated/inherited OS objects, never source or
// output paths. Platform implementations must enforce the timeout even if IPC stops responding.
[[nodiscard]] SupervisorResult run_worker_once(const SupervisorRequest &request);

// Executes one fixed-command renderer with the source on stdin and the bounded output on stdout.
// The child receives no source path, helper IPC endpoint or inherited environment.
[[nodiscard]] ExternalRendererResult
run_external_renderer_once(const ExternalRendererRequest &request);

} // namespace vove::worker
