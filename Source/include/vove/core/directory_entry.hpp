#pragma once

#include <cstdint>
#include <string>

namespace vove::core {

enum class EntryKind : std::uint8_t {
    file,
    directory,
};

enum class EntryState : std::uint8_t {
    discovered,
    metadata_ready,
    thumbnail_queued,
    thumbnail_ready,
    failed,
};

struct DirectoryEntry {
    std::uint64_t id{};
    EntryKind kind{EntryKind::file};
    EntryState state{EntryState::discovered};
    std::string name_utf8;
    std::string search_key_utf8;
    std::string path_utf8;
    std::string source_revision_utf8;
    std::uint64_t size_bytes{};
    std::int64_t modified_unix_ns{};
};

} // namespace vove::core
