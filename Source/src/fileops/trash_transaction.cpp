#include "vove/fileops/trash_transaction.hpp"

#include "vove/core/reserved_names.hpp"

#include "delete_identity.hpp"
#ifdef _WIN32
#include "windows/trash_security.hpp"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace vove::fileops {
namespace {

constexpr std::uint32_t transactionMagic = 0x31525456U;
constexpr std::uint32_t previousTransactionVersion = 8;
constexpr std::uint32_t legacyTransactionVersion = 7;
constexpr std::uint32_t directoryTransactionVersion = 9;
constexpr std::size_t maximumPayloadBytes = std::size_t{4} * 1024U * 1024U;

template <typename Integer>
void append_integer(std::vector<std::byte> &output, const Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    auto encoded = static_cast<Unsigned>(value);
    for (std::size_t offset{}; offset < sizeof(Integer); ++offset) {
        output.push_back(std::byte{static_cast<unsigned char>(encoded & 0xffU)});
        if constexpr (sizeof(Integer) > 1U) {
            if (offset + 1U < sizeof(Integer)) {
                encoded >>= 8U;
            }
        }
    }
}

void append_string(std::vector<std::byte> &output, const std::string_view text) {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("trash transaction string is too large");
    }
    append_integer(output, static_cast<std::uint32_t>(text.size()));
    const auto *first = reinterpret_cast<const std::byte *>(text.data());
    output.insert(output.end(), first, first + text.size());
}

class Cursor final {
  public:
    explicit Cursor(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename Integer> bool read(Integer &value) {
        if (remaining() < sizeof(Integer)) {
            return false;
        }
        using Unsigned = std::make_unsigned_t<Integer>;
        std::uint64_t decoded{};
        for (std::size_t offset{}; offset < sizeof(Integer); ++offset) {
            decoded |= static_cast<std::uint64_t>(
                           std::to_integer<unsigned char>(bytes_[position_ + offset]))
                       << (offset * 8U);
        }
        value = static_cast<Integer>(static_cast<Unsigned>(decoded));
        position_ += sizeof(Integer);
        return true;
    }

    bool read_string(std::string &value) {
        std::uint32_t size{};
        if (!read(size) || remaining() < size) {
            return false;
        }
        const auto *first = reinterpret_cast<const char *>(bytes_.data() + position_);
        value.assign(first, first + size);
        position_ += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

  private:
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};

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
    std::array<char, 32> operation{};
    const auto [operation_end, operation_error] =
        std::to_chars(operation.data(), operation.data() + operation.size(), value, 16);
    if (operation_error != std::errc{}) {
        throw std::runtime_error("trash identifier could not be generated");
    }
    return {operation.data(), operation_end};
}

std::filesystem::path stored_path(const std::filesystem::path &source,
                                  const std::uint64_t operation_id,
                                  const std::filesystem::path &vault_root,
                                  const std::size_t index) {
    std::array<char, 32> item{};
    const auto [item_end, item_error] =
        std::to_chars(item.data(), item.data() + item.size(), index, 16);
    if (item_error != std::errc{}) {
        throw std::runtime_error("trash filename could not be generated");
    }
    const auto prefix = core::kTrashItemFilenamePrefix;
    const auto leaf = std::string(reinterpret_cast<const char *>(prefix.data()), prefix.size()) +
                      std::string(item.data(), item_end);
    return trash_container_path(vault_root.empty() ? source.parent_path() : vault_root,
                                operation_id) /
           path_from_utf8(leaf);
}

bool expected_current(const TrashItem &item) {
    switch (item.location) {
    case TrashItemLocation::source:
        return item.current == item.restore_path;
    case TrashItemLocation::stored:
        return item.current == item.stored;
    case TrashItemLocation::deleted:
        return item.current.empty();
    }
    return false;
}

bool location_allowed(const TrashPhase phase, const TrashItemLocation location) {
    switch (phase) {
    case TrashPhase::prepared:
        return location == TrashItemLocation::source;
    case TrashPhase::moving:
    case TrashPhase::rollback:
        return location == TrashItemLocation::source || location == TrashItemLocation::stored;
    case TrashPhase::rollback_container_remove_intent:
        return location == TrashItemLocation::source;
    case TrashPhase::manifest_intent:
    case TrashPhase::published:
    case TrashPhase::restore_prepared:
        return location == TrashItemLocation::stored;
    case TrashPhase::restoring:
    case TrashPhase::restore_rollback:
        return location == TrashItemLocation::source || location == TrashItemLocation::stored;
    case TrashPhase::manifest_remove_intent:
    case TrashPhase::restore_container_remove_intent:
    case TrashPhase::restored:
        return location == TrashItemLocation::source;
    case TrashPhase::purge_prepared:
        return location == TrashItemLocation::stored;
    case TrashPhase::purging:
        return location == TrashItemLocation::stored || location == TrashItemLocation::deleted;
    case TrashPhase::purge_manifest_remove_intent:
    case TrashPhase::purge_container_remove_intent:
    case TrashPhase::purged:
        return location == TrashItemLocation::deleted;
    }
    return false;
}

bool active_step_matches(const TrashTransaction &transaction) {
    if (transaction.active_step == TrashStep::none) {
        return transaction.active_index == 0;
    }
    const auto location = transaction.items[transaction.active_index].location;
    if (transaction.active_step == TrashStep::store) {
        return (transaction.phase == TrashPhase::moving && location == TrashItemLocation::source) ||
               (transaction.phase == TrashPhase::restore_rollback &&
                location == TrashItemLocation::source);
    }
    if (transaction.active_step == TrashStep::restore) {
        return (transaction.phase == TrashPhase::rollback &&
                location == TrashItemLocation::stored) ||
               (transaction.phase == TrashPhase::restoring &&
                location == TrashItemLocation::stored);
    }
    return transaction.active_step == TrashStep::purge &&
           transaction.phase == TrashPhase::purging && location == TrashItemLocation::stored;
}

bool valid_relative_directory_entry_path(const std::filesystem::path &path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    for (const auto &component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

bool purge_order_before(const BasicDirectoryTransferEntry &left,
                        const BasicDirectoryTransferEntry &right) {
    if (left.depth != right.depth) {
        return left.depth > right.depth;
    }
    return left.relative_path.generic_u8string() > right.relative_path.generic_u8string();
}

bool valid_directory_payload(const TrashItem &item, std::string &detail_utf8) {
    if (item.kind == TrashItemKind::regular_file) {
        if (!item.directory_entries.empty() ||
            !item.directory_security_descriptors_sddl_utf8.empty() ||
            item.directory_purge_cursor != 0 ||
            item.payload_bytes != item.current_snapshot.size_bytes) {
            detail_utf8 = "regular trash item carries directory state";
            return false;
        }
        return true;
    }
    if (item.current_snapshot.size_bytes != 0 ||
        item.directory_entries.size() > kMaximumTrashDirectoryEntries ||
        (!item.directory_security_descriptors_sddl_utf8.empty() &&
         item.directory_security_descriptors_sddl_utf8.size() != item.directory_entries.size()) ||
        item.directory_purge_cursor > item.directory_entries.size()) {
        detail_utf8 = "trash directory state exceeds its bounds";
        return false;
    }
    if (item.location != TrashItemLocation::stored && item.directory_purge_cursor != 0) {
        detail_utf8 = "trash directory purge cursor is inconsistent with its location";
        return false;
    }
    std::unordered_set<std::string> paths;
    std::unordered_set<std::string> identities;
    std::uint64_t total_bytes{};
    const BasicDirectoryTransferEntry *previous{};
    for (const auto &entry : item.directory_entries) {
        const auto relative = path_utf8(entry.relative_path.lexically_normal());
        const auto identity = stable_object_identity(entry.source_revision_utf8);
        const auto directory = entry.kind == BasicDirectoryTransferEntryKind::directory;
        if (!valid_relative_directory_entry_path(entry.relative_path) || relative.empty() ||
            !valid_utf8(relative) || entry.depth == 0 ||
            entry.depth > kBasicDirectoryTransferMaximumDepth || identity.empty() ||
            !identities.insert(identity).second || !paths.insert(relative).second ||
            (directory && entry.size_bytes != 0) ||
            (!directory &&
             entry.size_bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes) ||
            (previous != nullptr && !purge_order_before(*previous, entry))) {
            detail_utf8 = "trash directory manifest is invalid or not in purge order";
            return false;
        }
        if (!directory) {
            total_bytes += entry.size_bytes;
        }
        previous = &entry;
    }
    if (total_bytes != item.payload_bytes) {
        detail_utf8 = "trash directory byte count is inconsistent";
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path trash_container_path(const std::filesystem::path &source_parent,
                                           const std::uint64_t operation_id) {
    const auto prefix = core::kTrashFilenamePrefix;
    const auto leaf = std::string(reinterpret_cast<const char *>(prefix.data()), prefix.size()) +
                      hexadecimal(operation_id);
    return source_parent / path_from_utf8(leaf);
}

std::filesystem::path trash_rescue_manifest_path(const TrashTransaction &transaction) {
    if (transaction.items.empty()) {
        return {};
    }
    return transaction.items.front().stored.parent_path() /
           path_from_utf8(std::string(
               reinterpret_cast<const char *>(core::kTrashRescueManifestFilename.data()),
               core::kTrashRescueManifestFilename.size()));
}

std::filesystem::path trash_rescue_plan_path(const TrashTransaction &transaction) {
    if (transaction.items.empty()) {
        return {};
    }
    return transaction.items.front().stored.parent_path() / "plan.vtrash";
}

bool valid_trash_transaction(const TrashTransaction &transaction, std::string &detail_utf8) {
    if (transaction.version != kTrashTransactionVersion || transaction.operation_id == 0 ||
        transaction.created_unix_ns <= 0 || transaction.items.empty() ||
        transaction.items.size() > kMaximumTrashItems) {
        detail_utf8 = "trash transaction header is invalid";
        return false;
    }
    if ((transaction.active_step == TrashStep::none && transaction.active_index != 0) ||
        (transaction.active_step != TrashStep::none &&
         transaction.active_index >= transaction.items.size()) ||
        !active_step_matches(transaction)) {
        detail_utf8 = "trash transaction active step is invalid";
        return false;
    }
    if (!valid_utf8(transaction.failure_detail_utf8)) {
        detail_utf8 = "trash transaction detail is invalid";
        return false;
    }
    if ((transaction.active_step == TrashStep::none &&
         transaction.active_evidence != OperationEvidence::none) ||
        (transaction.failure_status == OperationStatus::success &&
         transaction.active_evidence != OperationEvidence::none)) {
        detail_utf8 = "trash transaction evidence is inconsistent";
        return false;
    }

    const auto parent = transaction.items.front().original.parent_path().lexically_normal();
    const auto restore_parent =
        transaction.items.front().restore_path.parent_path().lexically_normal();
    const auto container = transaction.items.front().stored.parent_path().lexically_normal();
    if (container != trash_container_path(container.parent_path(), transaction.operation_id)) {
        detail_utf8 = "trash transaction container is inconsistent";
        return false;
    }
    std::unordered_set<std::string> originals;
    std::unordered_set<std::string> restore_paths;
    std::unordered_set<std::string> stored_paths;
    std::unordered_set<std::string> identities;
    for (std::size_t index{}; index < transaction.items.size(); ++index) {
        const auto &item = transaction.items[index];
        if (!item.original.is_absolute() || !item.restore_path.is_absolute() ||
            !item.stored.is_absolute() ||
            item.original.parent_path().lexically_normal() != parent ||
            item.restore_path.parent_path().lexically_normal() != restore_parent ||
            item.original.filename() != item.restore_path.filename() ||
            item.stored != stored_path(item.restore_path, transaction.operation_id,
                                       container.parent_path(), index) ||
            !expected_current(item) || !location_allowed(transaction.phase, item.location) ||
            core::is_trash_filename(item.original.filename().u8string())) {
            detail_utf8 = "trash transaction item path or location is inconsistent";
            return false;
        }
        if (item.current_snapshot.source_revision_utf8.empty() ||
#ifdef _WIN32
            !item.current_snapshot.source_revision_utf8.starts_with("win-file128:") ||
            !detail::valid_strong_delete_revision(item.current_snapshot.source_revision_utf8) ||
#else
            (!item.current_snapshot.source_revision_utf8.starts_with("posix:") &&
             !item.current_snapshot.source_revision_utf8.starts_with("posix2:")) ||
#endif
            stable_object_identity(item.current_snapshot.source_revision_utf8).empty()) {
            detail_utf8 = "trash transaction item identity is inconsistent";
            return false;
        }
        if (!valid_directory_payload(item, detail_utf8)) {
            return false;
        }
#ifdef _WIN32
        const auto preserve = item.payload_policy == TrashPayloadPolicy::preserve_permissions;
        if ((item.payload_policy != TrashPayloadPolicy::strict && !preserve) ||
            (preserve && (item.kind != TrashItemKind::regular_file ||
                          item.security_state != TrashSecurityState::original ||
                          item.original_security_descriptor_sddl_utf8.size() >
                              kMaximumTrashSecurityBaselineBytes ||
                          !detail::validate_preserved_trash_security(
                              item.original_security_descriptor_sddl_utf8, detail_utf8)))) {
            detail_utf8 = "trash payload policy is invalid";
            return false;
        }
        const auto source_hardened_during_store =
            item.location == TrashItemLocation::source &&
            item.security_state == TrashSecurityState::hardened &&
            transaction.active_step == TrashStep::store && transaction.active_index == index;
        if (item.original_security_descriptor_sddl_utf8.empty() ||
            !item.storage_identity_utf8.empty() ||
            !valid_utf8(item.original_security_descriptor_sddl_utf8) ||
            (item.location == TrashItemLocation::source &&
             item.security_state != TrashSecurityState::original &&
             !source_hardened_during_store) ||
            (!preserve && item.location != TrashItemLocation::source &&
             item.security_state != TrashSecurityState::hardened)) {
            detail_utf8 = "trash transaction item security state is inconsistent";
            return false;
        }
#else
        if (item.payload_policy != TrashPayloadPolicy::strict ||
            !item.original_security_descriptor_sddl_utf8.empty() ||
            item.security_state != TrashSecurityState::original ||
            !valid_utf8(item.storage_identity_utf8) ||
            (!item.storage_identity_utf8.empty() &&
             !item.storage_identity_utf8.starts_with("linux-smb:"))) {
            detail_utf8 = "POSIX trash transaction carries a foreign security state";
            return false;
        }
#endif
        const auto original = path_utf8(item.original.lexically_normal());
        const auto restore = path_utf8(item.restore_path.lexically_normal());
        const auto stored = path_utf8(item.stored.lexically_normal());
        const auto identity = stable_object_identity(item.current_snapshot.source_revision_utf8);
        if (!valid_utf8(original) || !valid_utf8(restore) || !valid_utf8(stored) ||
            !originals.insert(original).second || !restore_paths.insert(restore).second ||
            !stored_paths.insert(stored).second || !identities.insert(identity).second) {
            detail_utf8 = "trash transaction contains duplicate or invalid items";
            return false;
        }
    }
    return true;
}

bool trusted_posix_trash_storage(const TrashTransaction &transaction,
                                 const bool allow_restore_rebase) {
    if (transaction.items.empty()) {
        return false;
    }
    const auto parent = transaction.items.front().original.parent_path().lexically_normal();
    const auto container = transaction.items.front().stored.parent_path().lexically_normal();
    if (container != trash_container_path(parent, transaction.operation_id)) {
        return false;
    }
    return std::ranges::all_of(transaction.items, [&](const auto &item) {
        return item.original.parent_path().lexically_normal() == parent &&
               (allow_restore_rebase ||
                item.restore_path.lexically_normal() == item.original.lexically_normal()) &&
               item.stored.parent_path().lexically_normal() == container &&
               (item.current_snapshot.source_revision_utf8.starts_with("posix:") ||
                item.current_snapshot.source_revision_utf8.starts_with("posix2:")) &&
               (item.storage_identity_utf8.empty() ||
                item.storage_identity_utf8.starts_with("linux-smb:")) &&
               item.original_security_descriptor_sddl_utf8.empty() &&
               item.security_state == TrashSecurityState::original;
    });
}

TrashTransaction prepare_trash_transaction(const std::vector<TrashSource> &sources,
                                           const std::uint64_t operation_id,
                                           const std::int64_t created_unix_ns,
                                           const std::filesystem::path &vault_root) {
    if (sources.empty() || sources.size() > kMaximumTrashItems || operation_id == 0 ||
        created_unix_ns <= 0) {
        throw std::invalid_argument("trash sources are invalid");
    }
    TrashTransaction transaction;
    transaction.operation_id = operation_id;
    transaction.created_unix_ns = created_unix_ns;
    transaction.items.reserve(sources.size());
    for (std::size_t index{}; index < sources.size(); ++index) {
        const auto restore_path =
            sources[index].restore_path.empty() ? sources[index].path : sources[index].restore_path;
        transaction.items.push_back(
            {.original = sources[index].path,
             .restore_path = restore_path,
             .stored = stored_path(restore_path, operation_id, vault_root, index),
             .current = restore_path,
             .current_snapshot = sources[index].snapshot,
             .kind = sources[index].kind,
             .payload_bytes = sources[index].kind == TrashItemKind::regular_file
                                  ? sources[index].snapshot.size_bytes
                                  : sources[index].payload_bytes,
             .directory_entries = sources[index].directory_entries,
             .directory_security_descriptors_sddl_utf8 =
                 sources[index].directory_security_descriptors_sddl_utf8,
             .directory_purge_cursor = 0,
             .storage_identity_utf8 = sources[index].storage_identity_utf8,
             .location = TrashItemLocation::source,
             .security_state = TrashSecurityState::original,
             .original_security_descriptor_sddl_utf8 =
                 sources[index].original_security_descriptor_sddl_utf8,
             .payload_policy = sources[index].payload_policy});
    }
    std::string detail;
    if (!valid_trash_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    return transaction;
}

std::vector<std::byte> encode_trash_transaction(const TrashTransaction &transaction) {
    std::string detail;
    if (!valid_trash_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> payload;
    payload.reserve(128U + transaction.items.size() * 192U);
    append_integer(payload, transactionMagic);
    append_integer(payload, transaction.version);
    append_integer(payload, transaction.operation_id);
    append_integer(payload, transaction.created_unix_ns);
    append_integer(payload, static_cast<std::uint8_t>(transaction.phase));
    append_integer(payload, static_cast<std::uint8_t>(transaction.active_step));
    append_integer(payload, static_cast<std::uint8_t>(transaction.active_evidence));
    append_integer(payload, static_cast<std::uint8_t>(transaction.failure_status));
    append_integer(payload, transaction.active_index);
    append_string(payload, transaction.failure_detail_utf8);
    append_integer(payload, static_cast<std::uint32_t>(transaction.items.size()));
    for (const auto &item : transaction.items) {
        append_string(payload, path_utf8(item.original));
        append_string(payload, path_utf8(item.restore_path));
        append_string(payload, path_utf8(item.stored));
        append_string(payload, path_utf8(item.current));
        append_integer(payload, item.current_snapshot.size_bytes);
        append_integer(payload, item.current_snapshot.modified_unix_ns);
        append_string(payload, item.current_snapshot.source_revision_utf8);
        append_integer(payload, static_cast<std::uint8_t>(item.kind));
        append_integer(payload, item.payload_bytes);
        append_integer(payload, item.directory_purge_cursor);
        append_integer(payload, static_cast<std::uint32_t>(item.directory_entries.size()));
        for (std::size_t entry_index{}; entry_index < item.directory_entries.size();
             ++entry_index) {
            const auto &entry = item.directory_entries[entry_index];
            append_string(payload, path_utf8(entry.relative_path));
            append_integer(payload, entry.size_bytes);
            append_integer(payload, entry.modified_unix_ns);
            append_string(payload, entry.source_revision_utf8);
            append_integer(payload, entry.depth);
            append_integer(payload, static_cast<std::uint8_t>(entry.kind));
            append_integer(payload, static_cast<std::uint8_t>(0));
            append_string(
                payload,
                item.directory_security_descriptors_sddl_utf8.empty()
                    ? std::string_view{}
                    : std::string_view(item.directory_security_descriptors_sddl_utf8[entry_index]));
        }
        append_string(payload, item.storage_identity_utf8);
        append_integer(payload, static_cast<std::uint8_t>(item.location));
        append_integer(payload, static_cast<std::uint8_t>(item.security_state));
        append_string(payload, item.original_security_descriptor_sddl_utf8);
        append_integer(payload, static_cast<std::uint8_t>(item.payload_policy));
    }
    if (payload.size() > maximumPayloadBytes) {
        throw std::length_error("trash transaction exceeds the journal limit");
    }
    return payload;
}

bool decode_trash_transaction(const std::span<const std::byte> payload,
                              TrashTransaction &transaction, std::string &detail_utf8) {
    if (payload.size() > maximumPayloadBytes) {
        detail_utf8 = "trash transaction exceeds the journal limit";
        return false;
    }
    Cursor cursor(payload);
    TrashTransaction decoded;
    std::uint32_t magic{};
    std::uint8_t phase{};
    std::uint8_t step{};
    std::uint8_t evidence{};
    std::uint8_t failure{};
    std::uint32_t item_count{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(decoded.created_unix_ns) ||
        !cursor.read(phase) || !cursor.read(step) || !cursor.read(evidence) ||
        !cursor.read(failure) || !cursor.read(decoded.active_index) ||
        !cursor.read_string(decoded.failure_detail_utf8) || !cursor.read(item_count) ||
        magic != transactionMagic ||
        (decoded.version != legacyTransactionVersion &&
         decoded.version != previousTransactionVersion &&
         decoded.version != directoryTransactionVersion &&
         decoded.version != kTrashTransactionVersion) ||
        phase > static_cast<std::uint8_t>(TrashPhase::purged) ||
        step > static_cast<std::uint8_t>(TrashStep::purge) ||
        evidence > static_cast<std::uint8_t>(OperationEvidence::conflicting) ||
        failure > static_cast<std::uint8_t>(OperationStatus::file_in_use) || item_count == 0 ||
        item_count > kMaximumTrashItems) {
        detail_utf8 = "trash transaction header is invalid or truncated";
        return false;
    }
    decoded.phase = static_cast<TrashPhase>(phase);
    decoded.active_step = static_cast<TrashStep>(step);
    decoded.active_evidence = static_cast<OperationEvidence>(evidence);
    decoded.failure_status = static_cast<OperationStatus>(failure);
    decoded.items.reserve(item_count);
    for (std::uint32_t index{}; index < item_count; ++index) {
        std::string original;
        std::string restore_path;
        std::string stored;
        std::string current;
        TrashItem item;
        std::uint8_t location{};
        std::uint8_t security_state{};
        std::uint8_t item_kind{};
        std::uint32_t directory_entry_count{};
        if (!cursor.read_string(original) || !cursor.read_string(restore_path) ||
            !cursor.read_string(stored) || !cursor.read_string(current) ||
            !cursor.read(item.current_snapshot.size_bytes) ||
            !cursor.read(item.current_snapshot.modified_unix_ns) ||
            !cursor.read_string(item.current_snapshot.source_revision_utf8) ||
            (decoded.version >= directoryTransactionVersion &&
             (!cursor.read(item_kind) ||
              item_kind > static_cast<std::uint8_t>(TrashItemKind::directory) ||
              !cursor.read(item.payload_bytes) || !cursor.read(item.directory_purge_cursor) ||
              !cursor.read(directory_entry_count) ||
              directory_entry_count > kMaximumTrashDirectoryEntries)) ||
            !valid_utf8(original) || !valid_utf8(restore_path) || !valid_utf8(stored) ||
            !valid_utf8(current) || !valid_utf8(item.current_snapshot.source_revision_utf8) ||
            !valid_utf8(decoded.failure_detail_utf8)) {
            detail_utf8 = "trash transaction item is invalid or truncated";
            return false;
        }
        item.kind = decoded.version >= directoryTransactionVersion
                        ? static_cast<TrashItemKind>(item_kind)
                        : TrashItemKind::regular_file;
        if (decoded.version < directoryTransactionVersion) {
            item.payload_bytes = item.current_snapshot.size_bytes;
        }
        item.directory_entries.reserve(directory_entry_count);
        item.directory_security_descriptors_sddl_utf8.reserve(directory_entry_count);
        for (std::uint32_t entry_index{}; entry_index < directory_entry_count; ++entry_index) {
            BasicDirectoryTransferEntry entry;
            std::string relative;
            std::string security_descriptor;
            std::uint8_t entry_kind{};
            std::uint8_t reserved{};
            if (!cursor.read_string(relative) || !cursor.read(entry.size_bytes) ||
                !cursor.read(entry.modified_unix_ns) ||
                !cursor.read_string(entry.source_revision_utf8) || !cursor.read(entry.depth) ||
                !cursor.read(entry_kind) || !cursor.read(reserved) || reserved != 0 ||
                !cursor.read_string(security_descriptor) ||
                entry_kind >
                    static_cast<std::uint8_t>(BasicDirectoryTransferEntryKind::regular_file) ||
                !valid_utf8(relative) || !valid_utf8(entry.source_revision_utf8) ||
                !valid_utf8(security_descriptor)) {
                detail_utf8 = "trash directory entry is invalid or truncated";
                return false;
            }
            entry.relative_path = path_from_utf8(relative);
            entry.kind = static_cast<BasicDirectoryTransferEntryKind>(entry_kind);
            item.directory_entries.push_back(std::move(entry));
            item.directory_security_descriptors_sddl_utf8.push_back(std::move(security_descriptor));
        }
        if (std::ranges::all_of(item.directory_security_descriptors_sddl_utf8,
                                [](const std::string &value) { return value.empty(); })) {
            item.directory_security_descriptors_sddl_utf8.clear();
        }
        if ((decoded.version >= previousTransactionVersion &&
             !cursor.read_string(item.storage_identity_utf8)) ||
            !cursor.read(location) || !cursor.read(security_state) ||
            !cursor.read_string(item.original_security_descriptor_sddl_utf8) ||
            location > static_cast<std::uint8_t>(TrashItemLocation::deleted) ||
            security_state > static_cast<std::uint8_t>(TrashSecurityState::hardened) ||
            !valid_utf8(item.storage_identity_utf8) ||
            !valid_utf8(item.original_security_descriptor_sddl_utf8)) {
            detail_utf8 = "trash transaction item state is invalid or truncated";
            return false;
        }
        item.original = path_from_utf8(original);
        item.restore_path = path_from_utf8(restore_path);
        item.stored = path_from_utf8(stored);
        item.current = path_from_utf8(current);
        item.location = static_cast<TrashItemLocation>(location);
        item.security_state = static_cast<TrashSecurityState>(security_state);
        if (decoded.version >= 10) {
            std::uint8_t policy{};
            if (!cursor.read(policy) ||
                policy > static_cast<std::uint8_t>(TrashPayloadPolicy::preserve_permissions)) {
                detail_utf8 = "trash payload policy is invalid or truncated";
                return false;
            }
            item.payload_policy = static_cast<TrashPayloadPolicy>(policy);
        }
        decoded.items.push_back(std::move(item));
    }
    decoded.version = kTrashTransactionVersion;
    if (cursor.remaining() != 0 || !valid_trash_transaction(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "trash transaction contains trailing bytes";
        }
        return false;
    }
    transaction = std::move(decoded);
    return true;
}

} // namespace vove::fileops
