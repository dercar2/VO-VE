#pragma once

#include <cstdint>
#include <filesystem>

namespace vove::fileops::detail {

enum class TrashCleanupCrashPoint : std::uint8_t {
    rollback_container_retired,
    restore_container_retired,
    purge_container_retired,
};

using TrashCleanupCrashHook = void (*)(TrashCleanupCrashPoint);
using TrashManifestRepublishHook = void (*)(const std::filesystem::path &);

void set_trash_cleanup_crash_hook(TrashCleanupCrashHook hook) noexcept;
void set_trash_manifest_republish_hook(TrashManifestRepublishHook hook) noexcept;

} // namespace vove::fileops::detail
