#include "vove/fileops/permanent_delete_transaction.hpp"

#include "delete_identity.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace vove::fileops {
namespace {

constexpr std::uint32_t transactionMagic = 0x31544450U;
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
        throw std::length_error("permanent-delete transaction string is too large");
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

std::filesystem::path temporary_path(const std::filesystem::path &source,
                                     const std::uint64_t operation_id, const std::size_t index) {
    std::array<char, 32> operation{};
    const auto [operation_end, operation_error] =
        std::to_chars(operation.data(), operation.data() + operation.size(), operation_id, 16);
    std::array<char, 32> item{};
    const auto [item_end, item_error] =
        std::to_chars(item.data(), item.data() + item.size(), index, 16);
    if (operation_error != std::errc{} || item_error != std::errc{}) {
        throw std::runtime_error("permanent-delete temporary name could not be generated");
    }
    const auto leaf = ".vove-delete-" + std::string(operation.data(), operation_end) + '-' +
                      std::string(item.data(), item_end) + ".tmp";
    return source.parent_path() / path_from_utf8(leaf);
}

bool valid_phase(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(PermanentDeletePhase::deleting);
}

bool valid_step(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(PermanentDeleteStep::erase);
}

bool valid_location(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(PermanentDeleteItemLocation::deleted);
}

bool expected_current(const PermanentDeleteItem &item) {
    switch (item.location) {
    case PermanentDeleteItemLocation::source:
        return item.current == item.source;
    case PermanentDeleteItemLocation::temporary:
        return item.current == item.temporary;
    case PermanentDeleteItemLocation::deleted:
        return item.current.empty();
    }
    return false;
}

bool location_allowed(const PermanentDeletePhase phase,
                      const PermanentDeleteItemLocation location) {
    switch (phase) {
    case PermanentDeletePhase::prepared:
        return location == PermanentDeleteItemLocation::source;
    case PermanentDeletePhase::evacuating:
    case PermanentDeletePhase::rollback:
        return location == PermanentDeleteItemLocation::source ||
               location == PermanentDeleteItemLocation::temporary;
    case PermanentDeletePhase::permanent_delete_intent:
        return location == PermanentDeleteItemLocation::temporary;
    case PermanentDeletePhase::deleting:
        return location == PermanentDeleteItemLocation::temporary ||
               location == PermanentDeleteItemLocation::deleted;
    }
    return false;
}

bool active_step_matches(const PermanentDeleteTransaction &transaction) {
    if (transaction.active_step == PermanentDeleteStep::none) {
        return transaction.active_index == 0;
    }
    const auto location = transaction.items[transaction.active_index].location;
    switch (transaction.active_step) {
    case PermanentDeleteStep::evacuate:
        return transaction.phase == PermanentDeletePhase::evacuating &&
               location == PermanentDeleteItemLocation::source;
    case PermanentDeleteStep::rollback:
        return transaction.phase == PermanentDeletePhase::rollback &&
               location == PermanentDeleteItemLocation::temporary;
    case PermanentDeleteStep::erase:
        return transaction.phase == PermanentDeletePhase::deleting &&
               location == PermanentDeleteItemLocation::temporary;
    case PermanentDeleteStep::none:
        break;
    }
    return false;
}

} // namespace

bool valid_permanent_delete_transaction(const PermanentDeleteTransaction &transaction,
                                        std::string &detail_utf8) {
    if (transaction.version != kPermanentDeleteTransactionVersion ||
        transaction.operation_id == 0 || transaction.items.empty() ||
        transaction.items.size() > kMaximumPermanentDeleteItems) {
        detail_utf8 = "permanent-delete transaction header is invalid";
        return false;
    }
    if ((transaction.active_step == PermanentDeleteStep::none && transaction.active_index != 0) ||
        (transaction.active_step != PermanentDeleteStep::none &&
         transaction.active_index >= transaction.items.size()) ||
        !active_step_matches(transaction)) {
        detail_utf8 = "permanent-delete active step is invalid";
        return false;
    }
    if ((transaction.active_step == PermanentDeleteStep::none &&
         transaction.active_evidence != OperationEvidence::none) ||
        (transaction.failure_status == OperationStatus::success &&
         transaction.active_evidence != OperationEvidence::none)) {
        detail_utf8 = "permanent-delete evidence is inconsistent";
        return false;
    }

    const auto parent = transaction.items.front().source.parent_path().lexically_normal();
    std::unordered_map<std::string, bool> identities;
    for (const auto &item : transaction.items) {
        if (!item.source.is_absolute() || !item.temporary.is_absolute() ||
            item.source.parent_path().lexically_normal() != parent ||
            item.temporary.parent_path().lexically_normal() != parent || !expected_current(item) ||
            !location_allowed(transaction.phase, item.location)) {
            detail_utf8 = "permanent-delete item paths are inconsistent";
            return false;
        }
        std::string filename_detail;
        if (!valid_destination_filename(item.temporary.filename(), filename_detail)) {
            detail_utf8 = std::move(filename_detail);
            return false;
        }
        const auto identity = stable_object_identity(item.current_snapshot.source_revision_utf8);
#ifdef _WIN32
        if (!detail::valid_strong_delete_revision(item.current_snapshot.source_revision_utf8)) {
            detail_utf8 = "permanent-delete item has no strong deletion identity";
            return false;
        }
#endif
        if (identity.empty() || !identities.emplace(identity, true).second) {
            detail_utf8 = "permanent-delete identity is invalid or duplicated";
            return false;
        }
    }
    return true;
}

PermanentDeleteTransaction
prepare_permanent_delete_transaction(const std::vector<PermanentDeleteSource> &sources,
                                     const std::uint64_t operation_id) {
    if (sources.empty() || sources.size() > kMaximumPermanentDeleteItems || operation_id == 0) {
        throw std::invalid_argument("permanent-delete sources are invalid");
    }
    PermanentDeleteTransaction transaction;
    transaction.operation_id = operation_id;
    transaction.items.reserve(sources.size());
    for (std::size_t index{}; index < sources.size(); ++index) {
        transaction.items.push_back(
            {.source = sources[index].path,
             .temporary = temporary_path(sources[index].path, operation_id, index),
             .current = sources[index].path,
             .current_snapshot = sources[index].snapshot,
             .location = PermanentDeleteItemLocation::source});
    }
    std::string detail;
    if (!valid_permanent_delete_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    return transaction;
}

std::vector<std::byte>
encode_permanent_delete_transaction(const PermanentDeleteTransaction &transaction) {
    std::string detail;
    if (!valid_permanent_delete_transaction(transaction, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> payload;
    payload.reserve(128U + transaction.items.size() * 192U);
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
        append_integer(payload, static_cast<std::uint8_t>(0));
        append_integer(payload, static_cast<std::uint16_t>(0));
        append_integer(payload, item.current_snapshot.size_bytes);
        append_integer(payload, item.current_snapshot.modified_unix_ns);
        append_string(payload, path_utf8(item.source));
        append_string(payload, path_utf8(item.temporary));
        append_string(payload, path_utf8(item.current));
        append_string(payload, item.current_snapshot.source_revision_utf8);
    }
    if (payload.size() > maximumPayloadBytes) {
        throw std::length_error("permanent-delete transaction exceeds the journal limit");
    }
    return payload;
}

bool decode_permanent_delete_transaction(const std::span<const std::byte> payload,
                                         PermanentDeleteTransaction &transaction,
                                         std::string &detail_utf8) {
    if (payload.size() > maximumPayloadBytes) {
        detail_utf8 = "permanent-delete transaction exceeds the journal limit";
        return false;
    }
    Cursor cursor(payload);
    PermanentDeleteTransaction decoded;
    std::uint32_t magic{};
    std::uint8_t phase{};
    std::uint8_t step{};
    std::uint8_t failure{};
    std::uint8_t evidence{};
    std::uint32_t item_count{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(phase) || !cursor.read(step) ||
        !cursor.read(failure) || !cursor.read(evidence) || !cursor.read(decoded.active_index) ||
        !cursor.read(item_count) || !cursor.read_string(decoded.failure_detail_utf8) ||
        magic != transactionMagic || !valid_phase(phase) || !valid_step(step) ||
        failure > static_cast<std::uint8_t>(OperationStatus::file_in_use) ||
        evidence > static_cast<std::uint8_t>(OperationEvidence::conflicting) || item_count == 0 ||
        item_count > kMaximumPermanentDeleteItems) {
        detail_utf8 = "permanent-delete transaction header is invalid or truncated";
        return false;
    }
    decoded.phase = static_cast<PermanentDeletePhase>(phase);
    decoded.active_step = static_cast<PermanentDeleteStep>(step);
    decoded.failure_status = static_cast<OperationStatus>(failure);
    decoded.active_evidence = static_cast<OperationEvidence>(evidence);
    decoded.items.reserve(item_count);
    for (std::uint32_t index{}; index < item_count; ++index) {
        PermanentDeleteItem item;
        std::uint8_t location{};
        std::uint8_t reserved8{};
        std::uint16_t reserved16{};
        std::string source;
        std::string temporary;
        std::string current;
        if (!cursor.read(location) || !cursor.read(reserved8) || !cursor.read(reserved16) ||
            !cursor.read(item.current_snapshot.size_bytes) ||
            !cursor.read(item.current_snapshot.modified_unix_ns) || !cursor.read_string(source) ||
            !cursor.read_string(temporary) || !cursor.read_string(current) ||
            !cursor.read_string(item.current_snapshot.source_revision_utf8) || reserved8 != 0 ||
            reserved16 != 0 || !valid_location(location) || !valid_utf8(source) ||
            !valid_utf8(temporary) || !valid_utf8(current) ||
            !valid_utf8(item.current_snapshot.source_revision_utf8)) {
            detail_utf8 = "permanent-delete transaction item is invalid or truncated";
            return false;
        }
        item.source = path_from_utf8(source);
        item.temporary = path_from_utf8(temporary);
        item.current = path_from_utf8(current);
        item.location = static_cast<PermanentDeleteItemLocation>(location);
        decoded.items.push_back(std::move(item));
    }
    if (cursor.remaining() != 0 || !valid_permanent_delete_transaction(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "permanent-delete transaction contains trailing bytes";
        }
        return false;
    }
    transaction = std::move(decoded);
    return true;
}

} // namespace vove::fileops
