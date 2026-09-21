#include "vove/fileops/file_transfer_transaction.hpp"

#include "vove/core/reserved_names.hpp"
#include "vove/fileops/current_operation_journal.hpp"

#include "catalog_protocol_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t transactionMagic = 0x31544656U;
constexpr std::size_t maximumPayloadBytes = kMaximumCurrentOperationPayloadBytes;

std::string path_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    const auto *first = reinterpret_cast<const char8_t *>(text.data());
    return std::filesystem::path(std::u8string(first, first + text.size()));
}

bool valid_utf8(const std::string_view text) noexcept {
    if (text.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t count{};
        std::uint32_t code_point{};
        if ((first & 0xe0U) == 0xc0U) {
            count = 1;
            code_point = first & 0x1fU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= count; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((count == 2 && code_point < 0x800U) || (count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        index += count + 1U;
    }
    return true;
}

std::string hexadecimal(const std::uint64_t value) {
    std::array<char, 32> encoded{};
    const auto [end, error] =
        std::to_chars(encoded.data(), encoded.data() + encoded.size(), value, 16);
    if (error != std::errc{}) {
        throw std::runtime_error("file-transfer identifier could not be generated");
    }
    return {encoded.data(), end};
}

std::filesystem::path transfer_path(const std::filesystem::path &user_path,
                                    const std::u8string_view prefix,
                                    const std::uint64_t operation_id, const std::size_t index) {
    const auto leaf = std::string(reinterpret_cast<const char *>(prefix.data()), prefix.size()) +
                      hexadecimal(operation_id) + '-' + hexadecimal(index) + ".part";
    return user_path.parent_path() / path_from_utf8(leaf);
}

bool snapshot_empty(const SourceSnapshot &snapshot) noexcept {
    return snapshot.size_bytes == 0 && snapshot.modified_unix_ns == 0 &&
           snapshot.source_revision_utf8.empty();
}

bool valid_snapshot(const SourceSnapshot &snapshot) {
    return !stable_object_identity(snapshot.source_revision_utf8).empty();
}

bool digest_empty(const std::array<std::uint8_t, kFileTransferDigestBytes> &digest) noexcept {
    return std::ranges::all_of(digest, [](const std::uint8_t value) { return value == 0; });
}

bool token_empty(const std::array<std::uint8_t, kFileTransferRequestTokenBytes> &token) noexcept {
    return std::ranges::all_of(token, [](const std::uint8_t value) { return value == 0; });
}

bool same_snapshot(const SourceSnapshot &left, const SourceSnapshot &right) {
    return left.size_bytes == right.size_bytes && left.modified_unix_ns == right.modified_unix_ns &&
           same_source_revision(left.source_revision_utf8, right.source_revision_utf8);
}

bool staged_source_snapshot_matches(const SourceSnapshot &original, const SourceSnapshot &current) {
    return same_object_after_rename(original, current);
}

bool streamed_strategy(const FileTransferStrategy strategy) noexcept {
    return strategy == FileTransferStrategy::copy_stream ||
           strategy == FileTransferStrategy::move_stream;
}

bool final_item(const FileTransferKind kind, const FileTransferItem &item) noexcept {
    return item.destination_state == FileTransferDestinationState::published &&
           item.overwrite_state == OverwriteDestinationState::none &&
           ((kind == FileTransferKind::copy &&
             item.source_location == FileTransferSourceLocation::original) ||
            (kind == FileTransferKind::move &&
             item.source_location == FileTransferSourceLocation::removed));
}

bool pristine_item(const FileTransferKind kind, const FileTransferItem &item) noexcept {
    const auto expected_strategy = kind == FileTransferKind::copy
                                       ? FileTransferStrategy::copy_stream
                                       : FileTransferStrategy::move_undecided;
    return item.strategy == expected_strategy &&
           item.source_location == FileTransferSourceLocation::original &&
           item.destination_state == FileTransferDestinationState::absent &&
           item.overwrite_state != OverwriteDestinationState::evacuated &&
           item.overwrite_state != OverwriteDestinationState::evacuated_unexpected &&
           snapshot_empty(item.overwrite_backup_snapshot) &&
           snapshot_empty(item.destination_snapshot) && item.content_bytes == 0 &&
           digest_empty(item.content_sha256) && token_empty(item.request_token) &&
           item.move_stream_reason == MoveStreamReason::none && !item.content_proof_present;
}

bool strategy_and_locations_valid(const FileTransferKind kind, const FileTransferItem &item) {
    if (kind == FileTransferKind::copy) {
        return item.strategy == FileTransferStrategy::copy_stream &&
               item.move_stream_reason == MoveStreamReason::none &&
               item.source_location == FileTransferSourceLocation::original;
    }
    if (item.strategy == FileTransferStrategy::copy_stream) {
        return false;
    }
    if (item.strategy == FileTransferStrategy::move_undecided) {
        return item.move_stream_reason == MoveStreamReason::none &&
               item.source_location == FileTransferSourceLocation::original &&
               item.destination_state == FileTransferDestinationState::absent;
    }
    if (item.strategy == FileTransferStrategy::move_atomic) {
        return item.move_stream_reason == MoveStreamReason::none &&
               item.source_location == FileTransferSourceLocation::removed &&
               item.destination_state == FileTransferDestinationState::published;
    }
    if (item.move_stream_reason != MoveStreamReason::cross_device_no_commit) {
        return false;
    }
    if (item.destination_state == FileTransferDestinationState::absent) {
        return item.source_location == FileTransferSourceLocation::original ||
               item.source_location == FileTransferSourceLocation::staged;
    }
    if (item.destination_state == FileTransferDestinationState::published) {
        return item.source_location == FileTransferSourceLocation::staged ||
               item.source_location == FileTransferSourceLocation::removed;
    }
    return item.source_location == FileTransferSourceLocation::staged;
}

bool destination_proof_valid(const FileTransferItem &item) {
    if (item.destination_state == FileTransferDestinationState::absent) {
        return snapshot_empty(item.destination_snapshot) && item.content_bytes == 0 &&
               digest_empty(item.content_sha256) && !item.content_proof_present;
    }
    if (!valid_snapshot(item.destination_snapshot)) {
        return false;
    }
    if (item.strategy == FileTransferStrategy::move_atomic) {
        return item.destination_state == FileTransferDestinationState::published &&
               item.content_bytes == 0 && digest_empty(item.content_sha256) &&
               !item.content_proof_present &&
               same_object_identity(item.original_source_snapshot.source_revision_utf8,
                                    item.destination_snapshot.source_revision_utf8);
    }
    if (item.destination_state == FileTransferDestinationState::temp_reserved) {
        return item.content_bytes == 0 && digest_empty(item.content_sha256) &&
               !item.content_proof_present && !token_empty(item.request_token);
    }
    return streamed_strategy(item.strategy) &&
           item.content_bytes == item.original_source_snapshot.size_bytes &&
           item.destination_snapshot.size_bytes == item.content_bytes && item.content_proof_present;
}

bool active_step_matches(const FileTransferTransaction &transaction) {
    if (transaction.active_step == FileTransferStep::none) {
        return transaction.active_index == 0;
    }
    if (transaction.active_index >= transaction.items.size()) {
        return false;
    }
    const auto &item = transaction.items[transaction.active_index];
    switch (transaction.active_step) {
    case FileTransferStep::atomic_move:
        return transaction.kind == FileTransferKind::move &&
               item.strategy == FileTransferStrategy::move_undecided &&
               item.source_location == FileTransferSourceLocation::original &&
               item.destination_state == FileTransferDestinationState::absent &&
               (item.overwrite_state == OverwriteDestinationState::none ||
                item.overwrite_state == OverwriteDestinationState::evacuated ||
                (transaction.version < 6 &&
                 item.overwrite_state == OverwriteDestinationState::authorized));
    case FileTransferStep::stage_source:
        return transaction.kind == FileTransferKind::move &&
               item.strategy == FileTransferStrategy::move_stream &&
               item.source_location == FileTransferSourceLocation::original &&
               item.destination_state == FileTransferDestinationState::absent;
    case FileTransferStep::reserve_temp:
        return streamed_strategy(item.strategy) &&
               item.destination_state == FileTransferDestinationState::absent &&
               !token_empty(item.request_token) &&
               (transaction.kind == FileTransferKind::copy ||
                item.source_location == FileTransferSourceLocation::staged);
    case FileTransferStep::stream_temp:
        return streamed_strategy(item.strategy) &&
               item.destination_state == FileTransferDestinationState::temp_reserved;
    case FileTransferStep::publish_temp:
        return streamed_strategy(item.strategy) &&
               item.destination_state == FileTransferDestinationState::content_ready &&
               (item.overwrite_state == OverwriteDestinationState::none ||
                item.overwrite_state == OverwriteDestinationState::evacuated ||
                (transaction.version < 6 &&
                 item.overwrite_state == OverwriteDestinationState::authorized));
    case FileTransferStep::delete_source:
        return transaction.kind == FileTransferKind::move &&
               item.strategy == FileTransferStrategy::move_stream &&
               item.source_location == FileTransferSourceLocation::staged &&
               item.destination_state == FileTransferDestinationState::published;
    case FileTransferStep::restore_staged_source:
        return transaction.kind == FileTransferKind::move &&
               item.strategy == FileTransferStrategy::move_stream &&
               item.source_location == FileTransferSourceLocation::staged &&
               item.destination_state != FileTransferDestinationState::published &&
               transaction.phase == FileTransferPhase::prepublish_cleanup;
    case FileTransferStep::cleanup_temp:
        return streamed_strategy(item.strategy) &&
               transaction.phase == FileTransferPhase::prepublish_cleanup &&
               (item.destination_state == FileTransferDestinationState::temp_reserved ||
                item.destination_state == FileTransferDestinationState::content_ready);
    case FileTransferStep::evacuate_overwrite_destination:
        return transaction.version >= 6 &&
               item.overwrite_state == OverwriteDestinationState::authorized &&
               item.destination_state != FileTransferDestinationState::published;
    case FileTransferStep::restore_overwrite_destination:
        return transaction.version >= 6 &&
               transaction.phase == FileTransferPhase::prepublish_cleanup &&
               (item.overwrite_state == OverwriteDestinationState::evacuated ||
                item.overwrite_state == OverwriteDestinationState::evacuated_unexpected) &&
               item.destination_state != FileTransferDestinationState::published;
    case FileTransferStep::cleanup_overwrite_destination:
        return transaction.version >= 6 &&
               item.overwrite_state == OverwriteDestinationState::evacuated &&
               item.destination_state == FileTransferDestinationState::published &&
               ((transaction.kind == FileTransferKind::copy &&
                 item.source_location == FileTransferSourceLocation::original) ||
                (transaction.kind == FileTransferKind::move &&
                 item.source_location == FileTransferSourceLocation::removed));
    case FileTransferStep::none:
        break;
    }
    return false;
}

bool valid_kind(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferKind::move);
}

bool valid_phase(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferPhase::completed);
}

bool valid_strategy(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferStrategy::move_atomic);
}

bool valid_move_stream_reason(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(MoveStreamReason::cross_device_no_commit);
}

bool valid_source_location(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferSourceLocation::removed);
}

bool valid_destination_state(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferDestinationState::published);
}

bool valid_overwrite_state(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OverwriteDestinationState::evacuated_unexpected);
}

bool valid_step(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferStep::cleanup_overwrite_destination);
}

bool valid_transaction_version(const std::uint32_t version) noexcept {
    return version == 4 || version == 5 || version == kFileTransferTransactionVersion;
}

bool valid_status(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationStatus::file_in_use);
}

bool valid_evidence(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationEvidence::conflicting);
}

} // namespace

std::filesystem::path file_transfer_staged_source_path(const std::filesystem::path &source,
                                                       const std::uint64_t operation_id,
                                                       const std::size_t index) {
    return transfer_path(source, core::kTransferSourceFilenamePrefix, operation_id, index);
}

std::filesystem::path file_transfer_temp_destination_path(const std::filesystem::path &destination,
                                                          const std::uint64_t operation_id,
                                                          const std::size_t index) {
    return transfer_path(destination, core::kTransferDestinationFilenamePrefix, operation_id,
                         index);
}

std::filesystem::path file_transfer_overwrite_backup_path(const std::filesystem::path &destination,
                                                          const std::uint64_t operation_id,
                                                          const std::size_t index) {
    return transfer_path(destination, core::kTransferOverwriteBackupFilenamePrefix, operation_id,
                         index);
}

bool valid_file_transfer_transaction(const FileTransferTransaction &transaction,
                                     std::string &detail_utf8) {
    if ((transaction.kind != FileTransferKind::copy &&
         transaction.kind != FileTransferKind::move) ||
        !valid_transaction_version(transaction.version) || transaction.operation_id == 0 ||
        (transaction.cancelled &&
         (transaction.version < 5 || transaction.phase != FileTransferPhase::prepublish_cleanup)) ||
        transaction.items.empty() || transaction.items.size() > kMaximumFileTransferItems ||
        !valid_status(static_cast<std::uint8_t>(transaction.failure_status)) ||
        !valid_evidence(static_cast<std::uint8_t>(transaction.active_evidence)) ||
        !valid_evidence(static_cast<std::uint8_t>(transaction.failure_evidence))) {
        detail_utf8 = "file-transfer transaction header is invalid";
        return false;
    }
    if (!active_step_matches(transaction) ||
        (transaction.active_step == FileTransferStep::none &&
         transaction.active_evidence != OperationEvidence::none) ||
        (transaction.failure_status == OperationStatus::success &&
         (transaction.active_evidence != OperationEvidence::none ||
          transaction.failure_evidence != OperationEvidence::none ||
          !transaction.failure_detail_utf8.empty()))) {
        detail_utf8 = "file-transfer active step or evidence is inconsistent";
        return false;
    }
    if ((transaction.phase == FileTransferPhase::prepared ||
         transaction.phase == FileTransferPhase::completed) &&
        transaction.active_step != FileTransferStep::none) {
        detail_utf8 = "file-transfer terminal phase has an active step";
        return false;
    }

    std::unordered_set<std::string> source_identities;
    std::unordered_set<std::string> owned_paths;
    bool saw_non_final{};
    std::size_t first_non_final{};
    for (std::size_t index{}; index < transaction.items.size(); ++index) {
        const auto &item = transaction.items[index];
        const auto overwrite_authorized = !snapshot_empty(item.overwrite_destination_snapshot);
        const auto overwrite_backup_present = !snapshot_empty(item.overwrite_backup_snapshot);
        if (!valid_overwrite_state(static_cast<std::uint8_t>(item.overwrite_state)) ||
            (item.overwrite_state == OverwriteDestinationState::none &&
             (overwrite_authorized || overwrite_backup_present)) ||
            (item.overwrite_state == OverwriteDestinationState::authorized &&
             (!overwrite_authorized || overwrite_backup_present)) ||
            (item.overwrite_state == OverwriteDestinationState::evacuated &&
             (!overwrite_authorized || !overwrite_backup_present ||
              !same_object_after_rename(item.overwrite_destination_snapshot,
                                        item.overwrite_backup_snapshot))) ||
            (item.overwrite_state == OverwriteDestinationState::evacuated_unexpected &&
             (!overwrite_authorized || !overwrite_backup_present))) {
            detail_utf8 = "file-transfer overwrite state is invalid";
            return false;
        }
        if (!snapshot_empty(item.overwrite_destination_snapshot) &&
            (transaction.version < 5 || !valid_snapshot(item.overwrite_destination_snapshot) ||
             item.overwrite_destination_snapshot.source_revision_utf8.starts_with("win-file:") ||
             !valid_utf8(item.overwrite_destination_snapshot.source_revision_utf8))) {
            detail_utf8 = "file-transfer overwrite authorization is invalid";
            return false;
        }
        if (!snapshot_empty(item.overwrite_backup_snapshot) &&
            (transaction.version < 6 || !valid_snapshot(item.overwrite_backup_snapshot) ||
             item.overwrite_backup_snapshot.source_revision_utf8.starts_with("win-file:") ||
             !valid_utf8(item.overwrite_backup_snapshot.source_revision_utf8))) {
            detail_utf8 = "file-transfer overwrite backup proof is invalid";
            return false;
        }
        const auto source_text = path_utf8(item.source.lexically_normal());
        const auto staged_text = path_utf8(item.staged_source.lexically_normal());
        const auto temp_text = path_utf8(item.temp_destination.lexically_normal());
        const auto overwrite_backup_text = path_utf8(item.overwrite_backup.lexically_normal());
        const auto destination_text = path_utf8(item.destination.lexically_normal());
        if (!item.source.is_absolute() || !item.staged_source.is_absolute() ||
            !item.temp_destination.is_absolute() || !item.overwrite_backup.is_absolute() ||
            !item.destination.is_absolute() || item.source == item.destination ||
            item.staged_source !=
                file_transfer_staged_source_path(item.source, transaction.operation_id, index) ||
            item.temp_destination != file_transfer_temp_destination_path(
                                         item.destination, transaction.operation_id, index) ||
            item.overwrite_backup != file_transfer_overwrite_backup_path(
                                         item.destination, transaction.operation_id, index) ||
            core::is_transfer_filename(item.source.filename().u8string()) ||
            core::is_transfer_filename(item.destination.filename().u8string()) ||
            item.source.filename().u8string().find(u8':') != std::u8string::npos ||
            !owned_paths.insert(source_text).second || !owned_paths.insert(staged_text).second ||
            !owned_paths.insert(temp_text).second ||
            !owned_paths.insert(overwrite_backup_text).second ||
            !owned_paths.insert(destination_text).second) {
            detail_utf8 = "file-transfer paths are invalid or duplicated";
            return false;
        }
        std::string filename_detail;
        if (!valid_destination_filename(item.destination.filename(), filename_detail) ||
            !valid_destination_filename(item.staged_source.filename(), filename_detail) ||
            !valid_destination_filename(item.temp_destination.filename(), filename_detail) ||
            !valid_destination_filename(item.overwrite_backup.filename(), filename_detail)) {
            detail_utf8 = std::move(filename_detail);
            return false;
        }
        const auto source_identity =
            stable_object_identity(item.original_source_snapshot.source_revision_utf8);
        const auto restored_streamed_move =
            transaction.kind == FileTransferKind::move &&
            item.strategy == FileTransferStrategy::move_stream &&
            item.source_location == FileTransferSourceLocation::original;
        const auto source_snapshot_valid =
            item.source_location == FileTransferSourceLocation::original && !restored_streamed_move
                ? same_snapshot(item.original_source_snapshot, item.current_source_snapshot)
                : staged_source_snapshot_matches(item.original_source_snapshot,
                                                 item.current_source_snapshot);
        if (source_identity.empty() || !source_identities.insert(source_identity).second) {
            detail_utf8 = "file-transfer source object identity is invalid or duplicated";
            return false;
        }
        if (!valid_snapshot(item.current_source_snapshot)) {
            detail_utf8 = "file-transfer current source identity is invalid";
            return false;
        }
        if (!source_snapshot_valid) {
            detail_utf8 = "file-transfer current source snapshot no longer matches its origin";
            return false;
        }
        if (item.original_source_snapshot.source_revision_utf8.starts_with("win-file:") ||
            item.current_source_snapshot.source_revision_utf8.starts_with("win-file:") ||
            (!snapshot_empty(item.destination_snapshot) &&
             item.destination_snapshot.source_revision_utf8.starts_with("win-file:"))) {
            detail_utf8 = "file-transfer snapshot uses a weak Windows identity";
            return false;
        }
        if (item.source_parent_identity_utf8.starts_with("win-file:") ||
            item.destination_parent_identity_utf8.starts_with("win-file:") ||
            item.source_parent_identity_utf8.empty() ||
            item.destination_parent_identity_utf8.empty() ||
            stable_object_identity(item.source_parent_revision_utf8) !=
                item.source_parent_identity_utf8 ||
            stable_object_identity(item.destination_parent_revision_utf8) !=
                item.destination_parent_identity_utf8) {
            detail_utf8 = "file-transfer parent identity is invalid";
            return false;
        }
        if (!strategy_and_locations_valid(transaction.kind, item)) {
            detail_utf8 = "file-transfer strategy or source location is invalid";
            return false;
        }
        if (!destination_proof_valid(item)) {
            detail_utf8 = "file-transfer destination proof is invalid";
            return false;
        }
        if (transaction.phase == FileTransferPhase::prepublish_cleanup &&
            item.destination_state == FileTransferDestinationState::published &&
            !final_item(transaction.kind, item)) {
            detail_utf8 = "file-transfer cleanup cannot restore a published move";
            return false;
        }
        if (streamed_strategy(item.strategy) &&
            item.destination_state != FileTransferDestinationState::absent &&
            token_empty(item.request_token)) {
            detail_utf8 = "file-transfer temporary object has no request token";
            return false;
        }

        const auto is_final = final_item(transaction.kind, item);
        if (!is_final && !saw_non_final) {
            saw_non_final = true;
            first_non_final = index;
        } else if (is_final && saw_non_final) {
            detail_utf8 = "file-transfer completed items are not a prefix";
            return false;
        } else if (saw_non_final && index > first_non_final &&
                   !pristine_item(transaction.kind, item)) {
            detail_utf8 = "file-transfer has more than one in-progress item";
            return false;
        }
    }

    for (const auto &item : transaction.items) {
        if (!snapshot_empty(item.overwrite_destination_snapshot) &&
            source_identities.contains(
                stable_object_identity(item.overwrite_destination_snapshot.source_revision_utf8))) {
            detail_utf8 = "file-transfer overwrite destination aliases a source";
            return false;
        }
    }
    const auto all_final = !saw_non_final;
    if (transaction.phase == FileTransferPhase::prepared &&
        !std::ranges::all_of(transaction.items, [&](const FileTransferItem &item) {
            return pristine_item(transaction.kind, item);
        })) {
        detail_utf8 = "prepared file-transfer transaction is not pristine";
        return false;
    }
    if ((transaction.phase == FileTransferPhase::completed) != all_final) {
        detail_utf8 = "file-transfer completion phase does not match item state";
        return false;
    }
    if (transaction.active_step != FileTransferStep::none &&
        transaction.active_index != first_non_final) {
        detail_utf8 = "file-transfer active item is not the first unfinished item";
        return false;
    }
    return true;
}

FileTransferTransaction
prepare_file_transfer_transaction(const FileTransferKind kind,
                                  const std::vector<FileTransferSource> &sources,
                                  const std::uint64_t operation_id) {
    if (sources.empty() || sources.size() > kMaximumFileTransferItems || operation_id == 0) {
        throw std::invalid_argument("file-transfer sources are invalid");
    }
    FileTransferTransaction transaction;
    transaction.operation_id = operation_id;
    transaction.kind = kind;
    transaction.items.reserve(sources.size());
    for (std::size_t index{}; index < sources.size(); ++index) {
        const auto &source = sources[index];
        transaction.items.push_back(
            {.source = source.path,
             .staged_source = file_transfer_staged_source_path(source.path, operation_id, index),
             .temp_destination =
                 file_transfer_temp_destination_path(source.destination, operation_id, index),
             .overwrite_backup =
                 file_transfer_overwrite_backup_path(source.destination, operation_id, index),
             .destination = source.destination,
             .original_source_snapshot = source.snapshot,
             .current_source_snapshot = source.snapshot,
             .destination_snapshot = {},
             .source_parent_identity_utf8 =
                 stable_object_identity(source.source_parent_revision_utf8),
             .destination_parent_identity_utf8 =
                 stable_object_identity(source.destination_parent_revision_utf8),
             .source_parent_revision_utf8 = source.source_parent_revision_utf8,
             .destination_parent_revision_utf8 = source.destination_parent_revision_utf8,
             .content_sha256 = {},
             .request_token = {},
             .content_bytes = 0,
             .move_stream_reason = MoveStreamReason::none,
             .content_proof_present = false,
             .strategy = kind == FileTransferKind::copy ? FileTransferStrategy::copy_stream
                                                        : FileTransferStrategy::move_undecided,
             .source_location = FileTransferSourceLocation::original,
             .destination_state = FileTransferDestinationState::absent});
    }
    std::string detail;
    if (!valid_file_transfer_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    static_cast<void>(encode_file_transfer_transaction(transaction));
    return transaction;
}

std::vector<std::byte>
encode_file_transfer_transaction(const FileTransferTransaction &transaction) {
    std::string detail;
    if (!valid_file_transfer_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> payload;
    payload.reserve(160U + transaction.items.size() * 448U);
    append_integer(payload, transactionMagic);
    append_integer(payload, transaction.version);
    append_integer(payload, transaction.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(transaction.kind));
    append_integer(payload, static_cast<std::uint8_t>(transaction.phase));
    append_integer(payload, static_cast<std::uint8_t>(transaction.active_step));
    append_integer(payload, static_cast<std::uint8_t>(transaction.failure_status));
    append_integer(payload, static_cast<std::uint8_t>(transaction.active_evidence));
    append_integer(payload, static_cast<std::uint8_t>(transaction.failure_evidence));
    append_integer(payload, static_cast<std::uint16_t>(transaction.cancelled ? 1 : 0));
    append_integer(payload, transaction.active_index);
    append_integer(payload, static_cast<std::uint32_t>(transaction.items.size()));
    append_string(payload, transaction.failure_detail_utf8);
    for (const auto &item : transaction.items) {
        append_integer(payload, static_cast<std::uint8_t>(item.strategy));
        append_integer(payload, static_cast<std::uint8_t>(item.source_location));
        append_integer(payload, static_cast<std::uint8_t>(item.destination_state));
        append_integer(payload, static_cast<std::uint8_t>(item.move_stream_reason));
        append_integer(payload, static_cast<std::uint8_t>(item.content_proof_present ? 1 : 0));
        append_integer(payload, transaction.version >= 6
                                    ? static_cast<std::uint8_t>(item.overwrite_state)
                                    : static_cast<std::uint8_t>(0));
        append_integer(payload, static_cast<std::uint16_t>(0));
        append_integer(payload, item.original_source_snapshot.size_bytes);
        append_integer(payload, item.original_source_snapshot.modified_unix_ns);
        append_integer(payload, item.current_source_snapshot.size_bytes);
        append_integer(payload, item.current_source_snapshot.modified_unix_ns);
        append_integer(payload, item.destination_snapshot.size_bytes);
        append_integer(payload, item.destination_snapshot.modified_unix_ns);
        append_integer(payload, item.content_bytes);
        for (const auto value : item.content_sha256) {
            append_integer(payload, value);
        }
        for (const auto value : item.request_token) {
            append_integer(payload, value);
        }
        append_string(payload, path_utf8(item.source));
        append_string(payload, path_utf8(item.staged_source));
        append_string(payload, path_utf8(item.temp_destination));
        append_string(payload, path_utf8(item.destination));
        append_string(payload, item.original_source_snapshot.source_revision_utf8);
        append_string(payload, item.current_source_snapshot.source_revision_utf8);
        append_string(payload, item.destination_snapshot.source_revision_utf8);
        append_string(payload, item.source_parent_identity_utf8);
        append_string(payload, item.destination_parent_identity_utf8);
        append_string(payload, item.source_parent_revision_utf8);
        append_string(payload, item.destination_parent_revision_utf8);
        if (transaction.version >= 5) {
            append_integer(payload, item.overwrite_destination_snapshot.size_bytes);
            append_integer(payload, item.overwrite_destination_snapshot.modified_unix_ns);
            append_string(payload, item.overwrite_destination_snapshot.source_revision_utf8);
        }
        if (transaction.version >= 6) {
            append_integer(payload, item.overwrite_backup_snapshot.size_bytes);
            append_integer(payload, item.overwrite_backup_snapshot.modified_unix_ns);
            append_string(payload, item.overwrite_backup_snapshot.source_revision_utf8);
        }
    }
    if (payload.size() > maximumPayloadBytes) {
        throw std::length_error("file-transfer transaction exceeds the journal limit");
    }
    return payload;
}

bool decode_file_transfer_transaction(const std::span<const std::byte> payload,
                                      FileTransferTransaction &transaction,
                                      std::string &detail_utf8) {
    if (payload.size() > maximumPayloadBytes) {
        detail_utf8 = "file-transfer transaction exceeds the journal limit";
        return false;
    }
    Cursor cursor(payload);
    FileTransferTransaction decoded;
    std::uint32_t magic{};
    std::uint8_t kind{};
    std::uint8_t phase{};
    std::uint8_t step{};
    std::uint8_t status{};
    std::uint8_t active_evidence{};
    std::uint8_t failure_evidence{};
    std::uint16_t reserved16{};
    std::uint32_t item_count{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(kind) || !cursor.read(phase) ||
        !cursor.read(step) || !cursor.read(status) || !cursor.read(active_evidence) ||
        !cursor.read(failure_evidence) || !cursor.read(reserved16) ||
        !cursor.read(decoded.active_index) || !cursor.read(item_count) ||
        !cursor.read_string(decoded.failure_detail_utf8) || magic != transactionMagic ||
        !valid_kind(kind) || !valid_phase(phase) || !valid_step(step) || !valid_status(status) ||
        !valid_evidence(active_evidence) || !valid_evidence(failure_evidence) ||
        !valid_transaction_version(decoded.version) ||
        reserved16 > (decoded.version >= 5 ? 1U : 0U) || item_count == 0 ||
        item_count > kMaximumFileTransferItems) {
        detail_utf8 = "file-transfer transaction header is invalid or truncated";
        return false;
    }
    decoded.kind = static_cast<FileTransferKind>(kind);
    decoded.phase = static_cast<FileTransferPhase>(phase);
    decoded.active_step = static_cast<FileTransferStep>(step);
    decoded.failure_status = static_cast<OperationStatus>(status);
    decoded.active_evidence = static_cast<OperationEvidence>(active_evidence);
    decoded.failure_evidence = static_cast<OperationEvidence>(failure_evidence);
    decoded.cancelled = reserved16 != 0;
    decoded.items.reserve(item_count);
    for (std::uint32_t index{}; index < item_count; ++index) {
        FileTransferItem item;
        std::uint8_t strategy{};
        std::uint8_t source_location{};
        std::uint8_t destination_state{};
        std::uint8_t move_stream_reason{};
        std::uint8_t content_proof_present{};
        std::uint8_t item_reserved8{};
        std::uint16_t item_reserved16{};
        std::string source;
        std::string staged_source;
        std::string temp_destination;
        std::string destination;
        if (!cursor.read(strategy) || !cursor.read(source_location) ||
            !cursor.read(destination_state) || !cursor.read(move_stream_reason) ||
            !cursor.read(content_proof_present) || !cursor.read(item_reserved8) ||
            !cursor.read(item_reserved16) ||
            !cursor.read(item.original_source_snapshot.size_bytes) ||
            !cursor.read(item.original_source_snapshot.modified_unix_ns) ||
            !cursor.read(item.current_source_snapshot.size_bytes) ||
            !cursor.read(item.current_source_snapshot.modified_unix_ns) ||
            !cursor.read(item.destination_snapshot.size_bytes) ||
            !cursor.read(item.destination_snapshot.modified_unix_ns) ||
            !cursor.read(item.content_bytes) || !valid_strategy(strategy) ||
            !valid_source_location(source_location) ||
            !valid_destination_state(destination_state) ||
            !valid_move_stream_reason(move_stream_reason) || content_proof_present > 1 ||
            (decoded.version >= 6 ? !valid_overwrite_state(item_reserved8) : item_reserved8 != 0) ||
            item_reserved16 != 0) {
            detail_utf8 = "file-transfer transaction item header is invalid or truncated";
            return false;
        }
        for (auto &value : item.content_sha256) {
            if (!cursor.read(value)) {
                detail_utf8 = "file-transfer transaction digest is truncated";
                return false;
            }
        }
        for (auto &value : item.request_token) {
            if (!cursor.read(value)) {
                detail_utf8 = "file-transfer transaction request token is truncated";
                return false;
            }
        }
        if (!cursor.read_string(source) || !cursor.read_string(staged_source) ||
            !cursor.read_string(temp_destination) || !cursor.read_string(destination) ||
            !cursor.read_string(item.original_source_snapshot.source_revision_utf8) ||
            !cursor.read_string(item.current_source_snapshot.source_revision_utf8) ||
            !cursor.read_string(item.destination_snapshot.source_revision_utf8) ||
            !cursor.read_string(item.source_parent_identity_utf8) ||
            !cursor.read_string(item.destination_parent_identity_utf8) ||
            !cursor.read_string(item.source_parent_revision_utf8) ||
            !cursor.read_string(item.destination_parent_revision_utf8) || !valid_utf8(source) ||
            !valid_utf8(staged_source) || !valid_utf8(temp_destination) ||
            !valid_utf8(destination) ||
            !valid_utf8(item.original_source_snapshot.source_revision_utf8) ||
            !valid_utf8(item.current_source_snapshot.source_revision_utf8) ||
            !valid_utf8(item.destination_snapshot.source_revision_utf8) ||
            !valid_utf8(item.source_parent_identity_utf8) ||
            !valid_utf8(item.destination_parent_identity_utf8) ||
            !valid_utf8(item.source_parent_revision_utf8) ||
            !valid_utf8(item.destination_parent_revision_utf8)) {
            detail_utf8 = "file-transfer transaction item text is invalid or truncated";
            return false;
        }
        if (decoded.version >= 5 &&
            (!cursor.read(item.overwrite_destination_snapshot.size_bytes) ||
             !cursor.read(item.overwrite_destination_snapshot.modified_unix_ns) ||
             !cursor.read_string(item.overwrite_destination_snapshot.source_revision_utf8))) {
            detail_utf8 = "file-transfer overwrite authorization is truncated";
            return false;
        }
        if (decoded.version >= 6 &&
            (!cursor.read(item.overwrite_backup_snapshot.size_bytes) ||
             !cursor.read(item.overwrite_backup_snapshot.modified_unix_ns) ||
             !cursor.read_string(item.overwrite_backup_snapshot.source_revision_utf8))) {
            detail_utf8 = "file-transfer overwrite backup proof is truncated";
            return false;
        }
        item.source = path_from_utf8(source);
        item.staged_source = path_from_utf8(staged_source);
        item.temp_destination = path_from_utf8(temp_destination);
        item.destination = path_from_utf8(destination);
        item.overwrite_backup = file_transfer_overwrite_backup_path(
            item.destination, decoded.operation_id, static_cast<std::size_t>(index));
        item.strategy = static_cast<FileTransferStrategy>(strategy);
        item.move_stream_reason = static_cast<MoveStreamReason>(move_stream_reason);
        item.content_proof_present = content_proof_present != 0;
        item.source_location = static_cast<FileTransferSourceLocation>(source_location);
        item.destination_state = static_cast<FileTransferDestinationState>(destination_state);
        item.overwrite_state = decoded.version >= 6
                                   ? static_cast<OverwriteDestinationState>(item_reserved8)
                                   : (snapshot_empty(item.overwrite_destination_snapshot)
                                          ? OverwriteDestinationState::none
                                          : OverwriteDestinationState::authorized);
        decoded.items.push_back(std::move(item));
    }
    if (cursor.remaining() != 0 || !valid_utf8(decoded.failure_detail_utf8) ||
        !valid_file_transfer_transaction(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "file-transfer transaction contains trailing or invalid data";
        }
        return false;
    }
    transaction = std::move(decoded);
    return true;
}

} // namespace vove::fileops
