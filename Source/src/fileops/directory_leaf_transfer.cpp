#include "vove/fileops/directory_leaf_transfer.hpp"

#include "vove/fileops/current_operation_journal.hpp"

#include "catalog_protocol_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t journalMagic = 0x314c4456U;

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
            count = 1U;
            code_point = first & 0x1fU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            count = 2U;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= count; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((count == 2U && code_point < 0x800U) || (count == 3U && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        index += count + 1U;
    }
    return true;
}

bool digest_empty(const DirectoryTransferManifestDigest &digest) noexcept {
    return std::ranges::all_of(digest, [](const std::uint8_t value) { return value == 0U; });
}

template <std::size_t Size> bool bytes_empty(const std::array<std::uint8_t, Size> &bytes) noexcept {
    return std::ranges::all_of(bytes, [](const std::uint8_t value) { return value == 0U; });
}

bool strict_descendant(const std::filesystem::path &root, const std::filesystem::path &candidate,
                       const DirectoryPathSemantics semantics) {
    const auto root_key = directory_transfer_namespace_key(root, semantics);
    const auto candidate_key = directory_transfer_namespace_key(candidate, semantics);
    if (root_key.empty() || candidate_key.empty() || root_key == candidate_key ||
        !candidate_key.starts_with(root_key)) {
        return false;
    }
    return root_key.ends_with('/') || candidate_key[root_key.size()] == '/';
}

bool same_path(const std::filesystem::path &left, const std::filesystem::path &right,
               const DirectoryPathSemantics semantics) {
    const auto left_key = directory_transfer_namespace_key(left, semantics);
    return !left_key.empty() && left_key == directory_transfer_namespace_key(right, semantics);
}

bool same_snapshot(const SourceSnapshot &left, const SourceSnapshot &right) noexcept {
    return left.size_bytes == right.size_bytes && left.modified_unix_ns == right.modified_unix_ns &&
           left.source_revision_utf8 == right.source_revision_utf8;
}

bool valid_file_binding(const DirectoryLeafTransferBinding &binding, const FileTransferSource &file,
                        std::string &detail_utf8) {
    if (!file.path.is_absolute() || !file.destination.is_absolute() ||
        directory_transfer_canonical_object_identity(file.snapshot.source_revision_utf8,
                                                     binding.path_semantics)
            .empty() ||
        directory_transfer_canonical_object_identity(file.source_parent_revision_utf8,
                                                     binding.path_semantics)
            .empty() ||
        directory_transfer_canonical_object_identity(file.destination_parent_revision_utf8,
                                                     binding.path_semantics)
            .empty()) {
        detail_utf8 = "directory-leaf source evidence is invalid";
        return false;
    }
    if (!strict_descendant(binding.staging_root, file.destination, binding.path_semantics)) {
        detail_utf8 = "directory-leaf destination escapes its staging root";
        return false;
    }
    return true;
}

bool valid_binding(const DirectoryLeafTransferBinding &binding, std::string &detail_utf8) {
    if (binding.version != kDirectoryLeafTransferVersion || binding.parent_operation_id == 0U ||
        digest_empty(binding.parent_manifest_digest) ||
        bytes_empty(binding.staging_ownership_token) ||
        bytes_empty(binding.staging_root_identity_sha256)) {
        detail_utf8 = "directory-leaf parent binding is invalid";
        return false;
    }
    if (binding.root_index >= kMaximumDirectoryTransferRoots ||
        binding.entry_index >= kMaximumDirectoryTransferEntries) {
        detail_utf8 = "directory-leaf manifest index is out of range";
        return false;
    }
#ifdef _WIN32
    if (binding.path_semantics != DirectoryPathSemantics::windows_ordinal_nfc) {
#else
    if (binding.path_semantics != DirectoryPathSemantics::posix_exact) {
#endif
        detail_utf8 = "directory-leaf pathname model is not native to this platform";
        return false;
    }
    if (!binding.staging_root.is_absolute() || binding.staging_root.filename().empty() ||
        directory_transfer_namespace_key(binding.staging_root, binding.path_semantics).empty()) {
        detail_utf8 = "directory-leaf staging root is invalid or unsupported";
        return false;
    }
    const auto root_identity = directory_transfer_canonical_object_identity(
        binding.staging_root_identity_utf8, binding.path_semantics);
    if (root_identity.empty() || root_identity != binding.staging_root_identity_utf8 ||
        directory_transfer_object_identity_sha256(binding.path_semantics,
                                                  binding.staging_root_identity_utf8) !=
            binding.staging_root_identity_sha256) {
        detail_utf8 = "directory-leaf staging root identity is invalid";
        return false;
    }
    return true;
}

} // namespace

bool valid_directory_leaf_transfer_binding(const DirectoryLeafTransferBinding &binding,
                                           std::string &detail_utf8) {
    detail_utf8.clear();
    return valid_binding(binding, detail_utf8);
}

bool valid_directory_leaf_transfer_request(const DirectoryLeafTransferRequest &request,
                                           std::string &detail_utf8) {
    detail_utf8.clear();
    if (!valid_binding(request.binding, detail_utf8)) {
        return false;
    }
    return valid_file_binding(request.binding, request.file, detail_utf8);
}

bool valid_directory_leaf_transfer_journal(const DirectoryLeafTransferJournal &journal,
                                           std::string &detail_utf8) {
    detail_utf8.clear();
    if (!valid_binding(journal.binding, detail_utf8)) {
        return false;
    }
    if (!valid_file_binding(journal.binding, journal.original_file, detail_utf8)) {
        return false;
    }
    if (!valid_file_transfer_transaction(journal.transfer, detail_utf8)) {
        return false;
    }
    if (journal.transfer.kind != FileTransferKind::copy || journal.transfer.items.size() != 1U) {
        detail_utf8 = "directory-leaf journal must contain exactly one copy";
        return false;
    }
    if (!strict_descendant(journal.binding.staging_root, journal.transfer.items.front().destination,
                           journal.binding.path_semantics)) {
        detail_utf8 = "directory-leaf journal destination escapes its staging root";
        return false;
    }
    const auto &item = journal.transfer.items.front();
    if (!item.overwrite_destination_snapshot.source_revision_utf8.empty() ||
        journal.transfer.cancelled) {
        detail_utf8 = "directory-leaf transfer cannot authorize destination replacement";
        return false;
    }
    if (!same_path(item.source, journal.original_file.path, journal.binding.path_semantics) ||
        !same_path(item.destination, journal.original_file.destination,
                   journal.binding.path_semantics) ||
        !same_snapshot(item.original_source_snapshot, journal.original_file.snapshot) ||
        item.source_parent_identity_utf8 != directory_transfer_canonical_object_identity(
                                                journal.original_file.source_parent_revision_utf8,
                                                journal.binding.path_semantics) ||
        item.destination_parent_identity_utf8 !=
            directory_transfer_canonical_object_identity(
                journal.original_file.destination_parent_revision_utf8,
                journal.binding.path_semantics)) {
        detail_utf8 = "directory-leaf transaction changed its immutable source binding";
        return false;
    }
    return true;
}

DirectoryLeafTransferJournal
prepare_directory_leaf_transfer(const DirectoryLeafTransferRequest &request,
                                const std::uint64_t child_operation_id) {
    std::string detail;
    if (!valid_directory_leaf_transfer_request(request, detail)) {
        throw std::invalid_argument(detail);
    }
    DirectoryLeafTransferJournal journal{
        .binding = request.binding,
        .original_file = request.file,
        .transfer = prepare_file_transfer_transaction(FileTransferKind::copy, {request.file},
                                                      child_operation_id),
    };
    if (!valid_directory_leaf_transfer_journal(journal, detail)) {
        throw std::invalid_argument(detail);
    }
    return journal;
}

bool directory_leaf_transfer_matches(const DirectoryLeafTransferJournal &journal,
                                     const DirectoryLeafTransferRequest &expected,
                                     std::string &detail_utf8) {
    if (!valid_directory_leaf_transfer_journal(journal, detail_utf8) ||
        !valid_directory_leaf_transfer_request(expected, detail_utf8)) {
        return false;
    }
    return directory_leaf_transfer_request_matches(
        {.binding = journal.binding, .file = journal.original_file}, expected, detail_utf8);
}

bool directory_leaf_transfer_binding_matches(const DirectoryLeafTransferBinding &actual,
                                             const DirectoryLeafTransferBinding &expected,
                                             std::string &detail_utf8) {
    if (!valid_binding(actual, detail_utf8) || !valid_binding(expected, detail_utf8)) {
        return false;
    }
    if (actual.parent_operation_id != expected.parent_operation_id ||
        actual.parent_manifest_digest != expected.parent_manifest_digest ||
        actual.root_index != expected.root_index || actual.entry_index != expected.entry_index ||
        actual.path_semantics != expected.path_semantics ||
        actual.staging_ownership_token != expected.staging_ownership_token ||
        actual.staging_root_identity_sha256 != expected.staging_root_identity_sha256 ||
        actual.staging_root_identity_utf8 != expected.staging_root_identity_utf8 ||
        !same_path(actual.staging_root, expected.staging_root, expected.path_semantics)) {
        detail_utf8 = "directory-leaf journal belongs to a different parent frontier";
        return false;
    }
    detail_utf8.clear();
    return true;
}

bool directory_leaf_transfer_request_matches(const DirectoryLeafTransferRequest &actual,
                                             const DirectoryLeafTransferRequest &expected,
                                             std::string &detail_utf8) {
    if (!valid_directory_leaf_transfer_request(actual, detail_utf8) ||
        !valid_directory_leaf_transfer_request(expected, detail_utf8) ||
        !directory_leaf_transfer_binding_matches(actual.binding, expected.binding, detail_utf8)) {
        return false;
    }
    if (!same_path(actual.file.path, expected.file.path, expected.binding.path_semantics) ||
        !same_path(actual.file.destination, expected.file.destination,
                   expected.binding.path_semantics) ||
        !same_snapshot(actual.file.snapshot, expected.file.snapshot) ||
        actual.file.source_parent_revision_utf8 != expected.file.source_parent_revision_utf8 ||
        actual.file.destination_parent_revision_utf8 !=
            expected.file.destination_parent_revision_utf8) {
        detail_utf8 = "directory-leaf journal source evidence does not match its manifest entry";
        return false;
    }
    detail_utf8.clear();
    return true;
}

std::vector<std::byte> encode_directory_leaf_transfer(const DirectoryLeafTransferJournal &journal) {
    std::string detail;
    if (!valid_directory_leaf_transfer_journal(journal, detail)) {
        throw std::invalid_argument(detail);
    }
    const auto transaction = encode_file_transfer_transaction(journal.transfer);
    if (transaction.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("directory-leaf transaction is too large");
    }

    std::vector<std::byte> payload;
    payload.reserve(144U + path_utf8(journal.binding.staging_root).size() +
                    journal.binding.staging_root_identity_utf8.size() +
                    path_utf8(journal.original_file.path).size() +
                    path_utf8(journal.original_file.destination).size() +
                    journal.original_file.snapshot.source_revision_utf8.size() +
                    journal.original_file.source_parent_revision_utf8.size() +
                    journal.original_file.destination_parent_revision_utf8.size() +
                    transaction.size());
    append_integer(payload, journalMagic);
    append_integer(payload, journal.binding.version);
    append_integer(payload, journal.binding.parent_operation_id);
    append_integer(payload, journal.binding.root_index);
    append_integer(payload, journal.binding.entry_index);
    append_integer(payload, static_cast<std::uint8_t>(journal.binding.path_semantics));
    append_integer(payload, static_cast<std::uint8_t>(0U));
    append_integer(payload, static_cast<std::uint16_t>(0U));
    for (const auto value : journal.binding.parent_manifest_digest) {
        append_integer(payload, value);
    }
    for (const auto value : journal.binding.staging_ownership_token) {
        append_integer(payload, value);
    }
    for (const auto value : journal.binding.staging_root_identity_sha256) {
        append_integer(payload, value);
    }
    append_string(payload, path_utf8(journal.binding.staging_root));
    append_string(payload, journal.binding.staging_root_identity_utf8);
    append_integer(payload, journal.original_file.snapshot.size_bytes);
    append_integer(payload, journal.original_file.snapshot.modified_unix_ns);
    append_string(payload, path_utf8(journal.original_file.path));
    append_string(payload, path_utf8(journal.original_file.destination));
    append_string(payload, journal.original_file.snapshot.source_revision_utf8);
    append_string(payload, journal.original_file.source_parent_revision_utf8);
    append_string(payload, journal.original_file.destination_parent_revision_utf8);
    append_integer(payload, static_cast<std::uint32_t>(transaction.size()));
    payload.insert(payload.end(), transaction.begin(), transaction.end());
    if (payload.size() > kMaximumCurrentOperationPayloadBytes) {
        throw std::length_error("directory-leaf journal exceeds its size limit");
    }
    return payload;
}

bool decode_directory_leaf_transfer(const std::span<const std::byte> payload,
                                    DirectoryLeafTransferJournal &journal,
                                    std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() > kMaximumCurrentOperationPayloadBytes) {
        detail_utf8 = "directory-leaf journal exceeds its size limit";
        return false;
    }

    Cursor cursor(payload);
    std::uint32_t magic{};
    std::uint8_t semantics{};
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    DirectoryLeafTransferJournal decoded;
    if (!cursor.read(magic) || !cursor.read(decoded.binding.version) ||
        !cursor.read(decoded.binding.parent_operation_id) ||
        !cursor.read(decoded.binding.root_index) || !cursor.read(decoded.binding.entry_index) ||
        !cursor.read(semantics) || !cursor.read(reserved8) || !cursor.read(reserved16) ||
        magic != journalMagic || reserved8 != 0U || reserved16 != 0U) {
        detail_utf8 = "directory-leaf journal header is corrupt";
        return false;
    }
    for (auto &value : decoded.binding.parent_manifest_digest) {
        if (!cursor.read(value)) {
            detail_utf8 = "directory-leaf parent digest is truncated";
            return false;
        }
    }
    for (auto &value : decoded.binding.staging_ownership_token) {
        if (!cursor.read(value)) {
            detail_utf8 = "directory-leaf staging ownership token is truncated";
            return false;
        }
    }
    for (auto &value : decoded.binding.staging_root_identity_sha256) {
        if (!cursor.read(value)) {
            detail_utf8 = "directory-leaf staging root identity is truncated";
            return false;
        }
    }
    std::string staging_root;
    std::string source;
    std::string destination;
    std::uint32_t transaction_size{};
    if (!cursor.read_string(staging_root) ||
        !cursor.read_string(decoded.binding.staging_root_identity_utf8) ||
        !cursor.read(decoded.original_file.snapshot.size_bytes) ||
        !cursor.read(decoded.original_file.snapshot.modified_unix_ns) ||
        !cursor.read_string(source) || !cursor.read_string(destination) ||
        !cursor.read_string(decoded.original_file.snapshot.source_revision_utf8) ||
        !cursor.read_string(decoded.original_file.source_parent_revision_utf8) ||
        !cursor.read_string(decoded.original_file.destination_parent_revision_utf8) ||
        !valid_utf8(staging_root) || !valid_utf8(decoded.binding.staging_root_identity_utf8) ||
        !valid_utf8(source) || !valid_utf8(destination) ||
        !valid_utf8(decoded.original_file.snapshot.source_revision_utf8) ||
        !valid_utf8(decoded.original_file.source_parent_revision_utf8) ||
        !valid_utf8(decoded.original_file.destination_parent_revision_utf8) ||
        !cursor.read(transaction_size) || transaction_size == 0U ||
        cursor.remaining() != transaction_size) {
        detail_utf8 = "directory-leaf journal payload is corrupt";
        return false;
    }
    decoded.binding.path_semantics = static_cast<DirectoryPathSemantics>(semantics);
    try {
        decoded.binding.staging_root = path_from_utf8(staging_root);
        decoded.original_file.path = path_from_utf8(source);
        decoded.original_file.destination = path_from_utf8(destination);
    } catch (const std::exception &) {
        detail_utf8 = "directory-leaf staging root is not a valid path";
        return false;
    }
    const auto transaction_offset = payload.size() - cursor.remaining();
    if (!decode_file_transfer_transaction(payload.subspan(transaction_offset, transaction_size),
                                          decoded.transfer, detail_utf8) ||
        !valid_directory_leaf_transfer_journal(decoded, detail_utf8)) {
        return false;
    }
    journal = std::move(decoded);
    return true;
}

} // namespace vove::fileops
