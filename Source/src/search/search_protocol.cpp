#include "search_protocol.hpp"
#include "search_protocol_codec.hpp"

#include <array>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace vove::search::protocol {
namespace {

using detail::append_integer;
using detail::append_string;
using detail::Cursor;

constexpr std::uint32_t requestMagic = 0x51534756U;
constexpr std::uint32_t batchMagic = 0x42534756U;
constexpr std::size_t maximumQueryBytes = 1U << 12U;
constexpr std::size_t maximumPathBytes = 1U << 16U;

} // namespace

std::vector<std::byte> encode_request_payload(const SearchRequest &request) {
    if (request.query_utf8.size() > maximumQueryBytes ||
        request.allowed_roots_utf8.size() > kMaximumSearchRoots ||
        request.maximum_results > kMaximumSearchResults) {
        throw std::length_error("search request exceeds protocol limits");
    }
    std::vector<std::byte> output;
    output.reserve(64U + request.query_utf8.size() + request.allowed_roots_utf8.size() * 64U);
    append_integer(output, requestMagic);
    append_integer(output, kSearchProtocolVersion);
    append_integer(output, request.generation);
    append_integer(output, static_cast<std::uint64_t>(request.maximum_results));
    append_string(output, request.query_utf8);
    append_integer(output, static_cast<std::uint32_t>(request.allowed_roots_utf8.size()));
    for (const auto &root : request.allowed_roots_utf8) {
        if (root.size() > maximumPathBytes) {
            throw std::length_error("search root exceeds protocol limits");
        }
        append_string(output, root);
    }
    return output;
}

bool decode_request_payload(const std::span<const std::byte> payload, SearchRequest &request,
                            std::string &error) {
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint64_t maximum_results{};
    std::uint32_t root_count{};
    SearchRequest decoded;
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(decoded.generation) ||
        !cursor.read(maximum_results) ||
        !cursor.read_string(decoded.query_utf8, maximumQueryBytes) || !cursor.read(root_count)) {
        error = "search request is truncated";
        return false;
    }
    if (magic != requestMagic || version != kSearchProtocolVersion) {
        error = "search request protocol is incompatible";
        return false;
    }
    if (maximum_results > kMaximumSearchResults || root_count > kMaximumSearchRoots) {
        error = "search request exceeds protocol limits";
        return false;
    }
    decoded.maximum_results = static_cast<std::size_t>(maximum_results);
    decoded.allowed_roots_utf8.reserve(root_count);
    for (std::uint32_t index = 0; index < root_count; ++index) {
        std::string root;
        if (!cursor.read_string(root, maximumPathBytes)) {
            error = "search request root is truncated";
            return false;
        }
        decoded.allowed_roots_utf8.push_back(std::move(root));
    }
    if (cursor.remaining() != 0U) {
        error = "search request has trailing bytes";
        return false;
    }
    request = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_batch_payload(const SearchBatch &batch) {
    if (batch.entries.size() > kSearchBatchSize) {
        throw std::length_error("search batch exceeds protocol limits");
    }
    std::vector<std::byte> output;
    output.reserve(128U + batch.entries.size() * 192U);
    append_integer(output, batchMagic);
    append_integer(output, kSearchProtocolVersion);
    append_integer(output, batch.generation);
    append_integer(output, batch.total_matches);
    append_integer(output, batch.provider_index_modified_unix_ns);
    append_integer(output, static_cast<std::uint8_t>(batch.status));
    std::uint8_t flags{};
    flags |= batch.is_final ? 0x01U : 0U;
    flags |= batch.truncated ? 0x02U : 0U;
    append_integer(output, flags);
    append_integer(output, std::uint16_t{});
    append_string(output, batch.message_utf8);
    append_integer(output, static_cast<std::uint32_t>(batch.entries.size()));
    for (const auto &entry : batch.entries) {
        append_integer(output, entry.id);
        append_integer(output, static_cast<std::uint8_t>(entry.kind));
        append_integer(output, static_cast<std::uint8_t>(entry.state));
        append_integer(output, std::uint16_t{});
        append_integer(output, entry.size_bytes);
        append_integer(output, entry.modified_unix_ns);
        append_string(output, entry.name_utf8);
        append_string(output, entry.path_utf8);
        append_string(output, entry.source_revision_utf8);
    }
    return output;
}

bool decode_batch_payload(const std::span<const std::byte> payload, SearchBatch &batch,
                          std::string &error) {
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint8_t status{};
    std::uint8_t flags{};
    std::uint16_t reserved{};
    std::uint32_t entry_count{};
    SearchBatch decoded;
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(decoded.generation) ||
        !cursor.read(decoded.total_matches) ||
        !cursor.read(decoded.provider_index_modified_unix_ns) || !cursor.read(status) ||
        !cursor.read(flags) || !cursor.read(reserved) ||
        !cursor.read_string(decoded.message_utf8) || !cursor.read(entry_count)) {
        error = "search batch is truncated";
        return false;
    }
    if (magic != batchMagic || version != kSearchProtocolVersion || reserved != 0U ||
        status > static_cast<std::uint8_t>(SearchStatus::cancelled) ||
        entry_count > kSearchBatchSize || (flags & ~0x03U) != 0U) {
        error = "search batch header is invalid";
        return false;
    }
    decoded.status = static_cast<SearchStatus>(status);
    decoded.is_final = (flags & 0x01U) != 0U;
    decoded.truncated = (flags & 0x02U) != 0U;
    decoded.entries.reserve(entry_count);
    for (std::uint32_t index = 0; index < entry_count; ++index) {
        core::DirectoryEntry entry;
        std::uint8_t kind{};
        std::uint8_t state{};
        if (!cursor.read(entry.id) || !cursor.read(kind) || !cursor.read(state) ||
            !cursor.read(reserved) || !cursor.read(entry.size_bytes) ||
            !cursor.read(entry.modified_unix_ns) ||
            !cursor.read_string(entry.name_utf8, maximumPathBytes) ||
            !cursor.read_string(entry.path_utf8, maximumPathBytes) ||
            !cursor.read_string(entry.source_revision_utf8, maximumPathBytes)) {
            error = "search batch entry is truncated";
            return false;
        }
        if (kind > static_cast<std::uint8_t>(core::EntryKind::directory) ||
            state > static_cast<std::uint8_t>(core::EntryState::failed) || reserved != 0U) {
            error = "search batch entry is invalid";
            return false;
        }
        entry.kind = static_cast<core::EntryKind>(kind);
        entry.state = static_cast<core::EntryState>(state);
        decoded.entries.push_back(std::move(entry));
    }
    if (cursor.remaining() != 0U) {
        error = "search batch has trailing bytes";
        return false;
    }
    batch = std::move(decoded);
    return true;
}

std::vector<std::byte> frame_payload(const std::span<const std::byte> payload) {
    if (payload.size() > kSearchMaximumFrameBytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("search frame exceeds protocol limits");
    }
    std::vector<std::byte> frame;
    frame.reserve(kSearchFrameHeaderBytes + payload.size());
    append_integer(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

bool decode_frame_size(const std::span<const std::byte> header, std::size_t &size,
                       std::string &error) {
    Cursor cursor(header);
    std::uint32_t encoded_size{};
    if (header.size() != kSearchFrameHeaderBytes || !cursor.read(encoded_size) ||
        encoded_size > kSearchMaximumFrameBytes) {
        error = "search frame header is invalid";
        return false;
    }
    size = encoded_size;
    return true;
}

bool read_stream_frame(std::istream &input, std::vector<std::byte> &payload, std::string &error) {
    std::array<std::byte, kSearchFrameHeaderBytes> header{};
    input.read(reinterpret_cast<char *>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        error = "search frame header could not be read";
        return false;
    }
    std::size_t size{};
    if (!decode_frame_size(header, size, error)) {
        return false;
    }
    payload.resize(size);
    input.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        error = "search frame payload could not be read";
        return false;
    }
    return true;
}

bool write_stream_frame(std::ostream &output, const std::span<const std::byte> payload) {
    const auto frame = frame_payload(payload);
    output.write(reinterpret_cast<const char *>(frame.data()),
                 static_cast<std::streamsize>(frame.size()));
    output.flush();
    return output.good();
}

} // namespace vove::search::protocol
