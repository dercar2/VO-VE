#include "vove/fileops/file_operation_protocol.hpp"

#include "catalog_protocol_codec.hpp"

#include <cstdint>
#include <limits>
#include <string_view>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t requestMagic = 0x31524f46U;
constexpr std::uint32_t deleteRequestMagic = 0x31444f46U;
constexpr std::uint32_t createDirectoryRequestMagic = 0x31434f46U;
constexpr std::uint32_t resultMagic = 0x31534f46U;

std::string path_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    const auto *first = reinterpret_cast<const char8_t *>(text.data());
    return std::filesystem::path(std::u8string(first, first + text.size()));
}

bool valid_status(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(OperationStatus::file_in_use);
}

bool valid_mode(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(RenameMode::trash_restore_preserve_permissions);
}

bool replacement_mode(const RenameMode mode) noexcept {
    return mode == RenameMode::transfer_publish_replace ||
           mode == RenameMode::transfer_atomic_replace;
}

bool valid_delete_mode(const std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(DeleteMode::trash_purge_preserve_permissions);
}

bool valid_utf8(const std::string_view text) noexcept {
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1;
            code_point = first & 0x1FU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
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
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

} // namespace

std::vector<std::byte> encode_rename_request(const RenameRequest &request) {
    const auto source = path_utf8(request.source);
    const auto destination = path_utf8(request.destination);
    const auto destination_anchor = path_utf8(request.destination_anchor_path);
    std::vector<std::byte> payload;
    payload.reserve(64U + source.size() + destination.size() +
                    request.expected_source.source_revision_utf8.size() +
                    request.source_parent_identity_utf8.size() +
                    request.destination_parent_identity_utf8.size() + destination_anchor.size() +
                    request.destination_anchor_identity_utf8.size());
    append_integer(payload, requestMagic);
    append_integer(payload, kFileOperationProtocolVersion);
    append_integer(payload, request.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(request.action));
    append_integer(payload, static_cast<std::uint8_t>(request.mode));
    append_integer(payload, static_cast<std::uint8_t>(request.object_kind));
    append_integer(payload, static_cast<std::uint8_t>(0));
    append_integer(payload, request.expected_source.size_bytes);
    append_integer(payload, request.expected_source.modified_unix_ns);
    append_string(payload, source);
    append_string(payload, destination);
    append_string(payload, request.expected_source.source_revision_utf8);
    append_string(payload, request.source_parent_identity_utf8);
    append_string(payload, request.destination_parent_identity_utf8);
    append_string(payload, destination_anchor);
    append_string(payload, request.destination_anchor_identity_utf8);
    if (replacement_mode(request.mode)) {
        append_integer(payload, request.expected_destination.size_bytes);
        append_integer(payload, request.expected_destination.modified_unix_ns);
        append_string(payload, request.expected_destination.source_revision_utf8);
    }
    append_string(payload, request.trash_security_baseline_sddl_utf8);
    if (payload.size() > kMaximumOperationPayloadBytes) {
        throw std::length_error("file operation request exceeds the protocol limit");
    }
    return payload;
}

bool decode_rename_request(const std::span<const std::byte> payload, RenameRequest &request,
                           std::string &error) {
    if (payload.size() > kMaximumOperationPayloadBytes) {
        error = "file operation request exceeds the protocol limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint8_t action{};
    std::uint8_t mode{};
    std::uint8_t object_kind{};
    std::uint8_t reserved{};
    std::string source;
    std::string destination;
    std::string destination_anchor;
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(request.operation_id) ||
        !cursor.read(action) || !cursor.read(mode) || !cursor.read(object_kind) ||
        !cursor.read(reserved) || !cursor.read(request.expected_source.size_bytes) ||
        !cursor.read(request.expected_source.modified_unix_ns) || !cursor.read_string(source) ||
        !cursor.read_string(destination) ||
        !cursor.read_string(request.expected_source.source_revision_utf8) ||
        !cursor.read_string(request.source_parent_identity_utf8) ||
        !cursor.read_string(request.destination_parent_identity_utf8) ||
        !cursor.read_string(destination_anchor) ||
        !cursor.read_string(request.destination_anchor_identity_utf8)) {
        error = "file operation request is truncated";
        return false;
    }
    request.expected_destination = {};
    if (valid_mode(mode) && replacement_mode(static_cast<RenameMode>(mode)) &&
        (!cursor.read(request.expected_destination.size_bytes) ||
         !cursor.read(request.expected_destination.modified_unix_ns) ||
         !cursor.read_string(request.expected_destination.source_revision_utf8))) {
        error = "file replacement authorization is truncated";
        return false;
    }
    if (!cursor.read_string(request.trash_security_baseline_sddl_utf8)) {
        error = "Trash security baseline is truncated";
        return false;
    }
    if (magic != requestMagic || version != kFileOperationProtocolVersion || reserved != 0 ||
        action > static_cast<std::uint8_t>(RenameAction::reconcile_only) || !valid_mode(mode) ||
        object_kind > static_cast<std::uint8_t>(OperationObjectKind::directory) ||
        cursor.remaining() != 0) {
        error = "file operation request is incompatible";
        return false;
    }
    request.action = static_cast<RenameAction>(action);
    request.mode = static_cast<RenameMode>(mode);
    request.object_kind = static_cast<OperationObjectKind>(object_kind);
    if (!valid_utf8(source) || source.find('\0') != std::string::npos || !valid_utf8(destination) ||
        destination.find('\0') != std::string::npos ||
        !valid_utf8(request.expected_destination.source_revision_utf8) ||
        request.expected_destination.source_revision_utf8.find('\0') != std::string::npos ||
        !valid_utf8(request.expected_source.source_revision_utf8) ||
        request.expected_source.source_revision_utf8.find('\0') != std::string::npos ||
        !valid_utf8(request.source_parent_identity_utf8) ||
        request.source_parent_identity_utf8.find('\0') != std::string::npos ||
        !valid_utf8(request.destination_parent_identity_utf8) ||
        request.destination_parent_identity_utf8.find('\0') != std::string::npos ||
        !valid_utf8(destination_anchor) || destination_anchor.find('\0') != std::string::npos ||
        !valid_utf8(request.destination_anchor_identity_utf8) ||
        request.destination_anchor_identity_utf8.find('\0') != std::string::npos) {
        error = "file operation request text is invalid UTF-8";
        return false;
    }
    request.source = path_from_utf8(source);
    request.destination = path_from_utf8(destination);
    request.destination_anchor_path = path_from_utf8(destination_anchor);
    return valid_rename_request(request, error);
}

std::vector<std::byte> encode_delete_request(const DeleteRequest &request) {
    const auto source = path_utf8(request.source);
    const auto guard = path_utf8(request.guard_path);
    std::vector<std::byte> payload;
    payload.reserve(96U + source.size() + guard.size() +
                    request.expected_source.source_revision_utf8.size() +
                    request.source_parent_identity_utf8.size() +
                    request.expected_guard.source_revision_utf8.size() +
                    request.guard_parent_identity_utf8.size());
    append_integer(payload, deleteRequestMagic);
    append_integer(payload, kFileOperationProtocolVersion);
    append_integer(payload, request.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(request.action));
    append_integer(payload, static_cast<std::uint8_t>(request.mode));
    append_integer(payload, static_cast<std::uint8_t>(request.object_kind));
    append_integer(payload, static_cast<std::uint8_t>(0));
    append_integer(payload, request.expected_source.size_bytes);
    append_integer(payload, request.expected_source.modified_unix_ns);
    append_string(payload, source);
    append_string(payload, request.expected_source.source_revision_utf8);
    append_string(payload, request.source_parent_identity_utf8);
    append_string(payload, guard);
    append_integer(payload, request.expected_guard.size_bytes);
    append_integer(payload, request.expected_guard.modified_unix_ns);
    append_string(payload, request.expected_guard.source_revision_utf8);
    append_string(payload, request.guard_parent_identity_utf8);
    append_string(payload, request.trash_security_baseline_sddl_utf8);
    if (payload.size() > kMaximumOperationPayloadBytes) {
        throw std::length_error("delete request exceeds the protocol limit");
    }
    return payload;
}

bool decode_delete_request(const std::span<const std::byte> payload, DeleteRequest &request,
                           std::string &error) {
    if (payload.size() > kMaximumOperationPayloadBytes) {
        error = "delete request exceeds the protocol limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint8_t action{};
    std::uint8_t mode{};
    std::uint8_t object_kind{};
    std::uint8_t reserved{};
    std::string source;
    std::string guard;
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(request.operation_id) ||
        !cursor.read(action) || !cursor.read(mode) || !cursor.read(object_kind) ||
        !cursor.read(reserved) || !cursor.read(request.expected_source.size_bytes) ||
        !cursor.read(request.expected_source.modified_unix_ns) || !cursor.read_string(source) ||
        !cursor.read_string(request.expected_source.source_revision_utf8) ||
        !cursor.read_string(request.source_parent_identity_utf8) || !cursor.read_string(guard) ||
        !cursor.read(request.expected_guard.size_bytes) ||
        !cursor.read(request.expected_guard.modified_unix_ns) ||
        !cursor.read_string(request.expected_guard.source_revision_utf8) ||
        !cursor.read_string(request.guard_parent_identity_utf8) ||
        !cursor.read_string(request.trash_security_baseline_sddl_utf8)) {
        error = "delete request is truncated";
        return false;
    }
    if (magic != deleteRequestMagic || version != kFileOperationProtocolVersion || reserved != 0 ||
        action > static_cast<std::uint8_t>(DeleteAction::reconcile_only) ||
        !valid_delete_mode(mode) ||
        object_kind > static_cast<std::uint8_t>(OperationObjectKind::directory) ||
        cursor.remaining() != 0) {
        error = "delete request is incompatible";
        return false;
    }
    request.action = static_cast<DeleteAction>(action);
    request.mode = static_cast<DeleteMode>(mode);
    request.object_kind = static_cast<OperationObjectKind>(object_kind);
    if (!valid_utf8(source) || source.find('\0') != std::string::npos ||
        !valid_utf8(request.expected_source.source_revision_utf8) ||
        request.expected_source.source_revision_utf8.find('\0') != std::string::npos ||
        !valid_utf8(request.source_parent_identity_utf8) ||
        request.source_parent_identity_utf8.find('\0') != std::string::npos || !valid_utf8(guard) ||
        guard.find('\0') != std::string::npos ||
        !valid_utf8(request.expected_guard.source_revision_utf8) ||
        request.expected_guard.source_revision_utf8.find('\0') != std::string::npos ||
        !valid_utf8(request.guard_parent_identity_utf8) ||
        request.guard_parent_identity_utf8.find('\0') != std::string::npos) {
        error = "delete request text is invalid UTF-8";
        return false;
    }
    request.source = path_from_utf8(source);
    request.guard_path = path_from_utf8(guard);
    return valid_delete_request(request, error);
}

std::vector<std::byte> encode_create_directory_request(const CreateDirectoryRequest &request) {
    const auto destination = path_utf8(request.destination);
    std::vector<std::byte> payload;
    payload.reserve(48U + destination.size() + request.destination_parent_revision_utf8.size());
    append_integer(payload, createDirectoryRequestMagic);
    append_integer(payload, kFileOperationProtocolVersion);
    append_integer(payload, request.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(request.action));
    append_integer(payload, static_cast<std::uint8_t>(request.mode));
    append_integer(payload, static_cast<std::uint16_t>(0));
    append_string(payload, destination);
    append_string(payload, request.destination_parent_revision_utf8);
    if (payload.size() > kMaximumOperationPayloadBytes) {
        throw std::length_error("create-directory request exceeds the protocol limit");
    }
    return payload;
}

bool decode_create_directory_request(const std::span<const std::byte> payload,
                                     CreateDirectoryRequest &request, std::string &error) {
    if (payload.size() > kMaximumOperationPayloadBytes) {
        error = "create-directory request exceeds the protocol limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint8_t action{};
    std::uint8_t mode{};
    std::uint16_t reserved16{};
    std::string destination;
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(request.operation_id) ||
        !cursor.read(action) || !cursor.read(mode) || !cursor.read(reserved16) ||
        !cursor.read_string(destination) ||
        !cursor.read_string(request.destination_parent_revision_utf8)) {
        error = "create-directory request is truncated";
        return false;
    }
    if (magic != createDirectoryRequestMagic || version != kFileOperationProtocolVersion ||
        action > static_cast<std::uint8_t>(CreateDirectoryAction::reconcile_only) ||
        mode > static_cast<std::uint8_t>(CreateDirectoryMode::trash_internal_remove_empty) ||
        reserved16 != 0 || cursor.remaining() != 0) {
        error = "create-directory request is incompatible";
        return false;
    }
    if (!valid_utf8(destination) || destination.find('\0') != std::string::npos ||
        !valid_utf8(request.destination_parent_revision_utf8) ||
        request.destination_parent_revision_utf8.find('\0') != std::string::npos) {
        error = "create-directory request text is invalid UTF-8";
        return false;
    }
    request.action = static_cast<CreateDirectoryAction>(action);
    request.mode = static_cast<CreateDirectoryMode>(mode);
    request.destination = path_from_utf8(destination);
    return valid_create_directory_request(request, error);
}

bool decode_operation_request_kind(const std::span<const std::byte> payload,
                                   OperationRequestKind &kind, std::string &error) {
    if (payload.size() > kMaximumOperationPayloadBytes) {
        error = "file operation request exceeds the protocol limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    if (!cursor.read(magic) || !cursor.read(version)) {
        error = "file operation request is truncated";
        return false;
    }
    if (version != kFileOperationProtocolVersion) {
        error = "file operation request version is incompatible";
        return false;
    }
    if (magic == requestMagic) {
        kind = OperationRequestKind::rename;
        return true;
    }
    if (magic == deleteRequestMagic) {
        kind = OperationRequestKind::permanent_delete;
        return true;
    }
    if (magic == createDirectoryRequestMagic) {
        kind = OperationRequestKind::create_directory;
        return true;
    }
    error = "file operation request kind is unknown";
    return false;
}

std::vector<std::byte> encode_operation_result(const OperationResult &result) {
    std::vector<std::byte> payload;
    payload.reserve(72U + result.confirmed_snapshot.source_revision_utf8.size() +
                    result.detail_utf8.size());
    append_integer(payload, resultMagic);
    append_integer(payload, kFileOperationProtocolVersion);
    append_integer(payload, result.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(result.status));
    std::uint8_t flags{};
    flags |= result.source_present ? 0x01U : 0U;
    flags |= result.destination_present ? 0x02U : 0U;
    flags |= result.destination_matches_source ? 0x04U : 0U;
    flags |= result.source_matches_expected ? 0x08U : 0U;
    flags |= result.evidence == OperationEvidence::committed ? 0x10U : 0U;
    flags |= result.evidence == OperationEvidence::no_commit ? 0x20U : 0U;
    flags |= result.evidence == OperationEvidence::conflicting ? 0x30U : 0U;
    append_integer(payload, flags);
    append_integer(payload, static_cast<std::uint16_t>(0));
    append_integer(payload, result.platform_code);
    append_integer(payload, result.confirmed_snapshot.size_bytes);
    append_integer(payload, result.confirmed_snapshot.modified_unix_ns);
    append_string(payload, result.confirmed_snapshot.source_revision_utf8);
    append_string(payload, result.detail_utf8);
    if (payload.size() > kMaximumOperationPayloadBytes) {
        throw std::length_error("file operation result exceeds the protocol limit");
    }
    return payload;
}

bool decode_operation_result(const std::span<const std::byte> payload, OperationResult &result,
                             std::string &error) {
    if (payload.size() > kMaximumOperationPayloadBytes) {
        error = "file operation result exceeds the protocol limit";
        return false;
    }
    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint8_t status{};
    std::uint8_t flags{};
    std::uint16_t reserved{};
    if (!cursor.read(magic) || !cursor.read(version) || !cursor.read(result.operation_id) ||
        !cursor.read(status) || !cursor.read(flags) || !cursor.read(reserved) ||
        !cursor.read(result.platform_code) || !cursor.read(result.confirmed_snapshot.size_bytes) ||
        !cursor.read(result.confirmed_snapshot.modified_unix_ns) ||
        !cursor.read_string(result.confirmed_snapshot.source_revision_utf8) ||
        !cursor.read_string(result.detail_utf8)) {
        error = "file operation result is truncated";
        return false;
    }
    if (magic != resultMagic || version != kFileOperationProtocolVersion || reserved != 0 ||
        (flags & 0xc0U) != 0 || !valid_status(status) || cursor.remaining() != 0) {
        error = "file operation result is invalid";
        return false;
    }
    result.status = static_cast<OperationStatus>(status);
    result.source_present = (flags & 0x01U) != 0;
    result.destination_present = (flags & 0x02U) != 0;
    result.destination_matches_source = (flags & 0x04U) != 0;
    result.source_matches_expected = (flags & 0x08U) != 0;
    const auto evidence = flags & 0x30U;
    result.evidence = evidence == 0x10U   ? OperationEvidence::committed
                      : evidence == 0x20U ? OperationEvidence::no_commit
                      : evidence == 0x30U ? OperationEvidence::conflicting
                                          : OperationEvidence::none;
    if (!valid_utf8(result.confirmed_snapshot.source_revision_utf8) ||
        result.confirmed_snapshot.source_revision_utf8.find('\0') != std::string::npos ||
        !valid_utf8(result.detail_utf8) || result.detail_utf8.find('\0') != std::string::npos) {
        error = "file operation result text is invalid UTF-8";
        return false;
    }
    return true;
}

} // namespace vove::fileops
