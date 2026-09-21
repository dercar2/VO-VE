#pragma once

#include "vove/fileops/directory_transfer_manifest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kBasicDirectoryTransferManifestVersion = 2U;
inline constexpr std::size_t kBasicDirectoryTransferMaximumEntries = 100'000U;
inline constexpr std::uint16_t kBasicDirectoryTransferMaximumDepth = 128U;
inline constexpr std::size_t kBasicDirectoryTransferDigestBytes = 32U;
inline constexpr std::size_t kBasicDirectoryTransferMaximumManifestBytes =
    std::size_t{64} * 1'024U * 1'024U;
inline constexpr std::size_t kBasicDirectoryTransferMaximumRecoveryScanEntries = 10'000U;
inline constexpr std::size_t kBasicDirectoryTransferMaximumRecoveryCandidates = 256U;
inline constexpr std::size_t kBasicDirectoryTransferMaximumRecoveryLoadedBytes =
    std::size_t{8} * 1'024U * 1'024U;
inline constexpr std::size_t kBasicDirectoryTransferMaximumRecoveryLoadedEntries = 100'000U;

using BasicDirectoryTransferDigest = std::array<std::uint8_t, kBasicDirectoryTransferDigestBytes>;

enum class BasicDirectoryTransferEntryKind : std::uint8_t {
    directory,
    regular_file,
};

struct BasicDirectoryTransferEntry {
    std::filesystem::path relative_path;
    std::uint64_t size_bytes{};
    std::int64_t modified_unix_ns{};
    std::string source_revision_utf8;
    std::uint16_t depth{1U};
    BasicDirectoryTransferEntryKind kind{BasicDirectoryTransferEntryKind::regular_file};
};

struct BasicDirectoryTransferManifest {
    std::uint32_t version{kBasicDirectoryTransferManifestVersion};
    std::uint64_t operation_id{};
    FileTransferKind kind{FileTransferKind::copy};
    DirectoryPathSemantics path_semantics{DirectoryPathSemantics::windows_ordinal_nfc};
    std::filesystem::path source;
    std::filesystem::path staging_destination;
    std::filesystem::path destination;
    std::string source_revision_utf8;
    DirectoryTransferOwnershipToken ownership_token{};
    std::uint64_t total_bytes{};
    std::vector<BasicDirectoryTransferEntry> entries;
};

enum class BasicDirectoryTransferStatus : std::uint8_t {
    success,
    move_pending_publication,
    invalid_request,
    not_found,
    conflict,
    source_changed,
    staging_changed,
    permission_denied,
    authentication_required,
    disconnected,
    timed_out,
    unsupported,
    recovery_required,
    unknown_outcome,
    io_error,
    file_in_use,
};

enum class BasicDirectoryTransferPhase : std::uint8_t {
    enumerating,
    staging_ready,
    copying,
    verifying,
    publishing,
    retiring_source,
    deleting_source,
    completed,
};

struct BasicDirectoryTransferRequest {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::uint64_t operation_id{};
};

struct BasicDirectoryTransferRecoveryIdentity {
    std::uint64_t operation_id{};
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string source_revision_utf8;
};

struct BasicDirectoryTransferProgress {
    BasicDirectoryTransferPhase phase{BasicDirectoryTransferPhase::enumerating};
    std::size_t completed_entries{};
    std::size_t total_entries{};
    std::uint64_t completed_bytes{};
    std::uint64_t total_bytes{};
    std::filesystem::path current_path;
};

struct BasicDirectoryTransferResult {
    BasicDirectoryTransferStatus status{BasicDirectoryTransferStatus::io_error};
    std::error_code error;
    std::int64_t platform_code{};
    std::size_t completed_entries{};
    std::uint64_t completed_bytes{};
    std::filesystem::path staging_destination;
    std::filesystem::path destination;
    std::filesystem::path manifest_path;
    std::string detail_utf8;
    bool recovery_available{};

    [[nodiscard]] bool ok() const noexcept {
        return status == BasicDirectoryTransferStatus::success;
    }
};

struct BasicDirectoryTransferPlanResult : BasicDirectoryTransferResult {
    BasicDirectoryTransferManifest manifest;
};

struct BasicDirectoryTransferRecoveryScanResult : BasicDirectoryTransferResult {
    std::size_t inspected_entries{};
    std::size_t loaded_manifest_bytes{};
    std::size_t loaded_manifest_entries{};
    std::size_t deferred_candidates{};
    bool truncated{};
    std::vector<BasicDirectoryTransferPlanResult> candidates;
};

using BasicDirectoryTransferProgressCallback =
    std::function<void(const BasicDirectoryTransferProgress &)>;

[[nodiscard]] std::filesystem::path
basic_directory_transfer_manifest_path(const BasicDirectoryTransferManifest &manifest);
[[nodiscard]] bool
valid_basic_directory_transfer_manifest(const BasicDirectoryTransferManifest &manifest,
                                        std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_basic_directory_transfer_manifest(const BasicDirectoryTransferManifest &manifest);
[[nodiscard]] bool
decode_basic_directory_transfer_manifest(std::span<const std::byte> payload,
                                         BasicDirectoryTransferManifest &manifest,
                                         std::string &detail_utf8);
[[nodiscard]] BasicDirectoryTransferPlanResult
load_basic_directory_transfer_manifest(const std::filesystem::path &manifest_path);

[[nodiscard]] BasicDirectoryTransferPlanResult
plan_basic_directory_copy(const BasicDirectoryTransferRequest &request);
[[nodiscard]] BasicDirectoryTransferPlanResult
plan_basic_directory_move(const BasicDirectoryTransferRequest &request);
[[nodiscard]] BasicDirectoryTransferResult
execute_basic_directory_copy(const BasicDirectoryTransferManifest &manifest,
                             const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
execute_basic_directory_move(const BasicDirectoryTransferManifest &manifest,
                             const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
copy_directory_basic(const BasicDirectoryTransferRequest &request,
                     const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
move_directory_basic(const BasicDirectoryTransferRequest &request,
                     const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
reconcile_basic_directory_publication(const BasicDirectoryTransferManifest &manifest);
[[nodiscard]] BasicDirectoryTransferRecoveryScanResult
scan_basic_directory_recoveries(const std::filesystem::path &destination_parent);
[[nodiscard]] BasicDirectoryTransferResult
resume_basic_directory_copy(const std::filesystem::path &manifest_path,
                            const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
resume_basic_directory_move(const std::filesystem::path &manifest_path,
                            const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
resume_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
resume_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                const BasicDirectoryTransferRecoveryIdentity &expected_identity,
                                const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
discard_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                 const BasicDirectoryTransferProgressCallback &progress = {});
[[nodiscard]] BasicDirectoryTransferResult
discard_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                 const BasicDirectoryTransferRecoveryIdentity &expected_identity,
                                 const BasicDirectoryTransferProgressCallback &progress = {});

#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
enum class BasicDirectoryTransferTestFailurePoint : std::uint8_t {
    none,
    manifest_flush,
    staging_create,
    subdirectory_create,
    leaf_write,
    leaf_publish,
    final_audit,
    staging_namespace_flush,
    publication_proof_flush,
    publication_namespace_flush,
    publication_proof_read_disconnect,
    source_lock_unsupported,
    source_delete_lock_unsupported,
    source_marker_flush,
    atomic_move_volume_probe_disconnect,
    atomic_move_cross_device,
    atomic_move_proof_flush,
    atomic_move_marker_removal_flush,
    source_marker_read_disconnect,
    source_namespace_unsupported,
    source_retirement_namespace_flush,
    source_removal_namespace_flush,
    source_identity_disconnect,
    staging_identity_disconnect,
    directory_revision_disconnect,
    exception_after_staging_retirement,
    exception_after_manifest_retirement,
    exception_after_staging,
    exception_after_atomic_move_marker,
    exception_after_publication,
    exception_after_atomic_move_marker_removal,
    exception_after_source_retirement,
    unknown_outcome_after_source_entry_capture,
    unknown_outcome_after_source_entry_remove,
    exception_during_source_deletion,
    exception_during_reconciliation,
};

void basic_directory_transfer_test_fail_once(BasicDirectoryTransferTestFailurePoint point) noexcept;
[[nodiscard]] BasicDirectoryTransferResult execute_basic_directory_move_strict_for_test(
    const BasicDirectoryTransferManifest &manifest,
    const BasicDirectoryTransferProgressCallback &progress = {});
void basic_directory_transfer_test_lose_staging_reply_once() noexcept;
void basic_directory_transfer_test_lose_final_reply_once() noexcept;
void basic_directory_transfer_test_lose_source_retirement_reply_once() noexcept;
void basic_directory_transfer_test_pause_after_source_cleanup_validation_once() noexcept;
[[nodiscard]] bool basic_directory_transfer_test_source_cleanup_validation_is_paused() noexcept;
void basic_directory_transfer_test_release_source_cleanup_validation() noexcept;
void basic_directory_transfer_test_pause_before_cleanup_quarantine_once() noexcept;
[[nodiscard]] bool basic_directory_transfer_test_cleanup_quarantine_is_paused() noexcept;
void basic_directory_transfer_test_release_cleanup_quarantine() noexcept;
void basic_directory_transfer_test_pause_after_atomic_move_marker_check_once() noexcept;
[[nodiscard]] bool basic_directory_transfer_test_atomic_move_marker_check_is_paused() noexcept;
void basic_directory_transfer_test_release_atomic_move_marker_check() noexcept;
void basic_directory_transfer_test_pause_after_source_entry_capture_once() noexcept;
[[nodiscard]] bool basic_directory_transfer_test_source_entry_capture_is_paused() noexcept;
void basic_directory_transfer_test_release_source_entry_capture() noexcept;
void basic_directory_transfer_test_pause_after_staging_retirement_once() noexcept;
[[nodiscard]] bool basic_directory_transfer_test_staging_retirement_is_paused() noexcept;
void basic_directory_transfer_test_release_staging_retirement() noexcept;
void basic_directory_transfer_test_pause_after_manifest_retirement_once() noexcept;
[[nodiscard]] bool basic_directory_transfer_test_manifest_retirement_is_paused() noexcept;
void basic_directory_transfer_test_release_manifest_retirement() noexcept;
#endif

} // namespace vove::fileops
