#pragma once

#include <filesystem>

namespace vove::fileops::detail {

using MatchedRemoveBeforeClaimHook = void (*)(const std::filesystem::path &);

enum class MatchedRemoveCrashPoint {
    staging_durable,
    claim_durable,
    unlink_durable,
};

using MatchedRemoveCrashHook = void (*)(const std::filesystem::path &, MatchedRemoveCrashPoint);

void set_matched_remove_before_claim_hook(MatchedRemoveBeforeClaimHook hook) noexcept;
void set_matched_remove_crash_hook(MatchedRemoveCrashHook hook) noexcept;

} // namespace vove::fileops::detail
