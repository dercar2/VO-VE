#include "vove/fileops/directory_transfer_progress.hpp"

#include "catalog_protocol_codec.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::array<std::uint8_t, 8U> headerMagic{'V', 'O', 'V', 'E', 'D', 'T', 'L', '1'};
constexpr std::uint32_t recordMagic = 0x31525444U;
constexpr std::array<std::uint8_t, 8U> treeSeedDomain{'V', 'O', 'V', 'E', 'T', 'R', '1', 0U};
constexpr std::array<std::uint8_t, 8U> treeEntryDomain{'V', 'O', 'V', 'E', 'T', 'E', '1', 0U};
constexpr std::array<std::uint8_t, 13U> identityDomain{'V', 'O', 'V', 'E', '-', 'D', 'T',
                                                       '-', 'I', 'D', '-', 'v', '1'};

template <std::size_t Size> bool bytes_empty(const std::array<std::uint8_t, Size> &bytes) noexcept {
    return std::ranges::all_of(bytes, [](const std::uint8_t value) { return value == 0U; });
}

bool valid_event_kind(const DirectoryTransferProgressEventKind kind) noexcept {
    return kind == DirectoryTransferProgressEventKind::root_staging_owned ||
           kind == DirectoryTransferProgressEventKind::entry_materialized ||
           kind == DirectoryTransferProgressEventKind::root_published;
}

bool valid_object_kind(const DirectoryTransferProgressObjectKind kind) noexcept {
    return kind == DirectoryTransferProgressObjectKind::root ||
           kind == DirectoryTransferProgressObjectKind::directory ||
           kind == DirectoryTransferProgressObjectKind::regular_file;
}

template <std::size_t Size>
void append_bytes(std::vector<std::byte> &output, const std::array<std::uint8_t, Size> &bytes) {
    for (const auto value : bytes) {
        append_integer(output, value);
    }
}

template <std::size_t Size> bool read_bytes(Cursor &cursor, std::array<std::uint8_t, Size> &bytes) {
    for (auto &value : bytes) {
        if (!cursor.read(value)) {
            return false;
        }
    }
    return true;
}

DirectoryTransferProgressDigest digest_bytes(const std::span<const std::byte> bytes) noexcept {
    return detail::sha256(bytes);
}

std::vector<std::byte> encode_header_prefix(const DirectoryTransferProgressHeader &header) {
    std::vector<std::byte> payload;
    payload.reserve(kDirectoryTransferProgressHeaderBytes - kDirectoryTransferProgressDigestBytes);
    append_bytes(payload, headerMagic);
    append_integer(payload, header.version);
    append_integer(payload, header.header_bytes);
    append_integer(payload, header.record_bytes);
    append_integer(payload, header.hash_algorithm);
    append_integer(payload, static_cast<std::uint8_t>(header.kind));
    append_integer(payload, header.flags);
    append_integer(payload, header.root_count);
    append_integer(payload, header.entry_count);
    append_integer(payload, std::uint32_t{});
    append_integer(payload, header.operation_id);
    append_integer(payload, header.manifest_encoded_size);
    append_bytes(payload, header.manifest_digest);
    append_integer(payload, header.maximum_record_count);
    append_integer(payload, std::uint32_t{});
    append_integer(payload, std::uint32_t{});
    append_integer(payload, std::uint32_t{});
    if (payload.size() !=
        kDirectoryTransferProgressHeaderBytes - kDirectoryTransferProgressDigestBytes) {
        throw std::logic_error("directory-transfer progress header size accounting failed");
    }
    return payload;
}

std::vector<std::byte> encode_record_prefix(const DirectoryTransferProgressRecord &record) {
    std::vector<std::byte> payload;
    payload.reserve(kDirectoryTransferProgressRecordBytes - kDirectoryTransferProgressDigestBytes);
    append_integer(payload, recordMagic);
    append_integer(payload, record.version);
    append_integer(payload, static_cast<std::uint8_t>(record.kind));
    append_integer(payload, static_cast<std::uint8_t>(record.object_kind));
    append_integer(payload, std::uint8_t{});
    append_integer(payload, record.sequence);
    append_integer(payload, record.root_index);
    append_integer(payload, record.entry_index);
    append_integer(payload, record.content_bytes);
    append_bytes(payload, record.ownership_token);
    append_bytes(payload, record.object_identity_sha256);
    append_bytes(payload, record.content_sha256);
    append_bytes(payload, record.previous_record_sha256);
    if (payload.size() !=
        kDirectoryTransferProgressRecordBytes - kDirectoryTransferProgressDigestBytes) {
        throw std::logic_error("directory-transfer progress record size accounting failed");
    }
    return payload;
}

bool basic_header_valid(const DirectoryTransferProgressHeader &header, std::string &detail_utf8) {
    detail_utf8.clear();
    const auto expected_records = static_cast<std::size_t>(header.entry_count) +
                                  2U * static_cast<std::size_t>(header.root_count);
    if (header.version != kDirectoryTransferProgressVersion ||
        header.header_bytes != kDirectoryTransferProgressHeaderBytes ||
        header.record_bytes != kDirectoryTransferProgressRecordBytes ||
        header.hash_algorithm != 1U || header.kind != FileTransferKind::copy ||
        header.flags != 0U || header.operation_id == 0U || header.root_count == 0U ||
        header.root_count > kMaximumDirectoryTransferRoots ||
        header.entry_count > kMaximumDirectoryTransferEntries ||
        expected_records > kMaximumDirectoryTransferProgressRecords ||
        header.maximum_record_count != expected_records ||
        header.manifest_encoded_size < kDirectoryTransferManifestDigestBytes ||
        header.manifest_encoded_size > kMaximumDirectoryTransferManifestBytes ||
        bytes_empty(header.manifest_digest) || bytes_empty(header.header_sha256)) {
        detail_utf8 = "directory-transfer progress header is invalid";
        return false;
    }
    try {
        if (digest_bytes(encode_header_prefix(header)) != header.header_sha256) {
            detail_utf8 = "directory-transfer progress header digest is invalid";
            return false;
        }
    } catch (const std::exception &error) {
        detail_utf8 =
            std::string("directory-transfer progress header cannot be verified: ") + error.what();
        return false;
    }
    return true;
}

bool basic_record_valid(const DirectoryTransferProgressRecord &record, std::string &detail_utf8) {
    detail_utf8.clear();
    if (record.version != kDirectoryTransferProgressVersion || !valid_event_kind(record.kind) ||
        !valid_object_kind(record.object_kind) || record.sequence == 0U ||
        record.root_index >= kMaximumDirectoryTransferRoots ||
        (record.entry_index != kDirectoryTransferProgressNoEntry &&
         record.entry_index >= kMaximumDirectoryTransferEntries) ||
        bytes_empty(record.previous_record_sha256) || bytes_empty(record.record_sha256)) {
        detail_utf8 = "directory-transfer progress record is invalid";
        return false;
    }
    try {
        if (digest_bytes(encode_record_prefix(record)) != record.record_sha256) {
            detail_utf8 = "directory-transfer progress record digest is invalid";
            return false;
        }
    } catch (const std::exception &error) {
        detail_utf8 =
            std::string("directory-transfer progress record cannot be verified: ") + error.what();
        return false;
    }
    return true;
}

DirectoryTransferManifestDigest manifest_digest(const std::vector<std::byte> &encoded) {
    if (encoded.size() < kDirectoryTransferManifestDigestBytes) {
        throw std::invalid_argument("encoded directory-transfer manifest has no digest");
    }
    DirectoryTransferManifestDigest digest{};
    const auto first =
        encoded.end() - static_cast<std::ptrdiff_t>(kDirectoryTransferManifestDigestBytes);
    std::transform(first, encoded.end(), digest.begin(),
                   [](const std::byte value) { return std::to_integer<std::uint8_t>(value); });
    return digest;
}

bool control_binds_manifest(const DirectoryTransferControlRecord &control,
                            const DirectoryTransferManifest &manifest,
                            std::vector<std::byte> &encoded_manifest, std::string &detail_utf8) {
    if (!valid_directory_transfer_control(control, detail_utf8) ||
        !valid_directory_transfer_manifest(manifest, detail_utf8)) {
        return false;
    }
    if (control.kind != FileTransferKind::copy || manifest.kind != FileTransferKind::copy) {
        detail_utf8 = "directory-transfer progress version 1 supports copy only";
        return false;
    }
    try {
        encoded_manifest = encode_directory_transfer_manifest(manifest);
    } catch (const std::exception &error) {
        detail_utf8 = std::string("directory-transfer manifest cannot be encoded: ") + error.what();
        return false;
    }
    if (control.operation_id != manifest.operation_id ||
        control.root_count != manifest.roots.size() ||
        control.manifest_encoded_size != encoded_manifest.size() ||
        control.manifest_digest != manifest_digest(encoded_manifest)) {
        detail_utf8 = "directory-transfer control does not bind the progress manifest";
        return false;
    }
    return true;
}

DirectoryTransferProgressDigest tree_seed(const DirectoryTransferProgressHeader &header,
                                          const std::uint32_t root_index,
                                          const DirectoryTransferProgressOwnershipToken &token,
                                          const DirectoryTransferProgressDigest &identity) {
    std::vector<std::byte> bytes;
    bytes.reserve(treeSeedDomain.size() + header.manifest_digest.size() + sizeof(root_index) +
                  token.size() + identity.size());
    append_bytes(bytes, treeSeedDomain);
    append_bytes(bytes, header.manifest_digest);
    append_integer(bytes, root_index);
    append_bytes(bytes, token);
    append_bytes(bytes, identity);
    return digest_bytes(bytes);
}

DirectoryTransferProgressDigest tree_step(const DirectoryTransferProgressDigest &previous,
                                          const DirectoryTransferProgressRecord &record) {
    std::vector<std::byte> bytes;
    bytes.reserve(treeEntryDomain.size() + previous.size() + sizeof(record.entry_index) + 4U +
                  record.object_identity_sha256.size() + sizeof(record.content_bytes) +
                  record.content_sha256.size());
    append_bytes(bytes, treeEntryDomain);
    append_bytes(bytes, previous);
    append_integer(bytes, record.entry_index);
    append_integer(bytes, static_cast<std::uint8_t>(record.object_kind));
    append_integer(bytes, std::uint8_t{});
    append_integer(bytes, std::uint16_t{});
    append_bytes(bytes, record.object_identity_sha256);
    append_integer(bytes, record.content_bytes);
    append_bytes(bytes, record.content_sha256);
    return digest_bytes(bytes);
}

std::uint64_t root_content_bytes(const DirectoryTransferManifest &manifest,
                                 const DirectoryTransferManifestRoot &root) {
    std::uint64_t total{};
    const auto end = static_cast<std::size_t>(root.first_entry) + root.entry_count;
    for (auto index = static_cast<std::size_t>(root.first_entry); index < end; ++index) {
        const auto &entry = manifest.entries[index];
        if (entry.kind != DirectoryManifestEntryKind::regular_file) {
            continue;
        }
        if (entry.source_snapshot.size_bytes > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::overflow_error("directory-transfer root byte count overflowed");
        }
        total += entry.source_snapshot.size_bytes;
    }
    return total;
}

} // namespace

struct DirectoryTransferProgressStateAccess final {
    static bool state_binds_operation(const DirectoryTransferProgressHeader &header,
                                      const DirectoryTransferManifest &manifest,
                                      const DirectoryTransferProgressState &state,
                                      std::string &detail_utf8) {
        if (state.operation_id_ != header.operation_id ||
            state.operation_id_ != manifest.operation_id ||
            state.root_count_ != header.root_count || state.root_count_ != manifest.roots.size() ||
            state.entry_count_ != header.entry_count ||
            state.entry_count_ != manifest.entries.size() ||
            state.manifest_digest_ != header.manifest_digest || state.next_sequence_ == 0U ||
            state.next_sequence_ > static_cast<std::uint64_t>(header.maximum_record_count) + 1U ||
            state.completed_roots_ > manifest.roots.size() ||
            bytes_empty(state.last_record_sha256_)) {
            detail_utf8 = "directory-transfer progress state does not bind the prepared operation";
            return false;
        }

        std::uint64_t consumed = static_cast<std::uint64_t>(state.completed_roots_) * 2U;
        const auto active_fields_empty = bytes_empty(state.active_ownership_token_) &&
                                         bytes_empty(state.active_root_identity_sha256_) &&
                                         bytes_empty(state.rolling_tree_sha256_) &&
                                         state.active_content_bytes_ == 0U;
        switch (state.phase_) {
        case DirectoryTransferRootProgressPhase::awaiting_staging:
            if (state.completed_roots_ >= manifest.roots.size() || !active_fields_empty ||
                state.next_manifest_entry_ != manifest.roots[state.completed_roots_].first_entry) {
                detail_utf8 = "directory-transfer progress awaiting state is inconsistent";
                return false;
            }
            consumed += state.next_manifest_entry_;
            break;
        case DirectoryTransferRootProgressPhase::materializing: {
            if (state.completed_roots_ >= manifest.roots.size() ||
                bytes_empty(state.active_ownership_token_) ||
                bytes_empty(state.active_root_identity_sha256_) ||
                bytes_empty(state.rolling_tree_sha256_)) {
                detail_utf8 =
                    "directory-transfer progress materializing state has no ownership proof";
                return false;
            }
            const auto &root = manifest.roots[state.completed_roots_];
            const auto end = static_cast<std::uint32_t>(root.first_entry + root.entry_count);
            if (state.next_manifest_entry_ < root.first_entry || state.next_manifest_entry_ > end) {
                detail_utf8 = "directory-transfer progress entry cursor is outside its root";
                return false;
            }
            consumed += 1U + state.next_manifest_entry_;
            break;
        }
        case DirectoryTransferRootProgressPhase::complete:
            if (state.completed_roots_ != manifest.roots.size() || !active_fields_empty ||
                state.next_manifest_entry_ != kDirectoryTransferProgressNoEntry) {
                detail_utf8 = "completed directory-transfer progress state is inconsistent";
                return false;
            }
            consumed += state.entry_count_;
            break;
        }
        if (state.next_sequence_ != consumed + 1U) {
            detail_utf8 = "directory-transfer progress sequence does not match its state";
            return false;
        }
        return true;
    }

    static bool event_shape_valid(const DirectoryTransferProgressHeader &header,
                                  const DirectoryTransferProgressRecord &record,
                                  const DirectoryTransferManifest &manifest,
                                  DirectoryTransferProgressState &next, std::string &detail_utf8) {
        if (next.complete() || next.completed_roots_ >= manifest.roots.size()) {
            detail_utf8 = "directory-transfer progress continues after completion";
            return false;
        }
        if (record.root_index != next.completed_roots_) {
            detail_utf8 = "directory-transfer progress violates the strict root prefix";
            return false;
        }
        const auto &root = manifest.roots[next.completed_roots_];
        const auto root_end = static_cast<std::uint32_t>(root.first_entry + root.entry_count);

        switch (record.kind) {
        case DirectoryTransferProgressEventKind::root_staging_owned: {
            const auto expected_tree = tree_seed(header, record.root_index, record.ownership_token,
                                                 record.object_identity_sha256);
            if (next.phase_ != DirectoryTransferRootProgressPhase::awaiting_staging ||
                record.object_kind != DirectoryTransferProgressObjectKind::root ||
                record.entry_index != kDirectoryTransferProgressNoEntry ||
                record.content_bytes != 0U || record.ownership_token != root.ownership_token ||
                bytes_empty(record.object_identity_sha256) ||
                record.content_sha256 != expected_tree) {
                detail_utf8 = "directory-transfer staging-root evidence is invalid or out of order";
                return false;
            }
            next.phase_ = DirectoryTransferRootProgressPhase::materializing;
            next.next_manifest_entry_ = root.first_entry;
            next.active_content_bytes_ = 0U;
            next.active_ownership_token_ = record.ownership_token;
            next.active_root_identity_sha256_ = record.object_identity_sha256;
            next.rolling_tree_sha256_ = expected_tree;
            return true;
        }

        case DirectoryTransferProgressEventKind::entry_materialized: {
            if (next.phase_ != DirectoryTransferRootProgressPhase::materializing ||
                next.next_manifest_entry_ >= root_end ||
                record.entry_index != next.next_manifest_entry_ ||
                !bytes_empty(record.ownership_token) ||
                bytes_empty(record.object_identity_sha256)) {
                detail_utf8 = "directory-transfer entry evidence is invalid or out of order";
                return false;
            }
            const auto &entry = manifest.entries[next.next_manifest_entry_];
            const auto expected_kind = entry.kind == DirectoryManifestEntryKind::directory
                                           ? DirectoryTransferProgressObjectKind::directory
                                           : DirectoryTransferProgressObjectKind::regular_file;
            if (record.object_kind != expected_kind) {
                detail_utf8 = "directory-transfer entry kind does not match the manifest";
                return false;
            }
            if (entry.kind == DirectoryManifestEntryKind::directory) {
                if (record.content_bytes != 0U || !bytes_empty(record.content_sha256)) {
                    detail_utf8 = "directory-transfer directory evidence has a file payload";
                    return false;
                }
            } else {
                if (bytes_empty(record.content_sha256) ||
                    record.content_bytes != entry.source_snapshot.size_bytes ||
                    record.content_bytes >
                        std::numeric_limits<std::uint64_t>::max() - next.active_content_bytes_) {
                    detail_utf8 = "directory-transfer file evidence does not match the manifest";
                    return false;
                }
                next.active_content_bytes_ += record.content_bytes;
            }
            next.rolling_tree_sha256_ = tree_step(next.rolling_tree_sha256_, record);
            ++next.next_manifest_entry_;
            return true;
        }

        case DirectoryTransferProgressEventKind::root_published: {
            std::uint64_t expected_bytes{};
            try {
                expected_bytes = root_content_bytes(manifest, root);
            } catch (const std::exception &error) {
                detail_utf8 = error.what();
                return false;
            }
            if (next.phase_ != DirectoryTransferRootProgressPhase::materializing ||
                next.next_manifest_entry_ != root_end ||
                record.object_kind != DirectoryTransferProgressObjectKind::root ||
                record.entry_index != kDirectoryTransferProgressNoEntry ||
                record.ownership_token != next.active_ownership_token_ ||
                record.object_identity_sha256 != next.active_root_identity_sha256_ ||
                record.content_bytes != expected_bytes ||
                record.content_bytes != next.active_content_bytes_ ||
                record.content_sha256 != next.rolling_tree_sha256_) {
                detail_utf8 =
                    "directory-transfer root publication evidence is invalid or premature";
                return false;
            }
            ++next.completed_roots_;
            next.active_content_bytes_ = 0U;
            next.active_ownership_token_ = {};
            next.active_root_identity_sha256_ = {};
            next.rolling_tree_sha256_ = {};
            if (next.completed_roots_ == manifest.roots.size()) {
                next.phase_ = DirectoryTransferRootProgressPhase::complete;
                next.next_manifest_entry_ = kDirectoryTransferProgressNoEntry;
            } else {
                next.phase_ = DirectoryTransferRootProgressPhase::awaiting_staging;
                next.next_manifest_entry_ = manifest.roots[next.completed_roots_].first_entry;
            }
            return true;
        }
        }
        detail_utf8 = "directory-transfer progress event kind is invalid";
        return false;
    }

    static bool apply_record(const DirectoryTransferProgressHeader &header,
                             const DirectoryTransferManifest &manifest,
                             const DirectoryTransferProgressRecord &record,
                             DirectoryTransferProgressState &state, std::string &detail_utf8) {
        if (!state_binds_operation(header, manifest, state, detail_utf8)) {
            return false;
        }
        if (!basic_record_valid(record, detail_utf8)) {
            return false;
        }
        if (record.sequence != state.next_sequence_ ||
            record.sequence > header.maximum_record_count ||
            record.previous_record_sha256 != state.last_record_sha256_) {
            detail_utf8 = "directory-transfer progress hash chain or sequence is discontinuous";
            return false;
        }
        auto next = state;
        if (!event_shape_valid(header, record, manifest, next, detail_utf8)) {
            return false;
        }
        next.last_record_sha256_ = record.record_sha256;
        ++next.next_sequence_;
        if (next.complete() && next.next_sequence_ != header.maximum_record_count + 1U) {
            detail_utf8 = "completed directory-transfer progress has the wrong record count";
            return false;
        }
        state = next;
        return true;
    }

    static bool seal_and_apply(const DirectoryTransferProgressHeader &header,
                               const DirectoryTransferManifest &manifest,
                               DirectoryTransferProgressRecord candidate,
                               DirectoryTransferProgressState &state,
                               DirectoryTransferProgressRecord &record, std::string &detail_utf8) {
        if (!state_binds_operation(header, manifest, state, detail_utf8)) {
            return false;
        }
        candidate.sequence = state.next_sequence_;
        candidate.previous_record_sha256 = state.last_record_sha256_;
        try {
            candidate.record_sha256 = digest_bytes(encode_record_prefix(candidate));
        } catch (const std::exception &error) {
            detail_utf8 =
                std::string("directory-transfer progress cannot be sealed: ") + error.what();
            return false;
        }
        auto next = state;
        if (!apply_record(header, manifest, candidate, next, detail_utf8)) {
            return false;
        }
        state = next;
        record = candidate;
        return true;
    }
};

DirectoryTransferProgressPlan::DirectoryTransferProgressPlan(DirectoryTransferControlRecord control,
                                                             DirectoryTransferManifest manifest,
                                                             DirectoryTransferProgressHeader header)
    : control_(control), manifest_(std::move(manifest)), header_(header) {}

const DirectoryTransferControlRecord &DirectoryTransferProgressPlan::control() const noexcept {
    return control_;
}

const DirectoryTransferManifest &DirectoryTransferProgressPlan::manifest() const noexcept {
    return manifest_;
}

const DirectoryTransferProgressHeader &DirectoryTransferProgressPlan::header() const noexcept {
    return header_;
}

DirectoryTransferProgressDigest
directory_transfer_object_identity_sha256(const DirectoryPathSemantics semantics,
                                          const std::string_view source_revision_utf8) {
    const auto canonical =
        directory_transfer_canonical_object_identity(source_revision_utf8, semantics);
    if (canonical.empty()) {
        throw std::invalid_argument("directory-transfer native identity is invalid");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(identityDomain.size() + 1U + canonical.size());
    append_bytes(bytes, identityDomain);
    append_integer(bytes, static_cast<std::uint8_t>(semantics));
    const auto *first = reinterpret_cast<const std::byte *>(canonical.data());
    bytes.insert(bytes.end(), first, first + canonical.size());
    return digest_bytes(bytes);
}

namespace {

DirectoryTransferProgressHeader prepare_header(const DirectoryTransferControlRecord &control,
                                               const DirectoryTransferManifest &manifest) {
    std::vector<std::byte> encoded_manifest;
    std::string detail_utf8;
    if (!control_binds_manifest(control, manifest, encoded_manifest, detail_utf8)) {
        throw std::invalid_argument(detail_utf8);
    }
    DirectoryTransferProgressHeader header;
    header.operation_id = control.operation_id;
    header.root_count = control.root_count;
    header.entry_count = static_cast<std::uint32_t>(manifest.entries.size());
    header.manifest_encoded_size = control.manifest_encoded_size;
    header.manifest_digest = control.manifest_digest;
    header.maximum_record_count =
        static_cast<std::uint32_t>(manifest.entries.size() + 2U * manifest.roots.size());
    header.header_sha256 = digest_bytes(encode_header_prefix(header));
    if (!basic_header_valid(header, detail_utf8)) {
        throw std::logic_error(detail_utf8);
    }
    return header;
}

bool header_binds_plan(const DirectoryTransferProgressHeader &header,
                       const DirectoryTransferControlRecord &control,
                       const DirectoryTransferManifest &manifest, std::string &detail_utf8) {
    if (!basic_header_valid(header, detail_utf8)) {
        return false;
    }
    std::vector<std::byte> encoded_manifest;
    if (!control_binds_manifest(control, manifest, encoded_manifest, detail_utf8)) {
        return false;
    }
    const auto expected_records = manifest.entries.size() + 2U * manifest.roots.size();
    if (header.operation_id != control.operation_id || header.root_count != control.root_count ||
        header.entry_count != manifest.entries.size() ||
        header.maximum_record_count != expected_records ||
        header.manifest_encoded_size != control.manifest_encoded_size ||
        header.manifest_digest != control.manifest_digest) {
        detail_utf8 = "directory-transfer progress header does not bind the prepared operation";
        return false;
    }
    return true;
}

} // namespace

DirectoryTransferProgressPlan
prepare_directory_transfer_progress_plan(const DirectoryTransferControlRecord &control,
                                         DirectoryTransferManifest manifest) {
    auto header = prepare_header(control, manifest);
    return DirectoryTransferProgressPlan(control, std::move(manifest), header);
}

std::optional<DirectoryTransferProgressPlan> bind_directory_transfer_progress_plan(
    const DirectoryTransferProgressHeader &header, const DirectoryTransferControlRecord &control,
    DirectoryTransferManifest manifest, std::string &detail_utf8) {
    if (!header_binds_plan(header, control, manifest, detail_utf8)) {
        return std::nullopt;
    }
    return DirectoryTransferProgressPlan(control, std::move(manifest), header);
}

std::vector<std::byte>
encode_directory_transfer_progress_header(const DirectoryTransferProgressHeader &header) {
    std::string detail_utf8;
    if (!basic_header_valid(header, detail_utf8)) {
        throw std::invalid_argument(detail_utf8);
    }
    auto payload = encode_header_prefix(header);
    append_bytes(payload, header.header_sha256);
    if (payload.size() != kDirectoryTransferProgressHeaderBytes) {
        throw std::logic_error("directory-transfer progress header framing failed");
    }
    return payload;
}

bool decode_directory_transfer_progress_header(const std::span<const std::byte> payload,
                                               DirectoryTransferProgressHeader &header,
                                               std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() != kDirectoryTransferProgressHeaderBytes) {
        detail_utf8 = "directory-transfer progress header framing is invalid";
        return false;
    }
    Cursor cursor(payload);
    DirectoryTransferProgressHeader decoded;
    std::array<std::uint8_t, headerMagic.size()> magic{};
    std::uint8_t kind{};
    std::uint32_t reserved1{};
    std::uint32_t reserved2{};
    std::uint32_t reserved3{};
    std::uint32_t reserved4{};
    if (!read_bytes(cursor, magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.header_bytes) || !cursor.read(decoded.record_bytes) ||
        !cursor.read(decoded.hash_algorithm) || !cursor.read(kind) || !cursor.read(decoded.flags) ||
        !cursor.read(decoded.root_count) || !cursor.read(decoded.entry_count) ||
        !cursor.read(reserved1) || !cursor.read(decoded.operation_id) ||
        !cursor.read(decoded.manifest_encoded_size) ||
        !read_bytes(cursor, decoded.manifest_digest) ||
        !cursor.read(decoded.maximum_record_count) || !cursor.read(reserved2) ||
        !cursor.read(reserved3) || !cursor.read(reserved4) ||
        !read_bytes(cursor, decoded.header_sha256) || cursor.remaining() != 0U ||
        magic != headerMagic || kind != static_cast<std::uint8_t>(FileTransferKind::copy) ||
        reserved1 != 0U || reserved2 != 0U || reserved3 != 0U || reserved4 != 0U) {
        detail_utf8 = "directory-transfer progress header is invalid or truncated";
        return false;
    }
    decoded.kind = static_cast<FileTransferKind>(kind);
    if (!basic_header_valid(decoded, detail_utf8)) {
        return false;
    }
    header = decoded;
    return true;
}

std::vector<std::byte>
encode_directory_transfer_progress_record(const DirectoryTransferProgressRecord &record) {
    std::string detail_utf8;
    if (!basic_record_valid(record, detail_utf8)) {
        throw std::invalid_argument(detail_utf8);
    }
    auto payload = encode_record_prefix(record);
    append_bytes(payload, record.record_sha256);
    if (payload.size() != kDirectoryTransferProgressRecordBytes) {
        throw std::logic_error("directory-transfer progress record framing failed");
    }
    return payload;
}

bool decode_directory_transfer_progress_record(const std::span<const std::byte> payload,
                                               DirectoryTransferProgressRecord &record,
                                               std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() != kDirectoryTransferProgressRecordBytes) {
        detail_utf8 = "directory-transfer progress record framing is invalid";
        return false;
    }
    Cursor cursor(payload);
    DirectoryTransferProgressRecord decoded;
    std::uint32_t magic{};
    std::uint8_t kind{};
    std::uint8_t object_kind{};
    std::uint8_t flags{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) || !cursor.read(kind) ||
        !cursor.read(object_kind) || !cursor.read(flags) || !cursor.read(decoded.sequence) ||
        !cursor.read(decoded.root_index) || !cursor.read(decoded.entry_index) ||
        !cursor.read(decoded.content_bytes) || !read_bytes(cursor, decoded.ownership_token) ||
        !read_bytes(cursor, decoded.object_identity_sha256) ||
        !read_bytes(cursor, decoded.content_sha256) ||
        !read_bytes(cursor, decoded.previous_record_sha256) ||
        !read_bytes(cursor, decoded.record_sha256) || cursor.remaining() != 0U ||
        magic != recordMagic || flags != 0U ||
        !valid_event_kind(static_cast<DirectoryTransferProgressEventKind>(kind)) ||
        !valid_object_kind(static_cast<DirectoryTransferProgressObjectKind>(object_kind))) {
        detail_utf8 = "directory-transfer progress record is invalid or truncated";
        return false;
    }
    decoded.kind = static_cast<DirectoryTransferProgressEventKind>(kind);
    decoded.object_kind = static_cast<DirectoryTransferProgressObjectKind>(object_kind);
    if (!basic_record_valid(decoded, detail_utf8)) {
        return false;
    }
    record = decoded;
    return true;
}

std::uint64_t DirectoryTransferProgressState::next_sequence() const noexcept {
    return next_sequence_;
}

std::uint32_t DirectoryTransferProgressState::completed_roots() const noexcept {
    return completed_roots_;
}

std::uint32_t DirectoryTransferProgressState::next_manifest_entry() const noexcept {
    return next_manifest_entry_;
}

DirectoryTransferRootProgressPhase DirectoryTransferProgressState::phase() const noexcept {
    return phase_;
}

const DirectoryTransferProgressOwnershipToken &
DirectoryTransferProgressState::active_ownership_token() const noexcept {
    return active_ownership_token_;
}

const DirectoryTransferProgressDigest &
DirectoryTransferProgressState::active_root_identity_sha256() const noexcept {
    return active_root_identity_sha256_;
}

const DirectoryTransferProgressDigest &
DirectoryTransferProgressState::rolling_tree_sha256() const noexcept {
    return rolling_tree_sha256_;
}

bool DirectoryTransferProgressState::complete() const noexcept {
    return phase_ == DirectoryTransferRootProgressPhase::complete;
}

DirectoryTransferProgressState
initialize_directory_transfer_progress(const DirectoryTransferProgressPlan &plan) {
    DirectoryTransferProgressState initialized;
    initialized.operation_id_ = plan.header().operation_id;
    initialized.root_count_ = plan.header().root_count;
    initialized.entry_count_ = plan.header().entry_count;
    initialized.next_manifest_entry_ = plan.manifest().roots.front().first_entry;
    initialized.last_record_sha256_ = plan.header().header_sha256;
    initialized.manifest_digest_ = plan.header().manifest_digest;
    return initialized;
}

bool directory_transfer_progress_state_matches(const DirectoryTransferProgressPlan &plan,
                                               const DirectoryTransferProgressState &state,
                                               std::string &detail_utf8) {
    if (state.operation_id_ != plan.header().operation_id ||
        state.root_count_ != plan.header().root_count ||
        state.entry_count_ != plan.header().entry_count ||
        state.manifest_digest_ != plan.header().manifest_digest) {
        detail_utf8 = "directory-transfer progress state belongs to another immutable plan";
        return false;
    }
    detail_utf8.clear();
    return true;
}

bool advance_directory_transfer_progress(const DirectoryTransferProgressPlan &plan,
                                         const DirectoryTransferRootStagingEvidence &evidence,
                                         DirectoryTransferProgressState &state,
                                         DirectoryTransferProgressRecord &record,
                                         std::string &detail_utf8) {
    DirectoryTransferProgressRecord candidate;
    candidate.kind = DirectoryTransferProgressEventKind::root_staging_owned;
    candidate.object_kind = DirectoryTransferProgressObjectKind::root;
    candidate.root_index = evidence.root_index;
    candidate.entry_index = kDirectoryTransferProgressNoEntry;
    candidate.ownership_token = evidence.ownership_token;
    try {
        candidate.object_identity_sha256 = directory_transfer_object_identity_sha256(
            plan.manifest().path_semantics, evidence.object_revision_utf8);
    } catch (const std::exception &error) {
        detail_utf8 = error.what();
        return false;
    }
    candidate.content_sha256 =
        tree_seed(plan.header(), candidate.root_index, candidate.ownership_token,
                  candidate.object_identity_sha256);
    return DirectoryTransferProgressStateAccess::seal_and_apply(
        plan.header(), plan.manifest(), candidate, state, record, detail_utf8);
}

bool advance_directory_transfer_progress(const DirectoryTransferProgressPlan &plan,
                                         const DirectoryTransferEntryEvidence &evidence,
                                         DirectoryTransferProgressState &state,
                                         DirectoryTransferProgressRecord &record,
                                         std::string &detail_utf8) {
    DirectoryTransferProgressRecord candidate;
    candidate.kind = DirectoryTransferProgressEventKind::entry_materialized;
    candidate.object_kind = evidence.object_kind;
    candidate.root_index = evidence.root_index;
    candidate.entry_index = evidence.entry_index;
    candidate.content_bytes = evidence.content_bytes;
    candidate.content_sha256 = evidence.content_sha256;
    try {
        candidate.object_identity_sha256 = directory_transfer_object_identity_sha256(
            plan.manifest().path_semantics, evidence.object_revision_utf8);
    } catch (const std::exception &error) {
        detail_utf8 = error.what();
        return false;
    }
    return DirectoryTransferProgressStateAccess::seal_and_apply(
        plan.header(), plan.manifest(), candidate, state, record, detail_utf8);
}

bool advance_directory_transfer_progress(const DirectoryTransferProgressPlan &plan,
                                         const DirectoryTransferRootPublicationEvidence &evidence,
                                         DirectoryTransferProgressState &state,
                                         DirectoryTransferProgressRecord &record,
                                         std::string &detail_utf8) {
    DirectoryTransferProgressRecord candidate;
    candidate.kind = DirectoryTransferProgressEventKind::root_published;
    candidate.object_kind = DirectoryTransferProgressObjectKind::root;
    candidate.root_index = evidence.root_index;
    candidate.entry_index = kDirectoryTransferProgressNoEntry;
    candidate.content_bytes = evidence.content_bytes;
    candidate.ownership_token = evidence.ownership_token;
    candidate.content_sha256 = evidence.audited_tree_sha256;
    try {
        candidate.object_identity_sha256 = directory_transfer_object_identity_sha256(
            plan.manifest().path_semantics, evidence.destination_revision_utf8);
    } catch (const std::exception &error) {
        detail_utf8 = error.what();
        return false;
    }
    return DirectoryTransferProgressStateAccess::seal_and_apply(
        plan.header(), plan.manifest(), candidate, state, record, detail_utf8);
}

std::optional<DirectoryTransferProgressState>
replay_directory_transfer_progress(const DirectoryTransferProgressPlan &plan,
                                   const std::span<const DirectoryTransferProgressRecord> records,
                                   std::string &detail_utf8) {
    if (records.size() > plan.header().maximum_record_count) {
        detail_utf8 = "directory-transfer progress exceeds its record bound";
        return std::nullopt;
    }
    auto replayed = initialize_directory_transfer_progress(plan);
    for (const auto &record : records) {
        if (!DirectoryTransferProgressStateAccess::apply_record(plan.header(), plan.manifest(),
                                                                record, replayed, detail_utf8)) {
            return std::nullopt;
        }
    }
    return replayed;
}

} // namespace vove::fileops
