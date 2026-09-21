#include "vove/fileops/directory_transfer_control.hpp"

#include "vove/fileops/current_operation_journal.hpp"

#include "catalog_protocol_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t controlMagic = 0x31434456U;

bool digest_empty(const DirectoryTransferManifestDigest &digest) noexcept {
    return std::ranges::all_of(digest, [](const std::uint8_t value) { return value == 0U; });
}

bool valid_kind(const FileTransferKind kind) noexcept {
    return kind == FileTransferKind::copy || kind == FileTransferKind::move;
}

bool valid_current_journal_path(const std::filesystem::path &path) {
    return path.is_absolute() && path.filename() == kCurrentOperationJournalFilename &&
           path.lexically_normal() == path;
}

std::string operation_hex(const std::uint64_t operation_id) {
    if (operation_id == 0U) {
        throw std::invalid_argument("directory-transfer operation ID is zero");
    }
    std::array<char, 16> digits{};
    std::array<char, 16> output{};
    const auto [end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(), operation_id, 16);
    if (error != std::errc{}) {
        throw std::runtime_error("directory-transfer operation ID could not be encoded");
    }
    const auto count = static_cast<std::size_t>(end - digits.data());
    std::fill_n(output.begin(), output.size() - count, '0');
    std::copy(digits.begin(), end, output.end() - static_cast<std::ptrdiff_t>(count));
    return {output.begin(), output.end()};
}

std::filesystem::path derived_path(const std::filesystem::path &current_journal_path,
                                   const std::uint64_t operation_id,
                                   const std::string_view extension) {
    if (!valid_current_journal_path(current_journal_path)) {
        throw std::invalid_argument("current-operation journal path is not canonical");
    }
    return current_journal_path.parent_path() /
           (".vove-directory-" + operation_hex(operation_id) + std::string(extension));
}

bool stored_manifest_self_consistent(
    const DirectoryTransferManifestStoreReadResult &stored_manifest, std::string &detail_utf8) {
    if (!stored_manifest.ok() ||
        (stored_manifest.source != DurableJournalReadSource::primary &&
         stored_manifest.source != DurableJournalReadSource::previous) ||
        !valid_directory_transfer_manifest(stored_manifest.manifest, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "stored directory-transfer manifest is unavailable or invalid";
        }
        return false;
    }
    try {
        const auto encoded = encode_directory_transfer_manifest(stored_manifest.manifest);
        if (encoded.size() != stored_manifest.encoded_size ||
            encoded.size() < kDirectoryTransferManifestDigestBytes) {
            detail_utf8 = "stored directory-transfer manifest size is inconsistent";
            return false;
        }
        const auto digest_begin =
            encoded.end() - static_cast<std::ptrdiff_t>(kDirectoryTransferManifestDigestBytes);
        if (!std::equal(digest_begin, encoded.end(), stored_manifest.digest.begin(),
                        [](const std::byte left, const std::uint8_t right) {
                            return std::to_integer<std::uint8_t>(left) == right;
                        })) {
            detail_utf8 = "stored directory-transfer manifest digest is inconsistent";
            return false;
        }
        return true;
    } catch (const std::exception &error) {
        detail_utf8 =
            std::string("stored directory-transfer manifest cannot be verified: ") + error.what();
        return false;
    }
}

DirectoryTransferControlRecord make_control(const DirectoryTransferManifest &manifest,
                                            const std::span<const std::byte> encoded) {
    if (encoded.size() < kDirectoryTransferManifestDigestBytes) {
        throw std::invalid_argument("encoded directory-transfer manifest has no digest");
    }
    DirectoryTransferControlRecord record;
    record.operation_id = manifest.operation_id;
    record.kind = manifest.kind;
    record.root_count = static_cast<std::uint32_t>(manifest.roots.size());
    record.manifest_encoded_size = encoded.size();
    const auto digest_begin =
        encoded.end() - static_cast<std::ptrdiff_t>(kDirectoryTransferManifestDigestBytes);
    std::transform(digest_begin, encoded.end(), record.manifest_digest.begin(),
                   [](const std::byte value) { return std::to_integer<std::uint8_t>(value); });
    std::string detail;
    if (!valid_directory_transfer_control(record, detail)) {
        throw std::invalid_argument(detail);
    }
    return record;
}

bool control_matches_manifest(const DirectoryTransferControlRecord &record,
                              const DirectoryTransferManifestStoreReadResult &stored_manifest,
                              std::string &detail_utf8) {
    if (!valid_directory_transfer_control(record, detail_utf8) ||
        !stored_manifest_self_consistent(stored_manifest, detail_utf8)) {
        return false;
    }
    if (record.operation_id != stored_manifest.manifest.operation_id ||
        record.kind != stored_manifest.manifest.kind ||
        record.root_count != stored_manifest.manifest.roots.size() ||
        record.manifest_encoded_size != stored_manifest.encoded_size ||
        record.manifest_digest != stored_manifest.digest) {
        detail_utf8 = "directory-transfer control does not bind the stored manifest";
        return false;
    }
    return true;
}

DirectoryTransferBootstrapResult invalid_result(std::string detail_utf8) {
    return {.status = DirectoryTransferBootstrapStatus::invalid_argument,
            .error = {},
            .detail_utf8 = std::move(detail_utf8),
            .control = {},
            .stored_manifest = {},
            .manifest_path = {}};
}

DirectoryTransferBootstrapResult busy_result(const std::error_code &error,
                                             std::string detail_utf8) {
    return {.status = DirectoryTransferBootstrapStatus::busy,
            .error = error,
            .detail_utf8 = std::move(detail_utf8),
            .control = {},
            .stored_manifest = {},
            .manifest_path = {}};
}

DirectoryTransferBootstrapResult
map_manifest_result(const DirectoryTransferManifestStoreResult &result) {
    DirectoryTransferBootstrapStatus status{DirectoryTransferBootstrapStatus::io_error};
    switch (result.status) {
    case DirectoryTransferManifestStoreStatus::success:
        status = DirectoryTransferBootstrapStatus::success;
        break;
    case DirectoryTransferManifestStoreStatus::not_found:
        status = DirectoryTransferBootstrapStatus::not_found;
        break;
    case DirectoryTransferManifestStoreStatus::invalid_manifest:
        status = DirectoryTransferBootstrapStatus::invalid_argument;
        break;
    case DirectoryTransferManifestStoreStatus::incompatible_version:
        status = DirectoryTransferBootstrapStatus::incompatible_version;
        break;
    case DirectoryTransferManifestStoreStatus::corrupt:
        status = DirectoryTransferBootstrapStatus::corrupt;
        break;
    case DirectoryTransferManifestStoreStatus::payload_mismatch:
        status = DirectoryTransferBootstrapStatus::payload_mismatch;
        break;
    case DirectoryTransferManifestStoreStatus::payload_too_large:
        status = DirectoryTransferBootstrapStatus::payload_too_large;
        break;
    case DirectoryTransferManifestStoreStatus::io_error:
        status = DirectoryTransferBootstrapStatus::io_error;
        break;
    }
    return {.status = status,
            .error = result.error,
            .detail_utf8 = result.detail_utf8,
            .control = {},
            .stored_manifest = {},
            .manifest_path = {}};
}

DirectoryTransferBootstrapResult
map_manifest_read(const DirectoryTransferManifestStoreReadResult &result) {
    return map_manifest_result(result);
}

std::optional<DirectoryTransferControlRecord>
decode_current_control(const CurrentOperationJournalReadResult &current,
                       DirectoryTransferBootstrapResult &failure) {
    if (!current.ok()) {
        switch (current.status) {
        case DurableJournalStatus::not_found:
            failure.status = DirectoryTransferBootstrapStatus::not_found;
            break;
        case DurableJournalStatus::corrupt:
            failure.status = DirectoryTransferBootstrapStatus::corrupt;
            break;
        case DurableJournalStatus::payload_mismatch:
            failure.status = DirectoryTransferBootstrapStatus::payload_mismatch;
            break;
        case DurableJournalStatus::payload_too_large:
            failure.status = DirectoryTransferBootstrapStatus::payload_too_large;
            break;
        case DurableJournalStatus::io_error:
            failure.status = DirectoryTransferBootstrapStatus::io_error;
            break;
        case DurableJournalStatus::success:
            break;
        }
        failure.error = current.error;
        return std::nullopt;
    }
    if (current.encoding != CurrentOperationJournalEncoding::typed ||
        current.kind != CurrentOperationKind::directory_transfer) {
        failure.status = DirectoryTransferBootstrapStatus::payload_mismatch;
        failure.detail_utf8 = "another operation owns the current journal";
        return std::nullopt;
    }
    DirectoryTransferControlRecord record;
    std::string detail;
    if (!decode_directory_transfer_control(current.payload, record, detail)) {
        failure.status = DirectoryTransferBootstrapStatus::corrupt;
        failure.detail_utf8 = std::move(detail);
        return std::nullopt;
    }
    return record;
}

DirectoryTransferBootstrapResult load_locked(const std::filesystem::path &current_journal_path,
                                             const CurrentOperationLease &current_lease) {
    if (!current_lease.protects(current_journal_path)) {
        return busy_result(std::make_error_code(std::errc::operation_not_permitted),
                           "current-operation lease protects another path");
    }
    const CurrentOperationJournalStore current_store(current_journal_path);
    DirectoryTransferBootstrapResult failure;
    const auto control = decode_current_control(current_store.read(), failure);
    if (!control) {
        return failure;
    }
    std::filesystem::path manifest_path;
    try {
        manifest_path =
            directory_transfer_manifest_sidecar_path(current_journal_path, control->operation_id);
    } catch (const std::exception &error) {
        return invalid_result(error.what());
    }
    DirectoryTransferManifestStore manifest_store(manifest_path);
    std::error_code sidecar_lease_error;
    auto sidecar_lease = CurrentOperationLease::try_acquire(manifest_path, sidecar_lease_error);
    if (!sidecar_lease.owns_lock()) {
        return busy_result(sidecar_lease_error, "directory-transfer manifest sidecar is busy");
    }
    auto stored_manifest = manifest_store.read_locked(sidecar_lease);
    if (!stored_manifest.ok()) {
        auto result = map_manifest_read(stored_manifest);
        if (stored_manifest.status == DirectoryTransferManifestStoreStatus::not_found) {
            result.status = DirectoryTransferBootstrapStatus::recovery_required;
            result.detail_utf8 =
                "prepared directory-transfer control has no canonical manifest sidecar";
        }
        result.control = *control;
        result.stored_manifest = std::move(stored_manifest);
        result.manifest_path = std::move(manifest_path);
        return result;
    }
    std::string detail;
    if (!control_matches_manifest(*control, stored_manifest, detail)) {
        return {.status = DirectoryTransferBootstrapStatus::payload_mismatch,
                .error = {},
                .detail_utf8 = std::move(detail),
                .control = *control,
                .stored_manifest = std::move(stored_manifest),
                .manifest_path = std::move(manifest_path)};
    }
    return {.status = DirectoryTransferBootstrapStatus::success,
            .error = {},
            .detail_utf8 = {},
            .control = *control,
            .stored_manifest = std::move(stored_manifest),
            .manifest_path = std::move(manifest_path)};
}

} // namespace

std::filesystem::path
directory_transfer_manifest_sidecar_path(const std::filesystem::path &current_journal_path,
                                         const std::uint64_t operation_id) {
    return derived_path(current_journal_path, operation_id, ".manifest");
}

std::filesystem::path
directory_transfer_progress_ledger_path(const std::filesystem::path &current_journal_path,
                                        const std::uint64_t operation_id) {
    return derived_path(current_journal_path, operation_id, ".ledger");
}

bool valid_directory_transfer_control(const DirectoryTransferControlRecord &record,
                                      std::string &detail_utf8) {
    detail_utf8.clear();
    if (record.version != kDirectoryTransferControlVersion || record.operation_id == 0U ||
        !valid_kind(record.kind) || record.root_count == 0U ||
        record.root_count > kMaximumDirectoryTransferRoots ||
        record.manifest_encoded_size < kDirectoryTransferManifestDigestBytes ||
        record.manifest_encoded_size > kMaximumDirectoryTransferManifestBytes ||
        digest_empty(record.manifest_digest)) {
        detail_utf8 = "directory-transfer prepared control is invalid";
        return false;
    }
    return true;
}

std::vector<std::byte>
encode_directory_transfer_control(const DirectoryTransferControlRecord &record) {
    std::string detail;
    if (!valid_directory_transfer_control(record, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> payload;
    payload.reserve(kDirectoryTransferControlEncodedBytes);
    append_integer(payload, controlMagic);
    append_integer(payload, record.version);
    append_integer(payload, record.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(record.kind));
    append_integer(payload, std::uint8_t{});
    append_integer(payload, std::uint16_t{});
    append_integer(payload, record.root_count);
    append_integer(payload, record.manifest_encoded_size);
    for (const auto value : record.manifest_digest) {
        append_integer(payload, value);
    }
    if (payload.size() != kDirectoryTransferControlEncodedBytes) {
        throw std::logic_error("directory-transfer control size accounting failed");
    }
    return payload;
}

bool decode_directory_transfer_control(const std::span<const std::byte> payload,
                                       DirectoryTransferControlRecord &record,
                                       std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() != kDirectoryTransferControlEncodedBytes) {
        detail_utf8 = "directory-transfer control framing is invalid";
        return false;
    }
    Cursor cursor(payload);
    DirectoryTransferControlRecord decoded;
    std::uint32_t magic{};
    std::uint8_t kind{};
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(kind) || !cursor.read(reserved8) ||
        !cursor.read(reserved16) || !cursor.read(decoded.root_count) ||
        !cursor.read(decoded.manifest_encoded_size) || magic != controlMagic ||
        !valid_kind(static_cast<FileTransferKind>(kind)) || reserved8 != 0U || reserved16 != 0U) {
        detail_utf8 = "directory-transfer control header is invalid or truncated";
        return false;
    }
    decoded.kind = static_cast<FileTransferKind>(kind);
    for (auto &value : decoded.manifest_digest) {
        if (!cursor.read(value)) {
            detail_utf8 = "directory-transfer manifest digest is truncated";
            return false;
        }
    }
    if (cursor.remaining() != 0U || !valid_directory_transfer_control(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "directory-transfer control contains trailing or invalid data";
        }
        return false;
    }
    record = decoded;
    return true;
}

DirectoryTransferBootstrapResult
publish_prepared_directory_transfer(const std::filesystem::path &current_journal_path,
                                    const DirectoryTransferManifest &manifest) {
    if (!valid_current_journal_path(current_journal_path)) {
        return invalid_result("current-operation journal path is not canonical");
    }
    std::string manifest_detail;
    if (!valid_directory_transfer_manifest(manifest, manifest_detail)) {
        return invalid_result(std::move(manifest_detail));
    }
    DirectoryTransferControlRecord expected;
    std::vector<std::byte> encoded_manifest;
    std::vector<std::byte> encoded_control;
    try {
        encoded_manifest = encode_directory_transfer_manifest(manifest);
        expected = make_control(manifest, encoded_manifest);
        encoded_control = encode_directory_transfer_control(expected);
    } catch (const std::exception &error) {
        return invalid_result(error.what());
    }

    std::error_code current_lease_error;
    auto current_lease =
        CurrentOperationLease::try_acquire(current_journal_path, current_lease_error);
    if (!current_lease.owns_lock()) {
        return busy_result(current_lease_error, "current-operation journal is busy");
    }
    const CurrentOperationJournalStore current_store(current_journal_path);
    const auto current = current_store.read();
    std::optional<DirectoryTransferControlRecord> existing_control;
    if (current.status != DurableJournalStatus::not_found) {
        DirectoryTransferBootstrapResult failure;
        existing_control = decode_current_control(current, failure);
        if (!existing_control) {
            return failure;
        }
        if (encode_directory_transfer_control(*existing_control) != encoded_control) {
            return {.status = DirectoryTransferBootstrapStatus::payload_mismatch,
                    .error = {},
                    .detail_utf8 = "current directory-transfer control binds another manifest",
                    .control = *existing_control,
                    .stored_manifest = {},
                    .manifest_path = {}};
        }
    }

    const auto manifest_path =
        directory_transfer_manifest_sidecar_path(current_journal_path, manifest.operation_id);
    DirectoryTransferManifestStore manifest_store(manifest_path);
    std::error_code sidecar_lease_error;
    auto sidecar_lease = CurrentOperationLease::try_acquire(manifest_path, sidecar_lease_error);
    if (!sidecar_lease.owns_lock()) {
        return busy_result(sidecar_lease_error, "directory-transfer manifest sidecar is busy");
    }
    const auto persisted = manifest_store.persist_immutable_locked(manifest, sidecar_lease);
    if (!persisted.ok()) {
        auto result = map_manifest_result(persisted);
        result.manifest_path = manifest_path;
        return result;
    }
    auto stored_manifest = manifest_store.read_locked(sidecar_lease);
    if (!stored_manifest.ok()) {
        auto result = map_manifest_read(stored_manifest);
        if (stored_manifest.status == DirectoryTransferManifestStoreStatus::not_found) {
            if (existing_control) {
                result.status = DirectoryTransferBootstrapStatus::recovery_required;
                result.control = *existing_control;
                result.detail_utf8 =
                    "prepared directory-transfer control lost its canonical manifest sidecar";
            } else {
                result.status = DirectoryTransferBootstrapStatus::io_error;
                result.detail_utf8 =
                    "directory-transfer manifest sidecar vanished before control publication";
            }
        }
        result.stored_manifest = std::move(stored_manifest);
        result.manifest_path = manifest_path;
        return result;
    }

    std::string stored_detail;
    if (!control_matches_manifest(expected, stored_manifest, stored_detail)) {
        return {.status = DirectoryTransferBootstrapStatus::payload_mismatch,
                .error = {},
                .detail_utf8 = std::move(stored_detail),
                .control = expected,
                .stored_manifest = std::move(stored_manifest),
                .manifest_path = manifest_path};
    }
    for (std::size_t copy{}; copy < 2U; ++copy) {
        const auto written =
            current_store.write(CurrentOperationKind::directory_transfer, encoded_control);
        if (!written.ok()) {
            return {.status = written.status == DurableJournalStatus::payload_too_large
                                  ? DirectoryTransferBootstrapStatus::payload_too_large
                                  : DirectoryTransferBootstrapStatus::io_error,
                    .error = written.error,
                    .detail_utf8 = "directory-transfer control could not be published",
                    .control = expected,
                    .stored_manifest = std::move(stored_manifest),
                    .manifest_path = manifest_path};
        }
    }
    const auto verified_current = current_store.read();
    DirectoryTransferBootstrapResult decode_failure;
    const auto verified_control = decode_current_control(verified_current, decode_failure);
    if (!verified_control ||
        encode_directory_transfer_control(*verified_control) != encoded_control) {
        if (verified_control) {
            decode_failure.status = DirectoryTransferBootstrapStatus::payload_mismatch;
            decode_failure.detail_utf8 = "directory-transfer control changed while published";
        }
        return decode_failure;
    }
    stored_manifest = manifest_store.read_locked(sidecar_lease);
    if (!stored_manifest.ok()) {
        auto result = map_manifest_read(stored_manifest);
        if (stored_manifest.status == DirectoryTransferManifestStoreStatus::not_found) {
            result.status = DirectoryTransferBootstrapStatus::recovery_required;
            result.detail_utf8 =
                "published directory-transfer control lost its canonical manifest sidecar";
        }
        result.control = *verified_control;
        result.stored_manifest = std::move(stored_manifest);
        result.manifest_path = manifest_path;
        return result;
    }
    if (!control_matches_manifest(*verified_control, stored_manifest, manifest_detail)) {
        return {.status = DirectoryTransferBootstrapStatus::payload_mismatch,
                .error = {},
                .detail_utf8 = std::move(manifest_detail),
                .control = *verified_control,
                .stored_manifest = std::move(stored_manifest),
                .manifest_path = manifest_path};
    }
    return {.status = DirectoryTransferBootstrapStatus::success,
            .error = {},
            .detail_utf8 = {},
            .control = *verified_control,
            .stored_manifest = std::move(stored_manifest),
            .manifest_path = manifest_path};
}

DirectoryTransferBootstrapResult
load_prepared_directory_transfer(const std::filesystem::path &current_journal_path) {
    if (!valid_current_journal_path(current_journal_path)) {
        return invalid_result("current-operation journal path is not canonical");
    }
    std::error_code current_lease_error;
    auto current_lease =
        CurrentOperationLease::try_acquire(current_journal_path, current_lease_error);
    if (!current_lease.owns_lock()) {
        return busy_result(current_lease_error, "current-operation journal is busy");
    }
    return load_locked(current_journal_path, current_lease);
}

} // namespace vove::fileops
