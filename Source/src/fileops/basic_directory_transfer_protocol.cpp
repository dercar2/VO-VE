#include "vove/fileops/basic_directory_transfer_protocol.hpp"

#include "catalog_protocol_codec.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t requestMagic = 0x31514442U;
constexpr std::uint32_t progressMagic = 0x31504442U;
constexpr std::uint32_t resultMagic = 0x31534442U;
constexpr std::uint32_t completionAckMagic = 0x31414442U;

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

bool valid_text(const std::string_view text) noexcept {
    return text.size() <= kMaximumBasicDirectoryTransferProtocolBytes && valid_utf8(text);
}

bool valid_command(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(BasicDirectoryTransferCommand::discard);
}

bool valid_phase(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(BasicDirectoryTransferPhase::completed);
}

bool valid_status(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(BasicDirectoryTransferStatus::file_in_use);
}

bool header(Cursor &cursor, const std::uint32_t expected_magic, std::string &detail_utf8) {
    std::uint32_t magic{};
    std::uint32_t version{};
    if (!cursor.read(magic) || !cursor.read(version) || magic != expected_magic ||
        version != kBasicDirectoryTransferProtocolVersion) {
        detail_utf8 = "basic-directory-transfer protocol header is incompatible or truncated";
        return false;
    }
    return true;
}

bool absolute_path_or_empty(const std::filesystem::path &path) {
    return path.empty() || path.is_absolute();
}

bool valid_result(const BasicDirectoryTransferResult &result) {
    return valid_status(static_cast<std::uint8_t>(result.status)) &&
           absolute_path_or_empty(result.staging_destination) &&
           absolute_path_or_empty(result.destination) &&
           absolute_path_or_empty(result.manifest_path) && valid_text(result.detail_utf8) &&
           valid_text(path_utf8(result.staging_destination)) &&
           valid_text(path_utf8(result.destination)) && valid_text(path_utf8(result.manifest_path));
}

} // namespace

bool is_basic_directory_transfer_heartbeat(const BasicDirectoryTransferProgress &previous,
                                           const BasicDirectoryTransferProgress &current) noexcept {
    return previous.phase == current.phase &&
           previous.completed_entries == current.completed_entries &&
           previous.total_entries == current.total_entries &&
           previous.completed_bytes == current.completed_bytes &&
           previous.total_bytes == current.total_bytes &&
           previous.current_path == current.current_path;
}

bool valid_basic_directory_transfer_stream_request(
    const BasicDirectoryTransferStreamRequest &request, std::string &detail_utf8) {
    detail_utf8.clear();
    const auto resume = request.command == BasicDirectoryTransferCommand::resume;
    const auto discard = request.command == BasicDirectoryTransferCommand::discard;
    const auto exact = resume || discard;
    if (request.request_id == 0U || request.operation_id == 0U ||
        !valid_command(static_cast<std::uint8_t>(request.command)) ||
        !request.source.is_absolute() || !request.destination.is_absolute() ||
        request.expected_source_revision_utf8.empty() ||
        (exact && !request.manifest_path.is_absolute()) ||
        (!exact && !request.manifest_path.empty()) || !valid_text(path_utf8(request.source)) ||
        !valid_text(path_utf8(request.destination)) ||
        !valid_text(path_utf8(request.manifest_path)) ||
        !valid_text(request.expected_source_revision_utf8)) {
        detail_utf8 = "basic-directory-transfer request is incomplete or unsafe";
        return false;
    }
    return true;
}

std::vector<std::byte>
encode_basic_directory_transfer_stream_request(const BasicDirectoryTransferStreamRequest &request) {
    std::string detail;
    if (!valid_basic_directory_transfer_stream_request(request, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> output;
    append_integer(output, requestMagic);
    append_integer(output, kBasicDirectoryTransferProtocolVersion);
    append_integer(output, request.request_id);
    append_integer(output, request.operation_id);
    append_integer(output, static_cast<std::uint8_t>(request.command));
    append_integer(output, static_cast<std::uint8_t>(0));
    append_integer(output, static_cast<std::uint16_t>(0));
    append_string(output, path_utf8(request.source));
    append_string(output, path_utf8(request.destination));
    append_string(output, path_utf8(request.manifest_path));
    append_string(output, request.expected_source_revision_utf8);
    if (output.size() > kMaximumBasicDirectoryTransferProtocolBytes) {
        throw std::length_error("basic-directory-transfer request exceeds its protocol limit");
    }
    return output;
}

bool decode_basic_directory_transfer_stream_request(const std::span<const std::byte> payload,
                                                    BasicDirectoryTransferStreamRequest &request,
                                                    std::string &detail_utf8) {
    if (payload.size() > kMaximumBasicDirectoryTransferProtocolBytes) {
        detail_utf8 = "basic-directory-transfer request exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    BasicDirectoryTransferStreamRequest decoded;
    std::uint8_t command{};
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    std::string source;
    std::string destination;
    std::string manifest;
    std::string expected_source_revision;
    if (!header(cursor, requestMagic, detail_utf8) || !cursor.read(decoded.request_id) ||
        !cursor.read(decoded.operation_id) || !cursor.read(command) || !cursor.read(reserved8) ||
        !cursor.read(reserved16) || !cursor.read_string(source) ||
        !cursor.read_string(destination) || !cursor.read_string(manifest) ||
        !cursor.read_string(expected_source_revision) || reserved8 != 0U || reserved16 != 0U ||
        !valid_command(command) || cursor.remaining() != 0U || !valid_utf8(source) ||
        !valid_utf8(destination) || !valid_utf8(manifest)) {
        detail_utf8 = "basic-directory-transfer request is invalid or truncated";
        return false;
    }
    decoded.command = static_cast<BasicDirectoryTransferCommand>(command);
    decoded.source = path_from_utf8(source);
    decoded.destination = path_from_utf8(destination);
    decoded.manifest_path = path_from_utf8(manifest);
    decoded.expected_source_revision_utf8 = std::move(expected_source_revision);
    if (!valid_basic_directory_transfer_stream_request(decoded, detail_utf8)) {
        return false;
    }
    request = std::move(decoded);
    return true;
}

std::vector<std::byte>
encode_basic_directory_transfer_completion_ack(const BasicDirectoryTransferCompletionAck &ack) {
    if (ack.request_id == 0U) {
        throw std::invalid_argument("basic-directory-transfer completion ack is invalid");
    }
    std::vector<std::byte> output;
    append_integer(output, completionAckMagic);
    append_integer(output, kBasicDirectoryTransferProtocolVersion);
    append_integer(output, ack.request_id);
    return output;
}

bool decode_basic_directory_transfer_completion_ack(const std::span<const std::byte> payload,
                                                    BasicDirectoryTransferCompletionAck &ack,
                                                    std::string &detail_utf8) {
    Cursor cursor(payload);
    BasicDirectoryTransferCompletionAck decoded;
    if (!header(cursor, completionAckMagic, detail_utf8) || !cursor.read(decoded.request_id) ||
        decoded.request_id == 0U || cursor.remaining() != 0U) {
        detail_utf8 = "basic-directory-transfer completion ack is invalid or truncated";
        return false;
    }
    ack = decoded;
    return true;
}

std::vector<std::byte>
encode_basic_directory_transfer_progress_frame(const BasicDirectoryTransferProgressFrame &frame) {
    if (frame.request_id == 0U || !valid_phase(static_cast<std::uint8_t>(frame.progress.phase)) ||
        frame.progress.completed_entries > frame.progress.total_entries ||
        frame.progress.completed_bytes > frame.progress.total_bytes ||
        !absolute_path_or_empty(frame.progress.current_path) ||
        !valid_text(path_utf8(frame.progress.current_path))) {
        throw std::invalid_argument("basic-directory-transfer progress is invalid");
    }
    if (frame.progress.completed_entries > std::numeric_limits<std::uint64_t>::max() ||
        frame.progress.total_entries > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("basic-directory-transfer progress count exceeds protocol range");
    }
    std::vector<std::byte> output;
    append_integer(output, progressMagic);
    append_integer(output, kBasicDirectoryTransferProtocolVersion);
    append_integer(output, frame.request_id);
    append_integer(output, static_cast<std::uint8_t>(frame.progress.phase));
    append_integer(output, static_cast<std::uint8_t>(0));
    append_integer(output, static_cast<std::uint16_t>(0));
    append_integer(output, static_cast<std::uint64_t>(frame.progress.completed_entries));
    append_integer(output, static_cast<std::uint64_t>(frame.progress.total_entries));
    append_integer(output, frame.progress.completed_bytes);
    append_integer(output, frame.progress.total_bytes);
    append_string(output, path_utf8(frame.progress.current_path));
    return output;
}

bool decode_basic_directory_transfer_progress_frame(const std::span<const std::byte> payload,
                                                    BasicDirectoryTransferProgressFrame &frame,
                                                    std::string &detail_utf8) {
    if (payload.size() > kMaximumBasicDirectoryTransferProtocolBytes) {
        detail_utf8 = "basic-directory-transfer progress exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    BasicDirectoryTransferProgressFrame decoded;
    std::uint8_t phase{};
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    std::uint64_t completed_entries{};
    std::uint64_t total_entries{};
    std::string current_path;
    if (!header(cursor, progressMagic, detail_utf8) || !cursor.read(decoded.request_id) ||
        !cursor.read(phase) || !cursor.read(reserved8) || !cursor.read(reserved16) ||
        !cursor.read(completed_entries) || !cursor.read(total_entries) ||
        !cursor.read(decoded.progress.completed_bytes) ||
        !cursor.read(decoded.progress.total_bytes) || !cursor.read_string(current_path) ||
        reserved8 != 0U || reserved16 != 0U || !valid_phase(phase) || cursor.remaining() != 0U ||
        !valid_utf8(current_path) || completed_entries > std::numeric_limits<std::size_t>::max() ||
        total_entries > std::numeric_limits<std::size_t>::max()) {
        detail_utf8 = "basic-directory-transfer progress is invalid or truncated";
        return false;
    }
    decoded.progress.phase = static_cast<BasicDirectoryTransferPhase>(phase);
    decoded.progress.completed_entries = static_cast<std::size_t>(completed_entries);
    decoded.progress.total_entries = static_cast<std::size_t>(total_entries);
    decoded.progress.current_path = path_from_utf8(current_path);
    if (decoded.request_id == 0U ||
        decoded.progress.completed_entries > decoded.progress.total_entries ||
        decoded.progress.completed_bytes > decoded.progress.total_bytes ||
        !absolute_path_or_empty(decoded.progress.current_path)) {
        detail_utf8 = "basic-directory-transfer progress fields are inconsistent";
        return false;
    }
    frame = std::move(decoded);
    return true;
}

std::vector<std::byte>
encode_basic_directory_transfer_result_frame(const BasicDirectoryTransferResultFrame &frame) {
    if (frame.request_id == 0U || !valid_result(frame.result)) {
        throw std::invalid_argument("basic-directory-transfer result is invalid");
    }
    if (frame.result.completed_entries > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("basic-directory-transfer result count exceeds protocol range");
    }
    std::vector<std::byte> output;
    append_integer(output, resultMagic);
    append_integer(output, kBasicDirectoryTransferProtocolVersion);
    append_integer(output, frame.request_id);
    append_integer(output, static_cast<std::uint8_t>(frame.result.status));
    append_integer(output, static_cast<std::uint8_t>(frame.result.recovery_available ? 1U : 0U));
    append_integer(output, static_cast<std::uint16_t>(0));
    append_integer(output, frame.result.platform_code);
    append_integer(output, static_cast<std::int32_t>(frame.result.error.value()));
    append_integer(output, static_cast<std::uint32_t>(0));
    append_integer(output, static_cast<std::uint64_t>(frame.result.completed_entries));
    append_integer(output, frame.result.completed_bytes);
    append_string(output, path_utf8(frame.result.staging_destination));
    append_string(output, path_utf8(frame.result.destination));
    append_string(output, path_utf8(frame.result.manifest_path));
    append_string(output, frame.result.detail_utf8);
    if (output.size() > kMaximumBasicDirectoryTransferProtocolBytes) {
        throw std::length_error("basic-directory-transfer result exceeds its protocol limit");
    }
    return output;
}

bool decode_basic_directory_transfer_result_frame(const std::span<const std::byte> payload,
                                                  BasicDirectoryTransferResultFrame &frame,
                                                  std::string &detail_utf8) {
    if (payload.size() > kMaximumBasicDirectoryTransferProtocolBytes) {
        detail_utf8 = "basic-directory-transfer result exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    BasicDirectoryTransferResultFrame decoded;
    std::uint8_t status{};
    std::uint8_t recovery{};
    std::uint16_t reserved16{};
    std::int32_t error_value{};
    std::uint32_t reserved32{};
    std::uint64_t completed_entries{};
    std::string staging;
    std::string destination;
    std::string manifest;
    if (!header(cursor, resultMagic, detail_utf8) || !cursor.read(decoded.request_id) ||
        !cursor.read(status) || !cursor.read(recovery) || !cursor.read(reserved16) ||
        !cursor.read(decoded.result.platform_code) || !cursor.read(error_value) ||
        !cursor.read(reserved32) || !cursor.read(completed_entries) ||
        !cursor.read(decoded.result.completed_bytes) || !cursor.read_string(staging) ||
        !cursor.read_string(destination) || !cursor.read_string(manifest) ||
        !cursor.read_string(decoded.result.detail_utf8) || reserved16 != 0U || reserved32 != 0U ||
        recovery > 1U || !valid_status(status) || cursor.remaining() != 0U ||
        completed_entries > std::numeric_limits<std::size_t>::max() || !valid_utf8(staging) ||
        !valid_utf8(destination) || !valid_utf8(manifest) ||
        !valid_utf8(decoded.result.detail_utf8)) {
        detail_utf8 = "basic-directory-transfer result is invalid or truncated";
        return false;
    }
    decoded.result.status = static_cast<BasicDirectoryTransferStatus>(status);
    decoded.result.recovery_available = recovery != 0U;
    decoded.result.error = std::error_code(error_value, std::generic_category());
    decoded.result.completed_entries = static_cast<std::size_t>(completed_entries);
    decoded.result.staging_destination = path_from_utf8(staging);
    decoded.result.destination = path_from_utf8(destination);
    decoded.result.manifest_path = path_from_utf8(manifest);
    if (decoded.request_id == 0U || !valid_result(decoded.result)) {
        detail_utf8 = "basic-directory-transfer result fields are inconsistent";
        return false;
    }
    frame = std::move(decoded);
    return true;
}

bool decode_basic_directory_transfer_message_kind(const std::span<const std::byte> payload,
                                                  BasicDirectoryTransferMessageKind &kind,
                                                  std::string &detail_utf8) {
    if (payload.size() < sizeof(std::uint32_t) * 2U ||
        payload.size() > kMaximumBasicDirectoryTransferProtocolBytes) {
        detail_utf8 = "basic-directory-transfer helper message header is invalid";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    if (!cursor.read(magic) || !cursor.read(version) ||
        version != kBasicDirectoryTransferProtocolVersion) {
        detail_utf8 = "basic-directory-transfer helper message header is invalid";
        return false;
    }
    if (magic == progressMagic) {
        kind = BasicDirectoryTransferMessageKind::progress;
        return true;
    }
    if (magic == resultMagic) {
        kind = BasicDirectoryTransferMessageKind::complete;
        return true;
    }
    detail_utf8 = "basic-directory-transfer helper message kind is unknown";
    return false;
}

} // namespace vove::fileops
