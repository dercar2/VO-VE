#pragma once

#include "vove/fileops/file_operation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kFileTransferTransactionVersion = 6;
inline constexpr std::size_t kMaximumFileTransferItems = 10'000;
inline constexpr std::size_t kFileTransferDigestBytes = 32;
inline constexpr std::size_t kFileTransferRequestTokenBytes = 16;

enum class FileTransferKind : std::uint8_t {
    copy,
    move,
};

enum class FileTransferPhase : std::uint8_t {
    prepared,
    processing,
    prepublish_cleanup,
    completed,
};

enum class FileTransferStrategy : std::uint8_t {
    copy_stream,
    move_undecided,
    move_stream,
    move_atomic,
};

enum class MoveStreamReason : std::uint8_t {
    none,
    cross_device_no_commit,
};

enum class FileTransferSourceLocation : std::uint8_t {
    original,
    staged,
    removed,
};

enum class FileTransferDestinationState : std::uint8_t {
    absent,
    temp_reserved,
    content_ready,
    published,
};

enum class OverwriteDestinationState : std::uint8_t {
    none,
    authorized,
    evacuated,
    evacuated_unexpected,
};

enum class FileTransferStep : std::uint8_t {
    none,
    atomic_move,
    stage_source,
    reserve_temp,
    stream_temp,
    publish_temp,
    delete_source,
    restore_staged_source,
    cleanup_temp,
    evacuate_overwrite_destination,
    restore_overwrite_destination,
    cleanup_overwrite_destination,
};

struct FileTransferSource {
    std::filesystem::path path;
    std::filesystem::path destination;
    SourceSnapshot snapshot;
    std::string source_parent_revision_utf8;
    std::string destination_parent_revision_utf8;
    SourceSnapshot authorized_overwrite_destination{};
};

struct FileTransferItem {
    std::filesystem::path source;
    std::filesystem::path staged_source;
    std::filesystem::path temp_destination;
    std::filesystem::path overwrite_backup;
    std::filesystem::path destination;
    SourceSnapshot original_source_snapshot;
    SourceSnapshot current_source_snapshot;
    SourceSnapshot destination_snapshot;
    std::string source_parent_identity_utf8;
    std::string destination_parent_identity_utf8;
    std::string source_parent_revision_utf8;
    std::string destination_parent_revision_utf8;
    std::array<std::uint8_t, kFileTransferDigestBytes> content_sha256{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
    std::uint64_t content_bytes{};
    MoveStreamReason move_stream_reason{MoveStreamReason::none};
    bool content_proof_present{};
    FileTransferStrategy strategy{FileTransferStrategy::copy_stream};
    FileTransferSourceLocation source_location{FileTransferSourceLocation::original};
    FileTransferDestinationState destination_state{FileTransferDestinationState::absent};
    SourceSnapshot overwrite_destination_snapshot{};
    SourceSnapshot overwrite_backup_snapshot{};
    OverwriteDestinationState overwrite_state{OverwriteDestinationState::none};
};

struct FileTransferTransaction {
    std::uint32_t version{kFileTransferTransactionVersion};
    std::uint64_t operation_id{};
    FileTransferKind kind{FileTransferKind::copy};
    FileTransferPhase phase{FileTransferPhase::prepared};
    FileTransferStep active_step{FileTransferStep::none};
    std::uint32_t active_index{};
    OperationEvidence active_evidence{OperationEvidence::none};
    OperationStatus failure_status{OperationStatus::success};
    OperationEvidence failure_evidence{OperationEvidence::none};
    std::string failure_detail_utf8;
    std::vector<FileTransferItem> items;
    bool cancelled{};
};

[[nodiscard]] bool valid_file_transfer_transaction(const FileTransferTransaction &transaction,
                                                   std::string &detail_utf8);
[[nodiscard]] FileTransferTransaction
prepare_file_transfer_transaction(FileTransferKind kind,
                                  const std::vector<FileTransferSource> &sources,
                                  std::uint64_t operation_id);
[[nodiscard]] std::filesystem::path
file_transfer_staged_source_path(const std::filesystem::path &source, std::uint64_t operation_id,
                                 std::size_t index);
[[nodiscard]] std::filesystem::path
file_transfer_temp_destination_path(const std::filesystem::path &destination,
                                    std::uint64_t operation_id, std::size_t index);
[[nodiscard]] std::filesystem::path
file_transfer_overwrite_backup_path(const std::filesystem::path &destination,
                                    std::uint64_t operation_id, std::size_t index);
[[nodiscard]] std::vector<std::byte>
encode_file_transfer_transaction(const FileTransferTransaction &transaction);
[[nodiscard]] bool decode_file_transfer_transaction(std::span<const std::byte> payload,
                                                    FileTransferTransaction &transaction,
                                                    std::string &detail_utf8);

} // namespace vove::fileops
