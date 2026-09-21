#include "catalog_protocol.hpp"
#include "catalog_protocol_codec.hpp"

#include <array>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vove::platform::detail {

using namespace protocol;

std::vector<std::byte> encode_request_payload(const catalog::CatalogRequest &request) {
    const auto path = request.path.u8string();
    const std::string path_utf8(reinterpret_cast<const char *>(path.data()), path.size());
    const auto exact_entry_path = request.exact_entry_path.u8string();
    const std::string exact_entry_path_utf8(reinterpret_cast<const char *>(exact_entry_path.data()),
                                            exact_entry_path.size());

    std::vector<std::byte> payload;
    payload.reserve(36U + path_utf8.size() + exact_entry_path_utf8.size());
    append_integer(payload, requestMagic);
    append_integer(payload, kCatalogProtocolVersion);
    append_integer(payload, request.generation);
    append_integer(payload, static_cast<std::uint64_t>(request.maximum_entries));
    append_string(payload, path_utf8);
    append_string(payload, exact_entry_path_utf8);
    append_integer(payload, static_cast<std::uint8_t>(request.recursive));
    append_integer(payload, request.maximum_directories);
    append_integer(payload, request.maximum_depth);
    append_integer(payload, static_cast<std::int64_t>(request.total_timeout.count()));
    return payload;
}

bool decode_request_payload(const std::span<const std::byte> payload,
                            catalog::CatalogRequest &request, std::string &error) {
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint64_t maximum_entries{};
    std::string path_utf8;
    std::string exact_entry_path_utf8;
    std::uint8_t recursive{};
    std::int64_t total_timeout{};
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(request.generation) ||
        !cursor.read(maximum_entries) || !cursor.read_string(path_utf8) ||
        !cursor.read_string(exact_entry_path_utf8) || !cursor.read(recursive) ||
        !cursor.read(request.maximum_directories) || !cursor.read(request.maximum_depth) ||
        !cursor.read(total_timeout)) {
        error = "catalog request is truncated";
        return false;
    }
    if (magic != requestMagic || version != kCatalogProtocolVersion) {
        error = "catalog request protocol is incompatible";
        return false;
    }
    if (maximum_entries > std::numeric_limits<std::size_t>::max()) {
        error = "catalog request limit is too large";
        return false;
    }
    if (cursor.remaining() != 0) {
        error = "catalog request has trailing bytes";
        return false;
    }
    request.maximum_entries = static_cast<std::size_t>(maximum_entries);
    const auto *first = reinterpret_cast<const char8_t *>(path_utf8.data());
    request.path = std::filesystem::path(std::u8string(first, first + path_utf8.size()));
    const auto *exact_first = reinterpret_cast<const char8_t *>(exact_entry_path_utf8.data());
    request.exact_entry_path = std::filesystem::path(
        std::u8string(exact_first, exact_first + exact_entry_path_utf8.size()));
    request.recursive = recursive != 0;
    request.total_timeout = std::chrono::milliseconds(total_timeout);
    if (recursive > 1 || !catalog::valid_recursive_request(request) ||
        (request.recursive && (path_utf8.empty() || path_utf8.find('\0') != std::string::npos ||
                              path_utf8.size() > catalog::kRecursiveCatalogMaximumPathBytes))) {
        error = "catalog recursive request bounds or mode are invalid";
        return false;
    }
    return true;
}

std::vector<std::byte> encode_batch_payload(const catalog::CatalogBatch &batch) {
    if (batch.entries.size() > catalog::kCatalogBatchSize) {
        throw std::length_error("catalog batch exceeds the protocol entry limit");
    }

    std::vector<std::byte> payload;
    payload.reserve(128U + batch.entries.size() * 128U);
    append_integer(payload, batchMagic);
    append_integer(payload, kCatalogProtocolVersion);
    append_integer(payload, batch.generation);
    std::uint8_t flags{};
    if (batch.is_final) {
        flags |= 0x01U;
    }
    if (batch.truncated) {
        flags |= 0x02U;
    }
    if (batch.root_failed) {
        flags |= 0x04U;
    }
    append_integer(payload, flags);
    append_integer(payload, static_cast<std::uint8_t>(batch.error.kind));
    append_integer(payload, static_cast<std::uint16_t>(0));
    append_integer(payload, batch.error.platform_code);
    append_string(payload, batch.error.message_utf8);
    append_string(payload, batch.directory_revision_utf8);
    append_integer(payload, batch.directories_visited);
    append_integer(payload, static_cast<std::uint32_t>(batch.entries.size()));

    for (const auto &entry : batch.entries) {
        append_integer(payload, entry.id);
        append_integer(payload, static_cast<std::uint8_t>(entry.kind));
        append_integer(payload, static_cast<std::uint8_t>(entry.state));
        append_integer(payload, static_cast<std::uint16_t>(0));
        append_integer(payload, entry.size_bytes);
        append_integer(payload, entry.modified_unix_ns);
        append_string(payload, entry.name_utf8);
        append_string(payload, entry.path_utf8);
        append_string(payload, entry.source_revision_utf8);
    }
    return payload;
}

std::vector<std::byte> frame_payload(const std::span<const std::byte> payload) {
    if (payload.size() > kCatalogMaximumFrameBytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("catalog protocol frame is too large");
    }
    std::vector<std::byte> frame;
    frame.reserve(kCatalogFrameHeaderBytes + payload.size());
    append_integer(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

bool decode_frame_size(const std::span<const std::byte> header, std::size_t &size,
                       std::string &error) {
    Cursor cursor(header);
    std::uint32_t encoded_size{};
    if (header.size() != kCatalogFrameHeaderBytes || !cursor.read(encoded_size)) {
        error = "catalog frame header is invalid";
        return false;
    }
    if (encoded_size > kCatalogMaximumFrameBytes) {
        error = "catalog frame exceeds the size limit";
        return false;
    }
    size = encoded_size;
    return true;
}

bool read_stream_frame(std::istream &input, std::vector<std::byte> &payload, std::string &error,
                       const std::size_t maximum_bytes) {
    std::array<std::byte, kCatalogFrameHeaderBytes> header{};
    input.read(reinterpret_cast<char *>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        error = "catalog frame header could not be read";
        return false;
    }
    std::size_t size{};
    if (!decode_frame_size(header, size, error)) {
        return false;
    }
    if (size > maximum_bytes) {
        error = "protocol frame exceeds the caller size limit";
        return false;
    }
    payload.resize(size);
    input.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        error = "catalog frame payload could not be read";
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

} // namespace vove::platform::detail
