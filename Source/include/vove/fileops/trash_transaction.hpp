#pragma once

#include "vove/fileops/basic_directory_transfer.hpp"
#include "vove/fileops/file_operation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kTrashTransactionVersion = 9;
inline constexpr std::size_t kMaximumTrashItems = 10'000;
inline constexpr std::size_t kMaximumTrashDirectoryEntries = 10'000;

enum class TrashItemKind : std::uint8_t {
    regular_file,
    directory,
};

enum class TrashPhase : std::uint8_t {
    prepared,
    moving,
    rollback,
    rollback_container_remove_intent,
    manifest_intent,
    published,
    restore_prepared,
    restoring,
    restore_rollback,
    manifest_remove_intent,
    restore_container_remove_intent,
    restored,
    purge_prepared,
    purging,
    purge_manifest_remove_intent,
    purge_container_remove_intent,
    purged,
};

enum class TrashItemLocation : std::uint8_t {
    source,
    stored,
    deleted,
};

enum class TrashSecurityState : std::uint8_t {
    original,
    hardened,
};

enum class TrashStep : std::uint8_t {
    none,
    store,
    restore,
    purge,
};

struct TrashSource {
    std::filesystem::path path;
    SourceSnapshot snapshot;
    TrashItemKind kind{TrashItemKind::regular_file};
    std::uint64_t payload_bytes{};
    std::vector<BasicDirectoryTransferEntry> directory_entries;
    std::vector<std::string> directory_security_descriptors_sddl_utf8;
    std::string storage_identity_utf8;
    std::string original_security_descriptor_sddl_utf8;
    std::filesystem::path restore_path;
};

struct TrashItem {
    // User-facing path retained exactly as selected. Filesystem mutations use restore_path.
    std::filesystem::path original;
    std::filesystem::path restore_path;
    std::filesystem::path stored;
    std::filesystem::path current;
    SourceSnapshot current_snapshot;
    TrashItemKind kind{TrashItemKind::regular_file};
    std::uint64_t payload_bytes{};
    std::vector<BasicDirectoryTransferEntry> directory_entries;
    std::vector<std::string> directory_security_descriptors_sddl_utf8;
    std::uint32_t directory_purge_cursor{};
    std::string storage_identity_utf8;
    TrashItemLocation location{TrashItemLocation::source};
    TrashSecurityState security_state{TrashSecurityState::original};
    std::string original_security_descriptor_sddl_utf8;
};

struct TrashTransaction {
    std::uint32_t version{kTrashTransactionVersion};
    std::uint64_t operation_id{};
    std::int64_t created_unix_ns{};
    TrashPhase phase{TrashPhase::prepared};
    TrashStep active_step{TrashStep::none};
    std::uint32_t active_index{};
    OperationEvidence active_evidence{OperationEvidence::none};
    OperationStatus failure_status{OperationStatus::success};
    std::string failure_detail_utf8;
    std::vector<TrashItem> items;
};

[[nodiscard]] bool valid_trash_transaction(const TrashTransaction &transaction,
                                           std::string &detail_utf8);
[[nodiscard]] bool trusted_posix_trash_storage(const TrashTransaction &transaction,
                                               bool allow_restore_rebase = false);
[[nodiscard]] TrashTransaction
prepare_trash_transaction(const std::vector<TrashSource> &sources, std::uint64_t operation_id,
                          std::int64_t created_unix_ns,
                          const std::filesystem::path &vault_root = {});
[[nodiscard]] std::filesystem::path trash_container_path(const std::filesystem::path &source_parent,
                                                         std::uint64_t operation_id);
[[nodiscard]] std::filesystem::path trash_rescue_manifest_path(const TrashTransaction &transaction);
[[nodiscard]] std::filesystem::path trash_rescue_plan_path(const TrashTransaction &transaction);
[[nodiscard]] std::vector<std::byte> encode_trash_transaction(const TrashTransaction &transaction);
[[nodiscard]] bool decode_trash_transaction(std::span<const std::byte> payload,
                                            TrashTransaction &transaction,
                                            std::string &detail_utf8);

} // namespace vove::fileops
