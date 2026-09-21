#include "vove/fileops/file_transfer_protocol.hpp"

#include "vove/core/reserved_names.hpp"

#include "catalog_protocol_codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t requestMagic = 0x31514654U;
constexpr std::uint32_t reservationMagic = 0x31524654U;
constexpr std::uint32_t ackMagic = 0x31414654U;
constexpr std::uint32_t progressMagic = 0x31504654U;
constexpr std::uint32_t resultMagic = 0x31534654U;
constexpr std::array<std::byte, 8> reservationMarkerMagic{
    std::byte{'V'}, std::byte{'O'}, std::byte{'V'}, std::byte{'E'},
    std::byte{'-'}, std::byte{'R'}, std::byte{'S'}, std::byte{'V'}};
constexpr std::uint32_t reservationMarkerVersion = 1;
static_assert(kFileTransferReservationMarkerBytes ==
              reservationMarkerMagic.size() + sizeof(reservationMarkerVersion) +
                  sizeof(std::uint64_t) + sizeof(std::uint32_t) + kFileTransferRequestTokenBytes);

template <typename Integer>
void store_marker_integer(std::array<std::byte, kFileTransferReservationMarkerBytes> &output,
                          std::size_t &offset, const Integer value) noexcept {
    for (std::size_t index{}; index < sizeof(Integer); ++index) {
        output[offset++] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

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

bool valid_protocol_text(const std::string_view text) noexcept {
    return text.size() <= kMaximumFileTransferProtocolBytes && valid_utf8(text);
}

bool empty_snapshot(const SourceSnapshot &snapshot) noexcept {
    return snapshot.size_bytes == 0 && snapshot.modified_unix_ns == 0 &&
           snapshot.source_revision_utf8.empty();
}

bool valid_snapshot(const SourceSnapshot &snapshot) {
    return !stable_object_identity(snapshot.source_revision_utf8).empty();
}

bool valid_status(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationStatus::file_in_use);
}

bool valid_evidence(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationEvidence::conflicting);
}

bool valid_mode(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(FileTransferStreamMode::probe_reservation);
}

bool digest_empty(const std::array<std::uint8_t, kFileTransferDigestBytes> &digest) noexcept {
    return std::ranges::all_of(digest, [](const std::uint8_t value) { return value == 0; });
}

bool token_empty(const std::array<std::uint8_t, kFileTransferRequestTokenBytes> &token) noexcept {
    return std::ranges::all_of(token, [](const std::uint8_t value) { return value == 0; });
}

void append_token(std::vector<std::byte> &output,
                  const std::array<std::uint8_t, kFileTransferRequestTokenBytes> &token) {
    for (const auto value : token) {
        append_integer(output, value);
    }
}

bool read_token(Cursor &cursor, std::array<std::uint8_t, kFileTransferRequestTokenBytes> &token) {
    for (auto &value : token) {
        if (!cursor.read(value)) {
            return false;
        }
    }
    return true;
}

bool valid_result_pair(const OperationStatus status, const OperationEvidence evidence) noexcept {
    return status == OperationStatus::success ? evidence == OperationEvidence::committed
                                              : evidence != OperationEvidence::committed;
}

void append_snapshot(std::vector<std::byte> &output, const SourceSnapshot &snapshot) {
    append_integer(output, snapshot.size_bytes);
    append_integer(output, snapshot.modified_unix_ns);
    append_string(output, snapshot.source_revision_utf8);
}

bool read_snapshot(Cursor &cursor, SourceSnapshot &snapshot) {
    return cursor.read(snapshot.size_bytes) && cursor.read(snapshot.modified_unix_ns) &&
           cursor.read_string(snapshot.source_revision_utf8);
}

bool header(Cursor &cursor, const std::uint32_t expected_magic, std::string &detail_utf8) {
    std::uint32_t magic{};
    std::uint32_t version{};
    if (!cursor.read(magic) || !cursor.read(version) || magic != expected_magic ||
        version != kFileTransferProtocolVersion) {
        detail_utf8 = "file-transfer protocol header is incompatible or truncated";
        return false;
    }
    return true;
}

} // namespace

std::array<std::byte, kFileTransferReservationMarkerBytes> make_file_transfer_reservation_marker(
    const FileTransferReservationMarkerAddress &address) noexcept {
    std::array<std::byte, kFileTransferReservationMarkerBytes> marker{};
    std::ranges::copy(reservationMarkerMagic, marker.begin());
    std::size_t offset = reservationMarkerMagic.size();
    store_marker_integer(marker, offset, reservationMarkerVersion);
    store_marker_integer(marker, offset, address.operation_id);
    store_marker_integer(marker, offset, address.item_index);
    for (const auto value : address.request_token) {
        marker[offset++] = static_cast<std::byte>(value);
    }
    return marker;
}

bool matches_file_transfer_reservation_marker(
    const std::span<const std::byte> marker,
    const FileTransferReservationMarkerAddress &address) noexcept {
    return marker.size() == kFileTransferReservationMarkerBytes &&
           std::ranges::equal(marker, make_file_transfer_reservation_marker(address));
}

bool valid_file_transfer_stream_request(const FileTransferStreamRequest &request,
                                        std::string &detail_utf8) {
    const auto has_anchor_path = !request.destination_anchor_path.empty();
    const auto has_anchor_identity = !request.destination_anchor_identity_utf8.empty();
    const auto audit = request.mode == FileTransferStreamMode::audit_existing;
    const auto probe = request.mode == FileTransferStreamMode::probe_reservation;
    const auto transfer = !audit && !probe;
    const auto expected_temp_path =
        file_transfer_temp_destination_path(request.temp_destination.parent_path() / "placeholder",
                                            request.operation_id, request.item_index);
    const auto expected_probe_path = file_transfer_temp_destination_path(
        request.source.parent_path() / "placeholder", request.operation_id, request.item_index);
    if (request.operation_id == 0 || request.item_index >= kMaximumFileTransferItems ||
        token_empty(request.request_token) || !request.source.is_absolute() ||
        (transfer &&
         (!request.temp_destination.is_absolute() || request.source == request.temp_destination ||
          request.temp_destination != expected_temp_path ||
          !core::is_transfer_filename(request.temp_destination.filename().u8string()))) ||
        ((audit || probe) && !request.temp_destination.empty()) ||
        (probe && request.source != expected_probe_path) ||
        request.source.filename().u8string().find(u8':') != std::u8string::npos ||
        !valid_mode(static_cast<std::uint8_t>(request.mode)) ||
        (probe ? !empty_snapshot(request.expected_source)
               : !valid_snapshot(request.expected_source)) ||
        request.expected_source.source_revision_utf8.starts_with("win-file:") ||
        request.source_parent_identity_utf8.starts_with("win-file:") ||
        request.destination_parent_identity_utf8.starts_with("win-file:") ||
        stable_object_identity(request.source_parent_identity_utf8) !=
            request.source_parent_identity_utf8 ||
        (transfer && stable_object_identity(request.destination_parent_identity_utf8) !=
                         request.destination_parent_identity_utf8) ||
        ((audit || probe) && !request.destination_parent_identity_utf8.empty()) ||
        has_anchor_path != has_anchor_identity || (audit && !has_anchor_path) ||
        (probe && has_anchor_path) ||
        (has_anchor_path && (!request.destination_anchor_path.is_absolute() ||
                             request.destination_anchor_identity_utf8.starts_with("win-file:") ||
                             stable_object_identity(request.destination_anchor_identity_utf8) !=
                                 request.destination_anchor_identity_utf8))) {
        detail_utf8 = "file-transfer stream request is incomplete or unsafe";
        return false;
    }
    const auto expected_temp = request.mode == FileTransferStreamMode::resume_existing;
    if (expected_temp != valid_snapshot(request.expected_temp) ||
        (expected_temp && request.expected_temp.source_revision_utf8.starts_with("win-file:")) ||
        (expected_temp && request.expected_temp.size_bytes > request.expected_source.size_bytes) ||
        (!expected_temp && !empty_snapshot(request.expected_temp))) {
        detail_utf8 = "file-transfer stream request has an invalid temporary identity";
        return false;
    }
    if (!valid_protocol_text(path_utf8(request.source)) ||
        !valid_protocol_text(path_utf8(request.temp_destination)) ||
        !valid_protocol_text(request.expected_source.source_revision_utf8) ||
        !valid_protocol_text(request.expected_temp.source_revision_utf8) ||
        !valid_protocol_text(request.source_parent_identity_utf8) ||
        !valid_protocol_text(request.destination_parent_identity_utf8) ||
        !valid_protocol_text(path_utf8(request.destination_anchor_path)) ||
        !valid_protocol_text(request.destination_anchor_identity_utf8)) {
        detail_utf8 = "file-transfer stream request text is invalid UTF-8";
        return false;
    }
    return true;
}

std::vector<std::byte>
encode_file_transfer_stream_request(const FileTransferStreamRequest &request) {
    std::string detail;
    if (!valid_file_transfer_stream_request(request, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> output;
    append_integer(output, requestMagic);
    append_integer(output, kFileTransferProtocolVersion);
    append_integer(output, request.operation_id);
    append_integer(output, request.item_index);
    append_token(output, request.request_token);
    append_integer(output, static_cast<std::uint8_t>(request.mode));
    append_integer(output, static_cast<std::uint8_t>(0));
    append_integer(output, static_cast<std::uint16_t>(0));
    append_string(output, path_utf8(request.source));
    append_string(output, path_utf8(request.temp_destination));
    append_snapshot(output, request.expected_source);
    append_snapshot(output, request.expected_temp);
    append_string(output, request.source_parent_identity_utf8);
    append_string(output, request.destination_parent_identity_utf8);
    append_string(output, path_utf8(request.destination_anchor_path));
    append_string(output, request.destination_anchor_identity_utf8);
    if (output.size() > kMaximumFileTransferProtocolBytes) {
        throw std::length_error("file-transfer stream request exceeds its protocol limit");
    }
    return output;
}

bool decode_file_transfer_stream_request(const std::span<const std::byte> payload,
                                         FileTransferStreamRequest &request,
                                         std::string &detail_utf8) {
    if (payload.size() > kMaximumFileTransferProtocolBytes) {
        detail_utf8 = "file-transfer stream request exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    FileTransferStreamRequest decoded;
    std::uint8_t mode{};
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    std::string source;
    std::string temp;
    std::string destination_anchor;
    if (!header(cursor, requestMagic, detail_utf8) || !cursor.read(decoded.operation_id) ||
        !cursor.read(decoded.item_index) || !read_token(cursor, decoded.request_token) ||
        !cursor.read(mode) || !cursor.read(reserved8) || !cursor.read(reserved16) ||
        !cursor.read_string(source) || !cursor.read_string(temp) ||
        !read_snapshot(cursor, decoded.expected_source) ||
        !read_snapshot(cursor, decoded.expected_temp) ||
        !cursor.read_string(decoded.source_parent_identity_utf8) ||
        !cursor.read_string(decoded.destination_parent_identity_utf8) ||
        !cursor.read_string(destination_anchor) ||
        !cursor.read_string(decoded.destination_anchor_identity_utf8) || reserved8 != 0 ||
        reserved16 != 0 || !valid_mode(mode) || cursor.remaining() != 0 || !valid_utf8(source) ||
        !valid_utf8(temp) || !valid_utf8(destination_anchor)) {
        detail_utf8 = "file-transfer stream request is invalid or truncated";
        return false;
    }
    decoded.mode = static_cast<FileTransferStreamMode>(mode);
    decoded.source = path_from_utf8(source);
    decoded.temp_destination = path_from_utf8(temp);
    decoded.destination_anchor_path = path_from_utf8(destination_anchor);
    if (!valid_file_transfer_stream_request(decoded, detail_utf8)) {
        return false;
    }
    request = std::move(decoded);
    return true;
}

std::vector<std::byte>
encode_file_transfer_reservation(const FileTransferReservation &reservation) {
    if (reservation.operation_id == 0 || reservation.item_index >= kMaximumFileTransferItems ||
        token_empty(reservation.request_token) || !valid_snapshot(reservation.temp_snapshot) ||
        reservation.temp_snapshot.source_revision_utf8.starts_with("win-file:") ||
        reservation.destination_parent_identity_utf8.starts_with("win-file:") ||
        stable_object_identity(reservation.destination_parent_identity_utf8) !=
            reservation.destination_parent_identity_utf8 ||
        stable_object_identity(reservation.destination_parent_revision_utf8) !=
            reservation.destination_parent_identity_utf8 ||
        !valid_protocol_text(reservation.temp_snapshot.source_revision_utf8) ||
        !valid_protocol_text(reservation.destination_parent_identity_utf8) ||
        !valid_protocol_text(reservation.destination_parent_revision_utf8)) {
        throw std::invalid_argument("file-transfer reservation is invalid");
    }
    std::vector<std::byte> output;
    append_integer(output, reservationMagic);
    append_integer(output, kFileTransferProtocolVersion);
    append_integer(output, reservation.operation_id);
    append_integer(output, reservation.item_index);
    append_token(output, reservation.request_token);
    append_snapshot(output, reservation.temp_snapshot);
    append_string(output, reservation.destination_parent_identity_utf8);
    append_string(output, reservation.destination_parent_revision_utf8);
    if (output.size() > kMaximumFileTransferProtocolBytes) {
        throw std::length_error("file-transfer reservation exceeds its protocol limit");
    }
    return output;
}

bool decode_file_transfer_reservation(const std::span<const std::byte> payload,
                                      FileTransferReservation &reservation,
                                      std::string &detail_utf8) {
    if (payload.size() > kMaximumFileTransferProtocolBytes) {
        detail_utf8 = "file-transfer reservation exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    FileTransferReservation decoded;
    if (!header(cursor, reservationMagic, detail_utf8) || !cursor.read(decoded.operation_id) ||
        !cursor.read(decoded.item_index) || !read_token(cursor, decoded.request_token) ||
        !read_snapshot(cursor, decoded.temp_snapshot) ||
        !cursor.read_string(decoded.destination_parent_identity_utf8) ||
        !cursor.read_string(decoded.destination_parent_revision_utf8) || cursor.remaining() != 0 ||
        decoded.operation_id == 0 || decoded.item_index >= kMaximumFileTransferItems ||
        token_empty(decoded.request_token) || !valid_snapshot(decoded.temp_snapshot) ||
        decoded.temp_snapshot.source_revision_utf8.starts_with("win-file:") ||
        decoded.destination_parent_identity_utf8.starts_with("win-file:") ||
        stable_object_identity(decoded.destination_parent_identity_utf8) !=
            decoded.destination_parent_identity_utf8 ||
        stable_object_identity(decoded.destination_parent_revision_utf8) !=
            decoded.destination_parent_identity_utf8 ||
        !valid_utf8(decoded.temp_snapshot.source_revision_utf8) ||
        !valid_utf8(decoded.destination_parent_identity_utf8) ||
        !valid_utf8(decoded.destination_parent_revision_utf8)) {
        detail_utf8 = "file-transfer reservation is invalid or truncated";
        return false;
    }
    reservation = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_file_transfer_reservation_ack(const FileTransferReservationAck &ack) {
    if (ack.operation_id == 0 || ack.item_index >= kMaximumFileTransferItems ||
        token_empty(ack.request_token)) {
        throw std::invalid_argument("file-transfer reservation acknowledgement is invalid");
    }
    std::vector<std::byte> output;
    append_integer(output, ackMagic);
    append_integer(output, kFileTransferProtocolVersion);
    append_integer(output, ack.operation_id);
    append_integer(output, ack.item_index);
    append_token(output, ack.request_token);
    append_integer(output, static_cast<std::uint8_t>(ack.accepted ? 1U : 0U));
    append_integer(output, static_cast<std::uint8_t>(0));
    append_integer(output, static_cast<std::uint16_t>(0));
    return output;
}

bool decode_file_transfer_reservation_ack(const std::span<const std::byte> payload,
                                          FileTransferReservationAck &ack,
                                          std::string &detail_utf8) {
    if (payload.size() > kMaximumFileTransferProtocolBytes) {
        detail_utf8 = "file-transfer reservation acknowledgement exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    FileTransferReservationAck decoded;
    std::uint8_t accepted{};
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    if (!header(cursor, ackMagic, detail_utf8) || !cursor.read(decoded.operation_id) ||
        !cursor.read(decoded.item_index) || !read_token(cursor, decoded.request_token) ||
        !cursor.read(accepted) || !cursor.read(reserved8) || !cursor.read(reserved16) ||
        cursor.remaining() != 0 || decoded.operation_id == 0 ||
        decoded.item_index >= kMaximumFileTransferItems || token_empty(decoded.request_token) ||
        accepted > 1U || reserved8 != 0 || reserved16 != 0) {
        detail_utf8 = "file-transfer reservation acknowledgement is invalid or truncated";
        return false;
    }
    decoded.accepted = accepted != 0;
    ack = decoded;
    return true;
}

std::vector<std::byte> encode_file_transfer_progress(const FileTransferProgress &progress) {
    if (progress.operation_id == 0 || progress.item_index >= kMaximumFileTransferItems ||
        token_empty(progress.request_token)) {
        throw std::invalid_argument("file-transfer progress is invalid");
    }
    std::vector<std::byte> output;
    append_integer(output, progressMagic);
    append_integer(output, kFileTransferProtocolVersion);
    append_integer(output, progress.operation_id);
    append_integer(output, progress.item_index);
    append_token(output, progress.request_token);
    append_integer(output, progress.bytes_written);
    return output;
}

bool decode_file_transfer_progress(const std::span<const std::byte> payload,
                                   FileTransferProgress &progress, std::string &detail_utf8) {
    if (payload.size() > kMaximumFileTransferProtocolBytes) {
        detail_utf8 = "file-transfer progress exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    FileTransferProgress decoded;
    if (!header(cursor, progressMagic, detail_utf8) || !cursor.read(decoded.operation_id) ||
        !cursor.read(decoded.item_index) || !read_token(cursor, decoded.request_token) ||
        !cursor.read(decoded.bytes_written) || cursor.remaining() != 0 ||
        decoded.operation_id == 0 || decoded.item_index >= kMaximumFileTransferItems ||
        token_empty(decoded.request_token)) {
        detail_utf8 = "file-transfer progress is invalid or truncated";
        return false;
    }
    progress = decoded;
    return true;
}

std::vector<std::byte> encode_file_transfer_stream_result(const FileTransferStreamResult &result) {
    if (result.operation_id == 0 || result.item_index >= kMaximumFileTransferItems ||
        token_empty(result.request_token) ||
        !valid_status(static_cast<std::uint8_t>(result.status)) ||
        !valid_evidence(static_cast<std::uint8_t>(result.evidence)) ||
        !valid_result_pair(result.status, result.evidence) ||
        !valid_protocol_text(result.source_snapshot.source_revision_utf8) ||
        !valid_protocol_text(result.temp_snapshot.source_revision_utf8) ||
        !valid_protocol_text(result.detail_utf8) ||
        (result.ok() &&
         (!valid_snapshot(result.source_snapshot) || !valid_snapshot(result.temp_snapshot) ||
          result.bytes_written != result.source_snapshot.size_bytes ||
          result.temp_snapshot.size_bytes != result.bytes_written ||
          digest_empty(result.content_sha256)))) {
        throw std::invalid_argument("file-transfer stream result is invalid");
    }
    std::vector<std::byte> output;
    append_integer(output, resultMagic);
    append_integer(output, kFileTransferProtocolVersion);
    append_integer(output, result.operation_id);
    append_integer(output, result.item_index);
    append_token(output, result.request_token);
    append_integer(output, static_cast<std::uint8_t>(result.status));
    append_integer(output, static_cast<std::uint8_t>(result.evidence));
    append_integer(output, static_cast<std::uint16_t>(0));
    append_integer(output, result.platform_code);
    append_integer(output, result.bytes_written);
    append_snapshot(output, result.source_snapshot);
    append_snapshot(output, result.temp_snapshot);
    for (const auto value : result.content_sha256) {
        append_integer(output, value);
    }
    append_string(output, result.detail_utf8);
    if (output.size() > kMaximumFileTransferProtocolBytes) {
        throw std::length_error("file-transfer stream result exceeds its protocol limit");
    }
    return output;
}

bool decode_file_transfer_stream_result(const std::span<const std::byte> payload,
                                        FileTransferStreamResult &result,
                                        std::string &detail_utf8) {
    if (payload.size() > kMaximumFileTransferProtocolBytes) {
        detail_utf8 = "file-transfer stream result exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    FileTransferStreamResult decoded;
    std::uint8_t status{};
    std::uint8_t evidence{};
    std::uint16_t reserved{};
    if (!header(cursor, resultMagic, detail_utf8) || !cursor.read(decoded.operation_id) ||
        !cursor.read(decoded.item_index) || !read_token(cursor, decoded.request_token) ||
        !cursor.read(status) || !cursor.read(evidence) || !cursor.read(reserved) ||
        !cursor.read(decoded.platform_code) || !cursor.read(decoded.bytes_written) ||
        !read_snapshot(cursor, decoded.source_snapshot) ||
        !read_snapshot(cursor, decoded.temp_snapshot) || !valid_status(status) ||
        !valid_evidence(evidence) || reserved != 0) {
        detail_utf8 = "file-transfer stream result is invalid or truncated";
        return false;
    }
    for (auto &value : decoded.content_sha256) {
        if (!cursor.read(value)) {
            detail_utf8 = "file-transfer stream result digest is truncated";
            return false;
        }
    }
    if (!cursor.read_string(decoded.detail_utf8) || cursor.remaining() != 0 ||
        !valid_utf8(decoded.source_snapshot.source_revision_utf8) ||
        !valid_utf8(decoded.temp_snapshot.source_revision_utf8) ||
        !valid_utf8(decoded.detail_utf8)) {
        detail_utf8 = "file-transfer stream result text is invalid or truncated";
        return false;
    }
    decoded.status = static_cast<OperationStatus>(status);
    decoded.evidence = static_cast<OperationEvidence>(evidence);
    if (decoded.operation_id == 0 || decoded.item_index >= kMaximumFileTransferItems ||
        token_empty(decoded.request_token) ||
        !valid_result_pair(decoded.status, decoded.evidence) ||
        (decoded.ok() &&
         (!valid_snapshot(decoded.source_snapshot) || !valid_snapshot(decoded.temp_snapshot) ||
          decoded.bytes_written != decoded.source_snapshot.size_bytes ||
          decoded.temp_snapshot.size_bytes != decoded.bytes_written ||
          digest_empty(decoded.content_sha256)))) {
        detail_utf8 = "file-transfer stream result has inconsistent success evidence";
        return false;
    }
    result = std::move(decoded);
    return true;
}

bool decode_file_transfer_message_kind(const std::span<const std::byte> payload,
                                       FileTransferMessageKind &kind, std::string &detail_utf8) {
    if (payload.size() > kMaximumFileTransferProtocolBytes) {
        detail_utf8 = "file-transfer helper message exceeds its protocol limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    if (!cursor.read(magic) || !cursor.read(version) || version != kFileTransferProtocolVersion) {
        detail_utf8 = "file-transfer helper message header is invalid";
        return false;
    }
    if (magic == reservationMagic) {
        kind = FileTransferMessageKind::reservation;
        return true;
    }
    if (magic == progressMagic) {
        kind = FileTransferMessageKind::progress;
        return true;
    }
    if (magic == resultMagic) {
        kind = FileTransferMessageKind::complete;
        return true;
    }
    detail_utf8 = "file-transfer helper message kind is unknown";
    return false;
}

} // namespace vove::fileops
