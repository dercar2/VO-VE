#pragma once

#include "vove/fileops/file_operation.hpp"

#include <filesystem>
#include <string>
#include <stop_token>

namespace vove::fileops::detail {

class TrashDirectoryLease final {
  public:
    TrashDirectoryLease() noexcept = default;
    ~TrashDirectoryLease();
    TrashDirectoryLease(const TrashDirectoryLease &) = delete;
    TrashDirectoryLease &operator=(const TrashDirectoryLease &) = delete;
    TrashDirectoryLease(TrashDirectoryLease &&other) noexcept;
    TrashDirectoryLease &operator=(TrashDirectoryLease &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;

  private:
    explicit TrashDirectoryLease(void *handle) noexcept;
    void *handle_{};

    friend TrashDirectoryLease pin_owned_trash_directory(const std::filesystem::path &,
                                                         const std::wstring &, std::string &,
                                                         bool *);
};

struct TrashSecurityResult {
    OperationStatus status{OperationStatus::io_error};
    SourceSnapshot snapshot;
    std::string original_sddl_utf8;
    std::string detail_utf8;
    TrashPayloadPolicy payload_policy{TrashPayloadPolicy::strict};

    [[nodiscard]] bool ok() const noexcept {
        return status == OperationStatus::success;
    }
};

[[nodiscard]] TrashSecurityResult capture_trash_security(const std::filesystem::path &path,
                                                         const SourceSnapshot &expected,
                                                         bool directory = false,
                                                         bool allow_foreign_file_owner = false);

[[nodiscard]] bool validate_preserved_trash_security(const std::string &sddl_utf8,
                                                    std::string &detail_utf8);
[[nodiscard]] bool verify_preserved_trash_security_handle(void *handle,
                                                         const std::string &sddl_utf8,
                                                         std::string &detail_utf8);
[[nodiscard]] TrashSecurityResult verify_preserved_trash_payload(
    const std::filesystem::path &path, const SourceSnapshot &expected,
    const std::string &sddl_utf8, bool allow_revision_advance = false);

[[nodiscard]] TrashSecurityResult harden_trash_payload(const std::filesystem::path &path,
                                                       const SourceSnapshot &expected,
                                                       bool directory = false,
                                                       bool allow_revision_advance = false);

[[nodiscard]] TrashSecurityResult restore_trash_security(const std::filesystem::path &path,
                                                         const SourceSnapshot &expected,
                                                         const std::string &original_sddl_utf8,
                                                         bool directory = false,
                                                         bool allow_revision_advance = false);

[[nodiscard]] TrashSecurityResult verify_hardened_trash_payload(const std::filesystem::path &path,
                                                                const SourceSnapshot &expected,
                                                                bool allow_revision_advance = false,
                                                                bool directory = false);

[[nodiscard]] TrashSecurityResult
verify_restored_trash_payload(const std::filesystem::path &path, const SourceSnapshot &expected,
                              const std::string &original_sddl_utf8,
                              bool allow_revision_advance = false, bool directory = false);

[[nodiscard]] bool validate_restorable_trash_dacl(const std::string &sddl_utf8,
                                                  std::string &detail_utf8);

[[nodiscard]] bool current_user_sid_text(std::wstring &sid, std::string &detail_utf8);

#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
using TrashDirectoryCreateHook = void (*)(const std::filesystem::path &, void *);
void set_trash_directory_create_hook(TrashDirectoryCreateHook hook) noexcept;
using LegacyTrashOwnerObservationHook = void (*)(void **);
void set_legacy_trash_owner_observation_hook(LegacyTrashOwnerObservationHook hook) noexcept;
#endif

// Creates with explicit user ownership and a protected SYSTEM + user DACL; otherwise only verifies.
[[nodiscard]] bool secure_owned_trash_vault(const std::filesystem::path &vault,
                                            const std::wstring &current_user_sid,
                                            std::string &detail_utf8);

[[nodiscard]] bool verify_owned_trash_vault(const std::filesystem::path &vault,
                                            const std::wstring &current_user_sid,
                                            std::string &detail_utf8);

// Only for an explicitly requested new Trash move, under the current-operation lease.
// Never removes data or changes DACLs; accepts only the exact legacy Administrators-owned policy.
[[nodiscard]] bool repair_empty_legacy_trash_vault(const std::filesystem::path &vault,
                                                   const std::filesystem::path &canonical_vault,
                                                   const std::wstring &current_user_sid,
                                                   const std::stop_token &stop,
                                                   std::string &detail_utf8);

// Pins the exact protected current-user + SYSTEM directory object without FILE_SHARE_DELETE.
// The namespace entry cannot be renamed or removed until the returned lease is released.
[[nodiscard]] TrashDirectoryLease pin_owned_trash_directory(const std::filesystem::path &directory,
                                                            const std::wstring &current_user_sid,
                                                            std::string &detail_utf8,
                                                            bool *missing = nullptr);

[[nodiscard]] bool remove_empty_trash_container_handle_bound(const std::filesystem::path &container,
                                                             bool tolerate_not_empty,
                                                             std::string &detail_utf8);

} // namespace vove::fileops::detail
