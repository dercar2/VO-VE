#include "catalog_protocol.hpp"
#include "catalog_protocol_codec.hpp"

#include "vove/core/directory_model.hpp"

#include <cstdint>
#include <utility>

namespace vove::platform::detail {

namespace {

bool valid_error_kind(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(catalog::CatalogErrorKind::cancelled);
}

bool valid_entry_kind(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(core::EntryKind::directory);
}

} // namespace

bool decode_batch_payload(const std::span<const std::byte> payload, catalog::CatalogBatch &batch,
                          std::string &error) {
    protocol::Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint8_t flags{};
    std::uint8_t error_kind{};
    std::uint16_t reserved{};
    std::uint32_t entry_count{};
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(batch.generation) ||
        !cursor.read(flags) || !cursor.read(error_kind) || !cursor.read(reserved) ||
        !cursor.read(batch.error.platform_code) || !cursor.read_string(batch.error.message_utf8) ||
        !cursor.read_string(batch.directory_revision_utf8) ||
        !cursor.read(batch.directories_visited) || !cursor.read(entry_count)) {
        error = "catalog batch is truncated";
        return false;
    }
    if (magic != protocol::batchMagic || version != kCatalogProtocolVersion) {
        error = "catalog batch protocol is incompatible";
        return false;
    }
    if ((flags & 0xF8U) != 0 || reserved != 0 || !valid_error_kind(error_kind) ||
        entry_count > catalog::kCatalogBatchSize ||
        batch.directories_visited > catalog::kRecursiveCatalogMaximumDirectories ||
        ((flags & 0x04U) != 0 && ((flags & 0x03U) != 0x03U || error_kind == 0))) {
        error = "catalog batch header is invalid";
        return false;
    }

    batch.is_final = (flags & 0x01U) != 0;
    batch.truncated = (flags & 0x02U) != 0;
    batch.root_failed = (flags & 0x04U) != 0;
    batch.error.kind = static_cast<catalog::CatalogErrorKind>(error_kind);
    batch.entries.clear();
    batch.entries.reserve(entry_count);
    for (std::uint32_t index = 0; index < entry_count; ++index) {
        core::DirectoryEntry entry;
        std::uint8_t kind{};
        std::uint8_t state{};
        if (!cursor.read(entry.id) || !cursor.read(kind) || !cursor.read(state) ||
            !cursor.read(reserved) || !cursor.read(entry.size_bytes) ||
            !cursor.read(entry.modified_unix_ns) || !cursor.read_string(entry.name_utf8) ||
            !cursor.read_string(entry.path_utf8) ||
            !cursor.read_string(entry.source_revision_utf8)) {
            error = "catalog entry is truncated";
            return false;
        }
        if (!valid_entry_kind(kind) ||
            state > static_cast<std::uint8_t>(core::EntryState::failed) || reserved != 0) {
            error = "catalog entry header is invalid";
            return false;
        }
        entry.kind = static_cast<core::EntryKind>(kind);
        entry.state = static_cast<core::EntryState>(state);
        entry.search_key_utf8 = core::make_search_key(entry.name_utf8);
        batch.entries.push_back(std::move(entry));
    }
    if (cursor.remaining() != 0) {
        error = "catalog batch has trailing bytes";
        return false;
    }
    return true;
}

} // namespace vove::platform::detail
