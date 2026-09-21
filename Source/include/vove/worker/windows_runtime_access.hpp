#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace vove::worker {

inline constexpr wchar_t kWorkerProfileName[] = L"VO-VE.Worker.Sandbox.v1";

struct RuntimeAccessResult {
    std::uint32_t system_error{};
    std::string item;
    [[nodiscard]] bool ok() const noexcept { return system_error == 0; }
};

// Explicit, non-inheritable read/execute access for this product's decoder profile.
// Never changes ancestors, documents, unrelated files or system DLL permissions.
[[nodiscard]] RuntimeAccessResult prepare_windows_worker_runtime(
    const std::filesystem::path &directory);

} // namespace vove::worker
