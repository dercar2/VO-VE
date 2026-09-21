#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace vove::fileops {

inline constexpr std::uint32_t kFileOperationProtocolVersion = 16;
inline constexpr std::size_t kMaximumOperationPayloadBytes = 64U * 1024U;

enum class OperationStatus : std::uint8_t {
    success,
    invalid_request,
    not_found,
    conflict,
    source_changed,
    permission_denied,
    authentication_required,
    disconnected,
    timed_out,
    unsupported,
    unknown_outcome,
    io_error,
    cross_device,
    file_in_use,
};

enum class OperationEvidence : std::uint8_t {
    none,
    committed,
    no_commit,
    conflicting,
};

enum class RenameAction : std::uint8_t {
    execute,
    reconcile_only,
};

enum class OperationObjectKind : std::uint8_t {
    regular_file,
    directory,
};

enum class RenameMode : std::uint8_t {
    single_preserve_extension,
    batch_internal,
    trash_internal,
    trash_restore,
    transfer_atomic,
    transfer_stage,
    transfer_publish,
    transfer_restore,
    single_allow_extension_change,
    transfer_publish_replace,
    transfer_atomic_replace,
    transfer_overwrite_stage,
    transfer_overwrite_restore,
};

enum class DeleteAction : std::uint8_t {
    execute,
    reconcile_only,
};

enum class DeleteMode : std::uint8_t {
    permanent_remote,
    trash_purge,
    transfer_temp_cleanup,
    transfer_source_commit,
    transfer_overwrite_cleanup,
};

enum class DeleteTargetKind : std::uint8_t {
    local,
    remote,
    unknown,
};

struct DeleteTargetClassification {
    DeleteTargetKind kind{DeleteTargetKind::unknown};
    OperationStatus failure_status{OperationStatus::success};
    std::uint32_t platform_code{};
};

enum class CreateDirectoryAction : std::uint8_t {
    execute,
    reconcile_only,
};

enum class CreateDirectoryMode : std::uint8_t {
    user_visible,
    trash_internal,
    trash_internal_remove_empty,
};

struct SourceSnapshot {
    std::uint64_t size_bytes{};
    std::int64_t modified_unix_ns{};
    std::string source_revision_utf8;
};

struct RenameRequest {
    std::uint64_t operation_id{};
    RenameAction action{RenameAction::execute};
    RenameMode mode{RenameMode::single_preserve_extension};
    OperationObjectKind object_kind{OperationObjectKind::regular_file};
    std::filesystem::path source;
    std::filesystem::path destination;
    SourceSnapshot expected_source;
    // POSIX Trash uses the exact linux-smb mount identity here; other modes use parent identity.
    std::string source_parent_identity_utf8;
    std::string destination_parent_identity_utf8;
    std::filesystem::path destination_anchor_path;
    std::string destination_anchor_identity_utf8;
    SourceSnapshot expected_destination{};
};

struct DeleteRequest {
    std::uint64_t operation_id{};
    DeleteAction action{DeleteAction::execute};
    DeleteMode mode{DeleteMode::permanent_remote};
    OperationObjectKind object_kind{OperationObjectKind::regular_file};
    std::filesystem::path source;
    SourceSnapshot expected_source;
    // POSIX Trash uses the exact linux-smb mount identity here; other modes use parent identity.
    std::string source_parent_identity_utf8;
    std::filesystem::path guard_path;
    SourceSnapshot expected_guard;
    std::string guard_parent_identity_utf8;
};

struct CreateDirectoryRequest {
    std::uint64_t operation_id{};
    CreateDirectoryAction action{CreateDirectoryAction::execute};
    CreateDirectoryMode mode{CreateDirectoryMode::user_visible};
    std::filesystem::path destination;
    std::string destination_parent_revision_utf8;
};

struct OperationResult {
    std::uint64_t operation_id{};
    OperationStatus status{OperationStatus::io_error};
    OperationEvidence evidence{OperationEvidence::none};
    std::int64_t platform_code{};
    bool source_present{};
    bool source_matches_expected{};
    bool destination_present{};
    bool destination_matches_source{};
    SourceSnapshot confirmed_snapshot;
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == OperationStatus::success;
    }
};

[[nodiscard]] std::string stable_object_identity(std::string_view source_revision);
[[nodiscard]] bool same_object_identity(std::string_view left, std::string_view right);
[[nodiscard]] bool same_source_revision(std::string_view left, std::string_view right);
[[nodiscard]] bool same_object_after_rename(const SourceSnapshot &before,
                                            const SourceSnapshot &after);
[[nodiscard]] bool same_content_after_rename(const SourceSnapshot &before,
                                             const SourceSnapshot &after);
[[nodiscard]] bool
same_private_trash_payload_after_remount(const SourceSnapshot &before, const SourceSnapshot &after,
                                         std::string_view expected_storage_identity,
                                         std::string_view observed_storage_identity) noexcept;
[[nodiscard]] bool valid_destination_filename(const std::filesystem::path &path,
                                              std::string &detail_utf8);
[[nodiscard]] bool valid_rename_request(const RenameRequest &request, std::string &detail_utf8);
[[nodiscard]] bool valid_delete_request(const DeleteRequest &request, std::string &detail_utf8);
[[nodiscard]] bool valid_create_directory_request(const CreateDirectoryRequest &request,
                                                  std::string &detail_utf8);
[[nodiscard]] DeleteTargetClassification
inspect_delete_target(const std::filesystem::path &path) noexcept;
[[nodiscard]] DeleteTargetKind classify_delete_target(const std::filesystem::path &path) noexcept;

} // namespace vove::fileops
