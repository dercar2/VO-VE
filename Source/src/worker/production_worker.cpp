#include "runtime.hpp"

#include <array>
#include <cstddef>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/resource.h>
#endif
#include <unistd.h>
#endif

namespace vove::worker {
namespace {

[[nodiscard]] bool supervisor_controls_are_active() noexcept {
#ifdef _WIN32
    BOOL in_job{};
    if (IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) == FALSE || in_job == FALSE) {
        return false;
    }
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    auto restricted = IsTokenRestricted(token) != FALSE;
    DWORD required_bytes{};
    static_cast<void>(GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &required_bytes));
    std::array<std::byte, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> integrity{};
    if (required_bytes != 0 && required_bytes <= integrity.size() &&
        GetTokenInformation(token, TokenIntegrityLevel, integrity.data(),
                            static_cast<DWORD>(integrity.size()), &required_bytes) != FALSE) {
        const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(integrity.data());
        const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
        restricted =
            restricted || (count != 0 && *GetSidSubAuthority(label->Label.Sid, count - 1U) <=
                                             SECURITY_MANDATORY_LOW_RID);
    }
    static_cast<void>(CloseHandle(token));
    return restricted;
#elif defined(__linux__)
    rlimit descriptor_limit{};
    return ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 &&
           ::getrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0 && descriptor_limit.rlim_cur <= 64;
#else
    return false;
#endif
}

} // namespace

int run_production_worker(const std::string_view build_id, const CapabilitySet handler_capabilities,
                          const RuntimeExecutor executor, const AnimationExecutor animation_executor) {
#ifdef _WIN32
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
#else
    std::signal(SIGPIPE, SIG_IGN);
    constexpr auto input = STDIN_FILENO;
    constexpr auto output = STDOUT_FILENO;
#endif
    RuntimeOptions options;
    options.build_id = build_id;
    options.capabilities = capability_bit(Capability::read_only_source_token) |
                           capability_bit(Capability::write_only_output_token) |
                           capability_bit(Capability::color_management) | handler_capabilities;
    options.executor = executor;
    options.animation_executor = animation_executor;
#ifndef _WIN32
    options.receive_posix_objects = true;
#endif
    if (supervisor_controls_are_active()) {
        options.capabilities |= capability_bit(Capability::sandbox_active);
    }
    return static_cast<int>(run_worker_runtime(input, output, options));
}

} // namespace vove::worker
