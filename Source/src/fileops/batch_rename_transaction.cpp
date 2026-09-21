#include "vove/fileops/batch_rename_transaction.hpp"

#include "batch_rename_detail.hpp"

#include "vove/core/reserved_names.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace vove::fileops {
namespace {

constexpr std::uint32_t transactionMagic = 0x31544256U;
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
        throw std::length_error("batch transaction string is too large");
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
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

std::string temporary_leaf(const std::uint64_t operation_id, const std::size_t index) {
    std::array<char, 32> operation{};
    const auto [operation_end, operation_error] =
        std::to_chars(operation.data(), operation.data() + operation.size(), operation_id, 16);
    std::array<char, 32> item{};
    const auto [item_end, item_error] =
        std::to_chars(item.data(), item.data() + item.size(), index, 16);
    if (operation_error != std::errc{} || item_error != std::errc{}) {
        throw std::runtime_error("batch temporary name could not be generated");
    }
    return ".vove-" + std::string(operation.data(), operation_end) + '-' +
           std::string(item.data(), item_end) + ".tmp";
}

bool valid_phase(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(BatchTransactionPhase::publishing);
}

bool valid_step(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(BatchStepKind::publish);
}

bool valid_location(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(BatchItemLocation::destination);
}

bool valid_status(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationStatus::file_in_use);
}

bool valid_evidence(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationEvidence::conflicting);
}

bool expected_current_path(const BatchTransactionItem &item) {
    switch (item.location) {
    case BatchItemLocation::source:
        return item.current == item.source;
    case BatchItemLocation::temporary:
        return item.current == item.temporary;
    case BatchItemLocation::destination:
        return item.current == item.destination;
    }
    return false;
}

bool location_allowed_in_phase(const BatchTransactionPhase phase,
                               const BatchItemLocation location) {
    switch (phase) {
    case BatchTransactionPhase::prepared:
        return location == BatchItemLocation::source;
    case BatchTransactionPhase::evacuating:
    case BatchTransactionPhase::rollback:
        return location == BatchItemLocation::source || location == BatchItemLocation::temporary;
    case BatchTransactionPhase::commit_intent:
        return location == BatchItemLocation::temporary;
    case BatchTransactionPhase::publishing:
        return location == BatchItemLocation::temporary ||
               location == BatchItemLocation::destination;
    }
    return false;
}

bool active_step_matches_phase(const BatchRenameTransaction &transaction) {
    if (transaction.active_step == BatchStepKind::none) {
        return transaction.phase != BatchTransactionPhase::prepared ||
               transaction.active_index == 0;
    }
    const auto location = transaction.items[transaction.active_index].location;
    switch (transaction.active_step) {
    case BatchStepKind::evacuate:
        return transaction.phase == BatchTransactionPhase::evacuating &&
               location == BatchItemLocation::source;
    case BatchStepKind::rollback:
        return transaction.phase == BatchTransactionPhase::rollback &&
               location == BatchItemLocation::temporary;
    case BatchStepKind::publish:
        return transaction.phase == BatchTransactionPhase::publishing &&
               location == BatchItemLocation::temporary;
    case BatchStepKind::none:
        break;
    }
    return false;
}

} // namespace

bool valid_batch_transaction(const BatchRenameTransaction &transaction, std::string &detail_utf8) {
    if (transaction.version != kBatchRenameTransactionVersion || transaction.operation_id == 0 ||
        transaction.items.empty() || transaction.items.size() > kMaximumBatchRenameItems) {
        detail_utf8 = "batch transaction header is invalid";
        return false;
    }
    if (transaction.active_step == BatchStepKind::none) {
        if (transaction.active_index != 0) {
            detail_utf8 = "inactive batch transaction has an active index";
            return false;
        }
    } else if (transaction.active_index >= transaction.items.size()) {
        detail_utf8 = "batch transaction active index is invalid";
        return false;
    }
    if (!active_step_matches_phase(transaction)) {
        detail_utf8 = "batch transaction active step is inconsistent with its phase";
        return false;
    }
    if ((transaction.active_step == BatchStepKind::none &&
         transaction.active_evidence != OperationEvidence::none) ||
        (transaction.failure_status == OperationStatus::success &&
         transaction.active_evidence != OperationEvidence::none)) {
        detail_utf8 = "batch transaction evidence is inconsistent with its active step";
        return false;
    }

    const auto parent = transaction.items.front().source.parent_path().lexically_normal();
    std::unordered_map<std::string, bool> physical_ids;
    std::unordered_map<std::string, bool> source_names;
    std::unordered_map<std::string, bool> destination_names;
    for (std::size_t index{}; index < transaction.items.size(); ++index) {
        const auto &item = transaction.items[index];
        if ((item.object_kind != OperationObjectKind::regular_file &&
             item.object_kind != OperationObjectKind::directory) ||
            (item.object_kind == OperationObjectKind::directory &&
             item.current_snapshot.size_bytes != 0)) {
            detail_utf8 = "batch transaction object kind or directory size is invalid";
            return false;
        }
        const auto expected_temporary =
            parent / path_from_utf8(temporary_leaf(transaction.operation_id, index));
        if (!item.source.is_absolute() || !item.temporary.is_absolute() ||
            !item.destination.is_absolute() || !item.current.is_absolute() ||
            item.source.parent_path().lexically_normal() != parent ||
            item.temporary.parent_path().lexically_normal() != parent ||
            item.destination.parent_path().lexically_normal() != parent ||
            item.temporary != expected_temporary || !expected_current_path(item) ||
            !location_allowed_in_phase(transaction.phase, item.location)) {
            detail_utf8 = "batch transaction paths are inconsistent";
            return false;
        }
        std::string filename_detail;
        if (core::is_internal_filename(item.source.filename().u8string()) ||
            core::is_internal_filename(item.destination.filename().u8string()) ||
            !valid_destination_filename(item.temporary.filename(), filename_detail) ||
            !valid_destination_filename(item.destination.filename(), filename_detail)) {
            if (filename_detail.empty()) {
                filename_detail = "batch transaction names an internal VO-VE object";
            }
            detail_utf8 = std::move(filename_detail);
            return false;
        }
        if (!source_names.emplace(detail::batch_rename_collision_key(item.source), true).second ||
            !destination_names.emplace(detail::batch_rename_collision_key(item.destination), true)
                 .second) {
            detail_utf8 = "batch transaction source or destination names are duplicated";
            return false;
        }
        const auto physical_id = stable_object_identity(item.current_snapshot.source_revision_utf8);
        if (physical_id.empty() || !physical_ids.emplace(physical_id, true).second) {
            detail_utf8 = "batch transaction physical identity is invalid or duplicated";
            return false;
        }
    }
    return true;
}

BatchRenameTransaction prepare_batch_transaction(const BatchRenamePlan &plan,
                                                 const std::vector<BatchRenameSource> &sources,
                                                 const std::uint64_t operation_id) {
    if (!plan.valid() || plan.rows.size() != sources.size() || operation_id == 0) {
        throw std::invalid_argument("batch rename plan cannot create a transaction");
    }
    BatchRenameTransaction transaction;
    transaction.operation_id = operation_id;
    transaction.items.reserve(sources.size());
    for (std::size_t index{}; index < sources.size(); ++index) {
        if (!plan.rows[index].valid() || plan.rows[index].source != sources[index].path ||
            plan.rows[index].destination.empty()) {
            throw std::invalid_argument("batch rename plan and source snapshot differ");
        }
        const auto temporary =
            sources[index].path.parent_path() / path_from_utf8(temporary_leaf(operation_id, index));
        transaction.items.push_back(
            {.source = sources[index].path,
             .temporary = temporary,
             .destination = plan.rows[index].destination,
             .current = sources[index].path,
             .current_snapshot = {.size_bytes = sources[index].size_bytes,
                                  .modified_unix_ns = sources[index].modified_unix_ns,
                                  .source_revision_utf8 = sources[index].source_revision_utf8},
             .location = BatchItemLocation::source,
             .object_kind = sources[index].object_kind});
    }
    std::string detail;
    if (!valid_batch_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    // Validate the complete selection before removing no-ops: their names still occupy space.
    std::erase_if(transaction.items, [](const auto &item) {
        return item.source == item.destination;
    });
    for (std::size_t index{}; index < transaction.items.size(); ++index) {
        auto &item = transaction.items[index];
        item.temporary = item.source.parent_path() /
                         path_from_utf8(temporary_leaf(operation_id, index));
    }
    return transaction;
}

std::vector<std::byte> encode_batch_transaction(const BatchRenameTransaction &transaction) {
    std::string detail;
    if (!valid_batch_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> payload;
    payload.reserve(128U + transaction.items.size() * 256U);
    append_integer(payload, transactionMagic);
    append_integer(payload, transaction.version);
    append_integer(payload, transaction.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(transaction.phase));
    append_integer(payload, static_cast<std::uint8_t>(transaction.active_step));
    append_integer(payload, static_cast<std::uint8_t>(transaction.failure_status));
    append_integer(payload, static_cast<std::uint8_t>(transaction.active_evidence));
    append_integer(payload, transaction.active_index);
    append_integer(payload, static_cast<std::uint32_t>(transaction.items.size()));
    append_string(payload, transaction.failure_detail_utf8);
    for (const auto &item : transaction.items) {
        append_integer(payload, static_cast<std::uint8_t>(item.location));
        append_integer(payload, static_cast<std::uint8_t>(item.object_kind));
        append_integer(payload, static_cast<std::uint16_t>(0));
        append_integer(payload, item.current_snapshot.size_bytes);
        append_integer(payload, item.current_snapshot.modified_unix_ns);
        append_string(payload, path_utf8(item.source));
        append_string(payload, path_utf8(item.temporary));
        append_string(payload, path_utf8(item.destination));
        append_string(payload, path_utf8(item.current));
        append_string(payload, item.current_snapshot.source_revision_utf8);
    }
    if (payload.size() > maximumPayloadBytes) {
        throw std::length_error("batch transaction exceeds the journal limit");
    }
    return payload;
}

bool decode_batch_transaction(const std::span<const std::byte> payload,
                              BatchRenameTransaction &transaction, std::string &detail_utf8) {
    if (payload.size() > maximumPayloadBytes) {
        detail_utf8 = "batch transaction exceeds the journal limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint8_t phase{};
    std::uint8_t active_step{};
    std::uint8_t failure_status{};
    std::uint8_t active_evidence{};
    std::uint32_t item_count{};
    BatchRenameTransaction decoded;
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(phase) || !cursor.read(active_step) ||
        !cursor.read(failure_status) || !cursor.read(active_evidence) ||
        !cursor.read(decoded.active_index) || !cursor.read(item_count) ||
        !cursor.read_string(decoded.failure_detail_utf8) || magic != transactionMagic ||
        !valid_phase(phase) || !valid_step(active_step) || !valid_status(failure_status) ||
        !valid_evidence(active_evidence) ||
        (decoded.version != 2 && decoded.version != kBatchRenameTransactionVersion) ||
        item_count == 0 ||
        item_count > kMaximumBatchRenameItems) {
        detail_utf8 = "batch transaction header is invalid or truncated";
        return false;
    }
    decoded.phase = static_cast<BatchTransactionPhase>(phase);
    decoded.active_step = static_cast<BatchStepKind>(active_step);
    decoded.failure_status = static_cast<OperationStatus>(failure_status);
    decoded.active_evidence = static_cast<OperationEvidence>(active_evidence);
    decoded.items.reserve(item_count);
    for (std::uint32_t index{}; index < item_count; ++index) {
        std::uint8_t location{};
        std::uint8_t object_kind{};
        std::uint16_t item_reserved16{};
        std::string source;
        std::string temporary;
        std::string destination;
        std::string current;
        BatchTransactionItem item;
        if (!cursor.read(location) || !cursor.read(object_kind) ||
            !cursor.read(item_reserved16) || !cursor.read(item.current_snapshot.size_bytes) ||
            !cursor.read(item.current_snapshot.modified_unix_ns) || !cursor.read_string(source) ||
            !cursor.read_string(temporary) || !cursor.read_string(destination) ||
            !cursor.read_string(current) ||
            !cursor.read_string(item.current_snapshot.source_revision_utf8) ||
            (decoded.version == 2 && object_kind != 0) ||
            object_kind > static_cast<std::uint8_t>(OperationObjectKind::directory) ||
            item_reserved16 != 0 || !valid_location(location) ||
            !valid_utf8(source) || !valid_utf8(temporary) || !valid_utf8(destination) ||
            !valid_utf8(current) || !valid_utf8(item.current_snapshot.source_revision_utf8)) {
            detail_utf8 = "batch transaction item is invalid or truncated";
            return false;
        }
        item.source = path_from_utf8(source);
        item.temporary = path_from_utf8(temporary);
        item.destination = path_from_utf8(destination);
        item.current = path_from_utf8(current);
        item.location = static_cast<BatchItemLocation>(location);
        item.object_kind = static_cast<OperationObjectKind>(object_kind);
        decoded.items.push_back(std::move(item));
    }
    // Version 2 reserved this byte as zero and supported only regular files.
    decoded.version = kBatchRenameTransactionVersion;
    if (cursor.remaining() != 0 || !valid_utf8(decoded.failure_detail_utf8) ||
        !valid_batch_transaction(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "batch transaction has trailing or invalid data";
        }
        return false;
    }
    transaction = std::move(decoded);
    return true;
}

} // namespace vove::fileops
