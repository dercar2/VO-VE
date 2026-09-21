#include "vove/fileops/basic_directory_transfer.hpp"

#include "vove/core/reserved_names.hpp"
#include "vove/fileops/directory_root_staging.hpp"
#include "vove/platform/read_only_source.hpp"

#include "catalog_protocol_codec.hpp"
#include "file_in_use_error.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
#include <cstdlib>
#include <thread>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "windows/filesystem_error.hpp"
#include "windows/file_data_streams.hpp"
#else
#include "posix/file_identity.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <linux/magic.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif
#endif

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t manifestMagic = 0x31424456U;
constexpr std::uint32_t publicationProofMagic = 0x31504456U;
constexpr std::uint32_t publicationProofVersion = 1U;
constexpr std::size_t publicationProofEncodedBytes = 144U;
constexpr std::uint32_t atomicMoveProofMagic = 0x314D4156U;
constexpr std::uint32_t atomicMoveProofVersion = 1U;
constexpr std::size_t atomicMoveProofEncodedBytes =
    8U + kDirectoryRootOwnershipMarkerBytes + (2U * kBasicDirectoryTransferDigestBytes);
constexpr std::size_t copyBufferBytes = std::size_t{1} * 1'024U * 1'024U;
constexpr std::uint64_t copyProgressByteInterval = 64ULL * 1'024ULL * 1'024ULL;
constexpr auto copyProgressTimeInterval = std::chrono::seconds(5);
constexpr std::size_t serializedManifestFixedBytes = 112U;
constexpr std::size_t serializedManifestEntryFixedBytes = 28U;

#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
std::atomic<BasicDirectoryTransferTestFailurePoint> failurePointOnce{
    BasicDirectoryTransferTestFailurePoint::none};
std::atomic_bool loseStagingReplyOnce{};
std::atomic_bool loseFinalReplyOnce{};
std::atomic_bool loseSourceRetirementReplyOnce{};
std::atomic_bool pauseAfterSourceCleanupValidationOnce{};
std::atomic_bool sourceCleanupValidationPaused{};
std::atomic_bool releaseSourceCleanupValidation{};
std::atomic_bool pauseAfterAtomicMoveMarkerCheckOnce{};
std::atomic_bool atomicMoveMarkerCheckPaused{};
std::atomic_bool releaseAtomicMoveMarkerCheck{};
std::atomic_bool pauseAfterSourceEntryCaptureOnce{};
std::atomic_bool sourceEntryCapturePaused{};
std::atomic_bool releaseSourceEntryCapture{};
std::atomic_bool networkPauseUsed{};
std::atomic_bool pauseAfterStagingRetirementOnce{};
std::atomic_bool stagingRetirementPaused{};
std::atomic_bool releaseStagingRetirement{};
std::atomic_bool pauseAfterManifestRetirementOnce{};
std::atomic_bool manifestRetirementPaused{};
std::atomic_bool releaseManifestRetirement{};
std::atomic_bool pauseBeforeCleanupQuarantineOnce{};
std::atomic_bool cleanupQuarantinePaused{};
std::atomic_bool releaseCleanupQuarantine{};

bool consume_failure_point(const BasicDirectoryTransferTestFailurePoint point) noexcept {
    auto expected = point;
    return failurePointOnce.compare_exchange_strong(expected,
                                                    BasicDirectoryTransferTestFailurePoint::none);
}

void pause_network_phase_when_requested(const std::string_view phase) {
    const auto *configured = std::getenv("VOVE_BASIC_DIRECTORY_TRANSFER_NETWORK_SIGNAL");
    const auto *configured_phase = std::getenv("VOVE_BASIC_DIRECTORY_TRANSFER_NETWORK_PHASE");
    const auto selected = configured_phase == nullptr || *configured_phase == '\0'
                              ? phase == "copy"
                              : std::string_view(configured_phase) == phase;
    if (configured == nullptr || *configured == '\0' || !selected ||
        networkPauseUsed.exchange(true)) {
        return;
    }
    const auto signal = std::filesystem::path(configured);
    {
        std::ofstream ready(signal / "disconnect.ready", std::ios::binary | std::ios::trunc);
        ready << "ready\n";
    }
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        std::error_code error;
        if (std::filesystem::exists(signal / "disconnect.go", error) && !error) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("managed disconnect test signal timed out");
}

void pause_after_staging_retirement_when_requested() {
    if (!pauseAfterStagingRetirementOnce.exchange(false)) {
        return;
    }
    stagingRetirementPaused = true;
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        if (releaseStagingRetirement.load()) {
            stagingRetirementPaused = false;
            releaseStagingRetirement = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stagingRetirementPaused = false;
    throw std::runtime_error("staging retirement test signal timed out");
}

void pause_after_manifest_retirement_when_requested() {
    if (!pauseAfterManifestRetirementOnce.exchange(false)) {
        return;
    }
    manifestRetirementPaused = true;
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        if (releaseManifestRetirement.load()) {
            manifestRetirementPaused = false;
            releaseManifestRetirement = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    manifestRetirementPaused = false;
    throw std::runtime_error("manifest retirement test signal timed out");
}

void pause_after_source_cleanup_validation_when_requested() {
    if (!pauseAfterSourceCleanupValidationOnce.exchange(false)) {
        return;
    }
    sourceCleanupValidationPaused = true;
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        if (releaseSourceCleanupValidation.load()) {
            sourceCleanupValidationPaused = false;
            releaseSourceCleanupValidation = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sourceCleanupValidationPaused = false;
    throw std::runtime_error("source cleanup validation test signal timed out");
}

void pause_before_cleanup_quarantine_when_requested() {
    if (!pauseBeforeCleanupQuarantineOnce.exchange(false)) {
        return;
    }
    cleanupQuarantinePaused = true;
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        if (releaseCleanupQuarantine.load()) {
            cleanupQuarantinePaused = false;
            releaseCleanupQuarantine = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cleanupQuarantinePaused = false;
    throw std::runtime_error("cleanup quarantine test signal timed out");
}

void pause_after_atomic_move_marker_check_when_requested() {
    if (!pauseAfterAtomicMoveMarkerCheckOnce.exchange(false)) {
        return;
    }
    atomicMoveMarkerCheckPaused = true;
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        if (releaseAtomicMoveMarkerCheck.load()) {
            atomicMoveMarkerCheckPaused = false;
            releaseAtomicMoveMarkerCheck = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    atomicMoveMarkerCheckPaused = false;
    throw std::runtime_error("atomic move marker-check test signal timed out");
}

void pause_after_source_entry_capture_when_requested() {
    if (!pauseAfterSourceEntryCaptureOnce.exchange(false)) {
        return;
    }
    sourceEntryCapturePaused = true;
    for (std::size_t attempt{}; attempt < 6'000U; ++attempt) {
        if (releaseSourceEntryCapture.load()) {
            sourceEntryCapturePaused = false;
            releaseSourceEntryCapture = false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sourceEntryCapturePaused = false;
    throw std::runtime_error("source entry capture test signal timed out");
}
#endif

enum class ExecutionBoundaryPhase : std::uint8_t {
    none,
    control_created,
    manifest_persisted,
    staging_created,
    published,
    source_retired,
    completed,
};

struct TreeCapture {
    BasicDirectoryTransferStatus status{BasicDirectoryTransferStatus::success};
    std::error_code error;
    std::int64_t platform_code{};
    std::string detail_utf8;
    std::string root_revision_utf8;
    std::uint64_t total_bytes{};
    std::size_t serialized_entries_bytes{};
    std::vector<BasicDirectoryTransferEntry> entries;

    [[nodiscard]] bool ok() const noexcept {
        return status == BasicDirectoryTransferStatus::success;
    }
};

struct FileProof {
    BasicDirectoryTransferDigest digest{};
    std::uint64_t bytes{};
};

struct DirectoryPublicationProof {
    std::uint32_t version{publicationProofVersion};
    std::uint64_t operation_id{};
    std::uint64_t total_bytes{};
    std::uint32_t entry_count{};
    DirectoryTransferOwnershipToken ownership_token{};
    BasicDirectoryTransferDigest manifest_digest{};
    BasicDirectoryTransferDigest tree_digest{};
};

struct AtomicMoveCommittedProof {
    DirectoryRootOwnershipMarker marker;
    BasicDirectoryTransferDigest destination_identity_digest{};
};

struct RenameNoReplaceResult {
    BasicDirectoryTransferStatus status{BasicDirectoryTransferStatus::io_error};
    std::error_code error;
    std::int64_t platform_code{};
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == BasicDirectoryTransferStatus::success;
    }
};

RenameNoReplaceResult rename_no_replace(const std::filesystem::path &source,
                                        const std::filesystem::path &destination, bool known_cifs);

struct SourceShapeResult {
    BasicDirectoryTransferStatus status{BasicDirectoryTransferStatus::success};
    std::error_code error;
    std::int64_t platform_code{};
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == BasicDirectoryTransferStatus::success;
    }
};

template <std::size_t Size>
void append_bytes(std::vector<std::byte> &output, const std::array<std::uint8_t, Size> &bytes);
template <std::size_t Size> bool read_bytes(Cursor &cursor, std::array<std::uint8_t, Size> &bytes);
void report_progress(const BasicDirectoryTransferProgressCallback &progress,
                     BasicDirectoryTransferPhase phase,
                     const BasicDirectoryTransferManifest &manifest, std::size_t completed_entries,
                     std::uint64_t completed_bytes, const std::filesystem::path &current) noexcept;
std::optional<std::size_t> read_source(platform::ReadOnlySource &source,
                                       std::span<std::byte> buffer,
                                       std::error_code &error) noexcept;

std::string path_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    const auto *first = reinterpret_cast<const char8_t *>(text.data());
    return std::filesystem::path(std::u8string(first, first + text.size()));
}

#ifdef _WIN32
std::filesystem::path extended_length_path(const std::filesystem::path &path) {
    auto text = path.native();
    std::ranges::replace(text, L'/', L'\\');
    if (text.starts_with(LR"(\\?\)") || text.starts_with(LR"(\\.\)")) {
        return std::filesystem::path(std::move(text));
    }
    if (text.starts_with(LR"(\\)")) {
        return std::filesystem::path(LR"(\\?\UNC\)" + text.substr(2U));
    }
    if (path.is_absolute()) {
        return std::filesystem::path(LR"(\\?\)" + text);
    }
    return std::filesystem::path(std::move(text));
}
#endif

std::filesystem::path
basic_staging_destination_path(const std::filesystem::path &destination,
                               const std::uint64_t operation_id,
                               const DirectoryTransferOwnershipToken &ownership_token) {
    auto path =
        directory_transfer_staging_destination_path(destination, operation_id, 0U, ownership_token);
#ifdef _WIN32
    path = extended_length_path(path);
#endif
    return path;
}

std::filesystem::path publication_proof_path(const BasicDirectoryTransferManifest &manifest) {
    return basic_directory_transfer_manifest_path(manifest).parent_path() / "publication-proof";
}

std::filesystem::path atomic_move_proof_path(const BasicDirectoryTransferManifest &manifest) {
    return basic_directory_transfer_manifest_path(manifest).parent_path() / "atomic-move-proof";
}

std::filesystem::path atomic_move_marker_path(const std::filesystem::path &root) {
    auto path = root / std::filesystem::path(core::kTransferOwnershipMarkerFilename);
#ifdef _WIN32
    path = extended_length_path(path);
#endif
    return path;
}

std::filesystem::path source_retirement_path(const BasicDirectoryTransferManifest &manifest) {
    auto path = directory_transfer_staged_source_path(manifest.source, manifest.operation_id, 0U,
                                                      manifest.ownership_token);
#ifdef _WIN32
    path = extended_length_path(path);
#endif
    return path;
}

std::filesystem::path source_ownership_marker_path(const BasicDirectoryTransferManifest &manifest) {
    return source_retirement_path(manifest) /
           std::filesystem::path(core::kTransferOwnershipMarkerFilename);
}

std::filesystem::path
source_entry_retirement_tombstone_path(const BasicDirectoryTransferManifest &manifest,
                                       const BasicDirectoryTransferEntry &entry,
                                       const std::size_t entry_index) {
    return file_transfer_temp_destination_path(
        source_retirement_path(manifest) / entry.relative_path, manifest.operation_id, entry_index);
}

template <typename Integer> void hash_integer(detail::Sha256 &hash, const Integer value) noexcept {
    static_assert(std::is_integral_v<Integer> || std::is_enum_v<Integer>);
    if constexpr (std::is_enum_v<Integer>) {
        hash_integer(hash, static_cast<std::underlying_type_t<Integer>>(value));
    } else {
        using Unsigned = std::make_unsigned_t<Integer>;
        const auto converted = static_cast<Unsigned>(value);
        std::array<std::byte, sizeof(Unsigned)> bytes{};
        for (std::size_t index{}; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(converted >> (index * 8U));
        }
        hash.update(bytes);
    }
}

void hash_text(detail::Sha256 &hash, const std::string_view text) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(text.size()));
    hash.update(std::as_bytes(std::span(text.data(), text.size())));
}

BasicDirectoryTransferDigest manifest_digest(const BasicDirectoryTransferManifest &manifest) {
    const auto encoded = encode_basic_directory_transfer_manifest(manifest);
    BasicDirectoryTransferDigest digest{};
    const auto offset = encoded.size() - digest.size();
    for (std::size_t index{}; index < digest.size(); ++index) {
        digest[index] = std::to_integer<std::uint8_t>(encoded[offset + index]);
    }
    return digest;
}

DirectoryRootOwnershipMarker
atomic_move_marker_for(const BasicDirectoryTransferManifest &manifest) {
    static_assert(std::tuple_size_v<BasicDirectoryTransferDigest> ==
                  std::tuple_size_v<DirectoryTransferManifestDigest>);
    DirectoryTransferManifestDigest digest{};
    std::ranges::copy(manifest_digest(manifest), digest.begin());
    return {.parent_operation_id = manifest.operation_id,
            .parent_manifest_digest = digest,
            .root_index = 0U,
            .ownership_token = manifest.ownership_token};
}

bool atomic_move_marker_matches(const DirectoryRootOwnershipMarker &marker,
                                const BasicDirectoryTransferManifest &manifest) {
    const auto expected = atomic_move_marker_for(manifest);
    return marker.version == expected.version &&
           marker.parent_operation_id == expected.parent_operation_id &&
           marker.parent_manifest_digest == expected.parent_manifest_digest &&
           marker.root_index == expected.root_index &&
           marker.ownership_token == expected.ownership_token;
}

std::optional<BasicDirectoryTransferDigest>
atomic_move_identity_digest(const std::string_view root_revision,
                            const std::span<const BasicDirectoryTransferEntry> entries,
                            const DirectoryPathSemantics semantics) {
    const auto root_identity =
        directory_transfer_canonical_object_identity(root_revision, semantics);
    if (root_identity.empty()) {
        return std::nullopt;
    }
    detail::Sha256 hash;
    hash_text(hash, root_identity);
    hash_integer(hash, entries.size());
    for (const auto &entry : entries) {
        const auto identity =
            directory_transfer_canonical_object_identity(entry.source_revision_utf8, semantics);
        if (identity.empty()) {
            return std::nullopt;
        }
        hash_text(hash, path_utf8(entry.relative_path));
        hash_integer(hash, entry.kind);
        hash_integer(hash, entry.size_bytes);
        hash_text(hash, identity);
    }
    return hash.digest();
}

std::optional<BasicDirectoryTransferDigest>
atomic_move_destination_identity_digest(const TreeCapture &capture,
                                        const DirectoryPathSemantics semantics) {
    return capture.ok()
               ? atomic_move_identity_digest(capture.root_revision_utf8, capture.entries, semantics)
               : std::nullopt;
}

std::optional<BasicDirectoryTransferDigest>
atomic_move_expected_identity_digest(const BasicDirectoryTransferManifest &manifest) {
    return atomic_move_identity_digest(manifest.source_revision_utf8, manifest.entries,
                                       manifest.path_semantics);
}

std::vector<std::byte>
encode_atomic_move_committed_proof(const BasicDirectoryTransferManifest &manifest,
                                   const BasicDirectoryTransferDigest &identity_digest) {
    std::vector<std::byte> payload;
    payload.reserve(atomicMoveProofEncodedBytes);
    append_integer(payload, atomicMoveProofMagic);
    append_integer(payload, atomicMoveProofVersion);
    const auto marker = encode_directory_root_ownership_marker(atomic_move_marker_for(manifest));
    payload.insert(payload.end(), marker.begin(), marker.end());
    append_bytes(payload, identity_digest);
    append_bytes(payload, detail::sha256(payload));
    return payload;
}

bool decode_atomic_move_committed_proof(const std::span<const std::byte> payload,
                                        const BasicDirectoryTransferManifest &manifest,
                                        AtomicMoveCommittedProof &proof) {
    if (payload.size() != atomicMoveProofEncodedBytes) {
        return false;
    }
    const auto body = payload.first(payload.size() - kBasicDirectoryTransferDigestBytes);
    const auto expected_digest = detail::sha256(body);
    for (std::size_t index{}; index < expected_digest.size(); ++index) {
        if (std::to_integer<std::uint8_t>(payload[body.size() + index]) != expected_digest[index]) {
            return false;
        }
    }
    Cursor cursor(body.first(8U));
    std::uint32_t magic{};
    std::uint32_t version{};
    if (!cursor.read(magic) || !cursor.read(version) || cursor.remaining() != 0U ||
        magic != atomicMoveProofMagic || version != atomicMoveProofVersion) {
        return false;
    }
    DirectoryRootOwnershipMarker marker;
    std::string detail;
    const auto marker_payload = body.subspan(8U, kDirectoryRootOwnershipMarkerBytes);
    if (!decode_directory_root_ownership_marker(marker_payload, marker, detail) ||
        !atomic_move_marker_matches(marker, manifest)) {
        return false;
    }
    AtomicMoveCommittedProof decoded;
    decoded.marker = marker;
    const auto digest_offset = 8U + kDirectoryRootOwnershipMarkerBytes;
    for (std::size_t index{}; index < decoded.destination_identity_digest.size(); ++index) {
        decoded.destination_identity_digest[index] =
            std::to_integer<std::uint8_t>(body[digest_offset + index]);
    }
    proof = decoded;
    return true;
}

std::optional<BasicDirectoryTransferDigest>
tree_digest(const BasicDirectoryTransferManifest &manifest,
            const std::unordered_map<std::string, FileProof> &proofs) {
    detail::Sha256 hash;
    hash_integer(hash, manifest.entries.size());
    for (const auto &entry : manifest.entries) {
        hash_integer(hash, entry.kind);
        const auto relative = path_utf8(entry.relative_path);
        hash_text(hash, relative);
        hash_integer(hash, entry.size_bytes);
        if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            const auto key =
                directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics);
            const auto found = proofs.find(key);
            if (found == proofs.end() || found->second.bytes != entry.size_bytes) {
                return std::nullopt;
            }
            hash.update(std::as_bytes(std::span(found->second.digest)));
        }
    }
    return hash.digest();
}

std::vector<std::byte> encode_publication_proof(const DirectoryPublicationProof &proof) {
    std::vector<std::byte> payload;
    payload.reserve(publicationProofEncodedBytes);
    append_integer(payload, publicationProofMagic);
    append_integer(payload, proof.version);
    append_integer(payload, proof.operation_id);
    append_integer(payload, proof.total_bytes);
    append_integer(payload, proof.entry_count);
    append_integer(payload, static_cast<std::uint32_t>(0U));
    append_bytes(payload, proof.ownership_token);
    append_bytes(payload, proof.manifest_digest);
    append_bytes(payload, proof.tree_digest);
    append_bytes(payload, detail::sha256(payload));
    return payload;
}

bool decode_publication_proof(const std::span<const std::byte> payload,
                              DirectoryPublicationProof &proof) {
    if (payload.size() != publicationProofEncodedBytes) {
        return false;
    }
    const auto body = payload.first(payload.size() - kBasicDirectoryTransferDigestBytes);
    const auto expected_digest = detail::sha256(body);
    for (std::size_t index{}; index < expected_digest.size(); ++index) {
        if (std::to_integer<std::uint8_t>(payload[body.size() + index]) != expected_digest[index]) {
            return false;
        }
    }
    Cursor cursor(body);
    std::uint32_t magic{};
    std::uint32_t reserved{};
    DirectoryPublicationProof decoded;
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(decoded.total_bytes) ||
        !cursor.read(decoded.entry_count) || !cursor.read(reserved) ||
        !read_bytes(cursor, decoded.ownership_token) ||
        !read_bytes(cursor, decoded.manifest_digest) || !read_bytes(cursor, decoded.tree_digest) ||
        magic != publicationProofMagic || decoded.version != publicationProofVersion ||
        reserved != 0U || cursor.remaining() != 0U) {
        return false;
    }
    proof = decoded;
    return true;
}

bool publication_proof_matches_manifest(const DirectoryPublicationProof &proof,
                                        const BasicDirectoryTransferManifest &manifest) {
    return proof.operation_id == manifest.operation_id &&
           proof.total_bytes == manifest.total_bytes &&
           proof.entry_count == manifest.entries.size() &&
           proof.ownership_token == manifest.ownership_token &&
           proof.manifest_digest == manifest_digest(manifest);
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

template <std::size_t Size> bool bytes_empty(const std::array<std::uint8_t, Size> &bytes) noexcept {
    return std::ranges::all_of(bytes, [](const auto value) { return value == 0U; });
}

template <std::size_t Size>
void append_bytes(std::vector<std::byte> &output, const std::array<std::uint8_t, Size> &bytes) {
    std::ranges::transform(bytes, std::back_inserter(output),
                           [](const auto value) { return static_cast<std::byte>(value); });
}

template <std::size_t Size> bool read_bytes(Cursor &cursor, std::array<std::uint8_t, Size> &bytes) {
    for (auto &value : bytes) {
        if (!cursor.read(value)) {
            return false;
        }
    }
    return true;
}

constexpr DirectoryPathSemantics native_path_semantics() noexcept {
#ifdef _WIN32
    return DirectoryPathSemantics::windows_ordinal_nfc;
#else
    return DirectoryPathSemantics::posix_exact;
#endif
}

std::size_t path_depth(const std::filesystem::path &path) {
    return static_cast<std::size_t>(std::distance(path.begin(), path.end()));
}

#ifdef _WIN32
bool extended_windows_absolute_path(const std::filesystem::path &path) {
    auto native = path.native();
    std::ranges::replace(native, L'/', L'\\');
    const auto extended_drive = native.size() >= 7U && native.starts_with(LR"(\\?\)") &&
                                native[5] == L':' && native[6] == L'\\';
    const auto extended_unc =
        native.starts_with(LR"(\\?\UNC\)") && native.find(L'\\', 8U) != std::wstring::npos;
    return extended_drive || extended_unc;
}
#endif

std::filesystem::path normalized_absolute_path(const std::filesystem::path &path,
                                               std::error_code &error) {
#ifdef _WIN32
    if (extended_windows_absolute_path(path)) {
        error.clear();
        return path;
    }
#endif
    return std::filesystem::absolute(path, error).lexically_normal();
}

bool canonical_absolute_path(const std::filesystem::path &path) {
    bool absolute = path.is_absolute();
    bool extended{};
#ifdef _WIN32
    extended = extended_windows_absolute_path(path);
    absolute = absolute || extended;
#endif
    if (!absolute || path.empty() || (!extended && path != path.lexically_normal())) {
        return false;
    }
    return std::ranges::none_of(path, [](const auto &part) { return part == "." || part == ".."; });
}

bool canonical_relative_path(const std::filesystem::path &path) {
    if (path.empty() || path.is_absolute() || path != path.lexically_normal()) {
        return false;
    }
    return std::ranges::none_of(
        path, [](const auto &part) { return part.empty() || part == "." || part == ".."; });
}

bool contains_internal_name(const std::filesystem::path &path) {
    return std::ranges::any_of(
        path, [](const auto &part) { return core::is_internal_filename(part.u8string()); });
}

bool namespace_ancestor_or_same(const std::string_view ancestor, const std::string_view candidate) {
    if (ancestor.empty() || candidate.size() < ancestor.size() ||
        candidate.substr(0U, ancestor.size()) != ancestor) {
        return false;
    }
    return candidate.size() == ancestor.size() || candidate[ancestor.size()] == '/';
}

bool add_bounded_size(std::size_t &total, const std::size_t value,
                      const std::size_t limit) noexcept {
    if (total > limit || value > limit - total) {
        return false;
    }
    total += value;
    return true;
}

BasicDirectoryTransferStatus map_error_status(const std::error_code &error) noexcept {
    if (!error) {
        return BasicDirectoryTransferStatus::io_error;
    }
    if ((error.category() == std::system_category() && detail::file_in_use_error(error.value())) ||
        error == std::errc::device_or_resource_busy || error == std::errc::text_file_busy) {
        return BasicDirectoryTransferStatus::file_in_use;
    }
#ifdef _WIN32
    switch (
        platform::detail::classify_windows_filesystem_error(static_cast<DWORD>(error.value()))) {
    case platform::detail::WindowsFilesystemErrorKind::not_found:
        return BasicDirectoryTransferStatus::not_found;
    case platform::detail::WindowsFilesystemErrorKind::permission_denied:
        return BasicDirectoryTransferStatus::permission_denied;
    case platform::detail::WindowsFilesystemErrorKind::authentication_required:
        return BasicDirectoryTransferStatus::authentication_required;
    case platform::detail::WindowsFilesystemErrorKind::timed_out:
        return BasicDirectoryTransferStatus::timed_out;
    case platform::detail::WindowsFilesystemErrorKind::disconnected:
        return BasicDirectoryTransferStatus::disconnected;
    case platform::detail::WindowsFilesystemErrorKind::io_error:
        break;
    }
#else
    switch (error.value()) {
    case ENOENT:
    case ENOTDIR:
        return BasicDirectoryTransferStatus::not_found;
    case EACCES:
    case EPERM:
        return BasicDirectoryTransferStatus::permission_denied;
    case ETIMEDOUT:
        return BasicDirectoryTransferStatus::timed_out;
    case ENETDOWN:
    case ENETUNREACH:
    case ECONNRESET:
    case ECONNABORTED:
    case EHOSTUNREACH:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
#ifdef ESTALE
    case ESTALE:
#endif
        return BasicDirectoryTransferStatus::disconnected;
    default:
        break;
    }
#endif
    return BasicDirectoryTransferStatus::io_error;
}

#if defined(__linux__)
bool descriptor_is_cifs(const int descriptor) noexcept {
    struct statfs filesystem{};
    return descriptor >= 0 && ::fstatfs(descriptor, &filesystem) == 0 &&
           (filesystem.f_type == CIFS_SUPER_MAGIC || filesystem.f_type == SMB2_SUPER_MAGIC);
}
#endif

bool path_is_cifs(const std::filesystem::path &path) noexcept {
#if defined(__linux__)
    struct statfs filesystem{};
    return ::statfs(path.c_str(), &filesystem) == 0 &&
           (filesystem.f_type == CIFS_SUPER_MAGIC || filesystem.f_type == SMB2_SUPER_MAGIC);
#else
    static_cast<void>(path);
    return false;
#endif
}

BasicDirectoryTransferStatus map_transfer_error(const std::error_code &error,
                                                const bool known_cifs) noexcept {
#if defined(__linux__)
    if (known_cifs && error.value() == EIO) {
        return BasicDirectoryTransferStatus::disconnected;
    }
#else
    static_cast<void>(known_cifs);
#endif
    return map_error_status(error);
}

BasicDirectoryTransferStatus map_source_error(const platform::SourceOpenErrorKind kind,
                                              const std::int64_t platform_code) noexcept {
    if (detail::file_in_use_error(platform_code)) {
        return BasicDirectoryTransferStatus::file_in_use;
    }
    switch (kind) {
    case platform::SourceOpenErrorKind::none:
        return BasicDirectoryTransferStatus::success;
    case platform::SourceOpenErrorKind::invalid_path:
        return BasicDirectoryTransferStatus::invalid_request;
    case platform::SourceOpenErrorKind::not_found:
        return BasicDirectoryTransferStatus::not_found;
    case platform::SourceOpenErrorKind::access_denied:
        return BasicDirectoryTransferStatus::permission_denied;
    case platform::SourceOpenErrorKind::authentication_failed:
        return BasicDirectoryTransferStatus::authentication_required;
    case platform::SourceOpenErrorKind::timed_out:
        return BasicDirectoryTransferStatus::timed_out;
    case platform::SourceOpenErrorKind::disconnected:
        return BasicDirectoryTransferStatus::disconnected;
    case platform::SourceOpenErrorKind::not_regular_file:
    case platform::SourceOpenErrorKind::too_large:
        return BasicDirectoryTransferStatus::source_changed;
    case platform::SourceOpenErrorKind::io_error:
        return BasicDirectoryTransferStatus::io_error;
    }
    return BasicDirectoryTransferStatus::io_error;
}

std::error_code source_platform_error(const std::int64_t platform_code) noexcept {
    if (platform_code == 0) {
        return {};
    }
#ifdef _WIN32
    return {static_cast<int>(platform_code), std::system_category()};
#else
    return {static_cast<int>(platform_code), std::generic_category()};
#endif
}

BasicDirectoryTransferStatus map_source_operation_error(const platform::SourceOpenErrorKind kind,
                                                        const std::int64_t platform_code,
                                                        const bool known_cifs) noexcept {
    auto status = map_source_error(kind, platform_code);
#if defined(__linux__)
    if (status == BasicDirectoryTransferStatus::io_error && known_cifs && platform_code == EIO) {
        status = BasicDirectoryTransferStatus::disconnected;
    }
#else
    static_cast<void>(platform_code);
    static_cast<void>(known_cifs);
#endif
    return status;
}

enum class IdentityCheckRole : std::uint8_t {
    source,
    staging,
};

platform::SourceIdentityResult identity_result_for_transfer(const platform::ReadOnlySource &source,
                                                            const IdentityCheckRole role) noexcept {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    const auto point = role == IdentityCheckRole::source
                           ? BasicDirectoryTransferTestFailurePoint::source_identity_disconnect
                           : BasicDirectoryTransferTestFailurePoint::staging_identity_disconnect;
    if (consume_failure_point(point)) {
#ifdef _WIN32
        return {.status = platform::SourceIdentityStatus::unavailable,
                .error_kind = platform::SourceOpenErrorKind::disconnected,
                .platform_code = ERROR_BAD_NETPATH};
#else
        return {.status = platform::SourceIdentityStatus::unavailable,
                .error_kind = platform::SourceOpenErrorKind::disconnected,
                .platform_code = EIO};
#endif
    }
#else
    static_cast<void>(role);
#endif
    return source.identity_result();
}

platform::DirectoryRevisionResult
directory_revision_for_transfer(const std::filesystem::path &path) {
    auto result = platform::query_directory_revision(path);
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::directory_revision_disconnect)) {
        result.revision_utf8.clear();
#ifdef _WIN32
        result.error = {.kind = platform::SourceOpenErrorKind::disconnected,
                        .platform_code = ERROR_BAD_NETPATH,
                        .detail = "injected directory revision disconnect"};
#else
        result.error = {.kind = platform::SourceOpenErrorKind::disconnected,
                        .platform_code = EIO,
                        .detail = "injected directory revision disconnect"};
#endif
    }
#endif
    return result;
}

bool ordinary_directory(const std::filesystem::path &path,
                        const std::filesystem::file_status &status,
                        std::error_code &error) noexcept {
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        error.clear();
        return false;
    }
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = {static_cast<int>(GetLastError()), std::system_category()};
        return false;
    }
    error.clear();
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
#else
    static_cast<void>(path);
    error.clear();
    return true;
#endif
}

class RecoveryManifestLease final {
  public:
    RecoveryManifestLease() = default;
    RecoveryManifestLease(const RecoveryManifestLease &) = delete;
    RecoveryManifestLease &operator=(const RecoveryManifestLease &) = delete;

    ~RecoveryManifestLease() {
        release();
    }

    void release() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    [[nodiscard]] bool acquire(const std::filesystem::path &lease_path, std::error_code &error,
                               bool &busy) noexcept {
        error.clear();
        busy = false;
#ifdef _WIN32
        handle_ = CreateFileW(lease_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            busy = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
            error = {static_cast<int>(code), std::system_category()};
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(handle_, FileAttributeTagInfo, &attributes,
                                         sizeof(attributes)) == FALSE) {
            error = {static_cast<int>(GetLastError()), std::system_category()};
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        if ((attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
            error = std::make_error_code(std::errc::operation_not_supported);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
#else
        auto flags = O_RDWR | O_CLOEXEC;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        flags |= O_CREAT;
        descriptor_ = ::open(lease_path.c_str(), flags, 0600);
        if (descriptor_ < 0) {
            error = {errno, std::generic_category()};
            return false;
        }
        struct stat status{};
        const auto stat_result = fstat(descriptor_, &status);
        if (stat_result != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
            error = stat_result != 0 ? std::error_code(errno, std::generic_category())
                                     : std::make_error_code(std::errc::operation_not_supported);
            close(descriptor_);
            descriptor_ = -1;
            return false;
        }
        bool lock_unsupported_for_test{};
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        lock_unsupported_for_test = consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::source_delete_lock_unsupported);
#endif
        if (!lock_unsupported_for_test && flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            busy = errno == EWOULDBLOCK || errno == EAGAIN;
            error = {errno, std::generic_category()};
            close(descriptor_);
            descriptor_ = -1;
            return false;
        }
#endif
        return true;
    }

  private:
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

SourceShapeResult inspect_source_shape(const std::filesystem::path &path,
                                       const platform::ReadOnlySource &source) {
    static_cast<void>(path);
#ifdef _WIN32
    const auto handle = std::bit_cast<HANDLE>(source.native_object());
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandle(handle, &information) == FALSE ||
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        const auto code = GetLastError();
        const std::error_code error(static_cast<int>(code), std::system_category());
        return {.status = map_error_status(error),
                .error = error,
                .platform_code = static_cast<std::int64_t>(code),
                .detail_utf8 = "source file shape could not be inspected"};
    }
    if (information.nNumberOfLinks != 1U) {
        SourceShapeResult result;
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "hard-linked files are not copied in basic mode";
        return result;
    }
    constexpr DWORD unsupported_attributes =
        FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_ENCRYPTED;
    if ((basic.FileAttributes & unsupported_attributes) != 0U) {
        SourceShapeResult result;
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "reparse, sparse, and encrypted files are not copied in basic mode";
        return result;
    }
    const auto streams = platform::windows_detail::query_supported_transfer_data_streams(handle);
    if (!streams.ok()) {
        const std::error_code error(static_cast<int>(streams.error), std::system_category());
        return {.status = streams.error == ERROR_NOT_SUPPORTED || streams.error == ERROR_INVALID_FUNCTION
                              ? BasicDirectoryTransferStatus::unsupported : map_error_status(error),
                .error = error,
                .platform_code = static_cast<std::int64_t>(streams.error),
                .detail_utf8 = "source data streams are unsupported or could not be inspected"};
    }
    return {};
#else
    struct stat information{};
    const auto descriptor = static_cast<int>(source.native_object());
    if (::fstat(descriptor, &information) != 0) {
        const std::error_code error(errno, std::generic_category());
        return {.status = map_error_status(error),
                .error = error,
                .platform_code = error.value(),
                .detail_utf8 = "source file shape could not be inspected"};
    }
    if (!S_ISREG(information.st_mode) || information.st_nlink != 1) {
        SourceShapeResult result;
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "hard-linked and special files are not copied in basic mode";
        return result;
    }
    bool sparse{};
    bool allocation_count_reliable = true;
    if (information.st_size > 0) {
#if defined(SEEK_HOLE)
        errno = 0;
        const auto hole = ::lseek(descriptor, 0, SEEK_HOLE);
        const auto seek_error = errno;
        if (::lseek(descriptor, 0, SEEK_SET) < 0) {
            const std::error_code error(errno, std::generic_category());
            return {.status = map_error_status(error),
                    .error = error,
                    .platform_code = error.value(),
                    .detail_utf8 = "source file position could not be restored"};
        }
        if (hole >= 0) {
            sparse = hole < information.st_size;
            allocation_count_reliable = false;
        } else if (seek_error != EINVAL && seek_error != ENOTSUP && seek_error != EOPNOTSUPP &&
                   seek_error != ENXIO) {
            const std::error_code error(seek_error, std::generic_category());
            return {.status = map_error_status(error),
                    .error = error,
                    .platform_code = error.value(),
                    .detail_utf8 = "source sparse-file state could not be inspected"};
        }
#endif
#if defined(__linux__)
        struct statfs filesystem{};
        if (::fstatfs(descriptor, &filesystem) == 0 &&
            (filesystem.f_type == CIFS_SUPER_MAGIC || filesystem.f_type == SMB2_SUPER_MAGIC)) {
            allocation_count_reliable = false;
        }
#endif
        if (allocation_count_reliable && information.st_blocks >= 0) {
            sparse = static_cast<std::uint64_t>(information.st_blocks) * 512U <
                     static_cast<std::uint64_t>(information.st_size);
        }
    }
    if (sparse) {
        SourceShapeResult result;
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "sparse files are not copied in basic mode";
        return result;
    }
    return {};
#endif
}

std::uint64_t operation_id_from_token(const DirectoryTransferOwnershipToken &token) noexcept {
    std::uint64_t value{};
    for (std::size_t index{}; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(token[index]) << (index * 8U);
    }
    return value == 0U ? 1U : value;
}

BasicDirectoryTransferResult failure(BasicDirectoryTransferStatus status, std::string detail,
                                     std::error_code error = {},
                                     const std::int64_t platform_code = 0) {
    BasicDirectoryTransferResult result;
    result.status = status;
    result.error = error;
    result.platform_code = platform_code;
    result.detail_utf8 = std::move(detail);
    return result;
}

TreeCapture capture_tree_impl(const std::filesystem::path &source, const bool source_cifs,
                              const bool allow_ownership_marker = false,
                              const bool allow_internal_names = false) {
    TreeCapture capture;
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(source, error);
    if (error || !ordinary_directory(source, root_status, error)) {
        capture.status = error ? map_transfer_error(error, source_cifs)
                               : BasicDirectoryTransferStatus::source_changed;
        capture.error = error;
        capture.platform_code = error.value();
        capture.detail_utf8 = "source root is no longer an ordinary directory";
        return capture;
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    pause_network_phase_when_requested("enumeration");
#endif
    const auto root_revision = directory_revision_for_transfer(source);
    if (!root_revision) {
        capture.status = map_source_operation_error(root_revision.error.kind,
                                                    root_revision.error.platform_code, source_cifs);
        capture.error = source_platform_error(root_revision.error.platform_code);
        capture.platform_code = root_revision.error.platform_code;
        capture.detail_utf8 = "source root revision could not be read";
        return capture;
    }
    capture.root_revision_utf8 = root_revision.revision_utf8;
    std::filesystem::recursive_directory_iterator iterator(
        source, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        capture.status = map_transfer_error(error, source_cifs);
        capture.error = error;
        capture.platform_code = error.value();
        capture.detail_utf8 = "source directory could not be enumerated";
        return capture;
    }
    while (iterator != end) {
        if (capture.entries.size() >= kBasicDirectoryTransferMaximumEntries) {
            capture.status = BasicDirectoryTransferStatus::unsupported;
            capture.detail_utf8 = "source tree contains too many entries";
            return capture;
        }
        const auto relative = iterator->path().lexically_relative(source).lexically_normal();
        if (allow_ownership_marker &&
            relative == std::filesystem::path(core::kTransferOwnershipMarkerFilename)) {
            iterator.increment(error);
            if (error) {
                capture.status = map_transfer_error(error, source_cifs);
                capture.error = error;
                capture.platform_code = error.value();
                capture.detail_utf8 = "source directory enumeration was interrupted";
                return capture;
            }
            continue;
        }
        const auto depth = path_depth(relative);
        if (!canonical_relative_path(relative) || depth == 0U ||
            depth > kBasicDirectoryTransferMaximumDepth ||
            (!allow_internal_names && contains_internal_name(relative))) {
            capture.status = BasicDirectoryTransferStatus::unsupported;
            capture.detail_utf8 = "source tree contains an unsupported path";
            return capture;
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            capture.status = map_transfer_error(error, source_cifs);
            capture.error = error;
            capture.platform_code = error.value();
            capture.detail_utf8 = "source entry metadata could not be read";
            return capture;
        }
        BasicDirectoryTransferEntry entry;
        entry.relative_path = relative;
        entry.depth = static_cast<std::uint16_t>(depth);
        if (std::filesystem::is_symlink(status)) {
            capture.status = BasicDirectoryTransferStatus::unsupported;
            capture.detail_utf8 = "symbolic links and reparse links are not copied in basic mode";
            return capture;
        }
        if (std::filesystem::is_directory(status)) {
            if (!ordinary_directory(iterator->path(), status, error)) {
                capture.status = error ? map_transfer_error(error, source_cifs)
                                       : BasicDirectoryTransferStatus::unsupported;
                capture.error = error;
                capture.platform_code = error.value();
                capture.detail_utf8 = "reparse directories are not copied in basic mode";
                return capture;
            }
            entry.kind = BasicDirectoryTransferEntryKind::directory;
            const auto revision = directory_revision_for_transfer(iterator->path());
            if (!revision) {
                capture.status = map_source_operation_error(
                    revision.error.kind, revision.error.platform_code, source_cifs);
                capture.error = source_platform_error(revision.error.platform_code);
                capture.platform_code = revision.error.platform_code;
                capture.detail_utf8 = "source subdirectory revision could not be read";
                return capture;
            }
            entry.source_revision_utf8 = revision.revision_utf8;
        } else if (std::filesystem::is_regular_file(status)) {
            entry.kind = BasicDirectoryTransferEntryKind::regular_file;
            const auto opened = platform::open_read_only_source(
                iterator->path(), std::numeric_limits<std::uint64_t>::max());
            if (!opened.source) {
                capture.status = map_source_error(opened.error.kind, opened.error.platform_code);
#if defined(__linux__)
                if (source_cifs && opened.error.platform_code == EIO) {
                    capture.status = BasicDirectoryTransferStatus::disconnected;
                }
#endif
                capture.platform_code = opened.error.platform_code;
                capture.detail_utf8 = opened.error.detail;
                return capture;
            }
            const auto shape = inspect_source_shape(iterator->path(), *opened.source);
            if (!shape.ok()) {
                capture.status = shape.status == BasicDirectoryTransferStatus::io_error
                                     ? map_transfer_error(shape.error, source_cifs)
                                     : shape.status;
                capture.error = shape.error;
                capture.platform_code = shape.platform_code;
                capture.detail_utf8 = shape.detail_utf8;
                return capture;
            }
            entry.size_bytes = opened.source->size_bytes();
            entry.modified_unix_ns = opened.source->modified_unix_ns();
            entry.source_revision_utf8 = opened.source->source_revision_utf8();
            if (entry.size_bytes >
                std::numeric_limits<std::uint64_t>::max() - capture.total_bytes) {
                capture.status = BasicDirectoryTransferStatus::unsupported;
                capture.detail_utf8 = "source tree size exceeds the supported range";
                return capture;
            }
            capture.total_bytes += entry.size_bytes;
        } else {
            capture.status = BasicDirectoryTransferStatus::unsupported;
            capture.detail_utf8 = "source tree contains a special filesystem object";
            return capture;
        }
        const auto relative_bytes = path_utf8(entry.relative_path).size();
        std::size_t serialized_entry = serializedManifestEntryFixedBytes;
        if (!add_bounded_size(serialized_entry, relative_bytes,
                              kBasicDirectoryTransferMaximumManifestBytes) ||
            !add_bounded_size(serialized_entry, entry.source_revision_utf8.size(),
                              kBasicDirectoryTransferMaximumManifestBytes) ||
            !add_bounded_size(capture.serialized_entries_bytes, serialized_entry,
                              kBasicDirectoryTransferMaximumManifestBytes)) {
            capture.status = BasicDirectoryTransferStatus::unsupported;
            capture.detail_utf8 = "source tree exceeds the basic manifest byte budget";
            return capture;
        }
        capture.entries.emplace_back(std::move(entry));
        iterator.increment(error);
        if (error) {
            capture.status = map_transfer_error(error, source_cifs);
            capture.error = error;
            capture.platform_code = error.value();
            capture.detail_utf8 = "source directory enumeration was interrupted";
            return capture;
        }
    }
    const auto semantics = native_path_semantics();
    std::ranges::sort(capture.entries, [semantics](const auto &left, const auto &right) {
        return directory_transfer_namespace_key(left.relative_path, semantics) <
               directory_transfer_namespace_key(right.relative_path, semantics);
    });
    return capture;
}

TreeCapture capture_tree(const std::filesystem::path &source) {
    const auto source_cifs = path_is_cifs(source);
    try {
        return capture_tree_impl(source, source_cifs);
    } catch (const std::bad_alloc &) {
        TreeCapture capture;
        capture.status = BasicDirectoryTransferStatus::unsupported;
        return capture;
    } catch (const std::length_error &) {
        TreeCapture capture;
        capture.status = BasicDirectoryTransferStatus::unsupported;
        return capture;
    } catch (const std::filesystem::filesystem_error &exception) {
        TreeCapture capture;
        capture.status = map_transfer_error(exception.code(), source_cifs);
        capture.error = exception.code();
        capture.platform_code = exception.code().value();
        return capture;
    }
}

TreeCapture capture_tree_with_ownership_marker(const std::filesystem::path &source) {
    const auto source_cifs = path_is_cifs(source);
    try {
        return capture_tree_impl(source, source_cifs, true);
    } catch (const std::bad_alloc &) {
        TreeCapture capture;
        capture.status = BasicDirectoryTransferStatus::unsupported;
        return capture;
    } catch (const std::length_error &) {
        TreeCapture capture;
        capture.status = BasicDirectoryTransferStatus::unsupported;
        return capture;
    } catch (const std::filesystem::filesystem_error &exception) {
        TreeCapture capture;
        capture.status = map_transfer_error(exception.code(), source_cifs);
        capture.error = exception.code();
        capture.platform_code = exception.code().value();
        return capture;
    }
}

TreeCapture capture_owned_staging_tree(const std::filesystem::path &source) {
    const auto source_cifs = path_is_cifs(source);
    try {
        return capture_tree_impl(source, source_cifs, false, true);
    } catch (const std::bad_alloc &) {
        TreeCapture capture;
        capture.status = BasicDirectoryTransferStatus::unsupported;
        return capture;
    } catch (const std::length_error &) {
        TreeCapture capture;
        capture.status = BasicDirectoryTransferStatus::unsupported;
        return capture;
    } catch (const std::filesystem::filesystem_error &exception) {
        TreeCapture capture;
        capture.status = map_transfer_error(exception.code(), source_cifs);
        capture.error = exception.code();
        capture.platform_code = exception.code().value();
        return capture;
    }
}

bool same_tree_capture(const TreeCapture &expected, const TreeCapture &actual) {
    if (!expected.ok() || !actual.ok() ||
        expected.root_revision_utf8 != actual.root_revision_utf8 ||
        expected.total_bytes != actual.total_bytes ||
        expected.entries.size() != actual.entries.size()) {
        return false;
    }
    for (std::size_t index{}; index < expected.entries.size(); ++index) {
        const auto &left = expected.entries[index];
        const auto &right = actual.entries[index];
        if (left.relative_path != right.relative_path || left.kind != right.kind ||
            left.size_bytes != right.size_bytes ||
            left.modified_unix_ns != right.modified_unix_ns ||
            left.source_revision_utf8 != right.source_revision_utf8) {
            return false;
        }
    }
    return true;
}

bool transfer_timestamp_matches(std::int64_t expected, std::int64_t actual,
                                bool known_cifs) noexcept;

bool same_tree_physical_identity(const TreeCapture &expected, const TreeCapture &actual,
                                 const bool known_cifs) {
    if (!expected.ok() || !actual.ok() ||
        !same_object_identity(expected.root_revision_utf8, actual.root_revision_utf8) ||
        expected.total_bytes != actual.total_bytes ||
        expected.entries.size() != actual.entries.size()) {
        return false;
    }
    for (std::size_t index{}; index < expected.entries.size(); ++index) {
        const auto &left = expected.entries[index];
        const auto &right = actual.entries[index];
        if (left.relative_path != right.relative_path || left.kind != right.kind ||
            left.size_bytes != right.size_bytes ||
            !transfer_timestamp_matches(left.modified_unix_ns, right.modified_unix_ns,
                                        known_cifs) ||
            left.source_revision_utf8 != right.source_revision_utf8) {
            return false;
        }
    }
    return true;
}

constexpr std::uint64_t timestamp_distance_ns(const std::int64_t left,
                                              const std::int64_t right) noexcept {
    const auto magnitude = [](const std::int64_t value) constexpr noexcept {
        return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                         : static_cast<std::uint64_t>(value);
    };
    if ((left < 0) == (right < 0)) {
        const auto left_magnitude = magnitude(left);
        const auto right_magnitude = magnitude(right);
        return left_magnitude > right_magnitude ? left_magnitude - right_magnitude
                                                : right_magnitude - left_magnitude;
    }
    return magnitude(left) + magnitude(right);
}

static_assert(timestamp_distance_ns(-1, 0) == 1U);
static_assert(timestamp_distance_ns(std::numeric_limits<std::int64_t>::min(),
                                    std::numeric_limits<std::int64_t>::max()) ==
              std::numeric_limits<std::uint64_t>::max());

bool transfer_timestamp_matches(const std::int64_t expected, const std::int64_t actual,
                                const bool known_cifs) noexcept {
    constexpr std::uint64_t kCifsRenameTimestampToleranceNs = 2'000'000'000U;
    return timestamp_distance_ns(expected, actual) <=
           (known_cifs ? kCifsRenameTimestampToleranceNs : 0U);
}

bool same_owned_tree_ignoring_root_metadata(const BasicDirectoryTransferManifest &manifest,
                                            const TreeCapture &capture) {
    const auto expected_root = directory_transfer_canonical_object_identity(
        manifest.source_revision_utf8, manifest.path_semantics);
    const auto actual_root = directory_transfer_canonical_object_identity(
        capture.root_revision_utf8, manifest.path_semantics);
    if (!capture.ok() || expected_root.empty() || actual_root.empty() ||
        expected_root != actual_root || manifest.total_bytes != capture.total_bytes ||
        manifest.entries.size() != capture.entries.size()) {
        return false;
    }
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &expected = manifest.entries[index];
        const auto &actual = capture.entries[index];
        if (expected.relative_path != actual.relative_path || expected.kind != actual.kind ||
            expected.size_bytes != actual.size_bytes ||
            expected.modified_unix_ns != actual.modified_unix_ns ||
            expected.source_revision_utf8 != actual.source_revision_utf8) {
            return false;
        }
    }
    return true;
}

bool same_owned_content_ignoring_directory_metadata(const BasicDirectoryTransferManifest &manifest,
                                                    const TreeCapture &capture) {
    const auto expected_root = directory_transfer_canonical_object_identity(
        manifest.source_revision_utf8, manifest.path_semantics);
    const auto actual_root = directory_transfer_canonical_object_identity(
        capture.root_revision_utf8, manifest.path_semantics);
    if (!capture.ok() || expected_root.empty() || actual_root.empty() ||
        expected_root != actual_root || manifest.total_bytes != capture.total_bytes ||
        manifest.entries.size() != capture.entries.size()) {
        return false;
    }
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &expected = manifest.entries[index];
        const auto &actual = capture.entries[index];
        if (expected.relative_path != actual.relative_path || expected.kind != actual.kind ||
            expected.size_bytes != actual.size_bytes) {
            return false;
        }
        if (expected.kind == BasicDirectoryTransferEntryKind::regular_file) {
            if (expected.modified_unix_ns != actual.modified_unix_ns ||
                expected.source_revision_utf8 != actual.source_revision_utf8) {
                return false;
            }
            continue;
        }
        const auto expected_identity = directory_transfer_canonical_object_identity(
            expected.source_revision_utf8, manifest.path_semantics);
        const auto actual_identity = directory_transfer_canonical_object_identity(
            actual.source_revision_utf8, manifest.path_semantics);
        if (expected_identity.empty() || actual_identity.empty() ||
            expected_identity != actual_identity) {
            return false;
        }
    }
    return true;
}

bool same_replanned_source(const BasicDirectoryTransferManifest &original,
                           const BasicDirectoryTransferManifest &replanned) {
    const auto original_root = directory_transfer_canonical_object_identity(
        original.source_revision_utf8, original.path_semantics);
    const auto replanned_root = directory_transfer_canonical_object_identity(
        replanned.source_revision_utf8, replanned.path_semantics);
    if (original_root.empty() || replanned_root.empty() || original_root != replanned_root ||
        original.total_bytes != replanned.total_bytes ||
        original.entries.size() != replanned.entries.size()) {
        return false;
    }
    for (std::size_t index{}; index < original.entries.size(); ++index) {
        const auto &expected = original.entries[index];
        const auto &actual = replanned.entries[index];
        if (expected.relative_path != actual.relative_path || expected.kind != actual.kind ||
            expected.size_bytes != actual.size_bytes ||
            expected.modified_unix_ns != actual.modified_unix_ns ||
            expected.source_revision_utf8 != actual.source_revision_utf8) {
            return false;
        }
    }
    return true;
}

bool same_atomically_renamed_shape(const BasicDirectoryTransferManifest &manifest,
                                   const TreeCapture &capture, std::string &detail) {
    const auto known_cifs = path_is_cifs(manifest.destination);
    detail.clear();
    if (!capture.ok()) {
        detail = capture.detail_utf8;
        return false;
    }
    if (manifest.total_bytes != capture.total_bytes) {
        detail = "published atomic root has a different total byte count";
        return false;
    }
    if (manifest.entries.size() != capture.entries.size()) {
        detail = "published atomic root has a different entry count";
        return false;
    }
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &expected = manifest.entries[index];
        const auto &actual = capture.entries[index];
        if (expected.relative_path != actual.relative_path) {
            detail = "published atomic root has a different relative path";
            return false;
        }
        if (expected.kind != actual.kind) {
            detail = "published atomic root has a different entry kind";
            return false;
        }
        if (expected.size_bytes != actual.size_bytes) {
            detail = "published atomic root has a different file size";
            return false;
        }
        if (expected.kind == BasicDirectoryTransferEntryKind::regular_file &&
            !transfer_timestamp_matches(expected.modified_unix_ns, actual.modified_unix_ns,
                                        known_cifs)) {
            detail = "published atomic root has a different file modification time (expected " +
                     std::to_string(expected.modified_unix_ns) + ", actual " +
                     std::to_string(actual.modified_unix_ns) + ")";
            return false;
        }
    }
    return true;
}

std::string canonical_volume_identity(const std::string_view revision,
                                      const DirectoryPathSemantics semantics) {
    const auto identity = directory_transfer_canonical_object_identity(revision, semantics);
    const auto prefix_end = identity.find(':');
    const auto volume_end =
        prefix_end == std::string::npos ? std::string::npos : identity.find(':', prefix_end + 1U);
    return volume_end == std::string::npos ? std::string{} : identity.substr(0U, volume_end);
}

BasicDirectoryTransferResult
probe_atomic_move_volume(const BasicDirectoryTransferManifest &manifest, bool &same_volume) {
    same_volume = false;
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::atomic_move_volume_probe_disconnect)) {
#ifdef _WIN32
        const std::error_code error(ERROR_BAD_NETPATH, std::system_category());
#else
        const std::error_code error(ECONNRESET, std::generic_category());
#endif
        return failure(BasicDirectoryTransferStatus::disconnected,
                       "atomic move volume probe disconnected by test injection", error,
                       error.value());
    }
#endif
    const auto destination_parent =
        directory_revision_for_transfer(manifest.destination.parent_path());
    if (!destination_parent) {
        const auto known_cifs = path_is_cifs(manifest.destination.parent_path());
        return failure(map_source_operation_error(destination_parent.error.kind,
                                                  destination_parent.error.platform_code,
                                                  known_cifs),
                       "destination parent identity could not be read for atomic move", {},
                       destination_parent.error.platform_code);
    }
    const auto source_volume =
        canonical_volume_identity(manifest.source_revision_utf8, manifest.path_semantics);
    const auto destination_volume =
        canonical_volume_identity(destination_parent.revision_utf8, manifest.path_semantics);
    if (source_volume.empty() || destination_volume.empty()) {
        return failure(BasicDirectoryTransferStatus::unsupported,
                       "filesystem volume identity is unavailable for atomic move");
    }
    same_volume = source_volume == destination_volume;
    return failure(BasicDirectoryTransferStatus::success, {});
}

class ExclusiveWriter final {
  public:
    ExclusiveWriter() = default;
    ~ExclusiveWriter() {
        close();
    }
    ExclusiveWriter(const ExclusiveWriter &) = delete;
    ExclusiveWriter &operator=(const ExclusiveWriter &) = delete;

    [[nodiscard]] bool open(const std::filesystem::path &path, std::error_code &error) noexcept {
        close();
#ifdef _WIN32
        handle_ = CreateFileW(
            path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = {static_cast<int>(GetLastError()), std::system_category()};
            return false;
        }
#else
        descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor_ < 0) {
            error = {errno, std::generic_category()};
            return false;
        }
#endif
        error.clear();
        return true;
    }

    [[nodiscard]] bool open_existing(const std::filesystem::path &path,
                                     std::error_code &error) noexcept {
        close();
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH |
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = {static_cast<int>(GetLastError()), std::system_category()};
            return false;
        }
#else
        descriptor_ = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor_ < 0) {
            error = {errno, std::generic_category()};
            return false;
        }
#endif
        error.clear();
        return true;
    }

    [[nodiscard]] bool write(std::span<const std::byte> bytes, std::error_code &error) noexcept {
        std::size_t offset{};
        while (offset < bytes.size()) {
#ifdef _WIN32
            const auto remaining =
                std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max());
            DWORD written{};
            if (WriteFile(handle_, bytes.data() + offset, static_cast<DWORD>(remaining), &written,
                          nullptr) == FALSE ||
                written == 0U) {
                error = {static_cast<int>(GetLastError()), std::system_category()};
                return false;
            }
            offset += written;
#else
            const auto written = ::write(descriptor_, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                error = {written < 0 ? errno : EIO, std::generic_category()};
                return false;
            }
            offset += static_cast<std::size_t>(written);
#endif
        }
        error.clear();
        return true;
    }

    [[nodiscard]] bool flush(std::error_code &error) noexcept {
#ifdef _WIN32
        if (FlushFileBuffers(handle_) == FALSE) {
            error = {static_cast<int>(GetLastError()), std::system_category()};
            return false;
        }
#else
        if (::fsync(descriptor_) != 0) {
            error = {errno, std::generic_category()};
            return false;
        }
#endif
        error.clear();
        return true;
    }

#ifdef _WIN32
    [[nodiscard]] platform::windows_detail::WindowsDataStreamCopyResult copy_data_streams(
        const std::filesystem::path &source_path, const HANDLE source) {
        return platform::windows_detail::copy_supported_transfer_data_streams(source_path, source, handle_,
            platform::windows_detail::query_supported_transfer_data_streams(source),
            platform::windows_detail::query_supported_transfer_data_streams(handle_));
    }
#endif

    [[nodiscard]] bool on_cifs_filesystem() const noexcept {
#if defined(__linux__)
        return descriptor_is_cifs(descriptor_);
#else
        return false;
#endif
    }

  private:
    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

class SourceDeletionGuard final {
  public:
    SourceDeletionGuard() = default;
    ~SourceDeletionGuard() {
        release();
    }
    SourceDeletionGuard(const SourceDeletionGuard &) = delete;
    SourceDeletionGuard &operator=(const SourceDeletionGuard &) = delete;

    [[nodiscard]] bool acquire(const std::filesystem::path &path, const bool directory,
                               std::error_code &error, bool &busy) noexcept {
        release();
        error.clear();
        busy = false;
#ifdef _WIN32
        DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
        flags |= directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
        handle_ = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, flags, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            busy = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
            error = {static_cast<int>(code), std::system_category()};
            return false;
        }
#else
        const auto parent =
            path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
        filename_ = path.filename();
        if (filename_.empty() || filename_ == "." || filename_ == "..") {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        parent_descriptor_ = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        if (parent_descriptor_ < 0) {
            error = {errno, std::generic_category()};
            release();
            return false;
        }
        auto flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
#ifdef O_DIRECTORY
        if (directory) {
            flags |= O_DIRECTORY;
        }
#endif
        descriptor_ = ::openat(parent_descriptor_, filename_.c_str(), flags);
        if (descriptor_ < 0) {
            error = {errno, std::generic_category()};
            release();
            return false;
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            busy = errno == EWOULDBLOCK || errno == EAGAIN;
            const auto lock_error = errno;
            if (busy || (lock_error != ENOTSUP && lock_error != EOPNOTSUPP &&
                         lock_error != EINVAL && lock_error != ENOSYS)) {
                error = {lock_error, std::generic_category()};
                release();
                return false;
            }
        }
        struct stat status{};
        if (::fstat(descriptor_, &status) != 0) {
            error = {errno, std::generic_category()};
            release();
            return false;
        }
        if (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) {
            error = std::make_error_code(std::errc::operation_not_supported);
            release();
            return false;
        }
#endif
        return true;
    }

    [[nodiscard]] bool remove(const std::filesystem::path &path, const bool directory,
                              std::error_code &error) noexcept {
        error.clear();
#ifdef _WIN32
        static_cast<void>(path);
        static_cast<void>(directory);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        if (SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition,
                                       sizeof(disposition)) == FALSE) {
            error = {static_cast<int>(GetLastError()), std::system_category()};
            return false;
        }
        release();
        return true;
#else
        static_cast<void>(path);
        if (descriptor_ < 0) {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }
        struct stat pinned{};
        struct stat current{};
        if (parent_descriptor_ < 0 || ::fstat(descriptor_, &pinned) != 0 ||
            ::fstatat(parent_descriptor_, filename_.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
            error = {errno, std::generic_category()};
            return false;
        }
        const auto current_revision = platform::posix_identity::revision_at(
            parent_descriptor_, filename_, current, AT_SYMLINK_NOFOLLOW);
        const auto pinned_revision =
            platform::posix_identity::revision_from_descriptor(descriptor_, pinned);
        if (!same_source_revision(pinned_revision, current_revision) ||
            (directory ? !S_ISDIR(current.st_mode) : !S_ISREG(current.st_mode))) {
            error = std::make_error_code(std::errc::state_not_recoverable);
            return false;
        }
        const auto removed =
            ::unlinkat(parent_descriptor_, filename_.c_str(), directory ? AT_REMOVEDIR : 0);
        if (removed != 0) {
            error = {errno, std::generic_category()};
            return false;
        }
        release();
        return true;
#endif
    }

  private:
    void release() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
        if (parent_descriptor_ >= 0) {
            static_cast<void>(::close(parent_descriptor_));
            parent_descriptor_ = -1;
        }
        filename_.clear();
#endif
    }

#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
    int parent_descriptor_{-1};
    std::filesystem::path filename_;
#endif
};

BasicDirectoryTransferResult persist_manifest_once(const std::filesystem::path &path,
                                                   const std::span<const std::byte> payload,
                                                   const bool known_cifs) {
    std::error_code error;
    {
        ExclusiveWriter writer;
        if (!writer.open(path, error)) {
            return failure(map_transfer_error(error, known_cifs),
                           "directory transfer manifest could not be created", error,
                           error.value());
        }
        if (!writer.write(payload, error)) {
            return failure(map_transfer_error(error, writer.on_cifs_filesystem() || known_cifs),
                           "directory transfer manifest could not be persisted", error,
                           error.value());
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::manifest_flush)) {
            return failure(BasicDirectoryTransferStatus::io_error,
                           "directory transfer manifest flush failed by test injection");
        }
#endif
        if (!writer.flush(error)) {
            return failure(map_transfer_error(error, writer.on_cifs_filesystem() || known_cifs),
                           "directory transfer manifest could not be persisted", error,
                           error.value());
        }
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

bool unsupported_durability_error(const std::error_code &error) noexcept {
    return error == std::errc::operation_not_supported ||
           error == std::errc::function_not_supported || error == std::errc::invalid_argument;
}

std::filesystem::path cleanup_quarantine_path(const std::filesystem::path &root) {
    auto quarantine = root;
    quarantine += path_from_utf8(".deleting");
    return quarantine;
}

BasicDirectoryTransferResult quarantine_owned_tree(const std::filesystem::path &root,
                                                   const TreeCapture &expected,
                                                   std::filesystem::path &quarantine,
                                                   TreeCapture &quarantined_capture) {
    quarantine = cleanup_quarantine_path(root);
    auto renamed = rename_no_replace(root, quarantine, path_is_cifs(root));
    if (!renamed.ok()) {
        return failure(renamed.status, "owned cleanup root could not be quarantined", renamed.error,
                       renamed.platform_code);
    }
    quarantined_capture = capture_owned_staging_tree(quarantine);
    if (!same_tree_physical_identity(expected, quarantined_capture, path_is_cifs(quarantine))) {
        return failure(quarantined_capture.ok() ? BasicDirectoryTransferStatus::staging_changed
                                                : quarantined_capture.status,
                       quarantined_capture.ok()
                           ? "quarantined cleanup root changed physical identity"
                           : quarantined_capture.detail_utf8,
                       quarantined_capture.error, quarantined_capture.platform_code);
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult remove_captured_owned_tree(const std::filesystem::path &root,
                                                        const TreeCapture &expected) {
    const auto known_cifs = path_is_cifs(root);
    std::error_code error;
    SourceDeletionGuard root_guard;
    bool root_busy{};
    const auto root_guarded = root_guard.acquire(root, true, error, root_busy);
    if (!root_guarded) {
        return failure(root_busy ? BasicDirectoryTransferStatus::file_in_use
                                 : (unsupported_durability_error(error)
                                        ? BasicDirectoryTransferStatus::unsupported
                                        : map_transfer_error(error, known_cifs)),
                       root_busy ? "cleanup root is still open for mutation"
                                 : "cleanup root could not be anchored before removal",
                       error, error.value());
    }
    const auto current = capture_owned_staging_tree(root);
    if (!same_tree_capture(expected, current)) {
        return failure(
            current.ok() ? BasicDirectoryTransferStatus::staging_changed : current.status,
            current.ok() ? "anchored cleanup root changed before removal" : current.detail_utf8,
            current.error, current.platform_code);
    }

    std::vector<std::size_t> order(expected.entries.size());
    std::iota(order.begin(), order.end(), std::size_t{});
    std::ranges::sort(order, [&expected](const auto left, const auto right) {
        const auto &left_entry = expected.entries[left];
        const auto &right_entry = expected.entries[right];
        if (left_entry.depth != right_entry.depth) {
            return left_entry.depth > right_entry.depth;
        }
        return left_entry.relative_path.generic_u8string() >
               right_entry.relative_path.generic_u8string();
    });
    for (const auto index : order) {
        const auto &entry = expected.entries[index];
        const auto path = root / entry.relative_path;
        const auto directory = entry.kind == BasicDirectoryTransferEntryKind::directory;
        SourceDeletionGuard guard;
        bool busy{};
        const auto guarded = guard.acquire(path, directory, error, busy);
        if (!guarded) {
            return failure(busy ? BasicDirectoryTransferStatus::file_in_use
                                : (unsupported_durability_error(error)
                                       ? BasicDirectoryTransferStatus::unsupported
                                       : map_transfer_error(error, known_cifs)),
                           busy ? "cleanup entry is still open for mutation"
                                : "cleanup entry could not be anchored before removal",
                           error, error.value());
        }
        if (directory) {
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !ordinary_directory(path, status, error)) {
                return failure(error ? map_transfer_error(error, known_cifs)
                                     : BasicDirectoryTransferStatus::staging_changed,
                               "cleanup directory changed shape before removal", error,
                               error.value());
            }
            const auto revision = directory_revision_for_transfer(path);
            if (!revision ||
                !same_object_identity(entry.source_revision_utf8, revision.revision_utf8)) {
                return failure(revision ? BasicDirectoryTransferStatus::staging_changed
                                        : map_source_operation_error(revision.error.kind,
                                                                     revision.error.platform_code,
                                                                     known_cifs),
                               "cleanup directory changed physical identity before removal", {},
                               revision.error.platform_code);
            }
        } else {
            {
                auto opened = platform::open_read_only_source(
                    path, entry.size_bytes == 0U ? 1U : entry.size_bytes);
                if (!opened.source) {
                    return failure(map_source_operation_error(
                                       opened.error.kind, opened.error.platform_code, known_cifs),
                                   "cleanup file could not be opened before removal", {},
                                   opened.error.platform_code);
                }
                const auto identity =
                    identity_result_for_transfer(*opened.source, IdentityCheckRole::staging);
                if (opened.source->size_bytes() != entry.size_bytes ||
                    !transfer_timestamp_matches(entry.modified_unix_ns,
                                                opened.source->modified_unix_ns(), known_cifs) ||
                    !same_object_identity(entry.source_revision_utf8,
                                          opened.source->source_revision_utf8()) ||
                    identity.status != platform::SourceIdentityStatus::unchanged) {
                    return failure(BasicDirectoryTransferStatus::staging_changed,
                                   "cleanup file changed before removal");
                }
            }
        }
        const auto removed = guard.remove(path, directory, error);
        if (!removed || error) {
            return failure(error ? map_transfer_error(error, known_cifs)
                                 : BasicDirectoryTransferStatus::staging_changed,
                           "captured cleanup entry could not be removed", error, error.value());
        }
    }
    const auto root_removed = root_guard.remove(root, true, error);
    if (!root_removed || error) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "captured cleanup root could not be removed", error, error.value());
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

#ifndef _WIN32
BasicDirectoryTransferResult sync_directory_namespace(const std::filesystem::path &directory,
                                                      const bool known_cifs,
                                                      const std::string_view description) {
    const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0) {
        const std::error_code error(errno, std::generic_category());
        if (known_cifs && unsupported_durability_error(error)) {
            return failure(BasicDirectoryTransferStatus::success, {});
        }
        return failure(
            unsupported_durability_error(error) ? BasicDirectoryTransferStatus::unsupported
                                                : map_transfer_error(error, known_cifs),
            std::string(description) + " namespace could not be opened", error, error.value());
    }
    int result{};
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    const auto flush_error =
        result == 0 ? std::error_code{} : std::error_code(errno, std::generic_category());
    const auto close_result = ::close(descriptor);
    if (flush_error || close_result != 0) {
        const auto error =
            flush_error ? flush_error : std::error_code(errno, std::generic_category());
        if (known_cifs && unsupported_durability_error(error)) {
            return failure(BasicDirectoryTransferStatus::success, {});
        }
        return failure(unsupported_durability_error(error)
                           ? BasicDirectoryTransferStatus::unsupported
                           : map_transfer_error(error, known_cifs),
                       std::string(description) + " namespace durability could not be confirmed",
                       error, error.value());
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}
#endif

BasicDirectoryTransferResult sync_proof_namespace(const std::filesystem::path &path,
                                                  const std::string_view description) {
#ifdef _WIN32
    // Windows has no portable directory-fsync primitive. CREATE/WRITE_THROUGH plus a flushed file
    // is the same durability boundary used by the project's accepted Windows journal backend.
    static_cast<void>(path);
    static_cast<void>(description);
    return failure(BasicDirectoryTransferStatus::success, {});
#else
    const auto known_cifs = path_is_cifs(path.parent_path());
    for (const auto &directory : {path.parent_path(), path.parent_path().parent_path()}) {
        auto result = sync_directory_namespace(directory, known_cifs, description);
        if (!result.ok()) {
            return result;
        }
    }
    return failure(BasicDirectoryTransferStatus::success, {});
#endif
}

BasicDirectoryTransferResult
sync_staging_tree_namespace(const BasicDirectoryTransferManifest &manifest) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::staging_namespace_flush)) {
        return failure(BasicDirectoryTransferStatus::io_error,
                       "staging namespace flush failed by test injection");
    }
#endif
#ifdef _WIN32
    // Win32 has no supported directory-fsync primitive. Flushed files and write-through renames
    // cover process/network restart, but cannot promise survival across sudden storage power loss.
    static_cast<void>(manifest);
    return failure(BasicDirectoryTransferStatus::success, {});
#else
    const auto known_cifs = path_is_cifs(manifest.staging_destination);
    // Manifest entries are namespace-sorted, so reverse traversal flushes descendants before
    // their parents. The staging parent is last because it owns the staging-root directory entry.
    for (auto entry = manifest.entries.rbegin(); entry != manifest.entries.rend(); ++entry) {
        if (entry->kind != BasicDirectoryTransferEntryKind::directory) {
            continue;
        }
        auto result = sync_directory_namespace(manifest.staging_destination / entry->relative_path,
                                               known_cifs, "staging directory");
        if (!result.ok()) {
            return result;
        }
    }
    auto root_result =
        sync_directory_namespace(manifest.staging_destination, known_cifs, "staging directory");
    if (!root_result.ok()) {
        return root_result;
    }
    return sync_directory_namespace(manifest.staging_destination.parent_path(), known_cifs,
                                    "staging parent");
#endif
}

BasicDirectoryTransferResult
sync_source_parent_namespace(const BasicDirectoryTransferManifest &manifest) {
#ifdef _WIN32
    static_cast<void>(manifest);
    return failure(BasicDirectoryTransferStatus::success, {});
#else
    const auto parent = manifest.source.parent_path();
    return sync_directory_namespace(parent, path_is_cifs(parent), "source parent");
#endif
}

BasicDirectoryTransferResult
probe_source_namespace_capability(const BasicDirectoryTransferManifest &manifest) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::source_namespace_unsupported)) {
        return failure(BasicDirectoryTransferStatus::unsupported,
                       "source namespace durability is unsupported by test injection");
    }
#endif
    return sync_source_parent_namespace(manifest);
}

BasicDirectoryTransferResult
confirm_source_retirement_namespace(const BasicDirectoryTransferManifest &manifest) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::source_retirement_namespace_flush)) {
        return failure(BasicDirectoryTransferStatus::io_error,
                       "source retirement namespace flush failed by test injection");
    }
#endif
    return sync_source_parent_namespace(manifest);
}

BasicDirectoryTransferResult
confirm_source_removal_namespace(const BasicDirectoryTransferManifest &manifest) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::source_removal_namespace_flush)) {
        return failure(BasicDirectoryTransferStatus::io_error,
                       "source removal namespace flush failed by test injection");
    }
#endif
    return sync_source_parent_namespace(manifest);
}

BasicDirectoryTransferResult
probe_source_cleanup_capability(const BasicDirectoryTransferManifest &manifest) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::source_lock_unsupported)) {
        return failure(BasicDirectoryTransferStatus::unsupported,
                       "source cleanup lock is unsupported by test injection");
    }
#endif
    const auto source_cifs = path_is_cifs(manifest.source);
    for (const auto &entry : manifest.entries) {
        if (entry.kind != BasicDirectoryTransferEntryKind::regular_file) {
            continue;
        }
        SourceDeletionGuard guard;
        std::error_code error;
        bool busy{};
        if (!guard.acquire(manifest.source / entry.relative_path, false, error, busy)) {
            const auto unsupported = unsupported_durability_error(error);
            return failure(busy          ? BasicDirectoryTransferStatus::file_in_use
                           : unsupported ? BasicDirectoryTransferStatus::unsupported
                                         : map_transfer_error(error, source_cifs),
                           busy ? "source file is active and cannot be moved safely"
                                : (unsupported ? "source cleanup lock is unsupported"
                                               : "source cleanup lock could not be acquired"),
                           error, error.value());
        }
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult persist_payload_file_once(const std::filesystem::path &path,
                                                       const std::span<const std::byte> payload,
                                                       const std::string_view description) {
    const auto known_cifs = path_is_cifs(path.parent_path());
    std::error_code error;
    ExclusiveWriter writer;
    if (!writer.open(path, error)) {
        return failure(map_transfer_error(error, known_cifs),
                       std::string(description) + " could not be created", error, error.value());
    }
    if (!writer.write(payload, error)) {
        return failure(map_transfer_error(error, writer.on_cifs_filesystem() || known_cifs),
                       std::string(description) + " could not be persisted", error, error.value());
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    const auto failure_point = description == "directory publication proof"
                                   ? BasicDirectoryTransferTestFailurePoint::publication_proof_flush
                               : description == "atomic move committed proof"
                                   ? BasicDirectoryTransferTestFailurePoint::atomic_move_proof_flush
                                   : BasicDirectoryTransferTestFailurePoint::source_marker_flush;
    if (consume_failure_point(failure_point)) {
        return failure(BasicDirectoryTransferStatus::io_error,
                       std::string(description) + " flush failed by test injection");
    }
#endif
    if (!writer.flush(error)) {
        return failure(map_transfer_error(error, writer.on_cifs_filesystem() || known_cifs),
                       std::string(description) + " could not be persisted", error, error.value());
    }
    return sync_proof_namespace(path, description);
}

BasicDirectoryTransferResult persist_proof_file_once(const std::filesystem::path &path,
                                                     const DirectoryPublicationProof &proof,
                                                     const std::string_view description) {
    const auto payload = encode_publication_proof(proof);
    return persist_payload_file_once(path, payload, description);
}

BasicDirectoryTransferResult confirm_proof_file_durability(const std::filesystem::path &path,
                                                           const std::string_view description) {
    const auto known_cifs = path_is_cifs(path.parent_path());
    std::error_code error;
    ExclusiveWriter writer;
    if (!writer.open_existing(path, error)) {
        return failure(map_transfer_error(error, known_cifs),
                       std::string(description) + " could not be reopened for durability", error,
                       error.value());
    }
    if (!writer.flush(error)) {
        return failure(map_transfer_error(error, writer.on_cifs_filesystem() || known_cifs),
                       std::string(description) + " durability could not be confirmed", error,
                       error.value());
    }
    return sync_proof_namespace(path, description);
}

BasicDirectoryTransferResult load_proof_file(const std::filesystem::path &path,
                                             const BasicDirectoryTransferManifest &manifest,
                                             DirectoryPublicationProof &proof,
                                             const std::string_view description) {
    const auto known_cifs = path_is_cifs(path.parent_path());
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " is not an ordinary file", error, error.value());
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != publicationProofEncodedBytes) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " size is invalid", error, error.value());
    }
    auto opened = platform::open_read_only_source(path, publicationProofEncodedBytes);
    if (!opened.source) {
        auto status = map_source_error(opened.error.kind, opened.error.platform_code);
#if defined(__linux__)
        if (known_cifs && opened.error.platform_code == EIO) {
            status = BasicDirectoryTransferStatus::disconnected;
        }
#endif
        return failure(status, std::string(description) + " could not be opened", {},
                       opened.error.platform_code);
    }
    bool source_cifs = known_cifs;
#if defined(__linux__)
    source_cifs =
        source_cifs || descriptor_is_cifs(static_cast<int>(opened.source->native_object()));
#endif
    if (opened.source->size_bytes() != publicationProofEncodedBytes) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " size is invalid");
    }
    std::array<std::byte, publicationProofEncodedBytes> payload{};
    std::size_t offset{};
    while (offset < payload.size()) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        const auto read_disconnect_point =
            description == "directory publication proof"
                ? BasicDirectoryTransferTestFailurePoint::publication_proof_read_disconnect
                : BasicDirectoryTransferTestFailurePoint::source_marker_read_disconnect;
        if (consume_failure_point(read_disconnect_point)) {
#ifdef _WIN32
            error = {static_cast<int>(ERROR_NETNAME_DELETED), std::system_category()};
#else
            error = {ECONNRESET, std::generic_category()};
#endif
            return failure(map_transfer_error(error, true),
                           "directory publication proof could not be read", error, error.value());
        }
#endif
        const auto count = read_source(*opened.source, std::span(payload).subspan(offset), error);
        if (!count) {
            return failure(map_transfer_error(error, source_cifs),
                           std::string(description) + " could not be read", error, error.value());
        }
        if (*count == 0U) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           std::string(description) + " is truncated");
        }
        offset += *count;
    }
    const auto identity = opened.source->identity_result();
    if (identity.status != platform::SourceIdentityStatus::unchanged) {
        const auto identity_error = source_platform_error(identity.platform_code);
        const auto status = identity.status == platform::SourceIdentityStatus::changed
                                ? BasicDirectoryTransferStatus::staging_changed
                                : map_transfer_error(identity_error, source_cifs);
        return failure(status, std::string(description) + " changed while it was read",
                       identity_error, identity.platform_code);
    }
    if (!decode_publication_proof(payload, proof)) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " is corrupt");
    }
    try {
        if (!publication_proof_matches_manifest(proof, manifest)) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           std::string(description) + " does not match its manifest");
        }
    } catch (const std::exception &) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " could not be bound to its manifest");
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult
persist_atomic_move_evidence_once(const std::filesystem::path &path,
                                  const BasicDirectoryTransferManifest &manifest,
                                  const std::string_view description) {
    const auto payload = encode_directory_root_ownership_marker(atomic_move_marker_for(manifest));
    return persist_payload_file_once(path, payload, description);
}

BasicDirectoryTransferResult
load_atomic_move_evidence(const std::filesystem::path &path,
                          const BasicDirectoryTransferManifest &manifest,
                          const std::string_view description) {
    const auto known_cifs = path_is_cifs(path.parent_path());
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " is not an ordinary file", error, error.value());
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != kDirectoryRootOwnershipMarkerBytes) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " size is invalid", error, error.value());
    }
    auto opened = platform::open_read_only_source(path, kDirectoryRootOwnershipMarkerBytes);
    if (!opened.source) {
        auto mapped = map_source_error(opened.error.kind, opened.error.platform_code);
#if defined(__linux__)
        if (known_cifs && opened.error.platform_code == EIO) {
            mapped = BasicDirectoryTransferStatus::disconnected;
        }
#endif
        return failure(mapped, std::string(description) + " could not be opened", {},
                       opened.error.platform_code);
    }
    std::array<std::byte, kDirectoryRootOwnershipMarkerBytes> payload{};
    std::size_t offset{};
    while (offset < payload.size()) {
        const auto count = read_source(*opened.source, std::span(payload).subspan(offset), error);
        if (!count) {
            return failure(map_transfer_error(error, known_cifs),
                           std::string(description) + " could not be read", error, error.value());
        }
        if (*count == 0U) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           std::string(description) + " is truncated");
        }
        offset += *count;
    }
    const auto identity = opened.source->identity_result();
    if (identity.status != platform::SourceIdentityStatus::unchanged) {
        const auto identity_error = source_platform_error(identity.platform_code);
        return failure(identity.status == platform::SourceIdentityStatus::changed
                           ? BasicDirectoryTransferStatus::staging_changed
                           : map_transfer_error(identity_error, known_cifs),
                       std::string(description) + " changed while it was read", identity_error,
                       identity.platform_code);
    }
    DirectoryRootOwnershipMarker marker;
    std::string detail;
    if (!decode_directory_root_ownership_marker(payload, marker, detail) ||
        !atomic_move_marker_matches(marker, manifest)) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " does not match its manifest");
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult persist_atomic_move_committed_proof_once(
    const BasicDirectoryTransferManifest &manifest,
    const BasicDirectoryTransferDigest &destination_identity_digest) {
    const auto payload = encode_atomic_move_committed_proof(manifest, destination_identity_digest);
    return persist_payload_file_once(atomic_move_proof_path(manifest), payload,
                                     "atomic move committed proof");
}

BasicDirectoryTransferResult
load_atomic_move_committed_proof(const BasicDirectoryTransferManifest &manifest,
                                 AtomicMoveCommittedProof &proof) {
    const auto path = atomic_move_proof_path(manifest);
    const auto known_cifs = path_is_cifs(path.parent_path());
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "atomic move committed proof is not an ordinary file", error, error.value());
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != atomicMoveProofEncodedBytes) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "atomic move committed proof size is invalid", error, error.value());
    }
    auto opened = platform::open_read_only_source(path, atomicMoveProofEncodedBytes);
    if (!opened.source) {
        auto mapped = map_source_error(opened.error.kind, opened.error.platform_code);
#if defined(__linux__)
        if (known_cifs && opened.error.platform_code == EIO) {
            mapped = BasicDirectoryTransferStatus::disconnected;
        }
#endif
        return failure(mapped, "atomic move committed proof could not be opened", {},
                       opened.error.platform_code);
    }
    std::array<std::byte, atomicMoveProofEncodedBytes> payload{};
    std::size_t offset{};
    while (offset < payload.size()) {
        const auto count = read_source(*opened.source, std::span(payload).subspan(offset), error);
        if (!count) {
            return failure(map_transfer_error(error, known_cifs),
                           "atomic move committed proof could not be read", error, error.value());
        }
        if (*count == 0U) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "atomic move committed proof is truncated");
        }
        offset += *count;
    }
    const auto identity = opened.source->identity_result();
    if (identity.status != platform::SourceIdentityStatus::unchanged) {
        const auto identity_error = source_platform_error(identity.platform_code);
        return failure(identity.status == platform::SourceIdentityStatus::changed
                           ? BasicDirectoryTransferStatus::staging_changed
                           : map_transfer_error(identity_error, known_cifs),
                       "atomic move committed proof changed while it was read", identity_error,
                       identity.platform_code);
    }
    if (!decode_atomic_move_committed_proof(payload, manifest, proof)) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       "atomic move committed proof is corrupt or does not match its manifest");
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult
remove_atomic_move_marker(const std::filesystem::path &root,
                          const BasicDirectoryTransferManifest &manifest) {
    const auto marker = atomic_move_marker_path(root);
    auto loaded = load_atomic_move_evidence(marker, manifest, "atomic move ownership marker");
    if (!loaded.ok()) {
        return loaded;
    }
    std::error_code error;
    if (!std::filesystem::remove(marker, error) || error) {
        return failure(error ? map_transfer_error(error, path_is_cifs(root))
                             : BasicDirectoryTransferStatus::staging_changed,
                       "atomic move ownership marker could not be removed", error, error.value());
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::atomic_move_marker_removal_flush)) {
        return failure(BasicDirectoryTransferStatus::io_error,
                       "atomic move ownership marker removal flush failed by test injection");
    }
#endif
    return sync_proof_namespace(marker, "atomic move ownership marker removal");
}

BasicDirectoryTransferResult
remove_atomic_move_evidence_after_audit(const std::filesystem::path &path,
                                        const std::string_view description) {
    const auto known_cifs = path_is_cifs(path.parent_path());
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " is not an ordinary file", error, error.value());
    }
    if (!std::filesystem::remove(path, error) || error) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       std::string(description) + " could not be retired", error, error.value());
    }
    return sync_proof_namespace(path, std::string(description) + " retirement");
}

BasicDirectoryTransferResult
persist_publication_proof_once(const BasicDirectoryTransferManifest &manifest,
                               const DirectoryPublicationProof &proof) {
    return persist_proof_file_once(publication_proof_path(manifest), proof,
                                   "directory publication proof");
}

BasicDirectoryTransferResult load_publication_proof(const BasicDirectoryTransferManifest &manifest,
                                                    DirectoryPublicationProof &proof) {
    const auto path = publication_proof_path(manifest);
    auto loaded = load_proof_file(path, manifest, proof, "directory publication proof");
    if (!loaded.ok()) {
        return loaded;
    }
    return confirm_proof_file_durability(path, "directory publication proof");
}

BasicDirectoryTransferResult
ensure_publication_proof(const BasicDirectoryTransferManifest &manifest,
                         const std::unordered_map<std::string, FileProof> &proofs,
                         DirectoryPublicationProof &proof) {
    const auto digest = tree_digest(manifest, proofs);
    if (!digest) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       "directory publication proof lacks a copied file digest");
    }
    DirectoryPublicationProof expected{
        .version = publicationProofVersion,
        .operation_id = manifest.operation_id,
        .total_bytes = manifest.total_bytes,
        .entry_count = static_cast<std::uint32_t>(manifest.entries.size()),
        .ownership_token = manifest.ownership_token,
        .manifest_digest = manifest_digest(manifest),
        .tree_digest = *digest,
    };
    std::error_code error;
    const auto exists = std::filesystem::exists(publication_proof_path(manifest), error);
    if (error) {
        return failure(map_transfer_error(error, path_is_cifs(manifest.destination.parent_path())),
                       "directory publication proof state could not be read", error, error.value());
    }
    if (exists) {
        DirectoryPublicationProof loaded;
        auto result = load_publication_proof(manifest, loaded);
        if (!result.ok()) {
            return result;
        }
        if (loaded.tree_digest != expected.tree_digest) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "directory publication proof differs from staged content");
        }
        proof = loaded;
        return failure(BasicDirectoryTransferStatus::success, {});
    }
    auto persisted = persist_publication_proof_once(manifest, expected);
    if (persisted.ok()) {
        proof = expected;
    }
    return persisted;
}

bool remove_manifest_artifacts(const std::filesystem::path &manifest_path, std::error_code &error) {
    error.clear();
    std::filesystem::remove(manifest_path, error);
    if (error) {
        return false;
    }
    const auto control_directory = manifest_path.parent_path();
    std::filesystem::remove(control_directory / "publication-proof", error);
    if (error) {
        return false;
    }
    std::filesystem::remove(control_directory / "atomic-move-proof", error);
    if (error) {
        return false;
    }
    std::filesystem::remove(control_directory / "lease", error);
    if (error) {
        return false;
    }
    std::filesystem::remove(control_directory, error);
    return !error;
}

bool retire_manifest_before_lease_release(RecoveryManifestLease &lease,
                                          const std::filesystem::path &manifest_path,
                                          std::error_code &error) {
    error.clear();
    std::filesystem::remove(manifest_path, error);
    if (error) {
        lease.release();
        return false;
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    pause_after_manifest_retirement_when_requested();
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::exception_after_manifest_retirement)) {
        throw std::runtime_error("test exception after manifest retirement");
    }
#endif
    lease.release();
    const auto control_directory = manifest_path.parent_path();
    std::filesystem::remove(control_directory / "publication-proof", error);
    if (error) {
        return false;
    }
    std::filesystem::remove(control_directory / "atomic-move-proof", error);
    if (error) {
        return false;
    }
    std::filesystem::remove(control_directory / "lease", error);
    if (error) {
        return false;
    }
    std::filesystem::remove(control_directory, error);
    return !error;
}

std::optional<std::size_t> read_source(platform::ReadOnlySource &source,
                                       const std::span<std::byte> buffer,
                                       std::error_code &error) noexcept {
#ifdef _WIN32
    DWORD count{};
    if (ReadFile(std::bit_cast<HANDLE>(source.native_object()), buffer.data(),
                 static_cast<DWORD>(buffer.size()), &count, nullptr) == FALSE) {
        error = {static_cast<int>(GetLastError()), std::system_category()};
        return std::nullopt;
    }
    error.clear();
    return static_cast<std::size_t>(count);
#else
    while (true) {
        const auto count =
            ::read(static_cast<int>(source.native_object()), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            error = {errno, std::generic_category()};
            return std::nullopt;
        }
        error.clear();
        return static_cast<std::size_t>(count);
    }
#endif
}

#ifdef _WIN32
platform::windows_detail::WindowsDataStreamCopyResult bind_data_streams_to_file_proof(
    const std::filesystem::path &path, const HANDLE source,
    BasicDirectoryTransferDigest &digest) {
    const auto contents = platform::windows_detail::read_supported_transfer_data_streams(path, source);
    if (!contents.ok()) {
        return contents.result;
    }
    // Keep legacy primary-only proofs only for positively enumerated no-ADS files. Never
    // retry an ADS proof mismatch (including legacy uncertain-enumeration fallback proofs)
    // with a primary-only hash, or treat an enumeration failure as absence of metadata.
    if (!contents.streams.has_zone_identifier && !contents.streams.has_empty_encryptable) {
        return {};
    }
    detail::Sha256 hash;
    hash_text(hash, "VO-VE file proof data streams v1");
    hash.update(std::as_bytes(std::span(digest)));
    hash_integer(hash, static_cast<std::uint8_t>(contents.streams.has_zone_identifier));
    hash_integer(hash, contents.streams.zone_identifier_bytes);
    hash.update(contents.zone_identifier);
    hash_integer(hash, static_cast<std::uint8_t>(contents.streams.has_empty_encryptable));
    hash_integer(hash, std::uint64_t{0});
    digest = hash.digest();
    return {};
}
#endif

BasicDirectoryTransferResult
copy_file_to_staging(const BasicDirectoryTransferManifest &manifest,
                     const BasicDirectoryTransferEntry &entry, const std::size_t entry_index,
                     FileProof &proof, const bool source_root_cifs, const bool destination_cifs,
                     const BasicDirectoryTransferProgressCallback &progress,
                     const std::size_t completed_entries, const std::uint64_t completed_bytes) {
    const auto source_path = manifest.source / entry.relative_path;
    const auto destination_path = manifest.staging_destination / entry.relative_path;
    const auto temp_path =
        file_transfer_temp_destination_path(destination_path, manifest.operation_id, entry_index);
    auto opened = platform::open_read_only_source(source_path,
                                                  entry.size_bytes == 0U ? 1U : entry.size_bytes);
    if (!opened.source) {
        auto status = map_source_error(opened.error.kind, opened.error.platform_code);
#if defined(__linux__)
        if (source_root_cifs && opened.error.platform_code == EIO) {
            status = BasicDirectoryTransferStatus::disconnected;
        }
#endif
        auto result = failure(status, opened.error.detail, {}, opened.error.platform_code);
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    bool source_cifs = source_root_cifs;
#if defined(__linux__)
    source_cifs =
        source_cifs || descriptor_is_cifs(static_cast<int>(opened.source->native_object()));
#endif
    const auto shape = inspect_source_shape(source_path, *opened.source);
    if (!shape.ok()) {
        const auto status = shape.status == BasicDirectoryTransferStatus::io_error
                                ? map_transfer_error(shape.error, source_cifs)
                                : shape.status;
        auto result = failure(status, shape.detail_utf8, shape.error, shape.platform_code);
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    if (opened.source->size_bytes() != entry.size_bytes ||
        opened.source->modified_unix_ns() != entry.modified_unix_ns ||
        opened.source->source_revision_utf8() != entry.source_revision_utf8) {
        auto result = failure(BasicDirectoryTransferStatus::source_changed,
                              "source file changed after the directory manifest was built");
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    ExclusiveWriter writer;
    std::error_code error;
    if (!writer.open(temp_path, error)) {
        auto result = failure(map_transfer_error(error, destination_cifs),
                              "staging temporary file could not be created", error, error.value());
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    const auto writer_cifs = destination_cifs || writer.on_cifs_filesystem();
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::leaf_write)) {
        auto result = failure(BasicDirectoryTransferStatus::io_error,
                              "staging file write failed by test injection");
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
#endif
    detail::Sha256 hash;
    std::vector<std::byte> buffer(copyBufferBytes);
    std::uint64_t copied{};
    std::uint64_t last_reported_bytes{};
    auto last_progress = std::chrono::steady_clock::now();
    while (true) {
        const auto count = read_source(*opened.source, buffer, error);
        if (!count) {
            auto result = failure(map_transfer_error(error, source_cifs),
                                  "source file could not be read", error, error.value());
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            return result;
        }
        if (*count == 0U) {
            break;
        }
        if (*count > entry.size_bytes - copied) {
            auto result = failure(BasicDirectoryTransferStatus::source_changed,
                                  "source file grew while it was copied");
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            return result;
        }
        const auto bytes = std::span<const std::byte>(buffer.data(), *count);
        hash.update(bytes);
        if (!writer.write(bytes, error)) {
            auto result = failure(map_transfer_error(error, writer_cifs),
                                  "staging file could not be written", error, error.value());
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            return result;
        }
        copied += *count;
        const auto now = std::chrono::steady_clock::now();
        if (copied == entry.size_bytes ||
            copied - last_reported_bytes >= copyProgressByteInterval ||
            now - last_progress >= copyProgressTimeInterval) {
            report_progress(progress, BasicDirectoryTransferPhase::copying, manifest,
                            completed_entries, completed_bytes + copied, source_path);
            last_reported_bytes = copied;
            last_progress = now;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        if (copied == *count) {
            pause_network_phase_when_requested("copy");
        }
#endif
    }
    if (copied != entry.size_bytes) {
        auto result = failure(BasicDirectoryTransferStatus::source_changed,
                              "source file changed while it was copied");
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    auto digest = hash.digest();
#ifdef _WIN32
    auto streams = writer.copy_data_streams(source_path,
        std::bit_cast<HANDLE>(opened.source->native_object()));
    if (streams.ok()) {
        streams = bind_data_streams_to_file_proof(source_path,
            std::bit_cast<HANDLE>(opened.source->native_object()), digest);
    }
    if (!streams.ok()) {
        const std::error_code stream_error(static_cast<int>(streams.error), std::system_category());
        const auto status = streams.source_changed ? BasicDirectoryTransferStatus::source_changed
            : streams.error == ERROR_NOT_SUPPORTED || streams.error == ERROR_INVALID_FUNCTION
                ? BasicDirectoryTransferStatus::unsupported : map_transfer_error(stream_error, writer_cifs);
        auto result = failure(status,
                              streams.detail, stream_error, streams.error);
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
#endif
    const auto identity = identity_result_for_transfer(*opened.source, IdentityCheckRole::source);
    if (identity.status != platform::SourceIdentityStatus::unchanged) {
        const auto unavailable = identity.status == platform::SourceIdentityStatus::unavailable;
        const auto status = unavailable
                                ? map_source_operation_error(identity.error_kind,
                                                             identity.platform_code, source_cifs)
                                : BasicDirectoryTransferStatus::source_changed;
        const auto identity_error = source_platform_error(identity.platform_code);
        auto result = failure(status,
                              unavailable ? "source file identity could not be revalidated"
                                          : "source file changed while it was copied",
                              identity_error, identity.platform_code);
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    if (!writer.flush(error)) {
        auto result = failure(map_transfer_error(error, writer_cifs),
                              "staging file could not be flushed", error, error.value());
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        return result;
    }
    proof = {.digest = digest, .bytes = copied};
    auto result = failure(BasicDirectoryTransferStatus::success, {});
    result.staging_destination = manifest.staging_destination;
    result.destination = manifest.destination;
    return result;
}

RenameNoReplaceResult rename_no_replace(const std::filesystem::path &source,
                                        const std::filesystem::path &destination,
                                        const bool known_cifs = false) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE) {
        RenameNoReplaceResult result;
        result.status = BasicDirectoryTransferStatus::success;
        return result;
    }
    const auto code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        return {.status = BasicDirectoryTransferStatus::conflict,
                .error = {static_cast<int>(code), std::system_category()},
                .platform_code = static_cast<std::int64_t>(code),
                .detail_utf8 = "destination already exists"};
    }
    if (code == ERROR_NOT_SAME_DEVICE) {
        return {.status = BasicDirectoryTransferStatus::unsupported,
                .error = {static_cast<int>(code), std::system_category()},
                .platform_code = static_cast<std::int64_t>(code),
                .detail_utf8 = "no-replace rename crosses filesystem volumes"};
    }
    const std::error_code error(static_cast<int>(code), std::system_category());
#else
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) == 0) {
        RenameNoReplaceResult result;
        result.status = BasicDirectoryTransferStatus::success;
        return result;
    }
    const auto code = errno;
    if (code == EEXIST || code == ENOTEMPTY) {
        return {.status = BasicDirectoryTransferStatus::conflict,
                .error = {code, std::generic_category()},
                .platform_code = code,
                .detail_utf8 = "destination already exists"};
    }
    if (code == ENOSYS || code == EINVAL || code == EOPNOTSUPP) {
        return {.status = BasicDirectoryTransferStatus::unsupported,
                .error = {code, std::generic_category()},
                .platform_code = code,
                .detail_utf8 = "filesystem does not support no-replace rename"};
    }
    if (code == EXDEV) {
        return {.status = BasicDirectoryTransferStatus::unsupported,
                .error = {code, std::generic_category()},
                .platform_code = code,
                .detail_utf8 = "no-replace rename crosses filesystem volumes"};
    }
    const std::error_code error(code, std::generic_category());
#else
    RenameNoReplaceResult result;
    result.status = BasicDirectoryTransferStatus::unsupported;
    result.detail_utf8 = "platform does not provide no-replace rename";
    return result;
#endif
#endif
    return {.status = map_transfer_error(error, known_cifs),
            .error = error,
            .platform_code = error.value(),
            .detail_utf8 = "no-replace rename failed"};
}

bool is_cross_device_rename_failure(const RenameNoReplaceResult &result) noexcept {
#ifdef _WIN32
    return result.platform_code == ERROR_NOT_SAME_DEVICE;
#else
    return result.platform_code == EXDEV;
#endif
}

BasicDirectoryTransferResult
confirm_atomic_move_namespace(const BasicDirectoryTransferManifest &manifest) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::publication_namespace_flush)) {
        return failure(BasicDirectoryTransferStatus::io_error,
                       "atomic move namespace flush failed by test injection");
    }
#endif
#ifdef _WIN32
    static_cast<void>(manifest);
    return failure(BasicDirectoryTransferStatus::success, {});
#else
    const auto source_parent = manifest.source.parent_path();
    auto source_result = sync_directory_namespace(source_parent, path_is_cifs(source_parent),
                                                  "atomic move source parent");
    if (!source_result.ok()) {
        return source_result;
    }
    const auto destination_parent = manifest.destination.parent_path();
    if (source_parent == destination_parent) {
        return source_result;
    }
    return sync_directory_namespace(destination_parent, path_is_cifs(destination_parent),
                                    "atomic move destination parent");
#endif
}

BasicDirectoryTransferResult preserve_ambiguous_atomic_move_destination(std::string detail) {
    auto result =
        failure(BasicDirectoryTransferStatus::recovery_required,
                std::move(detail) + "; destination was left untouched for explicit recovery");
    result.recovery_available = true;
    return result;
}

BasicDirectoryTransferResult hash_file(const std::filesystem::path &path,
                                       const std::uint64_t expected_bytes, FileProof &proof,
                                       const BasicDirectoryTransferStatus changed_status,
                                       const bool known_cifs) {
    auto opened = platform::open_read_only_source(path, expected_bytes == 0U ? 1U : expected_bytes);
    if (!opened.source) {
        auto status = map_source_error(opened.error.kind, opened.error.platform_code);
#if defined(__linux__)
        if (known_cifs && opened.error.platform_code == EIO) {
            status = BasicDirectoryTransferStatus::disconnected;
        }
#endif
        if (status == BasicDirectoryTransferStatus::source_changed) {
            status = changed_status;
        }
        return failure(status, opened.error.detail, {}, opened.error.platform_code);
    }
    const auto shape = inspect_source_shape(path, *opened.source);
    if (!shape.ok()) {
        const auto status = shape.status == BasicDirectoryTransferStatus::io_error
                                ? map_transfer_error(shape.error, known_cifs)
                                : changed_status;
        return failure(status, "audited file has an unsupported filesystem shape", shape.error,
                       shape.platform_code);
    }
    if (opened.source->size_bytes() != expected_bytes) {
        return failure(changed_status, "audited file size differs from the expected proof");
    }
    detail::Sha256 hash;
    std::vector<std::byte> buffer(copyBufferBytes);
    std::uint64_t bytes{};
    std::error_code error;
    while (true) {
        const auto count = read_source(*opened.source, buffer, error);
        if (!count) {
            return failure(map_transfer_error(error, known_cifs),
                           "staging file could not be audited", error, error.value());
        }
        if (*count == 0U) {
            break;
        }
        hash.update(std::span<const std::byte>(buffer.data(), *count));
        bytes += *count;
    }
    if (bytes != expected_bytes) {
        return failure(changed_status, "audited file changed during the final audit");
    }
    auto digest = hash.digest();
#ifdef _WIN32
    const auto streams = bind_data_streams_to_file_proof(path,
        std::bit_cast<HANDLE>(opened.source->native_object()), digest);
    if (!streams.ok()) {
        const std::error_code stream_error(static_cast<int>(streams.error), std::system_category());
        const auto status = streams.source_changed || streams.error == ERROR_NOT_SUPPORTED ||
                                    streams.error == ERROR_INVALID_FUNCTION
                                ? changed_status : map_transfer_error(stream_error, known_cifs);
        return failure(status, streams.detail, stream_error, streams.error);
    }
#endif
    const auto role = changed_status == BasicDirectoryTransferStatus::source_changed
                          ? IdentityCheckRole::source
                          : IdentityCheckRole::staging;
    const auto identity = identity_result_for_transfer(*opened.source, role);
    if (identity.status != platform::SourceIdentityStatus::unchanged) {
        if (identity.status == platform::SourceIdentityStatus::changed) {
            return failure(changed_status, "audited file changed during the final audit");
        }
        const auto status =
            map_source_operation_error(identity.error_kind, identity.platform_code, known_cifs);
        return failure(status, "audited file identity could not be revalidated",
                       source_platform_error(identity.platform_code), identity.platform_code);
    }
    proof = {.digest = digest, .bytes = bytes};
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult
audit_staging(const BasicDirectoryTransferManifest &manifest,
              const std::unordered_map<std::string, FileProof> &proofs) {
    const auto destination_cifs = path_is_cifs(manifest.staging_destination);
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    pause_network_phase_when_requested("final-audit");
#endif
    std::unordered_map<std::string, const BasicDirectoryTransferEntry *> expected;
    expected.reserve(manifest.entries.size());
    for (const auto &entry : manifest.entries) {
        expected.emplace(
            directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics), &entry);
    }
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(manifest.staging_destination, error);
    if (error || !ordinary_directory(manifest.staging_destination, root_status, error)) {
        return failure(error ? map_transfer_error(error, destination_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "staging root is no longer an ordinary directory", error, error.value());
    }
    std::filesystem::recursive_directory_iterator iterator(
        manifest.staging_destination, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return failure(map_transfer_error(error, destination_cifs),
                       "staging tree could not be enumerated", error, error.value());
    }
    std::size_t seen{};
    while (iterator != end) {
        const auto relative =
            iterator->path().lexically_relative(manifest.staging_destination).lexically_normal();
        const auto key = directory_transfer_namespace_key(relative, manifest.path_semantics);
        const auto found = expected.find(key);
        if (found == expected.end()) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "staging tree contains an unplanned entry");
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            return failure(map_transfer_error(error, destination_cifs),
                           "staging entry metadata could not be read", error, error.value());
        }
        const auto &entry = *found->second;
        const auto directory_ok = entry.kind == BasicDirectoryTransferEntryKind::directory
                                      ? ordinary_directory(iterator->path(), status, error)
                                      : false;
        if (error || (entry.kind == BasicDirectoryTransferEntryKind::directory && !directory_ok) ||
            (entry.kind == BasicDirectoryTransferEntryKind::regular_file &&
             (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))) {
            return failure(error ? map_transfer_error(error, destination_cifs)
                                 : BasicDirectoryTransferStatus::staging_changed,
                           "staging entry kind differs from the manifest", error, error.value());
        }
        if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            const auto proof = proofs.find(key);
            if (proof == proofs.end()) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "staging file has no committed copy proof");
            }
            FileProof audited;
            auto audited_result =
                hash_file(iterator->path(), entry.size_bytes, audited,
                          BasicDirectoryTransferStatus::staging_changed, destination_cifs);
            if (!audited_result.ok()) {
                return audited_result;
            }
            if (audited.bytes != proof->second.bytes || audited.digest != proof->second.digest) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "staging file content differs from the copied proof");
            }
        }
        ++seen;
        iterator.increment(error);
        if (error) {
            return failure(map_transfer_error(error, destination_cifs),
                           "staging audit was interrupted", error, error.value());
        }
    }
    if (seen != manifest.entries.size()) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       "staging tree is missing a manifest entry");
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult audit_complete_tree_against_proof(
    const std::filesystem::path &root, const BasicDirectoryTransferManifest &manifest,
    const DirectoryPublicationProof &publication_proof,
    const BasicDirectoryTransferStatus changed_status, const bool allow_ownership_marker) {
    const auto known_cifs = path_is_cifs(root);
    std::unordered_map<std::string, const BasicDirectoryTransferEntry *> expected;
    expected.reserve(manifest.entries.size());
    for (const auto &entry : manifest.entries) {
        expected.emplace(
            directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics), &entry);
    }
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error || !ordinary_directory(root, root_status, error)) {
        return failure(error ? map_transfer_error(error, known_cifs) : changed_status,
                       "audited directory root is no longer ordinary", error, error.value());
    }
    std::unordered_map<std::string, FileProof> proofs;
    proofs.reserve(manifest.entries.size());
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return failure(map_transfer_error(error, known_cifs),
                       "directory proof audit could not enumerate its root", error, error.value());
    }
    std::size_t seen{};
    while (iterator != end) {
        const auto relative = iterator->path().lexically_relative(root).lexically_normal();
        if (allow_ownership_marker &&
            relative == std::filesystem::path(core::kTransferOwnershipMarkerFilename)) {
            iterator.increment(error);
            if (error) {
                return failure(map_transfer_error(error, known_cifs),
                               "directory proof audit was interrupted", error, error.value());
            }
            continue;
        }
        const auto key = directory_transfer_namespace_key(relative, manifest.path_semantics);
        const auto found = expected.find(key);
        if (found == expected.end()) {
            return failure(changed_status, "audited directory contains an unplanned entry");
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            return failure(map_transfer_error(error, known_cifs),
                           "audited directory entry metadata could not be read", error,
                           error.value());
        }
        const auto &entry = *found->second;
        const auto directory_ok = entry.kind == BasicDirectoryTransferEntryKind::directory
                                      ? ordinary_directory(iterator->path(), status, error)
                                      : false;
        if (error || (entry.kind == BasicDirectoryTransferEntryKind::directory && !directory_ok) ||
            (entry.kind == BasicDirectoryTransferEntryKind::regular_file &&
             (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))) {
            return failure(error ? map_transfer_error(error, known_cifs) : changed_status,
                           "audited directory entry kind differs from the manifest", error,
                           error.value());
        }
        if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            FileProof proof;
            auto hashed =
                hash_file(iterator->path(), entry.size_bytes, proof, changed_status, known_cifs);
            if (!hashed.ok()) {
                return hashed;
            }
            proofs.emplace(key, proof);
        }
        ++seen;
        iterator.increment(error);
        if (error) {
            return failure(map_transfer_error(error, known_cifs),
                           "directory proof audit was interrupted", error, error.value());
        }
    }
    if (seen != manifest.entries.size()) {
        return failure(changed_status, "audited directory is missing a manifest entry");
    }
    const auto digest = tree_digest(manifest, proofs);
    if (!digest || *digest != publication_proof.tree_digest) {
        return failure(changed_status, "audited directory content differs from publication proof");
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult move_recovery_required(
    const BasicDirectoryTransferManifest &manifest, std::string detail,
    const std::error_code error = {}, const std::int64_t platform_code = 0,
    const BasicDirectoryTransferStatus status = BasicDirectoryTransferStatus::recovery_required) {
    auto result = failure(status, std::move(detail), error, platform_code);
    result.staging_destination = manifest.staging_destination;
    result.destination = manifest.destination;
    result.manifest_path = basic_directory_transfer_manifest_path(manifest);
    result.recovery_available = true;
    return result;
}

BasicDirectoryTransferResult path_presence(const std::filesystem::path &path, const bool known_cifs,
                                           bool &present) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        present = false;
        return failure(BasicDirectoryTransferStatus::success, {});
    }
    if (error) {
        return failure(map_transfer_error(error, known_cifs),
                       "directory move namespace state could not be read", error, error.value());
    }
    present = status.type() != std::filesystem::file_type::not_found;
    return failure(BasicDirectoryTransferStatus::success, {});
}

#ifdef _WIN32
BasicDirectoryTransferResult verify_retired_file_data_streams(
    const std::filesystem::path &source_path, const HANDLE source,
    const std::filesystem::path &staging_path, const std::uint64_t primary_bytes) {
    const auto read_failure = [](const platform::windows_detail::WindowsDataStreamCopyResult &result) {
        const std::error_code error(static_cast<int>(result.error), std::system_category());
        const auto status = result.source_changed || result.error == ERROR_NOT_SUPPORTED ||
                                    result.error == ERROR_INVALID_FUNCTION
                                ? BasicDirectoryTransferStatus::staging_changed
                                : map_error_status(error);
        return failure(status, result.detail, error, result.error);
    };
    const auto original = platform::windows_detail::read_supported_transfer_data_streams(source_path, source);
    if (!original.ok()) {
        return read_failure(original.result);
    }
    auto staged = platform::open_read_only_source(staging_path, primary_bytes == 0U ? 1U : primary_bytes);
    if (!staged.source) {
        return failure(map_source_operation_error(staged.error.kind, staged.error.platform_code, false),
                       "staged metadata could not be opened before source deletion", {},
                       staged.error.platform_code);
    }
    const auto copied = platform::windows_detail::read_supported_transfer_data_streams(
        staging_path, std::bit_cast<HANDLE>(staged.source->native_object()));
    if (!copied.ok()) {
        return read_failure(copied.result);
    }
    // Old uncertain-enumeration fallbacks could leave no ADS in staging at all. A matching
    // legacy primary-only tree proof cannot authorize deleting metadata still on the source.
    if (original.streams.has_zone_identifier != copied.streams.has_zone_identifier ||
        original.streams.zone_identifier_bytes != copied.streams.zone_identifier_bytes ||
        original.streams.has_empty_encryptable != copied.streams.has_empty_encryptable ||
        original.zone_identifier != copied.zone_identifier) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       "retired source data streams differ from the staged copy");
    }
    const auto identity = identity_result_for_transfer(*staged.source, IdentityCheckRole::staging);
    if (identity.status != platform::SourceIdentityStatus::unchanged) {
        return failure(identity.status == platform::SourceIdentityStatus::changed
                           ? BasicDirectoryTransferStatus::staging_changed
                           : map_source_operation_error(identity.error_kind, identity.platform_code, false),
                       "staged metadata identity could not be confirmed before source deletion",
                       source_platform_error(identity.platform_code), identity.platform_code);
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}
#endif

BasicDirectoryTransferResult
validate_retired_source_subset(const BasicDirectoryTransferManifest &manifest,
                               const DirectoryPublicationProof &publication_proof,
                               bool require_marker);

BasicDirectoryTransferResult
ensure_source_ownership_marker(const BasicDirectoryTransferManifest &manifest,
                               const DirectoryPublicationProof &publication_proof) {
    const auto marker = source_ownership_marker_path(manifest);
    bool present{};
    auto observed = path_presence(marker, path_is_cifs(marker.parent_path()), present);
    if (!observed.ok()) {
        return observed;
    }
    if (present) {
        DirectoryPublicationProof loaded;
        auto result = load_proof_file(marker, manifest, loaded, "source retirement marker");
        if (result.ok() && loaded.tree_digest == publication_proof.tree_digest) {
            return failure(BasicDirectoryTransferStatus::success, {});
        }
        if (result.status != BasicDirectoryTransferStatus::staging_changed) {
            return result;
        }
        auto audited = validate_retired_source_subset(manifest, publication_proof, false);
        if (!audited.ok()) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "invalid source retirement marker cannot be repaired safely");
        }
        std::error_code error;
        if (!std::filesystem::remove(marker, error) || error) {
            return failure(error ? map_transfer_error(error, path_is_cifs(marker.parent_path()))
                                 : BasicDirectoryTransferStatus::staging_changed,
                           "invalid source retirement marker could not be retired", error,
                           error.value());
        }
    }
    return persist_proof_file_once(marker, publication_proof, "source retirement marker");
}

BasicDirectoryTransferResult
validate_retired_source_subset(const BasicDirectoryTransferManifest &manifest,
                               const DirectoryPublicationProof &publication_proof,
                               const bool require_marker) {
    const auto retirement = source_retirement_path(manifest);
    const auto known_cifs = path_is_cifs(retirement);
    if (require_marker) {
        DirectoryPublicationProof marker;
        auto loaded = load_proof_file(source_ownership_marker_path(manifest), manifest, marker,
                                      "source retirement marker");
        if (!loaded.ok()) {
            return loaded;
        }
        if (marker.tree_digest != publication_proof.tree_digest) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "source retirement marker has the wrong content proof");
        }
    }
    std::unordered_map<std::string, const BasicDirectoryTransferEntry *> expected;
    expected.reserve(manifest.entries.size() * 2U);
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &entry = manifest.entries[index];
        expected.emplace(
            directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics), &entry);
        const auto tombstone_relative =
            source_entry_retirement_tombstone_path(manifest, entry, index)
                .lexically_relative(retirement)
                .lexically_normal();
        expected.emplace(
            directory_transfer_namespace_key(tombstone_relative, manifest.path_semantics), &entry);
    }
    std::unordered_set<const BasicDirectoryTransferEntry *> seen;
    seen.reserve(manifest.entries.size());
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(retirement, error);
    if (error || !ordinary_directory(retirement, root_status, error)) {
        return failure(error ? map_transfer_error(error, known_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "source retirement root is no longer ordinary", error, error.value());
    }
    const auto root_revision = directory_revision_for_transfer(retirement);
    if (!root_revision) {
        return failure(map_source_operation_error(root_revision.error.kind,
                                                  root_revision.error.platform_code, known_cifs),
                       "source retirement root identity could not be read", {},
                       root_revision.error.platform_code);
    }
    const auto expected_root_identity = directory_transfer_canonical_object_identity(
        manifest.source_revision_utf8, manifest.path_semantics);
    const auto actual_root_identity = directory_transfer_canonical_object_identity(
        root_revision.revision_utf8, manifest.path_semantics);
    if (expected_root_identity.empty() || actual_root_identity != expected_root_identity) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       "source retirement root identity changed");
    }
    std::filesystem::recursive_directory_iterator iterator(
        retirement, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return failure(map_transfer_error(error, known_cifs),
                       "source retirement tree could not be enumerated", error, error.value());
    }
    while (iterator != end) {
        const auto relative = iterator->path().lexically_relative(retirement).lexically_normal();
        if (relative == std::filesystem::path(core::kTransferOwnershipMarkerFilename)) {
            iterator.increment(error);
            if (error) {
                return failure(map_transfer_error(error, known_cifs),
                               "source retirement validation was interrupted", error,
                               error.value());
            }
            continue;
        }
        const auto key = directory_transfer_namespace_key(relative, manifest.path_semantics);
        const auto found = expected.find(key);
        if (found == expected.end()) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "source retirement tree contains an unplanned entry");
        }
        if (!seen.emplace(found->second).second) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "source retirement entry exists under two names");
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            return failure(map_transfer_error(error, known_cifs),
                           "source retirement entry metadata could not be read", error,
                           error.value());
        }
        const auto &entry = *found->second;
        if (entry.kind == BasicDirectoryTransferEntryKind::directory) {
            if (!ordinary_directory(iterator->path(), status, error)) {
                return failure(error ? map_transfer_error(error, known_cifs)
                                     : BasicDirectoryTransferStatus::staging_changed,
                               "source retirement directory changed shape", error, error.value());
            }
            const auto revision = directory_revision_for_transfer(iterator->path());
            if (!revision) {
                return failure(map_source_operation_error(revision.error.kind,
                                                          revision.error.platform_code, known_cifs),
                               "source retirement directory identity could not be read", {},
                               revision.error.platform_code);
            }
            const auto expected_identity = directory_transfer_canonical_object_identity(
                entry.source_revision_utf8, manifest.path_semantics);
            const auto actual_identity = directory_transfer_canonical_object_identity(
                revision.revision_utf8, manifest.path_semantics);
            if (expected_identity.empty() || actual_identity != expected_identity) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "source retirement directory identity changed");
            }
        } else {
            if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "source retirement file changed shape");
            }
            auto opened = platform::open_read_only_source(
                iterator->path(), entry.size_bytes == 0U ? 1U : entry.size_bytes);
            if (!opened.source) {
                return failure(map_source_operation_error(opened.error.kind,
                                                          opened.error.platform_code, known_cifs),
                               "source retirement file could not be validated", {},
                               opened.error.platform_code);
            }
            const auto expected_identity = directory_transfer_canonical_object_identity(
                entry.source_revision_utf8, manifest.path_semantics);
            const auto actual_identity = directory_transfer_canonical_object_identity(
                opened.source->source_revision_utf8(), manifest.path_semantics);
            const auto identity_matches =
                !expected_identity.empty() && !actual_identity.empty()
                    ? expected_identity == actual_identity
                    : opened.source->source_revision_utf8() == entry.source_revision_utf8;
            if (opened.source->size_bytes() != entry.size_bytes ||
                !transfer_timestamp_matches(entry.modified_unix_ns,
                                            opened.source->modified_unix_ns(), known_cifs) ||
                !identity_matches) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "source retirement file changed before deletion");
            }
#ifdef _WIN32
            auto streams = verify_retired_file_data_streams(iterator->path(),
                std::bit_cast<HANDLE>(opened.source->native_object()),
                manifest.staging_destination / entry.relative_path, entry.size_bytes);
            if (!streams.ok()) {
                return streams;
            }
#endif
            const auto identity =
                identity_result_for_transfer(*opened.source, IdentityCheckRole::source);
            if (identity.status != platform::SourceIdentityStatus::unchanged) {
                return failure(identity.status == platform::SourceIdentityStatus::changed
                                   ? BasicDirectoryTransferStatus::staging_changed
                                   : map_source_operation_error(identity.error_kind,
                                                                identity.platform_code, known_cifs),
                               "source retirement file identity could not be confirmed",
                               source_platform_error(identity.platform_code),
                               identity.platform_code);
            }
        }
        iterator.increment(error);
        if (error) {
            return failure(map_transfer_error(error, known_cifs),
                           "source retirement validation was interrupted", error, error.value());
        }
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult
delete_retired_source(const BasicDirectoryTransferManifest &manifest,
                      const DirectoryPublicationProof &publication_proof,
                      const bool retirement_prevalidated) {
    const auto retirement = source_retirement_path(manifest);
    const auto known_cifs = path_is_cifs(retirement);
    if (!retirement_prevalidated) {
        auto validated = validate_retired_source_subset(manifest, publication_proof, true);
        if (!validated.ok()) {
            return validated;
        }
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    pause_after_source_cleanup_validation_when_requested();
#endif
    std::error_code error;
    for (std::size_t remaining = manifest.entries.size(); remaining > 0U; --remaining) {
        const auto entry_index = remaining - 1U;
        const auto &entry = manifest.entries[entry_index];
        const auto path = retirement / entry.relative_path;
        const auto tombstone = source_entry_retirement_tombstone_path(manifest, entry, entry_index);
        bool original_present{};
        auto original_state = path_presence(path, known_cifs, original_present);
        if (!original_state.ok()) {
            return original_state;
        }
        bool tombstone_present{};
        auto tombstone_state = path_presence(tombstone, known_cifs, tombstone_present);
        if (!tombstone_state.ok()) {
            return tombstone_state;
        }
        if (original_present && tombstone_present) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "source retirement entry exists under two names");
        }
        if (!original_present && !tombstone_present) {
            continue;
        }
        if (original_present) {
            auto captured = rename_no_replace(path, tombstone, known_cifs);
            if (!captured.ok()) {
                return failure(captured.status,
                               "source retirement entry could not be captured before deletion",
                               captured.error, captured.platform_code);
            }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
            pause_after_source_entry_capture_when_requested();
            pause_network_phase_when_requested("source-delete");
            if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::
                                          unknown_outcome_after_source_entry_capture)) {
                return failure(BasicDirectoryTransferStatus::unknown_outcome,
                               "source entry retirement reply was lost by the test hook");
            }
#endif
        }
        const auto &captured_path = tombstone;
        const auto status = std::filesystem::symlink_status(captured_path, error);
        if (error) {
            return failure(map_transfer_error(error, known_cifs),
                           "captured source retirement entry state could not be read", error,
                           error.value());
        }
        std::optional<platform::SourceOpenResult> opened;
        SourceDeletionGuard deletion_guard;
        bool busy{};
        const auto directory = entry.kind == BasicDirectoryTransferEntryKind::directory;
        if (!deletion_guard.acquire(captured_path, directory, error, busy)) {
            return failure(busy ? BasicDirectoryTransferStatus::file_in_use
                                : (unsupported_durability_error(error)
                                       ? BasicDirectoryTransferStatus::unsupported
                                       : map_transfer_error(error, known_cifs)),
                           busy ? "source retirement entry is still open for mutation"
                                : "source retirement entry could not be anchored before deletion",
                           error, error.value());
        }
        if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            opened.emplace(platform::open_read_only_source(
                captured_path, entry.size_bytes == 0U ? 1U : entry.size_bytes));
            if (!opened->source) {
                return failure(map_source_operation_error(opened->error.kind,
                                                          opened->error.platform_code, known_cifs),
                               "source retirement file could not be revalidated before deletion",
                               {}, opened->error.platform_code);
            }
            const auto expected_identity = directory_transfer_canonical_object_identity(
                entry.source_revision_utf8, manifest.path_semantics);
            const auto captured_identity = directory_transfer_canonical_object_identity(
                opened->source->source_revision_utf8(), manifest.path_semantics);
            const auto identity_matches =
                !expected_identity.empty() && !captured_identity.empty()
                    ? expected_identity == captured_identity
                    : opened->source->source_revision_utf8() == entry.source_revision_utf8;
            if (opened->source->size_bytes() != entry.size_bytes ||
                !transfer_timestamp_matches(entry.modified_unix_ns,
                                            opened->source->modified_unix_ns(), known_cifs) ||
                !identity_matches) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "source retirement file changed immediately before deletion");
            }
            const auto identity =
                identity_result_for_transfer(*opened->source, IdentityCheckRole::source);
            if (identity.status != platform::SourceIdentityStatus::unchanged) {
                return failure(identity.status == platform::SourceIdentityStatus::changed
                                   ? BasicDirectoryTransferStatus::staging_changed
                                   : map_source_operation_error(identity.error_kind,
                                                                identity.platform_code, known_cifs),
                               "source retirement file identity changed before deletion",
                               source_platform_error(identity.platform_code),
                               identity.platform_code);
            }
        } else {
            if (!ordinary_directory(captured_path, status, error)) {
                return failure(error ? map_transfer_error(error, known_cifs)
                                     : BasicDirectoryTransferStatus::staging_changed,
                               "source retirement directory changed before deletion", error,
                               error.value());
            }
            const auto revision = directory_revision_for_transfer(captured_path);
            if (!revision) {
                return failure(map_source_operation_error(revision.error.kind,
                                                          revision.error.platform_code, known_cifs),
                               "captured source directory identity could not be read", {},
                               revision.error.platform_code);
            }
            const auto expected_identity = directory_transfer_canonical_object_identity(
                entry.source_revision_utf8, manifest.path_semantics);
            const auto captured_identity = directory_transfer_canonical_object_identity(
                revision.revision_utf8, manifest.path_semantics);
            if (expected_identity.empty() || captured_identity != expected_identity) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "captured source directory identity changed before deletion");
            }
        }
        const auto removed = deletion_guard.remove(captured_path, directory, error);
        if (!removed || error) {
            return failure(error ? map_transfer_error(error, known_cifs)
                                 : BasicDirectoryTransferStatus::staging_changed,
                           "source retirement entry could not be removed", error, error.value());
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_network_phase_when_requested("source-delete-remove");
        if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::
                                      unknown_outcome_after_source_entry_remove)) {
            return failure(BasicDirectoryTransferStatus::unknown_outcome,
                           "source entry removal reply was lost by the test hook");
        }
        if (consume_failure_point(
                BasicDirectoryTransferTestFailurePoint::exception_during_source_deletion)) {
            throw std::runtime_error("test exception during source deletion");
        }
#endif
    }
    const auto marker_path = source_ownership_marker_path(manifest);
    bool marker_present{};
    auto marker_state = path_presence(marker_path, known_cifs, marker_present);
    if (!marker_state.ok()) {
        return marker_state;
    }
    if (marker_present) {
        SourceDeletionGuard marker_guard;
        bool marker_busy{};
        if (!marker_guard.acquire(marker_path, false, error, marker_busy)) {
            return failure(marker_busy ? BasicDirectoryTransferStatus::file_in_use
                                       : map_transfer_error(error, known_cifs),
                           marker_busy ? "source retirement marker is still open for mutation"
                                       : "source retirement marker could not be anchored",
                           error, error.value());
        }
        if (!marker_guard.remove(marker_path, false, error)) {
            return failure(error ? map_transfer_error(error, known_cifs)
                                 : BasicDirectoryTransferStatus::source_changed,
                           "source retirement marker could not be removed", error, error.value());
        }
    } else {
        const auto retirement_revision = directory_revision_for_transfer(retirement);
        if (!retirement_revision) {
            return failure(map_source_operation_error(retirement_revision.error.kind,
                                                      retirement_revision.error.platform_code,
                                                      known_cifs),
                           "source retirement marker removal could not be reconciled", {},
                           retirement_revision.error.platform_code);
        }
    }
    bool retirement_still_present{};
    auto retirement_state = path_presence(retirement, known_cifs, retirement_still_present);
    if (!retirement_state.ok()) {
        return retirement_state;
    }
    if (retirement_still_present) {
        SourceDeletionGuard retirement_guard;
        bool retirement_busy{};
        if (!retirement_guard.acquire(retirement, true, error, retirement_busy)) {
            return failure(retirement_busy ? BasicDirectoryTransferStatus::file_in_use
                                           : map_transfer_error(error, known_cifs),
                           retirement_busy ? "source retirement root is still open for mutation"
                                           : "source retirement root could not be anchored",
                           error, error.value());
        }
        if (!retirement_guard.remove(retirement, true, error)) {
            return failure(error ? map_transfer_error(error, known_cifs)
                                 : BasicDirectoryTransferStatus::source_changed,
                           "source retirement root could not be removed", error, error.value());
        }
    } else {
        const auto parent_revision = directory_revision_for_transfer(retirement.parent_path());
        if (!parent_revision) {
            return failure(map_source_operation_error(parent_revision.error.kind,
                                                      parent_revision.error.platform_code,
                                                      known_cifs),
                           "source retirement root removal could not be reconciled", {},
                           parent_revision.error.platform_code);
        }
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult
continue_move_source_cleanup(const BasicDirectoryTransferManifest &manifest,
                             const DirectoryPublicationProof &publication_proof,
                             const BasicDirectoryTransferProgressCallback &progress,
                             ExecutionBoundaryPhase &boundary_phase,
                             const bool staging_durability_confirmed) {
    report_progress(progress, BasicDirectoryTransferPhase::retiring_source, manifest,
                    manifest.entries.size(), manifest.total_bytes, manifest.source);
    const auto retirement = source_retirement_path(manifest);
    const auto source_cifs = path_is_cifs(manifest.source);
    bool source_present{};
    auto source_state = path_presence(manifest.source, source_cifs, source_present);
    if (!source_state.ok()) {
        return move_recovery_required(manifest, source_state.detail_utf8, source_state.error,
                                      source_state.platform_code, source_state.status);
    }
    bool retirement_present{};
    auto retirement_state = path_presence(retirement, source_cifs, retirement_present);
    if (!retirement_state.ok()) {
        return move_recovery_required(manifest, retirement_state.detail_utf8,
                                      retirement_state.error, retirement_state.platform_code,
                                      retirement_state.status);
    }
    if (source_present && retirement_present) {
        return move_recovery_required(
            manifest,
            "both original and retired source roots exist; source cleanup was not attempted");
    }
    if (!source_present && !retirement_present) {
        auto durable_removal = confirm_source_removal_namespace(manifest);
        if (!durable_removal.ok()) {
            return move_recovery_required(
                manifest, "source retirement removal durability could not be confirmed",
                durable_removal.error, durable_removal.platform_code, durable_removal.status);
        }
        return failure(BasicDirectoryTransferStatus::success, {});
    }
    if (!staging_durability_confirmed) {
        auto staging_audit = audit_complete_tree_against_proof(
            manifest.staging_destination, manifest, publication_proof,
            BasicDirectoryTransferStatus::staging_changed, false);
        if (!staging_audit.ok()) {
            return move_recovery_required(
                manifest, "hidden staged copy could not be confirmed before source retirement",
                staging_audit.error, staging_audit.platform_code, staging_audit.status);
        }
        auto durable_staging = sync_staging_tree_namespace(manifest);
        if (!durable_staging.ok()) {
            return move_recovery_required(
                manifest,
                "hidden staged copy durability could not be confirmed before source retirement",
                durable_staging.error, durable_staging.platform_code, durable_staging.status);
        }
    }
    if (source_present) {
        auto capability = probe_source_cleanup_capability(manifest);
        if (!capability.ok() &&
            !(source_cifs && capability.status == BasicDirectoryTransferStatus::unsupported)) {
            return move_recovery_required(manifest, capability.detail_utf8, capability.error,
                                          capability.platform_code, capability.status);
        }
        auto namespace_capability = probe_source_namespace_capability(manifest);
        if (!namespace_capability.ok() &&
            !(source_cifs &&
              namespace_capability.status == BasicDirectoryTransferStatus::unsupported)) {
            return move_recovery_required(
                manifest, namespace_capability.detail_utf8, namespace_capability.error,
                namespace_capability.platform_code, namespace_capability.status);
        }
        auto source_capture = capture_tree(manifest.source);
        if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
            return move_recovery_required(
                manifest,
                source_capture.ok() ? "source changed before move retirement"
                                    : source_capture.detail_utf8,
                source_capture.error, source_capture.platform_code,
                source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                    : source_capture.status);
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_network_phase_when_requested("source-retire");
#endif
        auto retired = rename_no_replace(manifest.source, retirement, source_cifs);
        if (!retired.ok()) {
            return move_recovery_required(manifest, "source retirement was not confirmed",
                                          retired.error, retired.platform_code, retired.status);
        }
        boundary_phase = ExecutionBoundaryPhase::source_retired;
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        if (consume_failure_point(
                BasicDirectoryTransferTestFailurePoint::exception_after_source_retirement)) {
            throw std::runtime_error("test exception after source retirement");
        }
        if (loseSourceRetirementReplyOnce.exchange(false)) {
            return move_recovery_required(manifest, "source retirement reply was lost", {}, 0,
                                          BasicDirectoryTransferStatus::unknown_outcome);
        }
#endif
    }
    auto durable_retirement = confirm_source_retirement_namespace(manifest);
    if (!durable_retirement.ok()) {
        return move_recovery_required(
            manifest, "source retirement durability could not be confirmed",
            durable_retirement.error, durable_retirement.platform_code, durable_retirement.status);
    }
    bool marker_present{};
    auto marker_state =
        path_presence(source_ownership_marker_path(manifest), source_cifs, marker_present);
    if (!marker_state.ok()) {
        return move_recovery_required(manifest, marker_state.detail_utf8, marker_state.error,
                                      marker_state.platform_code, marker_state.status);
    }
    if (!marker_present) {
        std::error_code empty_error;
        const auto empty = std::filesystem::is_empty(retirement, empty_error);
        if (!empty_error && empty) {
            const auto removed = std::filesystem::remove(retirement, empty_error);
            if (!empty_error && removed) {
                auto durable_removal = confirm_source_removal_namespace(manifest);
                if (!durable_removal.ok()) {
                    return move_recovery_required(
                        manifest,
                        "empty source retirement removal durability could not be confirmed",
                        durable_removal.error, durable_removal.platform_code,
                        durable_removal.status);
                }
                return failure(BasicDirectoryTransferStatus::success, {});
            }
        }
        auto retirement_audit = validate_retired_source_subset(manifest, publication_proof, false);
        if (!retirement_audit.ok()) {
            return move_recovery_required(
                manifest, "retired source subset could not be confirmed before deletion",
                retirement_audit.error, retirement_audit.platform_code, retirement_audit.status);
        }
    }
    auto marker = ensure_source_ownership_marker(manifest, publication_proof);
    if (!marker.ok()) {
        return move_recovery_required(manifest, marker.detail_utf8, marker.error,
                                      marker.platform_code, marker.status);
    }
    report_progress(progress, BasicDirectoryTransferPhase::deleting_source, manifest,
                    manifest.entries.size(), manifest.total_bytes, retirement);
    // A continuous run just validated the complete retired tree before writing the marker. Resume
    // must repeat that validation because arbitrary time may have elapsed since the marker write.
    auto deleted = delete_retired_source(manifest, publication_proof, !marker_present);
    if (!deleted.ok()) {
        return move_recovery_required(
            manifest,
            "source cleanup did not finish; hidden staged copy remains: " + deleted.detail_utf8,
            deleted.error, deleted.platform_code, deleted.status);
    }
    auto durable_removal = confirm_source_removal_namespace(manifest);
    if (!durable_removal.ok()) {
        return move_recovery_required(
            manifest, "source retirement removal durability could not be confirmed",
            durable_removal.error, durable_removal.platform_code, durable_removal.status);
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

BasicDirectoryTransferResult
publish_staged_root(const BasicDirectoryTransferManifest &manifest,
                    const BasicDirectoryTransferProgressCallback &progress,
                    const std::size_t completed_entries, const std::uint64_t completed_bytes,
                    ExecutionBoundaryPhase &boundary_phase) {
    BasicDirectoryTransferResult result;
    result.status = BasicDirectoryTransferStatus::io_error;
    result.completed_entries = completed_entries;
    result.completed_bytes = completed_bytes;
    result.staging_destination = manifest.staging_destination;
    result.destination = manifest.destination;
    result.manifest_path = basic_directory_transfer_manifest_path(manifest);
    report_progress(progress, BasicDirectoryTransferPhase::publishing, manifest, completed_entries,
                    completed_bytes, manifest.destination);
    auto published = rename_no_replace(manifest.staging_destination, manifest.destination,
                                       path_is_cifs(manifest.destination.parent_path()));
    if (!published.ok()) {
        result.status = published.status;
        if (manifest.kind == FileTransferKind::move &&
            published.status == BasicDirectoryTransferStatus::conflict) {
            result.status = BasicDirectoryTransferStatus::move_pending_publication;
            result.recovery_available = true;
            published.detail_utf8 =
                "source was retired, but final destination is occupied; hidden copy remains";
        } else if (published.status == BasicDirectoryTransferStatus::disconnected ||
                   published.status == BasicDirectoryTransferStatus::timed_out ||
                   published.status == BasicDirectoryTransferStatus::io_error) {
            result.status = BasicDirectoryTransferStatus::unknown_outcome;
            result.recovery_available = true;
            published.detail_utf8 = "final directory publication outcome is unknown";
        }
        result.error = published.error;
        result.platform_code = published.platform_code;
        result.detail_utf8 = std::move(published.detail_utf8);
        return result;
    }
    boundary_phase = ExecutionBoundaryPhase::published;
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::publication_namespace_flush)) {
        result.status = BasicDirectoryTransferStatus::unknown_outcome;
        result.recovery_available = true;
        result.detail_utf8 = "published directory namespace flush failed by test injection";
        return result;
    }
#endif
    auto durable_publication = sync_proof_namespace(manifest.destination, "published directory");
    if (!durable_publication.ok()) {
        result.status = BasicDirectoryTransferStatus::unknown_outcome;
        result.recovery_available = true;
        result.error = durable_publication.error;
        result.platform_code = durable_publication.platform_code;
        result.detail_utf8 =
            "final directory publication durability is unknown: " + durable_publication.detail_utf8;
        return result;
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(
            BasicDirectoryTransferTestFailurePoint::exception_after_publication)) {
        throw std::runtime_error("test exception after final publication");
    }
    if (loseFinalReplyOnce.exchange(false)) {
        result.status = BasicDirectoryTransferStatus::unknown_outcome;
        result.recovery_available = true;
        result.detail_utf8 = "final directory publication reply was lost by the test hook";
        return result;
    }
#endif
    result.status = BasicDirectoryTransferStatus::success;
    return result;
}

BasicDirectoryTransferResult
audit_published_tree_against_source(const BasicDirectoryTransferManifest &manifest) {
    const auto source_cifs = path_is_cifs(manifest.source);
    const auto destination_cifs = path_is_cifs(manifest.destination);
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    pause_network_phase_when_requested("reconciliation-audit");
#endif
    const auto source_capture = capture_tree(manifest.source);
    if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
        return failure(source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                           : source_capture.status,
                       source_capture.ok() ? "source tree changed before publication reconciliation"
                                           : source_capture.detail_utf8,
                       source_capture.error, source_capture.platform_code);
    }
    std::unordered_map<std::string, const BasicDirectoryTransferEntry *> expected;
    expected.reserve(manifest.entries.size());
    for (const auto &entry : manifest.entries) {
        expected.emplace(
            directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics), &entry);
    }
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(manifest.destination, error);
    if (error || !ordinary_directory(manifest.destination, root_status, error)) {
        return failure(error ? map_transfer_error(error, destination_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "published root is not an ordinary directory", error, error.value());
    }
    std::filesystem::recursive_directory_iterator iterator(
        manifest.destination, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return failure(map_transfer_error(error, destination_cifs),
                       "published tree could not be enumerated", error, error.value());
    }
    std::size_t seen{};
    while (iterator != end) {
        const auto relative =
            iterator->path().lexically_relative(manifest.destination).lexically_normal();
        const auto key = directory_transfer_namespace_key(relative, manifest.path_semantics);
        const auto found = expected.find(key);
        if (found == expected.end()) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "published tree contains an unplanned entry");
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            return failure(map_transfer_error(error, destination_cifs),
                           "published entry metadata could not be read", error, error.value());
        }
        const auto &entry = *found->second;
        const auto directory_ok = entry.kind == BasicDirectoryTransferEntryKind::directory
                                      ? ordinary_directory(iterator->path(), status, error)
                                      : false;
        if (error || (entry.kind == BasicDirectoryTransferEntryKind::directory && !directory_ok) ||
            (entry.kind == BasicDirectoryTransferEntryKind::regular_file &&
             (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))) {
            return failure(error ? map_transfer_error(error, destination_cifs)
                                 : BasicDirectoryTransferStatus::staging_changed,
                           "published entry kind differs from the manifest", error, error.value());
        }
        if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            FileProof source_proof;
            auto source_result =
                hash_file(manifest.source / entry.relative_path, entry.size_bytes, source_proof,
                          BasicDirectoryTransferStatus::source_changed, source_cifs);
            if (!source_result.ok()) {
                return source_result;
            }
            FileProof destination_proof;
            auto destination_result =
                hash_file(iterator->path(), entry.size_bytes, destination_proof,
                          BasicDirectoryTransferStatus::staging_changed, destination_cifs);
            if (!destination_result.ok()) {
                return destination_result;
            }
            if (source_proof.bytes != destination_proof.bytes ||
                source_proof.digest != destination_proof.digest) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "published file content differs from its source");
            }
        }
        ++seen;
        iterator.increment(error);
        if (error) {
            return failure(map_transfer_error(error, destination_cifs),
                           "published tree audit was interrupted", error, error.value());
        }
    }
    if (seen != manifest.entries.size()) {
        return failure(BasicDirectoryTransferStatus::staging_changed,
                       "published tree is missing a manifest entry");
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

bool is_basic_recovery_control_name(const std::filesystem::path &filename) {
    constexpr std::u8string_view suffix = u8".control";
    const auto name = filename.u8string();
    if (name.size() <= suffix.size()) {
        return false;
    }
    const auto prefix = std::u8string_view{name}.substr(0U, name.size() - suffix.size());
    const auto tail = std::u8string_view{name}.substr(name.size() - suffix.size());
#ifdef _WIN32
    return core::ascii_istarts_with(prefix, core::kTransferDestinationFilenamePrefix) &&
           core::ascii_iequal(tail, suffix);
#else
    return prefix.starts_with(core::kTransferDestinationFilenamePrefix) && tail == suffix;
#endif
}

std::filesystem::path
basic_directory_transfer_retirement_path(const BasicDirectoryTransferManifest &manifest) {
    auto path = manifest.staging_destination;
    path += ".retiring";
    return path;
}

BasicDirectoryTransferResult
audit_staging_for_restart(const BasicDirectoryTransferManifest &manifest) {
    const auto source_cifs = path_is_cifs(manifest.source);
    const auto destination_cifs = path_is_cifs(manifest.staging_destination);
    auto source_capture = capture_tree(manifest.source);
    if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
        return failure(source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                           : source_capture.status,
                       source_capture.ok() ? "source changed before directory restart"
                                           : source_capture.detail_utf8,
                       source_capture.error, source_capture.platform_code);
    }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    pause_network_phase_when_requested("restart-audit");
#endif
    std::unordered_map<std::string, std::pair<const BasicDirectoryTransferEntry *, std::size_t>>
        expected;
    std::unordered_map<std::string, std::pair<const BasicDirectoryTransferEntry *, std::size_t>>
        temporary;
    expected.reserve(manifest.entries.size());
    temporary.reserve(manifest.entries.size());
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &entry = manifest.entries[index];
        expected.emplace(
            directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics),
            std::pair{&entry, index});
        if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            const auto destination = manifest.staging_destination / entry.relative_path;
            const auto temp =
                file_transfer_temp_destination_path(destination, manifest.operation_id, index);
            temporary.emplace(
                directory_transfer_namespace_key(
                    temp.lexically_relative(manifest.staging_destination), manifest.path_semantics),
                std::pair{&entry, index});
        }
    }

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(manifest.staging_destination, error);
    if (error || !ordinary_directory(manifest.staging_destination, root_status, error)) {
        return failure(error ? map_transfer_error(error, destination_cifs)
                             : BasicDirectoryTransferStatus::staging_changed,
                       "restart staging root is not an ordinary directory", error, error.value());
    }
    std::unordered_set<std::size_t> completed_files;
    std::unordered_set<std::size_t> partial_files;
    std::filesystem::recursive_directory_iterator iterator(
        manifest.staging_destination, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return failure(map_transfer_error(error, destination_cifs),
                       "restart staging tree could not be enumerated", error, error.value());
    }
    std::size_t seen{};
    while (iterator != end) {
        if (++seen > kBasicDirectoryTransferMaximumEntries * 2U) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "restart staging tree exceeds its bounded entry count");
        }
        const auto relative =
            iterator->path().lexically_relative(manifest.staging_destination).lexically_normal();
        const auto key = directory_transfer_namespace_key(relative, manifest.path_semantics);
        const auto status = iterator->symlink_status(error);
        if (error) {
            return failure(map_transfer_error(error, destination_cifs),
                           "restart staging entry could not be inspected", error, error.value());
        }
        if (const auto found = expected.find(key); found != expected.end()) {
            const auto &[entry, index] = found->second;
            const auto directory_ok = entry->kind == BasicDirectoryTransferEntryKind::directory
                                          ? ordinary_directory(iterator->path(), status, error)
                                          : false;
            if (error ||
                (entry->kind == BasicDirectoryTransferEntryKind::directory && !directory_ok) ||
                (entry->kind == BasicDirectoryTransferEntryKind::regular_file &&
                 (!std::filesystem::is_regular_file(status) ||
                  std::filesystem::is_symlink(status)))) {
                return failure(error ? map_transfer_error(error, destination_cifs)
                                     : BasicDirectoryTransferStatus::staging_changed,
                               "restart staging entry kind differs from the manifest", error,
                               error.value());
            }
            if (entry->kind == BasicDirectoryTransferEntryKind::regular_file) {
                FileProof source_proof;
                auto source_result = hash_file(
                    manifest.source / entry->relative_path, entry->size_bytes, source_proof,
                    BasicDirectoryTransferStatus::source_changed, source_cifs);
                if (!source_result.ok()) {
                    return source_result;
                }
                FileProof staging_proof;
                auto staging_result =
                    hash_file(iterator->path(), entry->size_bytes, staging_proof,
                              BasicDirectoryTransferStatus::staging_changed, destination_cifs);
                if (!staging_result.ok()) {
                    return staging_result;
                }
                if (source_proof.bytes != staging_proof.bytes ||
                    source_proof.digest != staging_proof.digest) {
                    return failure(BasicDirectoryTransferStatus::staging_changed,
                                   "restart staging file differs from its retained source");
                }
                completed_files.insert(index);
            }
        } else if (const auto found = temporary.find(key); found != temporary.end()) {
            const auto &[entry, index] = found->second;
            if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
                return failure(BasicDirectoryTransferStatus::staging_changed,
                               "restart temporary leaf is not an ordinary file");
            }
            auto opened = platform::open_read_only_source(iterator->path(), entry->size_bytes);
            if (!opened.source) {
                auto status = BasicDirectoryTransferStatus::staging_changed;
#if defined(__linux__)
                if (destination_cifs && opened.error.platform_code == EIO) {
                    status = BasicDirectoryTransferStatus::disconnected;
                }
#endif
                return failure(status, "restart temporary leaf could not be validated", {},
                               opened.error.platform_code);
            }
            const auto shape = inspect_source_shape(iterator->path(), *opened.source);
            if (!shape.ok() || opened.source->size_bytes() > entry->size_bytes) {
                const auto status = shape.status == BasicDirectoryTransferStatus::io_error
                                        ? map_transfer_error(shape.error, destination_cifs)
                                        : BasicDirectoryTransferStatus::staging_changed;
                return failure(status, "restart temporary leaf has an unsupported shape",
                               shape.error, shape.platform_code);
            }
            const auto identity =
                identity_result_for_transfer(*opened.source, IdentityCheckRole::staging);
            if (identity.status != platform::SourceIdentityStatus::unchanged) {
                const auto status =
                    identity.status == platform::SourceIdentityStatus::changed
                        ? BasicDirectoryTransferStatus::staging_changed
                        : map_source_operation_error(identity.error_kind, identity.platform_code,
                                                     destination_cifs);
                return failure(status, "restart temporary leaf identity could not be revalidated",
                               source_platform_error(identity.platform_code),
                               identity.platform_code);
            }
            partial_files.insert(index);
        } else {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "restart staging tree contains an unplanned entry");
        }
        iterator.increment(error);
        if (error) {
            return failure(map_transfer_error(error, destination_cifs),
                           "restart staging audit was interrupted", error, error.value());
        }
    }
    for (const auto index : partial_files) {
        if (completed_files.contains(index)) {
            return failure(BasicDirectoryTransferStatus::staging_changed,
                           "restart staging contains both temporary and published leaf content");
        }
    }
    source_capture = capture_tree(manifest.source);
    if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
        return failure(source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                           : source_capture.status,
                       source_capture.ok() ? "source changed during directory restart audit"
                                           : source_capture.detail_utf8,
                       source_capture.error, source_capture.platform_code);
    }
    return failure(BasicDirectoryTransferStatus::success, {});
}

void report_progress(const BasicDirectoryTransferProgressCallback &callback,
                     const BasicDirectoryTransferPhase phase,
                     const BasicDirectoryTransferManifest &manifest,
                     const std::size_t completed_entries, const std::uint64_t completed_bytes,
                     const std::filesystem::path &current = {}) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback({.phase = phase,
                  .completed_entries = completed_entries,
                  .total_entries = manifest.entries.size(),
                  .completed_bytes = completed_bytes,
                  .total_bytes = manifest.total_bytes,
                  .current_path = current});
    } catch (...) {
        // Progress is observational. A UI callback must never alter the file operation outcome.
        static_cast<void>(std::current_exception());
    }
}

BasicDirectoryTransferResult execution_exception_result(
    const BasicDirectoryTransferManifest &manifest, const ExecutionBoundaryPhase phase,
    const BasicDirectoryTransferStatus fallback_status, const std::error_code error = {},
    const std::int64_t platform_code = 0) noexcept {
    BasicDirectoryTransferResult result;
    if (phase == ExecutionBoundaryPhase::completed) {
        result.status = BasicDirectoryTransferStatus::success;
    } else if (phase == ExecutionBoundaryPhase::published) {
        result.status = BasicDirectoryTransferStatus::unknown_outcome;
    } else if (phase != ExecutionBoundaryPhase::none) {
        result.status = BasicDirectoryTransferStatus::recovery_required;
    } else {
        result.status = fallback_status;
    }
    result.recovery_available = result.status == BasicDirectoryTransferStatus::recovery_required ||
                                result.status == BasicDirectoryTransferStatus::unknown_outcome;
    result.error = error;
    result.platform_code = platform_code;
    try {
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = basic_directory_transfer_manifest_path(manifest);
        if (result.status == BasicDirectoryTransferStatus::unknown_outcome) {
            result.detail_utf8 =
                "directory publication completed, but its final outcome requires reconciliation";
        } else if (result.status == BasicDirectoryTransferStatus::recovery_required) {
            result.detail_utf8 = "directory transfer stopped after creating recoverable state";
        } else {
            result.detail_utf8 = "directory transfer stopped before creating recoverable state";
        }
    } catch (...) {
        static_cast<void>(std::current_exception());
    }
    return result;
}

void advertise_execution_recovery(BasicDirectoryTransferResult &result,
                                  const ExecutionBoundaryPhase phase) noexcept {
    const auto manifest_authority_exists = phase == ExecutionBoundaryPhase::manifest_persisted ||
                                           phase == ExecutionBoundaryPhase::staging_created ||
                                           phase == ExecutionBoundaryPhase::published ||
                                           phase == ExecutionBoundaryPhase::source_retired;
    if (!result.ok() && manifest_authority_exists) {
        result.recovery_available = true;
    }
}

BasicDirectoryTransferResult
reconciliation_exception_result(const BasicDirectoryTransferManifest &manifest,
                                const std::error_code error = {},
                                const std::int64_t platform_code = 0) noexcept {
    BasicDirectoryTransferResult result;
    result.status = BasicDirectoryTransferStatus::unknown_outcome;
    result.recovery_available = true;
    result.error = error;
    result.platform_code = platform_code;
    try {
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = basic_directory_transfer_manifest_path(manifest);
        result.detail_utf8 = "directory publication could not be fully reconciled";
    } catch (...) {
        static_cast<void>(std::current_exception());
    }
    return result;
}

} // namespace

std::filesystem::path
basic_directory_transfer_manifest_path(const BasicDirectoryTransferManifest &manifest) {
    auto path = manifest.staging_destination;
    path += ".control";
    return path / "manifest";
}

bool valid_basic_directory_transfer_manifest(const BasicDirectoryTransferManifest &manifest,
                                             std::string &detail_utf8) {
    detail_utf8.clear();
    const auto supported_version =
        manifest.version == kBasicDirectoryTransferManifestVersion ||
        (manifest.version == 1U && manifest.kind == FileTransferKind::copy);
    if (!supported_version || manifest.operation_id == 0U ||
        (manifest.kind != FileTransferKind::copy && manifest.kind != FileTransferKind::move) ||
        manifest.path_semantics != native_path_semantics() ||
        bytes_empty(manifest.ownership_token) || manifest.source_revision_utf8.empty()) {
        detail_utf8 = "basic directory manifest identity is invalid";
        return false;
    }
    if (!canonical_absolute_path(manifest.source) || contains_internal_name(manifest.source)) {
        detail_utf8 = "basic directory manifest source root path is invalid";
        return false;
    }
    if (!canonical_absolute_path(manifest.staging_destination)) {
        detail_utf8 = "basic directory manifest staging root path is invalid";
        return false;
    }
    if (!canonical_absolute_path(manifest.destination) ||
        contains_internal_name(manifest.destination)) {
        detail_utf8 = "basic directory manifest destination root path is invalid";
        return false;
    }
    const auto expected_staging = basic_staging_destination_path(
        manifest.destination, manifest.operation_id, manifest.ownership_token);
    const auto expected_staging_key =
        directory_transfer_namespace_key(expected_staging, manifest.path_semantics);
    const auto actual_staging_key =
        directory_transfer_namespace_key(manifest.staging_destination, manifest.path_semantics);
    const auto staging_parent_key = directory_transfer_namespace_key(
        manifest.staging_destination.parent_path(), manifest.path_semantics);
    const auto destination_parent_key = directory_transfer_namespace_key(
        manifest.destination.parent_path(), manifest.path_semantics);
    if (expected_staging_key.empty() || actual_staging_key != expected_staging_key) {
        detail_utf8 = "basic directory manifest staging leaf is invalid";
        return false;
    }
    if (staging_parent_key.empty() || staging_parent_key != destination_parent_key) {
        detail_utf8 = "basic directory manifest staging parent is invalid";
        return false;
    }
    std::string filename_detail;
    if (!valid_destination_filename(manifest.destination.filename(), filename_detail)) {
        detail_utf8 = "basic directory destination is invalid: " + filename_detail;
        return false;
    }
    const auto source_key =
        directory_transfer_namespace_key(manifest.source, manifest.path_semantics);
    const auto destination_key =
        directory_transfer_namespace_key(manifest.destination, manifest.path_semantics);
    const auto staging_key =
        directory_transfer_namespace_key(manifest.staging_destination, manifest.path_semantics);
    if (source_key.empty() || destination_key.empty() || staging_key.empty() ||
        namespace_ancestor_or_same(source_key, destination_key) ||
        namespace_ancestor_or_same(destination_key, source_key) ||
        namespace_ancestor_or_same(source_key, staging_key) ||
        namespace_ancestor_or_same(staging_key, source_key)) {
        detail_utf8 = "basic directory source and destination overlap";
        return false;
    }
    if (manifest.entries.size() > kBasicDirectoryTransferMaximumEntries) {
        detail_utf8 = "basic directory manifest has too many entries";
        return false;
    }
    std::unordered_set<std::string> paths;
    std::uint64_t total_bytes{};
    std::string previous_key;
    for (const auto &entry : manifest.entries) {
        const auto depth = path_depth(entry.relative_path);
        const auto key =
            directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics);
        if (!canonical_relative_path(entry.relative_path) ||
            contains_internal_name(entry.relative_path) || depth == 0U ||
            depth > kBasicDirectoryTransferMaximumDepth || entry.depth != depth || key.empty() ||
            entry.source_revision_utf8.empty() || (!previous_key.empty() && key <= previous_key) ||
            !paths.emplace(key).second) {
            detail_utf8 = "basic directory manifest entry path is invalid";
            return false;
        }
        if (entry.kind == BasicDirectoryTransferEntryKind::directory) {
            if (entry.size_bytes != 0U) {
                detail_utf8 = "basic directory manifest directory has a file size";
                return false;
            }
        } else if (entry.kind == BasicDirectoryTransferEntryKind::regular_file) {
            if (entry.size_bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes) {
                detail_utf8 = "basic directory manifest size overflows";
                return false;
            }
            total_bytes += entry.size_bytes;
        } else {
            detail_utf8 = "basic directory manifest entry kind is invalid";
            return false;
        }
        if (entry.relative_path.has_parent_path()) {
            const auto parent = entry.relative_path.parent_path();
            if (!parent.empty()) {
                const auto parent_key =
                    directory_transfer_namespace_key(parent, manifest.path_semantics);
                if (!paths.contains(parent_key)) {
                    detail_utf8 = "basic directory manifest entry has no parent directory";
                    return false;
                }
            }
        }
        previous_key = key;
    }
    if (total_bytes != manifest.total_bytes) {
        detail_utf8 = "basic directory manifest byte total is invalid";
        return false;
    }
    return true;
}

std::vector<std::byte>
encode_basic_directory_transfer_manifest(const BasicDirectoryTransferManifest &manifest) {
    std::string detail;
    if (!valid_basic_directory_transfer_manifest(manifest, detail)) {
        throw std::invalid_argument(detail);
    }
    std::vector<std::byte> payload;
    payload.reserve(256U + manifest.entries.size() * 96U);
    append_integer(payload, manifestMagic);
    append_integer(payload, manifest.version);
    append_integer(payload, manifest.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(manifest.path_semantics));
    append_integer(payload, static_cast<std::uint8_t>(manifest.kind));
    append_integer(payload, static_cast<std::uint16_t>(0U));
    append_integer(payload, manifest.total_bytes);
    append_integer(payload, static_cast<std::uint32_t>(manifest.entries.size()));
    append_bytes(payload, manifest.ownership_token);
    append_string(payload, path_utf8(manifest.source));
    append_string(payload, path_utf8(manifest.staging_destination));
    append_string(payload, path_utf8(manifest.destination));
    append_string(payload, manifest.source_revision_utf8);
    for (const auto &entry : manifest.entries) {
        append_integer(payload, static_cast<std::uint8_t>(entry.kind));
        append_integer(payload, static_cast<std::uint8_t>(0U));
        append_integer(payload, entry.depth);
        append_integer(payload, entry.size_bytes);
        append_integer(payload, entry.modified_unix_ns);
        append_string(payload, path_utf8(entry.relative_path));
        append_string(payload, entry.source_revision_utf8);
    }
    const auto digest = detail::sha256(payload);
    append_bytes(payload, digest);
    if (payload.size() > kBasicDirectoryTransferMaximumManifestBytes) {
        throw std::length_error("basic directory manifest is too large");
    }
    return payload;
}

bool decode_basic_directory_transfer_manifest(const std::span<const std::byte> payload,
                                              BasicDirectoryTransferManifest &manifest,
                                              std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() < kBasicDirectoryTransferDigestBytes ||
        payload.size() > kBasicDirectoryTransferMaximumManifestBytes) {
        detail_utf8 = "basic directory manifest size is invalid";
        return false;
    }
    const auto body = payload.first(payload.size() - kBasicDirectoryTransferDigestBytes);
    const auto digest = detail::sha256(body);
    for (std::size_t index{}; index < digest.size(); ++index) {
        if (static_cast<std::uint8_t>(payload[body.size() + index]) != digest[index]) {
            detail_utf8 = "basic directory manifest digest is invalid";
            return false;
        }
    }
    Cursor cursor(body);
    BasicDirectoryTransferManifest decoded;
    std::uint32_t magic{};
    std::uint8_t semantics{};
    std::uint8_t kind{};
    std::uint16_t reserved16{};
    std::uint32_t count{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(semantics) || !cursor.read(kind) ||
        !cursor.read(reserved16) || !cursor.read(decoded.total_bytes) || !cursor.read(count) ||
        !read_bytes(cursor, decoded.ownership_token) || magic != manifestMagic ||
        (decoded.version != 1U && decoded.version != kBasicDirectoryTransferManifestVersion) ||
        (decoded.version == 1U && kind != static_cast<std::uint8_t>(FileTransferKind::copy)) ||
        kind > static_cast<std::uint8_t>(FileTransferKind::move) || reserved16 != 0U ||
        count > kBasicDirectoryTransferMaximumEntries) {
        detail_utf8 = "basic directory manifest header is invalid";
        return false;
    }
    decoded.kind = static_cast<FileTransferKind>(kind);
    decoded.path_semantics = static_cast<DirectoryPathSemantics>(semantics);
    std::array<std::string, 4> root_strings;
    for (auto &value : root_strings) {
        if (!cursor.read_string(value) || !valid_utf8(value)) {
            detail_utf8 = "basic directory manifest root string is invalid";
            return false;
        }
    }
    decoded.source = path_from_utf8(root_strings[0]);
    decoded.staging_destination = path_from_utf8(root_strings[1]);
    decoded.destination = path_from_utf8(root_strings[2]);
    decoded.source_revision_utf8 = std::move(root_strings[3]);
    decoded.entries.reserve(count);
    for (std::uint32_t index{}; index < count; ++index) {
        BasicDirectoryTransferEntry entry;
        std::uint8_t kind{};
        std::uint8_t reserved{};
        std::string relative;
        if (!cursor.read(kind) || !cursor.read(reserved) || !cursor.read(entry.depth) ||
            !cursor.read(entry.size_bytes) || !cursor.read(entry.modified_unix_ns) ||
            !cursor.read_string(relative) || !cursor.read_string(entry.source_revision_utf8) ||
            reserved != 0U || !valid_utf8(relative) || !valid_utf8(entry.source_revision_utf8)) {
            detail_utf8 = "basic directory manifest entry is invalid";
            return false;
        }
        entry.kind = static_cast<BasicDirectoryTransferEntryKind>(kind);
        entry.relative_path = path_from_utf8(relative);
        decoded.entries.emplace_back(std::move(entry));
    }
    if (cursor.remaining() != 0U ||
        !valid_basic_directory_transfer_manifest(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "basic directory manifest has trailing data";
        }
        return false;
    }
    manifest = std::move(decoded);
    return true;
}

namespace {

constexpr std::size_t recoveryManifestHeaderBytes = 32U;

std::optional<std::size_t>
recovery_manifest_declared_entries(const std::span<const std::byte> payload) {
    if (payload.size() < recoveryManifestHeaderBytes) {
        return std::nullopt;
    }
    const auto read_u32 = [&](const std::size_t offset) {
        std::uint32_t value{};
        for (std::size_t byte{}; byte < sizeof(value); ++byte) {
            value |=
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[offset + byte]))
                << (byte * 8U);
        }
        return value;
    };
    const auto version = read_u32(4U);
    const auto kind = std::to_integer<std::uint8_t>(payload[17U]);
    if (read_u32(0U) != manifestMagic ||
        (version != 1U && version != kBasicDirectoryTransferManifestVersion) ||
        (version == 1U && kind != static_cast<std::uint8_t>(FileTransferKind::copy)) ||
        kind > static_cast<std::uint8_t>(FileTransferKind::move) ||
        std::to_integer<std::uint8_t>(payload[18U]) != 0U ||
        std::to_integer<std::uint8_t>(payload[19U]) != 0U) {
        return std::nullopt;
    }
    const auto count = read_u32(28U);
    if (count > kBasicDirectoryTransferMaximumEntries) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(count);
}

struct RecoveryManifestLoadBudget {
    std::size_t maximum_bytes{};
    std::size_t maximum_entries{};
    bool defer_over_budget{};
    bool known_cifs{};
};

struct RecoveryManifestLoadAccounting {
    std::size_t loaded_bytes{};
    std::size_t loaded_entries{};
};

BasicDirectoryTransferPlanResult
load_basic_directory_transfer_manifest_bounded(const std::filesystem::path &manifest_path,
                                               const RecoveryManifestLoadBudget budget,
                                               RecoveryManifestLoadAccounting &accounting) {
    BasicDirectoryTransferPlanResult result;
    accounting = {};
    std::error_code error;
    const auto absolute = normalized_absolute_path(manifest_path, error);
    result.manifest_path = absolute;
    if (error) {
        result.status = map_transfer_error(error, budget.known_cifs);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "recovery manifest path could not be made absolute";
        return result;
    }
    const auto status = std::filesystem::symlink_status(absolute, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        result.status = error ? map_transfer_error(error, budget.known_cifs)
                        : status.type() == std::filesystem::file_type::not_found
                            ? BasicDirectoryTransferStatus::not_found
                            : BasicDirectoryTransferStatus::staging_changed;
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "recovery manifest is not an ordinary file";
        return result;
    }
    const auto size = std::filesystem::file_size(absolute, error);
    if (error) {
        result.status = map_transfer_error(error, budget.known_cifs);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "recovery manifest size could not be read";
        return result;
    }
    if (size > budget.maximum_bytes || size > std::numeric_limits<std::size_t>::max()) {
        result.status = budget.defer_over_budget ? BasicDirectoryTransferStatus::recovery_required
                                                 : BasicDirectoryTransferStatus::staging_changed;
        result.detail_utf8 =
            budget.defer_over_budget
                ? "recovery candidate was deferred by the bounded scan; inspect it explicitly"
                : "recovery manifest exceeds its byte budget";
        return result;
    }
    try {
#ifdef _WIN32
        SetLastError(ERROR_SUCCESS);
#else
        errno = 0;
#endif
        std::ifstream input(absolute, std::ios::binary);
        if (!input) {
            const std::error_code open_error{
#ifdef _WIN32
                static_cast<int>(GetLastError()), std::system_category()
#else
                errno, std::generic_category()
#endif
            };
            result.status = open_error ? map_transfer_error(open_error, budget.known_cifs)
                                       : BasicDirectoryTransferStatus::io_error;
            result.error = open_error;
            result.platform_code = open_error.value();
            result.detail_utf8 = "recovery manifest could not be opened";
            return result;
        }
        std::vector<std::byte> payload(static_cast<std::size_t>(size));
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_network_phase_when_requested("manifest");
#endif
#ifdef _WIN32
        SetLastError(ERROR_SUCCESS);
#else
        errno = 0;
#endif
        input.read(reinterpret_cast<char *>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));
        if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
            const std::error_code read_error{
#ifdef _WIN32
                static_cast<int>(GetLastError()), std::system_category()
#else
                errno, std::generic_category()
#endif
            };
            result.status = read_error ? map_transfer_error(read_error, budget.known_cifs)
                                       : BasicDirectoryTransferStatus::staging_changed;
            result.error = read_error;
            result.platform_code = read_error.value();
            result.detail_utf8 = "recovery manifest changed while it was read";
            return result;
        }
        if (input.peek() != std::ifstream::traits_type::eof()) {
            result.status = BasicDirectoryTransferStatus::staging_changed;
            result.detail_utf8 = "recovery manifest changed while it was read";
            return result;
        }
        accounting.loaded_bytes = payload.size();
        const auto declared_entries = recovery_manifest_declared_entries(payload);
        if (declared_entries && *declared_entries > budget.maximum_entries) {
            result.status = budget.defer_over_budget
                                ? BasicDirectoryTransferStatus::recovery_required
                                : BasicDirectoryTransferStatus::staging_changed;
            result.detail_utf8 =
                budget.defer_over_budget
                    ? "recovery candidate was deferred by the bounded scan; inspect it explicitly"
                    : "recovery manifest exceeds its entry budget";
            return result;
        }
        if (!decode_basic_directory_transfer_manifest(payload, result.manifest,
                                                      result.detail_utf8)) {
            result.status = BasicDirectoryTransferStatus::staging_changed;
            return result;
        }
    } catch (const std::bad_alloc &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "recovery manifest exceeds the available memory";
        return result;
    } catch (const std::length_error &) {
        result.status = BasicDirectoryTransferStatus::staging_changed;
        result.detail_utf8 = "recovery manifest length is invalid";
        return result;
    }
    const auto expected_key = directory_transfer_namespace_key(
        basic_directory_transfer_manifest_path(result.manifest), result.manifest.path_semantics);
    const auto actual_key =
        directory_transfer_namespace_key(absolute, result.manifest.path_semantics);
    if (expected_key.empty() || actual_key.empty() || expected_key != actual_key) {
        result.status = BasicDirectoryTransferStatus::staging_changed;
        result.detail_utf8 = "recovery manifest is outside its bound control directory";
        result.manifest = {};
        return result;
    }
    result.manifest_path = basic_directory_transfer_manifest_path(result.manifest);
    result.status = BasicDirectoryTransferStatus::success;
    result.recovery_available = true;
    accounting.loaded_entries = result.manifest.entries.size();
    result.staging_destination = result.manifest.staging_destination;
    result.destination = result.manifest.destination;
    return result;
}

BasicDirectoryTransferPlanResult
load_basic_directory_transfer_manifest_with_context(const std::filesystem::path &manifest_path,
                                                    const bool known_cifs) {
    RecoveryManifestLoadAccounting accounting;
    return load_basic_directory_transfer_manifest_bounded(
        manifest_path,
        {.maximum_bytes = kBasicDirectoryTransferMaximumManifestBytes,
         .maximum_entries = kBasicDirectoryTransferMaximumEntries,
         .known_cifs = known_cifs},
        accounting);
}

} // namespace

BasicDirectoryTransferPlanResult
load_basic_directory_transfer_manifest(const std::filesystem::path &manifest_path) {
    return load_basic_directory_transfer_manifest_with_context(
        manifest_path, path_is_cifs(manifest_path.parent_path()));
}

BasicDirectoryTransferPlanResult
plan_basic_directory_transfer_impl(const BasicDirectoryTransferRequest &request,
                                   const FileTransferKind kind) {
    BasicDirectoryTransferPlanResult result;
    if (request.source.empty() || request.destination.empty()) {
        result.status = BasicDirectoryTransferStatus::invalid_request;
        result.detail_utf8 = "source and destination paths are required";
        return result;
    }
    std::error_code error;
    const auto source = std::filesystem::absolute(request.source, error).lexically_normal();
    if (error) {
        result.status = map_error_status(error);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "source path could not be made absolute";
        return result;
    }
    const auto source_cifs = path_is_cifs(source);
    const auto destination =
        std::filesystem::absolute(request.destination, error).lexically_normal();
    if (error) {
        result.status = map_error_status(error);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "destination path could not be made absolute";
        return result;
    }
    const auto destination_cifs = path_is_cifs(destination.parent_path());
    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error || !ordinary_directory(source, source_status, error)) {
        result.status = error ? map_transfer_error(error, source_cifs)
                              : BasicDirectoryTransferStatus::invalid_request;
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "source must be an ordinary directory";
        return result;
    }
    const auto parent_status = std::filesystem::status(destination.parent_path(), error);
    if (error || !ordinary_directory(destination.parent_path(), parent_status, error)) {
        result.status = error ? map_transfer_error(error, destination_cifs)
                              : BasicDirectoryTransferStatus::invalid_request;
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "destination parent must be an existing directory";
        return result;
    }
    if (std::filesystem::exists(destination, error)) {
        result.status = BasicDirectoryTransferStatus::conflict;
        result.destination = destination;
        result.detail_utf8 = "destination already exists";
        return result;
    }
    if (error) {
        result.status = map_transfer_error(error, destination_cifs);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "destination state could not be read";
        return result;
    }
    DirectoryTransferOwnershipToken token{};
    if (!generate_directory_transfer_ownership_token(token, error)) {
        result.status = BasicDirectoryTransferStatus::io_error;
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "directory transfer token could not be generated";
        return result;
    }
    const auto operation_id =
        request.operation_id == 0U ? operation_id_from_token(token) : request.operation_id;
    TreeCapture captured;
    try {
        captured = capture_tree(source);
    } catch (const std::bad_alloc &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "source tree exceeds the available planning memory";
        return result;
    } catch (const std::length_error &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "source tree exceeds the basic manifest byte budget";
        return result;
    } catch (const std::filesystem::filesystem_error &exception) {
        result.status = map_transfer_error(exception.code(), source_cifs);
        result.error = exception.code();
        result.platform_code = exception.code().value();
        result.detail_utf8 = "source tree could not be planned";
        return result;
    }
    if (!captured.ok()) {
        result.status = captured.status;
        result.error = captured.error;
        result.platform_code = captured.platform_code;
        result.detail_utf8 = std::move(captured.detail_utf8);
        return result;
    }
    result.manifest = {
        .version = kBasicDirectoryTransferManifestVersion,
        .operation_id = operation_id,
        .kind = kind,
        .path_semantics = native_path_semantics(),
        .source = source,
        .staging_destination = basic_staging_destination_path(destination, operation_id, token),
        .destination = destination,
        .source_revision_utf8 = std::move(captured.root_revision_utf8),
        .ownership_token = token,
        .total_bytes = captured.total_bytes,
        .entries = std::move(captured.entries),
    };
    bool manifest_within_budget{};
    try {
        std::size_t serialized_size = serializedManifestFixedBytes;
        manifest_within_budget =
            add_bounded_size(serialized_size, captured.serialized_entries_bytes,
                             kBasicDirectoryTransferMaximumManifestBytes) &&
            add_bounded_size(serialized_size, path_utf8(result.manifest.source).size(),
                             kBasicDirectoryTransferMaximumManifestBytes) &&
            add_bounded_size(serialized_size, path_utf8(result.manifest.staging_destination).size(),
                             kBasicDirectoryTransferMaximumManifestBytes) &&
            add_bounded_size(serialized_size, path_utf8(result.manifest.destination).size(),
                             kBasicDirectoryTransferMaximumManifestBytes) &&
            add_bounded_size(serialized_size, result.manifest.source_revision_utf8.size(),
                             kBasicDirectoryTransferMaximumManifestBytes);
    } catch (const std::bad_alloc &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "source tree exceeds the available planning memory";
        return result;
    } catch (const std::length_error &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "source tree exceeds the basic manifest byte budget";
        return result;
    }
    if (!manifest_within_budget) {
        result.status = BasicDirectoryTransferStatus::unsupported;
        result.detail_utf8 = "source tree exceeds the basic manifest byte budget";
        return result;
    }
    if (!valid_basic_directory_transfer_manifest(result.manifest, result.detail_utf8)) {
        result.status = BasicDirectoryTransferStatus::invalid_request;
        return result;
    }
    result.status = BasicDirectoryTransferStatus::success;
    result.staging_destination = result.manifest.staging_destination;
    result.destination = result.manifest.destination;
    result.manifest_path = basic_directory_transfer_manifest_path(result.manifest);
    return result;
}

BasicDirectoryTransferPlanResult
plan_basic_directory_copy(const BasicDirectoryTransferRequest &request) {
    return plan_basic_directory_transfer_impl(request, FileTransferKind::copy);
}

BasicDirectoryTransferPlanResult
plan_basic_directory_move(const BasicDirectoryTransferRequest &request) {
    return plan_basic_directory_transfer_impl(request, FileTransferKind::move);
}

BasicDirectoryTransferResult execute_basic_directory_copy_impl(
    const BasicDirectoryTransferManifest &manifest,
    const BasicDirectoryTransferProgressCallback &progress, ExecutionBoundaryPhase &boundary_phase,
    const bool resume_from_persisted_manifest = false,
    RecoveryManifestLease *inherited_lease = nullptr, const bool allow_atomic_root_move = false) {
    std::string detail;
    if (!valid_basic_directory_transfer_manifest(manifest, detail)) {
        return failure(BasicDirectoryTransferStatus::invalid_request, std::move(detail));
    }
    BasicDirectoryTransferResult result;
    result.status = BasicDirectoryTransferStatus::io_error;
    result.staging_destination = manifest.staging_destination;
    result.destination = manifest.destination;
    result.manifest_path = basic_directory_transfer_manifest_path(manifest);
    std::error_code error;
    const auto source_cifs = path_is_cifs(manifest.source);
    const auto destination_cifs = path_is_cifs(manifest.destination.parent_path());
    RecoveryManifestLease execution_lease;
    auto *active_lease = inherited_lease;
    if (std::filesystem::exists(manifest.destination, error)) {
        result.status = BasicDirectoryTransferStatus::conflict;
        result.detail_utf8 = "destination already exists";
        return result;
    }
    if (error) {
        result.status = map_transfer_error(error, destination_cifs);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "destination state could not be read";
        return result;
    }
    const auto control_directory = result.manifest_path.parent_path();
    if (resume_from_persisted_manifest) {
        if (active_lease == nullptr) {
            result.status = BasicDirectoryTransferStatus::invalid_request;
            result.detail_utf8 = "resumed execution requires an acquired recovery lease";
            return result;
        }
        boundary_phase = ExecutionBoundaryPhase::manifest_persisted;
    } else {
        std::vector<std::byte> encoded;
        try {
            encoded = encode_basic_directory_transfer_manifest(manifest);
        } catch (const std::exception &exception) {
            result.status = BasicDirectoryTransferStatus::invalid_request;
            result.detail_utf8 = exception.what();
            return result;
        }
        if (!std::filesystem::create_directory(control_directory, error)) {
            result.status = error ? map_transfer_error(error, destination_cifs)
                                  : BasicDirectoryTransferStatus::conflict;
            result.error = error;
            result.platform_code = error.value();
            result.detail_utf8 = error ? "directory transfer control directory could not be created"
                                       : "directory transfer control directory already exists";
            return result;
        }
        boundary_phase = ExecutionBoundaryPhase::control_created;
        bool lease_busy{};
        if (!execution_lease.acquire(control_directory / "lease", error, lease_busy)) {
            result.status = lease_busy ? BasicDirectoryTransferStatus::recovery_required
                                       : (error == std::errc::operation_not_supported
                                              ? BasicDirectoryTransferStatus::unsupported
                                              : map_transfer_error(error, destination_cifs));
            result.error = error;
            result.platform_code = error.value();
            result.detail_utf8 = lease_busy ? "directory transfer control is already leased"
                                            : "directory transfer lease could not be acquired";
            std::error_code cleanup_error;
            std::filesystem::remove(control_directory, cleanup_error);
            return result;
        }
        active_lease = &execution_lease;
        const auto persisted =
            persist_manifest_once(result.manifest_path, encoded, destination_cifs);
        if (!persisted.ok()) {
            result.status = persisted.status;
            result.error = persisted.error;
            result.platform_code = persisted.platform_code;
            result.detail_utf8 = persisted.detail_utf8;
            std::error_code cleanup_error;
            if (!retire_manifest_before_lease_release(*active_lease, result.manifest_path,
                                                      cleanup_error)) {
                result.detail_utf8 += "; hidden recovery control directory remains";
            }
            return result;
        }
        boundary_phase = ExecutionBoundaryPhase::manifest_persisted;
    }
    if (allow_atomic_root_move && manifest.kind == FileTransferKind::move) {
        bool same_volume{};
        const auto volume_probe = probe_atomic_move_volume(manifest, same_volume);
        if (!volume_probe.ok() &&
            volume_probe.status != BasicDirectoryTransferStatus::unsupported) {
            auto blocked = volume_probe;
            blocked.recovery_available = true;
            blocked.staging_destination = manifest.staging_destination;
            blocked.destination = manifest.destination;
            blocked.manifest_path = result.manifest_path;
            return blocked;
        }
        if (volume_probe.ok() && same_volume) {
            auto capability = probe_source_cleanup_capability(manifest);
            if (!capability.ok() &&
                !(source_cifs && capability.status == BasicDirectoryTransferStatus::unsupported)) {
                capability.recovery_available = true;
                capability.staging_destination = manifest.staging_destination;
                capability.destination = manifest.destination;
                capability.manifest_path = result.manifest_path;
                return capability;
            }
            const auto source_capture = capture_tree(manifest.source);
            if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
                result.status = source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                    : source_capture.status;
                result.error = source_capture.error;
                result.platform_code = source_capture.platform_code;
                result.detail_utf8 = source_capture.ok() ? "source changed before atomic root move"
                                                         : source_capture.detail_utf8;
                std::error_code cleanup_error;
                if (!retire_manifest_before_lease_release(*active_lease, result.manifest_path,
                                                          cleanup_error)) {
                    result.recovery_available = true;
                    result.detail_utf8 += "; hidden recovery manifest remains";
                } else {
                    boundary_phase = ExecutionBoundaryPhase::none;
                }
                return result;
            }
            const auto expected_identity = atomic_move_expected_identity_digest(manifest);
            if (!expected_identity) {
                return execute_basic_directory_copy_impl(manifest, progress, boundary_phase, true,
                                                         active_lease, false);
            }
            auto marker = persist_atomic_move_evidence_once(
                atomic_move_marker_path(manifest.source), manifest, "atomic move ownership marker");
            if (!marker.ok()) {
                marker.recovery_available = true;
                marker.staging_destination = manifest.staging_destination;
                marker.destination = manifest.destination;
                marker.manifest_path = result.manifest_path;
                marker.detail_utf8 = "atomic move marker could not be committed";
                return marker;
            }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
            if (consume_failure_point(
                    BasicDirectoryTransferTestFailurePoint::exception_after_atomic_move_marker)) {
                throw std::runtime_error("test exception after atomic move marker");
            }
#endif
            report_progress(progress, BasicDirectoryTransferPhase::publishing, manifest, 0U, 0U,
                            manifest.destination);
            RenameNoReplaceResult moved;
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
            if (consume_failure_point(
                    BasicDirectoryTransferTestFailurePoint::atomic_move_cross_device) ||
                (std::getenv("VOVE_TEST_TRANSFER_DIRECTORY_CROSS_DEVICE") != nullptr &&
                 std::string_view(std::getenv("VOVE_TEST_TRANSFER_DIRECTORY_CROSS_DEVICE")) ==
                     "1")) {
#ifdef _WIN32
                moved = {.status = BasicDirectoryTransferStatus::unsupported,
                         .error = {ERROR_NOT_SAME_DEVICE, std::system_category()},
                         .platform_code = ERROR_NOT_SAME_DEVICE,
                         .detail_utf8 = "atomic move crossed volumes by test injection"};
#else
                moved = {.status = BasicDirectoryTransferStatus::unsupported,
                         .error = {EXDEV, std::generic_category()},
                         .platform_code = EXDEV,
                         .detail_utf8 = "atomic move crossed volumes by test injection"};
#endif
            } else
#endif
            {
                moved = rename_no_replace(manifest.source, manifest.destination,
                                          source_cifs || destination_cifs);
            }
            if (moved.ok()) {
                boundary_phase = ExecutionBoundaryPhase::published;
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
                if (consume_failure_point(
                        BasicDirectoryTransferTestFailurePoint::exception_after_publication)) {
                    throw std::runtime_error("test exception after atomic root move");
                }
                if (loseFinalReplyOnce.exchange(false)) {
                    result.status = BasicDirectoryTransferStatus::unknown_outcome;
                    result.recovery_available = true;
                    result.completed_entries = manifest.entries.size();
                    result.completed_bytes = manifest.total_bytes;
                    result.detail_utf8 = "atomic root move reply was lost by the test hook";
                    return result;
                }
#endif
                auto marker_check =
                    load_atomic_move_evidence(atomic_move_marker_path(manifest.destination),
                                              manifest, "published atomic move ownership marker");
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
                pause_after_atomic_move_marker_check_when_requested();
#endif
                if (!marker_check.ok()) {
                    const auto invalid_evidence =
                        marker_check.status == BasicDirectoryTransferStatus::not_found ||
                        marker_check.status == BasicDirectoryTransferStatus::staging_changed ||
                        marker_check.status == BasicDirectoryTransferStatus::source_changed;
                    auto blocked = invalid_evidence
                                       ? preserve_ambiguous_atomic_move_destination(
                                             "published root lacks its bound ownership marker")
                                       : marker_check;
                    if (!invalid_evidence) {
                        blocked.status = BasicDirectoryTransferStatus::unknown_outcome;
                        blocked.recovery_available = true;
                    }
                    blocked.destination = manifest.destination;
                    blocked.manifest_path = result.manifest_path;
                    return blocked;
                }
                const auto published_capture =
                    capture_tree_with_ownership_marker(manifest.destination);
                std::string published_shape_detail;
                if (!same_atomically_renamed_shape(manifest, published_capture,
                                                   published_shape_detail)) {
                    const auto operational_failure =
                        !published_capture.ok() &&
                        published_capture.status != BasicDirectoryTransferStatus::source_changed &&
                        published_capture.status != BasicDirectoryTransferStatus::staging_changed &&
                        published_capture.status != BasicDirectoryTransferStatus::unsupported;
                    auto blocked =
                        operational_failure
                            ? failure(BasicDirectoryTransferStatus::unknown_outcome,
                                      published_capture.detail_utf8, published_capture.error,
                                      published_capture.platform_code)
                            : preserve_ambiguous_atomic_move_destination(
                                  published_shape_detail.empty()
                                      ? "published root changed during atomic move"
                                      : published_shape_detail);
                    blocked.recovery_available = true;
                    blocked.destination = manifest.destination;
                    blocked.manifest_path = result.manifest_path;
                    return blocked;
                }
                const auto published_identity = atomic_move_destination_identity_digest(
                    published_capture, manifest.path_semantics);
                if (!published_identity || *published_identity != *expected_identity) {
                    auto blocked = preserve_ambiguous_atomic_move_destination(
                        published_identity
                            ? "published root is not the physical source tree"
                            : "published root has no stable physical identity proof");
                    blocked.destination = manifest.destination;
                    blocked.manifest_path = result.manifest_path;
                    return blocked;
                }
                auto durable = confirm_atomic_move_namespace(manifest);
                if (!durable.ok()) {
                    durable.status = BasicDirectoryTransferStatus::unknown_outcome;
                    durable.recovery_available = true;
                    durable.completed_entries = manifest.entries.size();
                    durable.completed_bytes = manifest.total_bytes;
                    durable.destination = manifest.destination;
                    durable.manifest_path = result.manifest_path;
                    durable.detail_utf8 =
                        "atomic root move completed, but namespace durability is unknown";
                    return durable;
                }
                auto committed =
                    persist_atomic_move_committed_proof_once(manifest, *expected_identity);
                if (!committed.ok()) {
                    committed.recovery_available = true;
                    committed.completed_entries = manifest.entries.size();
                    committed.completed_bytes = manifest.total_bytes;
                    committed.destination = manifest.destination;
                    committed.manifest_path = result.manifest_path;
                    committed.detail_utf8 =
                        "atomic root move completed, but its commit proof is not durable";
                    return committed;
                }
                auto marker_removed = remove_atomic_move_marker(manifest.destination, manifest);
                if (!marker_removed.ok()) {
                    marker_removed.recovery_available = true;
                    marker_removed.completed_entries = manifest.entries.size();
                    marker_removed.completed_bytes = manifest.total_bytes;
                    marker_removed.destination = manifest.destination;
                    marker_removed.manifest_path = result.manifest_path;
                    return marker_removed;
                }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
                if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::
                                              exception_after_atomic_move_marker_removal)) {
                    throw std::runtime_error("test exception after atomic move marker removal");
                }
#endif
                result.status = BasicDirectoryTransferStatus::success;
                result.completed_entries = manifest.entries.size();
                result.completed_bytes = manifest.total_bytes;
                report_progress(progress, BasicDirectoryTransferPhase::completed, manifest,
                                result.completed_entries, result.completed_bytes,
                                manifest.destination);
                std::error_code cleanup_error;
                if (!retire_manifest_before_lease_release(*active_lease, result.manifest_path,
                                                          cleanup_error)) {
                    result.recovery_available = true;
                    result.error = cleanup_error;
                    result.platform_code = cleanup_error.value();
                    result.detail_utf8 =
                        "atomic root move completed, but its hidden recovery manifest remains";
                    boundary_phase = ExecutionBoundaryPhase::published;
                } else {
                    boundary_phase = ExecutionBoundaryPhase::completed;
                }
                return result;
            }
            result.status = moved.status;
            result.error = moved.error;
            result.platform_code = moved.platform_code;
            result.detail_utf8 = std::move(moved.detail_utf8);
            const auto cross_device = is_cross_device_rename_failure(moved);
            const auto uncertain = result.status == BasicDirectoryTransferStatus::disconnected ||
                                   result.status == BasicDirectoryTransferStatus::timed_out ||
                                   result.status == BasicDirectoryTransferStatus::io_error;
            if (uncertain) {
                result.status = BasicDirectoryTransferStatus::unknown_outcome;
                result.recovery_available = true;
                result.detail_utf8 = "atomic root move outcome is unknown";
                return result;
            }
            auto marker_removed = remove_atomic_move_marker(manifest.source, manifest);
            if (!marker_removed.ok()) {
                marker_removed.recovery_available = true;
                marker_removed.destination = manifest.destination;
                marker_removed.manifest_path = result.manifest_path;
                return marker_removed;
            }
            if (!cross_device) {
                std::error_code cleanup_error;
                if (!retire_manifest_before_lease_release(*active_lease, result.manifest_path,
                                                          cleanup_error)) {
                    result.recovery_available = true;
                    result.detail_utf8 += "; hidden recovery manifest remains";
                } else {
                    boundary_phase = ExecutionBoundaryPhase::none;
                }
                return result;
            }
            result.status = BasicDirectoryTransferStatus::success;
            result.error.clear();
            result.platform_code = 0;
            result.detail_utf8.clear();
            std::error_code cleanup_error;
            if (!retire_manifest_before_lease_release(*active_lease, result.manifest_path,
                                                      cleanup_error)) {
                result.status = BasicDirectoryTransferStatus::recovery_required;
                result.error = cleanup_error;
                result.platform_code = cleanup_error.value();
                result.recovery_available = true;
                result.detail_utf8 =
                    "cross-volume fallback could not retire its stale atomic manifest";
                return result;
            }
            boundary_phase = ExecutionBoundaryPhase::none;
            auto replanned =
                plan_basic_directory_transfer_impl({.source = manifest.source,
                                                    .destination = manifest.destination,
                                                    .operation_id = manifest.operation_id},
                                                   FileTransferKind::move);
            if (!replanned.ok()) {
                result.status = replanned.status;
                result.error = replanned.error;
                result.platform_code = replanned.platform_code;
                result.recovery_available = false;
                result.detail_utf8 = replanned.detail_utf8;
                result.manifest_path = replanned.manifest_path;
                return result;
            }
            if (!same_replanned_source(manifest, replanned.manifest)) {
                result.status = BasicDirectoryTransferStatus::source_changed;
                result.recovery_available = false;
                result.detail_utf8 =
                    "source changed while the atomic move was replanned for cross-volume copy";
                result.manifest_path.clear();
                return result;
            }
            return execute_basic_directory_copy_impl(replanned.manifest, progress, boundary_phase,
                                                     false, nullptr, false);
        }
    }
    bool staging_created{};
    bool staging_create_denied{};
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::staging_create)) {
        staging_create_denied = true;
        error = std::make_error_code(std::errc::permission_denied);
    } else
#endif
    {
        staging_created = std::filesystem::create_directory(manifest.staging_destination, error);
    }
    if (!staging_created) {
        const auto status = staging_create_denied ? BasicDirectoryTransferStatus::permission_denied
                            : error               ? map_transfer_error(error, destination_cifs)
                                                  : BasicDirectoryTransferStatus::recovery_required;
        const auto uncertain = status == BasicDirectoryTransferStatus::disconnected ||
                               status == BasicDirectoryTransferStatus::timed_out ||
                               status == BasicDirectoryTransferStatus::io_error;
        result.status = uncertain ? BasicDirectoryTransferStatus::unknown_outcome : status;
        result.recovery_available = uncertain || !error;
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 =
            uncertain ? "staging directory creation outcome is unknown"
                      : (error ? "staging directory could not be created"
                               : "staging directory already exists and requires recovery");
        if (!uncertain && error) {
            std::error_code cleanup_error;
            if (!retire_manifest_before_lease_release(*active_lease, result.manifest_path,
                                                      cleanup_error)) {
                result.recovery_available = true;
                result.detail_utf8 += "; hidden recovery manifest remains";
            } else {
                boundary_phase = ExecutionBoundaryPhase::none;
            }
        }
        return result;
    }
    boundary_phase = ExecutionBoundaryPhase::staging_created;
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (loseStagingReplyOnce.exchange(false)) {
        result.status = BasicDirectoryTransferStatus::unknown_outcome;
        result.recovery_available = true;
        result.detail_utf8 = "staging directory creation reply was lost by the test hook";
        return result;
    }
    if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::exception_after_staging)) {
        throw std::runtime_error("test exception after staging creation");
    }
#endif
    report_progress(progress, BasicDirectoryTransferPhase::staging_ready, manifest, 0U, 0U,
                    manifest.staging_destination);

    std::unordered_map<std::string, FileProof> proofs;
    proofs.reserve(manifest.entries.size());
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &entry = manifest.entries[index];
        report_progress(progress, BasicDirectoryTransferPhase::copying, manifest,
                        result.completed_entries, result.completed_bytes,
                        manifest.source / entry.relative_path);
        const auto destination = manifest.staging_destination / entry.relative_path;
        if (entry.kind == BasicDirectoryTransferEntryKind::directory) {
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
            if (consume_failure_point(
                    BasicDirectoryTransferTestFailurePoint::subdirectory_create)) {
                result.status = BasicDirectoryTransferStatus::io_error;
                result.detail_utf8 = "staging subdirectory creation failed by test injection";
                return result;
            }
#endif
            if (!std::filesystem::create_directory(destination, error)) {
                result.status = error ? map_transfer_error(error, destination_cifs)
                                      : BasicDirectoryTransferStatus::staging_changed;
                result.error = error;
                result.platform_code = error.value();
                result.detail_utf8 = error ? "staging subdirectory could not be created"
                                           : "staging subdirectory already exists";
                return result;
            }
        } else {
            FileProof proof;
            auto copied =
                copy_file_to_staging(manifest, entry, index, proof, source_cifs, destination_cifs,
                                     progress, result.completed_entries, result.completed_bytes);
            if (!copied.ok()) {
                copied.completed_entries = result.completed_entries;
                copied.completed_bytes = result.completed_bytes;
                copied.manifest_path = result.manifest_path;
                return copied;
            }
            const auto temp =
                file_transfer_temp_destination_path(destination, manifest.operation_id, index);
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
            if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::leaf_publish)) {
                result.status = BasicDirectoryTransferStatus::io_error;
                result.detail_utf8 = "staging leaf publication failed by test injection";
                return result;
            }
#endif
            auto published = rename_no_replace(temp, destination, destination_cifs);
            if (!published.ok()) {
                result.status = published.status;
                result.error = published.error;
                result.platform_code = published.platform_code;
                result.detail_utf8 = std::move(published.detail_utf8);
                return result;
            }
            proofs.emplace(
                directory_transfer_namespace_key(entry.relative_path, manifest.path_semantics),
                proof);
            result.completed_bytes += entry.size_bytes;
        }
        ++result.completed_entries;
    }

    report_progress(progress, BasicDirectoryTransferPhase::verifying, manifest,
                    result.completed_entries, result.completed_bytes, manifest.staging_destination);
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
    if (consume_failure_point(BasicDirectoryTransferTestFailurePoint::final_audit)) {
        result.status = BasicDirectoryTransferStatus::staging_changed;
        result.detail_utf8 = "final staging audit failed by test injection";
        return result;
    }
#endif
    DirectoryPublicationProof publication_proof;
    if (manifest.kind == FileTransferKind::move) {
        auto durable_staging = sync_staging_tree_namespace(manifest);
        if (!durable_staging.ok()) {
            durable_staging.recovery_available = true;
            durable_staging.completed_entries = result.completed_entries;
            durable_staging.completed_bytes = result.completed_bytes;
            durable_staging.staging_destination = manifest.staging_destination;
            durable_staging.destination = manifest.destination;
            durable_staging.manifest_path = result.manifest_path;
            return durable_staging;
        }
        auto proof_result = ensure_publication_proof(manifest, proofs, publication_proof);
        if (!proof_result.ok()) {
            proof_result.completed_entries = result.completed_entries;
            proof_result.completed_bytes = result.completed_bytes;
            proof_result.staging_destination = manifest.staging_destination;
            proof_result.destination = manifest.destination;
            proof_result.manifest_path = result.manifest_path;
            return proof_result;
        }
        // MOVE reads the private tree once before source retirement and once immediately before
        // publication. The copy-time hashes build the proof without a redundant third tree read.
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_network_phase_when_requested("move-staging-audit");
#endif
        auto audited = audit_complete_tree_against_proof(
            manifest.staging_destination, manifest, publication_proof,
            BasicDirectoryTransferStatus::staging_changed, false);
        if (!audited.ok()) {
            audited.completed_entries = result.completed_entries;
            audited.completed_bytes = result.completed_bytes;
            audited.staging_destination = manifest.staging_destination;
            audited.destination = manifest.destination;
            audited.manifest_path = result.manifest_path;
            return audited;
        }
    } else {
        auto audited = audit_staging(manifest, proofs);
        if (!audited.ok()) {
            audited.completed_entries = result.completed_entries;
            audited.completed_bytes = result.completed_bytes;
            audited.staging_destination = manifest.staging_destination;
            audited.destination = manifest.destination;
            audited.manifest_path = result.manifest_path;
            return audited;
        }
    }
    if (manifest.kind == FileTransferKind::move) {
        auto cleanup = continue_move_source_cleanup(manifest, publication_proof, progress,
                                                    boundary_phase, true);
        cleanup.completed_entries = result.completed_entries;
        cleanup.completed_bytes = result.completed_bytes;
        cleanup.destination = manifest.destination;
        cleanup.manifest_path = result.manifest_path;
        if (!cleanup.ok()) {
            return cleanup;
        }
        auto final_staging_audit = audit_complete_tree_against_proof(
            manifest.staging_destination, manifest, publication_proof,
            BasicDirectoryTransferStatus::staging_changed, false);
        if (!final_staging_audit.ok()) {
            final_staging_audit.completed_entries = result.completed_entries;
            final_staging_audit.completed_bytes = result.completed_bytes;
            final_staging_audit.staging_destination = manifest.staging_destination;
            final_staging_audit.destination = manifest.destination;
            final_staging_audit.manifest_path = result.manifest_path;
            return final_staging_audit;
        }
    } else {
        auto source_capture = capture_tree(manifest.source);
        // SMB can settle metadata for each parent directory after its children were created. Keep
        // every directory's physical identity and all regular-file metadata strict, while not
        // mistaking those harmless directory timestamp updates for changed source content.
        if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
            result.status = source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                : source_capture.status;
            result.error = source_capture.error;
            result.platform_code = source_capture.platform_code;
            result.detail_utf8 = source_capture.ok()
                                     ? "source tree changed before directory publication"
                                     : std::move(source_capture.detail_utf8);
            return result;
        }
    }

    result = publish_staged_root(manifest, progress, result.completed_entries,
                                 result.completed_bytes, boundary_phase);
    if (!result.ok()) {
        return result;
    }

    report_progress(progress, BasicDirectoryTransferPhase::completed, manifest,
                    result.completed_entries, result.completed_bytes, manifest.destination);
    std::error_code cleanup_error;
    const auto removed =
        retire_manifest_before_lease_release(*active_lease, result.manifest_path, cleanup_error);
    result.status = BasicDirectoryTransferStatus::success;
    if (!removed) {
        result.recovery_available = true;
        result.detail_utf8 = "directory was published, but its hidden recovery manifest remains";
        boundary_phase = ExecutionBoundaryPhase::published;
    } else {
        boundary_phase = ExecutionBoundaryPhase::completed;
    }
    return result;
}

BasicDirectoryTransferResult
execute_basic_directory_copy(const BasicDirectoryTransferManifest &manifest,
                             const BasicDirectoryTransferProgressCallback &progress) {
    if (manifest.kind != FileTransferKind::copy) {
        return failure(BasicDirectoryTransferStatus::invalid_request,
                       "copy execution requires a copy manifest");
    }
    ExecutionBoundaryPhase boundary_phase{ExecutionBoundaryPhase::none};
    try {
        auto result = execute_basic_directory_copy_impl(manifest, progress, boundary_phase);
        advertise_execution_recovery(result, boundary_phase);
        return result;
    } catch (const std::bad_alloc &) {
        return execution_exception_result(manifest, boundary_phase,
                                          BasicDirectoryTransferStatus::unsupported);
    } catch (const std::length_error &) {
        return execution_exception_result(manifest, boundary_phase,
                                          BasicDirectoryTransferStatus::unsupported);
    } catch (const std::filesystem::filesystem_error &exception) {
        return execution_exception_result(
            manifest, boundary_phase,
            map_transfer_error(exception.code(),
                               path_is_cifs(manifest.source) ||
                                   path_is_cifs(manifest.destination.parent_path())),
            exception.code(), exception.code().value());
    } catch (...) {
        return execution_exception_result(manifest, boundary_phase,
                                          BasicDirectoryTransferStatus::io_error);
    }
}

static BasicDirectoryTransferResult
execute_basic_directory_move_mode(const BasicDirectoryTransferManifest &manifest,
                                  const BasicDirectoryTransferProgressCallback &progress,
                                  const bool allow_atomic_root_move) {
    if (manifest.kind != FileTransferKind::move) {
        return failure(BasicDirectoryTransferStatus::invalid_request,
                       "move execution requires a move manifest");
    }
    ExecutionBoundaryPhase boundary_phase{ExecutionBoundaryPhase::none};
    try {
        auto result = execute_basic_directory_copy_impl(manifest, progress, boundary_phase, false,
                                                        nullptr, allow_atomic_root_move);
        advertise_execution_recovery(result, boundary_phase);
        return result;
    } catch (const std::bad_alloc &) {
        return execution_exception_result(manifest, boundary_phase,
                                          BasicDirectoryTransferStatus::unsupported);
    } catch (const std::length_error &) {
        return execution_exception_result(manifest, boundary_phase,
                                          BasicDirectoryTransferStatus::unsupported);
    } catch (const std::filesystem::filesystem_error &exception) {
        return execution_exception_result(
            manifest, boundary_phase,
            map_transfer_error(exception.code(),
                               path_is_cifs(manifest.source) ||
                                   path_is_cifs(manifest.destination.parent_path())),
            exception.code(), exception.code().value());
    } catch (...) {
        return execution_exception_result(manifest, boundary_phase,
                                          BasicDirectoryTransferStatus::recovery_required);
    }
}

BasicDirectoryTransferResult
execute_basic_directory_move(const BasicDirectoryTransferManifest &manifest,
                             const BasicDirectoryTransferProgressCallback &progress) {
    return execute_basic_directory_move_mode(manifest, progress, true);
}

BasicDirectoryTransferResult
reconcile_basic_directory_publication_impl(const BasicDirectoryTransferManifest &manifest) {
    std::string detail;
    if (!valid_basic_directory_transfer_manifest(manifest, detail)) {
        return failure(BasicDirectoryTransferStatus::invalid_request, std::move(detail));
    }
    if (manifest.kind == FileTransferKind::move) {
        auto result =
            failure(BasicDirectoryTransferStatus::recovery_required,
                    "directory move publication must be resumed under its recovery lease");
        result.recovery_available = true;
        result.staging_destination = source_retirement_path(manifest);
        result.destination = manifest.destination;
        result.manifest_path = basic_directory_transfer_manifest_path(manifest);
        return result;
    }
    BasicDirectoryTransferResult result;
    result.status = BasicDirectoryTransferStatus::unknown_outcome;
    result.recovery_available = true;
    result.staging_destination = manifest.staging_destination;
    result.destination = manifest.destination;
    result.manifest_path = basic_directory_transfer_manifest_path(manifest);
    const auto destination_cifs = path_is_cifs(manifest.destination.parent_path());
    std::error_code error;
    const auto destination_present = std::filesystem::exists(manifest.destination, error);
    if (error) {
        result.status = map_transfer_error(error, destination_cifs);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "final destination cannot currently be observed";
        return result;
    }
    const auto staging_present = std::filesystem::exists(manifest.staging_destination, error);
    if (error) {
        result.status = map_transfer_error(error, destination_cifs);
        result.error = error;
        result.platform_code = error.value();
        result.detail_utf8 = "staging destination cannot currently be observed";
        return result;
    }
    if (staging_present && !destination_present) {
        result.status = BasicDirectoryTransferStatus::recovery_required;
        result.detail_utf8 = "unfinished hidden staging tree requires explicit resume or cleanup";
        return result;
    }
    if (!staging_present && destination_present) {
        auto audited = audit_published_tree_against_source(manifest);
        if (!audited.ok()) {
            if (audited.status == BasicDirectoryTransferStatus::disconnected ||
                audited.status == BasicDirectoryTransferStatus::timed_out ||
                audited.status == BasicDirectoryTransferStatus::authentication_required ||
                audited.status == BasicDirectoryTransferStatus::permission_denied) {
                result.status = audited.status;
            }
            result.error = audited.error;
            result.platform_code = audited.platform_code;
            result.detail_utf8 =
                "published destination cannot be proven from the retained source: " +
                audited.detail_utf8;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        if (consume_failure_point(
                BasicDirectoryTransferTestFailurePoint::exception_during_reconciliation)) {
            throw std::runtime_error("test exception during publication reconciliation");
        }
#endif
        std::error_code cleanup_error;
        const auto removed = remove_manifest_artifacts(result.manifest_path, cleanup_error);
        result.status = BasicDirectoryTransferStatus::success;
        result.recovery_available = false;
        result.completed_entries = manifest.entries.size();
        result.completed_bytes = manifest.total_bytes;
        result.detail_utf8 = removed ? "final directory publication was confirmed by observation"
                                     : "publication was confirmed, but its hidden manifest remains";
        return result;
    }
    result.detail_utf8 =
        staging_present ? "staging and final destination both exist; no automatic action is safe"
                        : "neither staging nor final destination exists; outcome is unknown";
    return result;
}

BasicDirectoryTransferResult
reconcile_basic_directory_publication(const BasicDirectoryTransferManifest &manifest) {
    try {
        return reconcile_basic_directory_publication_impl(manifest);
    } catch (const std::bad_alloc &) {
        return reconciliation_exception_result(manifest);
    } catch (const std::length_error &) {
        return reconciliation_exception_result(manifest);
    } catch (const std::filesystem::filesystem_error &exception) {
        auto result =
            reconciliation_exception_result(manifest, exception.code(), exception.code().value());
        result.status =
            map_transfer_error(exception.code(), path_is_cifs(manifest.destination.parent_path()));
        return result;
    } catch (...) {
        return reconciliation_exception_result(manifest);
    }
}

BasicDirectoryTransferResult
copy_directory_basic(const BasicDirectoryTransferRequest &request,
                     const BasicDirectoryTransferProgressCallback &progress) {
    if (progress) {
        try {
            progress({.phase = BasicDirectoryTransferPhase::enumerating,
                      .current_path = request.source});
        } catch (...) {
            static_cast<void>(std::current_exception());
        }
    }
    auto planned = plan_basic_directory_copy(request);
    if (!planned.ok()) {
        BasicDirectoryTransferResult result;
        result.status = planned.status;
        result.error = planned.error;
        result.platform_code = planned.platform_code;
        result.staging_destination = std::move(planned.staging_destination);
        result.destination = std::move(planned.destination);
        result.manifest_path = std::move(planned.manifest_path);
        result.detail_utf8 = std::move(planned.detail_utf8);
        return result;
    }
    return execute_basic_directory_copy(planned.manifest, progress);
}

BasicDirectoryTransferResult
move_directory_basic(const BasicDirectoryTransferRequest &request,
                     const BasicDirectoryTransferProgressCallback &progress) {
    if (progress) {
        try {
            progress({.phase = BasicDirectoryTransferPhase::enumerating,
                      .current_path = request.source});
        } catch (...) {
            static_cast<void>(std::current_exception());
        }
    }
    auto planned = plan_basic_directory_move(request);
    if (!planned.ok()) {
        BasicDirectoryTransferResult result;
        result.status = planned.status;
        result.error = planned.error;
        result.platform_code = planned.platform_code;
        result.staging_destination = std::move(planned.staging_destination);
        result.destination = std::move(planned.destination);
        result.manifest_path = std::move(planned.manifest_path);
        result.detail_utf8 = std::move(planned.detail_utf8);
        return result;
    }
    return execute_basic_directory_move_mode(planned.manifest, progress, true);
}

BasicDirectoryTransferRecoveryScanResult
scan_basic_directory_recoveries(const std::filesystem::path &destination_parent) {
    BasicDirectoryTransferRecoveryScanResult result;
    const auto destination_cifs = path_is_cifs(destination_parent);
    try {
        std::error_code error;
        const auto parent = std::filesystem::absolute(destination_parent, error).lexically_normal();
        if (error) {
            result.status = map_error_status(error);
            result.error = error;
            result.platform_code = error.value();
            result.detail_utf8 = "recovery scan root could not be made absolute";
            return result;
        }
        const auto parent_cifs = path_is_cifs(parent);
        const auto parent_status = std::filesystem::symlink_status(parent, error);
        if (error || !ordinary_directory(parent, parent_status, error)) {
            result.status = error ? map_transfer_error(error, parent_cifs)
                                  : BasicDirectoryTransferStatus::invalid_request;
            result.error = error;
            result.platform_code = error.value();
            result.detail_utf8 = "recovery scan root is not an ordinary directory";
            return result;
        }
        std::filesystem::directory_iterator iterator(
            parent, std::filesystem::directory_options::none, error);
        const std::filesystem::directory_iterator end;
        if (error) {
            result.status = map_transfer_error(error, parent_cifs);
            result.error = error;
            result.platform_code = error.value();
            result.detail_utf8 = "recovery scan root could not be enumerated";
            return result;
        }
        while (iterator != end) {
            if (++result.inspected_entries > kBasicDirectoryTransferMaximumRecoveryScanEntries) {
                result.truncated = true;
                break;
            }
            const auto status = iterator->symlink_status(error);
            if (error) {
                result.status = map_transfer_error(error, parent_cifs);
                result.error = error;
                result.platform_code = error.value();
                result.detail_utf8 = "recovery scan entry could not be inspected";
                return result;
            }
            if (is_basic_recovery_control_name(iterator->path().filename()) &&
                ordinary_directory(iterator->path(), status, error)) {
                if (result.candidates.size() >= kBasicDirectoryTransferMaximumRecoveryCandidates) {
                    result.truncated = true;
                    break;
                }
                const auto manifest_path = iterator->path() / "manifest";
                RecoveryManifestLoadAccounting accounting;
                auto candidate = load_basic_directory_transfer_manifest_bounded(
                    manifest_path,
                    {.maximum_bytes = kBasicDirectoryTransferMaximumRecoveryLoadedBytes -
                                      result.loaded_manifest_bytes,
                     .maximum_entries = kBasicDirectoryTransferMaximumRecoveryLoadedEntries -
                                        result.loaded_manifest_entries,
                     .defer_over_budget = true,
                     .known_cifs = parent_cifs},
                    accounting);
                result.loaded_manifest_bytes += accounting.loaded_bytes;
                result.loaded_manifest_entries += accounting.loaded_entries;
                if (candidate.status == BasicDirectoryTransferStatus::recovery_required) {
                    ++result.deferred_candidates;
                    result.truncated = true;
                }
                result.candidates.push_back(std::move(candidate));
            }
            if (error) {
                result.status = map_transfer_error(error, parent_cifs);
                result.error = error;
                result.platform_code = error.value();
                result.detail_utf8 = "recovery control directory could not be inspected";
                return result;
            }
            iterator.increment(error);
            if (error) {
                result.status = map_transfer_error(error, parent_cifs);
                result.error = error;
                result.platform_code = error.value();
                result.detail_utf8 = "recovery scan was interrupted";
                return result;
            }
        }
        result.status = BasicDirectoryTransferStatus::success;
        return result;
    } catch (const std::bad_alloc &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
    } catch (const std::length_error &) {
        result.status = BasicDirectoryTransferStatus::unsupported;
    } catch (const std::filesystem::filesystem_error &exception) {
        result.status = map_transfer_error(exception.code(), destination_cifs);
        result.error = exception.code();
        result.platform_code = exception.code().value();
    } catch (...) {
        result.status = BasicDirectoryTransferStatus::io_error;
    }
    return result;
}

namespace {

void cleanup_manifestless_control_directory(const std::filesystem::path &manifest_path,
                                            BasicDirectoryTransferResult &result) {
    if (result.status != BasicDirectoryTransferStatus::not_found) {
        return;
    }
    const auto control_directory = manifest_path.parent_path();
    if (!is_basic_recovery_control_name(control_directory.filename())) {
        return;
    }
    auto staging = control_directory;
    staging.replace_extension();
    std::error_code error;
    if (std::filesystem::exists(staging, error) || error) {
        return;
    }
    RecoveryManifestLease lease;
    bool lease_busy{};
    if (!lease.acquire(control_directory / "lease", error, lease_busy)) {
        return;
    }
    const auto manifest_present = std::filesystem::exists(manifest_path, error);
    if (error || manifest_present || std::filesystem::exists(staging, error) || error) {
        return;
    }
    std::filesystem::directory_iterator iterator(control_directory,
                                                 std::filesystem::directory_options::none, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const auto filename = iterator->path().filename();
        const auto status = iterator->symlink_status(error);
        if (error ||
            (filename != "lease" && filename != "publication-proof" &&
             filename != "atomic-move-proof") ||
            !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
            return;
        }
        iterator.increment(error);
    }
    if (error) {
        return;
    }
    std::filesystem::remove(control_directory / "publication-proof", error);
    if (error) {
        return;
    }
    std::filesystem::remove(control_directory / "atomic-move-proof", error);
    if (error) {
        return;
    }
    lease.release();
    std::filesystem::remove(control_directory / "lease", error);
    if (error) {
        return;
    }
    if (!std::filesystem::remove(control_directory, error) || error) {
        return;
    }
    result.error.clear();
    result.platform_code = 0;
    result.recovery_available = false;
    result.detail_utf8 = "completed stale recovery-control cleanup; no manifest remains";
}

BasicDirectoryTransferResult
resume_basic_directory_copy_impl(const std::filesystem::path &manifest_path,
                                 const BasicDirectoryTransferProgressCallback &progress,
                                 BasicDirectoryTransferManifest &recovery_context,
                                 const std::optional<FileTransferKind> expected_kind,
                                 const BasicDirectoryTransferRecoveryIdentity *expected_identity) {
    std::error_code error;
    const auto normalized_manifest = normalized_absolute_path(manifest_path, error);
    if (error) {
        BasicDirectoryTransferResult result;
        result.status = map_error_status(error);
        result.error = error;
        result.platform_code = error.value();
        result.manifest_path = manifest_path;
        result.detail_utf8 = "recovery manifest path could not be made absolute";
        return result;
    }
    const auto control_directory = normalized_manifest.parent_path();
    const auto destination_cifs = path_is_cifs(control_directory);
    const auto control_status = std::filesystem::symlink_status(control_directory, error);
    if (error || !ordinary_directory(control_directory, control_status, error)) {
        BasicDirectoryTransferResult result;
        result.status = error ? map_transfer_error(error, destination_cifs)
                              : BasicDirectoryTransferStatus::unsupported;
        result.error = error;
        result.platform_code = error.value();
        result.manifest_path = normalized_manifest;
        result.detail_utf8 = "recovery control path is not an ordinary directory";
        return result;
    }
    const auto preflight =
        load_basic_directory_transfer_manifest_with_context(normalized_manifest, destination_cifs);
    if (!preflight.ok()) {
        BasicDirectoryTransferResult result;
        result.status = preflight.status;
        result.error = preflight.error;
        result.platform_code = preflight.platform_code;
        result.staging_destination = preflight.staging_destination;
        result.destination = preflight.destination;
        result.manifest_path = preflight.manifest_path;
        result.detail_utf8 = preflight.detail_utf8;
        cleanup_manifestless_control_directory(normalized_manifest, result);
        return result;
    }
    recovery_context = preflight.manifest;
    RecoveryManifestLease lease;
    bool lease_busy{};
    if (!lease.acquire(control_directory / "lease", error, lease_busy)) {
        BasicDirectoryTransferResult result;
        result.status = lease_busy ? BasicDirectoryTransferStatus::recovery_required
                                   : (error == std::errc::operation_not_supported
                                          ? BasicDirectoryTransferStatus::unsupported
                                          : map_transfer_error(error, destination_cifs));
        result.error = error;
        result.platform_code = error.value();
        result.manifest_path = normalized_manifest;
        result.detail_utf8 = lease_busy ? "another recovery attempt owns this manifest"
                                        : "recovery manifest could not be locked safely";
        return result;
    }
    const auto loaded =
        load_basic_directory_transfer_manifest_with_context(normalized_manifest, destination_cifs);
    if (!loaded.ok()) {
        BasicDirectoryTransferResult result;
        result.status = loaded.status;
        result.error = loaded.error;
        result.platform_code = loaded.platform_code;
        result.staging_destination = loaded.staging_destination;
        result.destination = loaded.destination;
        result.manifest_path = loaded.manifest_path;
        result.detail_utf8 = loaded.detail_utf8;
        return result;
    }
    const auto &manifest = loaded.manifest;
    recovery_context = manifest;
    if (expected_identity != nullptr &&
        (expected_identity->operation_id == 0U || !expected_identity->source.is_absolute() ||
         !expected_identity->destination.is_absolute() ||
         expected_identity->source_revision_utf8.empty() ||
         manifest.operation_id != expected_identity->operation_id ||
         manifest.source != expected_identity->source ||
         manifest.destination != expected_identity->destination ||
         manifest.source_revision_utf8 != expected_identity->source_revision_utf8)) {
        BasicDirectoryTransferResult result;
        result.status = BasicDirectoryTransferStatus::recovery_required;
        result.recovery_available = true;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        result.detail_utf8 = "recovery manifest identity does not match the requested operation";
        return result;
    }
    if (expected_kind && manifest.kind != *expected_kind) {
        BasicDirectoryTransferResult result;
        result.status = BasicDirectoryTransferStatus::invalid_request;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        result.detail_utf8 = *expected_kind == FileTransferKind::copy
                                 ? "copy recovery requires a copy manifest"
                                 : "move recovery requires a move manifest";
        return result;
    }
    const auto release_and_finalize_success = [&](BasicDirectoryTransferResult result,
                                                  const bool completion_already_reported =
                                                      false) -> BasicDirectoryTransferResult {
        if (!result.ok()) {
            return result;
        }
        if (!completion_already_reported) {
            report_progress(progress, BasicDirectoryTransferPhase::completed, manifest,
                            result.completed_entries, result.completed_bytes, manifest.destination);
        }
        std::error_code cleanup_error;
        if (!retire_manifest_before_lease_release(lease, loaded.manifest_path, cleanup_error)) {
            result.error = cleanup_error;
            result.platform_code = cleanup_error.value();
            result.recovery_available = true;
            result.detail_utf8 =
                "directory was published, but its hidden recovery manifest remains";
        } else {
            result.recovery_available = false;
        }
        return result;
    };
    const auto destination_present = std::filesystem::exists(manifest.destination, error);
    if (error) {
        auto result = reconciliation_exception_result(manifest, error, error.value());
        result.status = map_transfer_error(error, destination_cifs);
        result.detail_utf8 = "destination state could not be observed before restart";
        return result;
    }
    if (destination_present) {
        if (manifest.kind == FileTransferKind::copy) {
            return release_and_finalize_success(reconcile_basic_directory_publication(manifest));
        }
        bool staging_present{};
        auto staging_state =
            path_presence(manifest.staging_destination, destination_cifs, staging_present);
        bool source_present{};
        auto source_state =
            path_presence(manifest.source, path_is_cifs(manifest.source), source_present);
        bool source_retirement_present{};
        auto retirement_state =
            path_presence(source_retirement_path(manifest), path_is_cifs(manifest.source),
                          source_retirement_present);
        if (!staging_state.ok() || !source_state.ok() || !retirement_state.ok()) {
            auto result = !staging_state.ok()  ? staging_state
                          : !source_state.ok() ? source_state
                                               : retirement_state;
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        const auto proof_path = publication_proof_path(manifest);
        const auto proof_present = std::filesystem::exists(proof_path, error);
        if (error) {
            auto result = reconciliation_exception_result(manifest, error, error.value());
            result.status = map_transfer_error(error, destination_cifs);
            result.detail_utf8 = "atomic move proof state could not be observed";
            return result;
        }
        bool source_marker_present{};
        if (source_present) {
            auto marker_state = path_presence(atomic_move_marker_path(manifest.source),
                                              path_is_cifs(manifest.source), source_marker_present);
            if (!marker_state.ok()) {
                marker_state.destination = manifest.destination;
                marker_state.manifest_path = loaded.manifest_path;
                return marker_state;
            }
        }
        if (!staging_present && source_present && !source_retirement_present && !proof_present &&
            source_marker_present) {
            auto marker_removed = remove_atomic_move_marker(manifest.source, manifest);
            if (!marker_removed.ok()) {
                marker_removed.recovery_available = true;
                marker_removed.destination = manifest.destination;
                marker_removed.manifest_path = loaded.manifest_path;
                return marker_removed;
            }
            auto result = failure(BasicDirectoryTransferStatus::conflict,
                                  "atomic root move did not replace the existing destination");
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            std::error_code cleanup_error;
            if (!retire_manifest_before_lease_release(lease, loaded.manifest_path, cleanup_error)) {
                result.recovery_available = true;
                result.error = cleanup_error;
                result.platform_code = cleanup_error.value();
                result.detail_utf8 += "; hidden recovery manifest remains";
            }
            return result;
        }
        if (!staging_present && !source_present && !source_retirement_present && !proof_present) {
            bool marker_present{};
            auto marker_state = path_presence(atomic_move_marker_path(manifest.destination),
                                              destination_cifs, marker_present);
            if (!marker_state.ok()) {
                marker_state.destination = manifest.destination;
                marker_state.manifest_path = loaded.manifest_path;
                return marker_state;
            }
            bool committed_proof_present{};
            auto committed_state = path_presence(atomic_move_proof_path(manifest), destination_cifs,
                                                 committed_proof_present);
            if (!committed_state.ok()) {
                committed_state.destination = manifest.destination;
                committed_state.manifest_path = loaded.manifest_path;
                return committed_state;
            }
            bool marker_valid{};
            if (marker_present) {
                auto marker =
                    load_atomic_move_evidence(atomic_move_marker_path(manifest.destination),
                                              manifest, "atomic move ownership marker");
                if (!marker.ok()) {
                    marker.recovery_available = true;
                    marker.destination = manifest.destination;
                    marker.manifest_path = loaded.manifest_path;
                    return marker;
                }
                marker_valid = true;
            }
            AtomicMoveCommittedProof committed_proof;
            if (committed_proof_present) {
                auto committed = load_atomic_move_committed_proof(manifest, committed_proof);
                if (!committed.ok()) {
                    if (marker_valid &&
                        committed.status == BasicDirectoryTransferStatus::staging_changed) {
                        committed = remove_atomic_move_evidence_after_audit(
                            atomic_move_proof_path(manifest),
                            "corrupt atomic move committed proof");
                        if (committed.ok()) {
                            committed_proof_present = false;
                        }
                    }
                    if (!committed.ok()) {
                        committed.recovery_available = true;
                        committed.destination = manifest.destination;
                        committed.manifest_path = loaded.manifest_path;
                        return committed;
                    }
                }
            }
            auto captured = marker_present
                                ? capture_tree_with_ownership_marker(manifest.destination)
                                : capture_tree(manifest.destination);
            std::string shape_detail;
            const auto captured_identity =
                atomic_move_destination_identity_digest(captured, manifest.path_semantics);
            const auto expected_identity = atomic_move_expected_identity_digest(manifest);
            const auto marker_matches =
                marker_present && same_atomically_renamed_shape(manifest, captured, shape_detail);
            const auto proof_matches =
                committed_proof_present && captured_identity && expected_identity &&
                committed_proof.destination_identity_digest == *expected_identity &&
                *captured_identity == *expected_identity;
            const auto identity_matches =
                captured_identity && expected_identity && *captured_identity == *expected_identity;
            const auto evidence_matches = identity_matches && (!marker_present || marker_matches) &&
                                          (!committed_proof_present || proof_matches);
            if (!evidence_matches) {
                auto result = move_recovery_required(
                    manifest,
                    captured.ok()
                        ? (shape_detail.empty()
                               ? "published root is not the original atomically moved tree"
                               : shape_detail)
                        : captured.detail_utf8,
                    captured.error, captured.platform_code,
                    captured.ok() ? BasicDirectoryTransferStatus::staging_changed
                                  : captured.status);
                result.manifest_path = loaded.manifest_path;
                return result;
            }
            auto durable = confirm_atomic_move_namespace(manifest);
            if (!durable.ok()) {
                durable.status = BasicDirectoryTransferStatus::unknown_outcome;
                durable.recovery_available = true;
                durable.destination = manifest.destination;
                durable.manifest_path = loaded.manifest_path;
                durable.detail_utf8 =
                    "atomic root move is present, but namespace durability is unknown";
                return durable;
            }
            if (!committed_proof_present) {
                if (!expected_identity) {
                    auto result = move_recovery_required(
                        manifest, "published atomic root has no stable physical identity proof");
                    result.manifest_path = loaded.manifest_path;
                    return result;
                }
                auto committed =
                    persist_atomic_move_committed_proof_once(manifest, *expected_identity);
                if (!committed.ok()) {
                    committed.recovery_available = true;
                    committed.destination = manifest.destination;
                    committed.manifest_path = loaded.manifest_path;
                    return committed;
                }
            }
            if (marker_present) {
                auto marker_removed = remove_atomic_move_marker(manifest.destination, manifest);
                if (!marker_removed.ok()) {
                    marker_removed.recovery_available = true;
                    marker_removed.destination = manifest.destination;
                    marker_removed.manifest_path = loaded.manifest_path;
                    return marker_removed;
                }
            }
            auto result = failure(BasicDirectoryTransferStatus::success,
                                  "atomic root move was confirmed by durable ownership evidence");
            result.completed_entries = manifest.entries.size();
            result.completed_bytes = manifest.total_bytes;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return release_and_finalize_success(std::move(result));
        }
        DirectoryPublicationProof publication_proof;
        auto proof_result = load_publication_proof(manifest, publication_proof);
        if (!proof_result.ok()) {
            proof_result.staging_destination = source_retirement_path(manifest);
            proof_result.destination = manifest.destination;
            proof_result.manifest_path = loaded.manifest_path;
            return proof_result;
        }
        if (staging_present && !source_present && !source_retirement_present) {
            auto result =
                failure(BasicDirectoryTransferStatus::move_pending_publication,
                        "final destination is occupied; verified hidden move remains unpublished");
            result.recovery_available = true;
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        if (staging_present || source_present || source_retirement_present) {
            auto result = move_recovery_required(
                manifest, "published move has an unexpected source or staging namespace occupant");
            result.manifest_path = loaded.manifest_path;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_network_phase_when_requested("reconciliation-audit");
#endif
        auto audited =
            audit_complete_tree_against_proof(manifest.destination, manifest, publication_proof,
                                              BasicDirectoryTransferStatus::staging_changed, false);
        audited.completed_entries = manifest.entries.size();
        audited.completed_bytes = manifest.total_bytes;
        audited.destination = manifest.destination;
        audited.manifest_path = loaded.manifest_path;
        return release_and_finalize_success(std::move(audited));
    }
    const auto staging_present = std::filesystem::exists(manifest.staging_destination, error);
    if (error) {
        auto result = reconciliation_exception_result(manifest, error, error.value());
        result.status = map_transfer_error(error, destination_cifs);
        result.detail_utf8 = "staging state could not be observed before restart";
        return result;
    }
    const auto retirement_path = basic_directory_transfer_retirement_path(manifest);
    const auto quarantine_path = cleanup_quarantine_path(retirement_path);
    auto retirement_present = std::filesystem::exists(retirement_path, error);
    if (error) {
        auto result = reconciliation_exception_result(manifest, error, error.value());
        result.status = map_transfer_error(error, destination_cifs);
        result.detail_utf8 = "staging retirement state could not be observed before restart";
        return result;
    }
    const auto quarantine_present = std::filesystem::exists(quarantine_path, error);
    if (error) {
        auto result = reconciliation_exception_result(manifest, error, error.value());
        result.status = map_transfer_error(error, destination_cifs);
        result.detail_utf8 = "cleanup quarantine state could not be observed before restart";
        return result;
    }
    if ((retirement_present && quarantine_present) ||
        (staging_present && (retirement_present || quarantine_present))) {
        BasicDirectoryTransferResult result;
        result.status = BasicDirectoryTransferStatus::staging_changed;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        result.detail_utf8 = "multiple active, retired, or quarantined staging roots exist";
        return result;
    }
    const auto retirement = quarantine_present ? quarantine_path : retirement_path;
    retirement_present = retirement_present || quarantine_present;
    if (manifest.kind == FileTransferKind::move) {
        bool source_present{};
        auto source_state =
            path_presence(manifest.source, path_is_cifs(manifest.source), source_present);
        if (!source_state.ok()) {
            source_state.staging_destination = manifest.staging_destination;
            source_state.destination = manifest.destination;
            source_state.manifest_path = loaded.manifest_path;
            return source_state;
        }
        bool source_retirement_present{};
        auto source_retirement_state =
            path_presence(source_retirement_path(manifest), path_is_cifs(manifest.source),
                          source_retirement_present);
        if (!source_retirement_state.ok()) {
            source_retirement_state.staging_destination = source_retirement_path(manifest);
            source_retirement_state.destination = manifest.destination;
            source_retirement_state.manifest_path = loaded.manifest_path;
            return source_retirement_state;
        }
        if (staging_present) {
            DirectoryPublicationProof publication_proof;
            auto proof_result = load_publication_proof(manifest, publication_proof);
            if (proof_result.ok()) {
                ExecutionBoundaryPhase boundary_phase =
                    source_retirement_present ? ExecutionBoundaryPhase::source_retired
                                              : ExecutionBoundaryPhase::staging_created;
                auto cleanup = continue_move_source_cleanup(manifest, publication_proof, progress,
                                                            boundary_phase, false);
                cleanup.completed_entries = manifest.entries.size();
                cleanup.completed_bytes = manifest.total_bytes;
                cleanup.destination = manifest.destination;
                cleanup.manifest_path = loaded.manifest_path;
                if (!cleanup.ok()) {
                    return cleanup;
                }
                auto final_staging_audit = audit_complete_tree_against_proof(
                    manifest.staging_destination, manifest, publication_proof,
                    BasicDirectoryTransferStatus::staging_changed, false);
                if (!final_staging_audit.ok()) {
                    final_staging_audit.completed_entries = manifest.entries.size();
                    final_staging_audit.completed_bytes = manifest.total_bytes;
                    final_staging_audit.staging_destination = manifest.staging_destination;
                    final_staging_audit.destination = manifest.destination;
                    final_staging_audit.manifest_path = loaded.manifest_path;
                    return final_staging_audit;
                }
                auto result = publish_staged_root(manifest, progress, manifest.entries.size(),
                                                  manifest.total_bytes, boundary_phase);
                if (!result.ok()) {
                    return result;
                }
                return release_and_finalize_success(std::move(result));
            }
            const auto source_intact = source_present && !source_retirement_present;
            const auto restartable_proof_failure =
                proof_result.status == BasicDirectoryTransferStatus::not_found ||
                proof_result.status == BasicDirectoryTransferStatus::staging_changed;
            if (!source_intact || !restartable_proof_failure) {
                proof_result.staging_destination = manifest.staging_destination;
                proof_result.destination = manifest.destination;
                proof_result.manifest_path = loaded.manifest_path;
                return proof_result;
            }
        } else if (source_retirement_present || !source_present) {
            auto result = move_recovery_required(
                manifest, "move lost its hidden staged copy before final publication");
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        bool atomic_proof_present{};
        auto atomic_proof_state =
            path_presence(atomic_move_proof_path(manifest), destination_cifs, atomic_proof_present);
        if (!atomic_proof_state.ok()) {
            atomic_proof_state.destination = manifest.destination;
            atomic_proof_state.manifest_path = loaded.manifest_path;
            return atomic_proof_state;
        }
        if (atomic_proof_present) {
            auto result = move_recovery_required(
                manifest, "committed atomic move proof exists without its destination root");
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        bool atomic_marker_present{};
        auto atomic_marker_state =
            path_presence(atomic_move_marker_path(manifest.source), path_is_cifs(manifest.source),
                          atomic_marker_present);
        if (!atomic_marker_state.ok()) {
            atomic_marker_state.destination = manifest.destination;
            atomic_marker_state.manifest_path = loaded.manifest_path;
            return atomic_marker_state;
        }
        if (atomic_marker_present) {
            auto marker_removed = remove_atomic_move_marker(manifest.source, manifest);
            if (!marker_removed.ok()) {
                marker_removed.recovery_available = true;
                marker_removed.destination = manifest.destination;
                marker_removed.manifest_path = loaded.manifest_path;
                return marker_removed;
            }
            std::error_code cleanup_error;
            if (!retire_manifest_before_lease_release(lease, loaded.manifest_path, cleanup_error)) {
                auto result = move_recovery_required(
                    manifest,
                    "aborted atomic move marker was removed, but recovery control remains",
                    cleanup_error, cleanup_error.value(), BasicDirectoryTransferStatus::io_error);
                result.manifest_path = loaded.manifest_path;
                return result;
            }
            return move_directory_basic({.source = manifest.source,
                                         .destination = manifest.destination,
                                         .operation_id = manifest.operation_id},
                                        progress);
        }
    }
    const auto source_capture = capture_tree(manifest.source);
    if (!same_owned_content_ignoring_directory_metadata(manifest, source_capture)) {
        auto result = failure(source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                  : source_capture.status,
                              source_capture.ok() ? "source changed before staging was recreated"
                                                  : source_capture.detail_utf8,
                              source_capture.error, source_capture.platform_code);
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        return result;
    }
    if (staging_present) {
        auto audited = audit_staging_for_restart(manifest);
        if (!audited.ok()) {
            audited.staging_destination = manifest.staging_destination;
            audited.destination = manifest.destination;
            audited.manifest_path = loaded.manifest_path;
            return audited;
        }
        auto retired =
            rename_no_replace(manifest.staging_destination, retirement_path, destination_cifs);
        if (!retired.ok()) {
            BasicDirectoryTransferResult result;
            result.status = retired.status;
            result.error = retired.error;
            result.platform_code = retired.platform_code;
            result.detail_utf8 = std::move(retired.detail_utf8);
            if (retired.status == BasicDirectoryTransferStatus::disconnected ||
                retired.status == BasicDirectoryTransferStatus::timed_out ||
                retired.status == BasicDirectoryTransferStatus::io_error) {
                result.status = BasicDirectoryTransferStatus::unknown_outcome;
                result.detail_utf8 = "staging retirement outcome is unknown";
            }
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        if (consume_failure_point(
                BasicDirectoryTransferTestFailurePoint::exception_after_staging_retirement)) {
            throw std::runtime_error("test exception after staging retirement");
        }
        pause_after_staging_retirement_when_requested();
#endif
        retirement_present = true;
    }
    if (retirement_present) {
        auto retired_manifest = manifest;
        retired_manifest.staging_destination = retirement;
        auto audited = audit_staging_for_restart(retired_manifest);
        if (!audited.ok()) {
            audited.staging_destination = retirement;
            audited.destination = manifest.destination;
            audited.manifest_path = loaded.manifest_path;
            return audited;
        }
        const auto retirement_capture = capture_owned_staging_tree(retirement);
        if (!retirement_capture.ok()) {
            auto result = failure(retirement_capture.status,
                                  "retired unfinished data could not be captured before removal",
                                  retirement_capture.error, retirement_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = retirement;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_after_source_cleanup_validation_when_requested();
        pause_after_staging_retirement_when_requested();
#endif
        const auto current_source_capture = capture_tree(manifest.source);
        if (!same_tree_capture(source_capture, current_source_capture)) {
            auto result =
                failure(current_source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                    : current_source_capture.status,
                        current_source_capture.ok()
                            ? "source changed before retired unfinished data could be removed"
                            : current_source_capture.detail_utf8,
                        current_source_capture.error, current_source_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = retirement;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        const auto current_retirement_capture = capture_owned_staging_tree(retirement);
        if (!same_tree_capture(retirement_capture, current_retirement_capture)) {
            auto result = failure(
                current_retirement_capture.ok() ? BasicDirectoryTransferStatus::staging_changed
                                                : current_retirement_capture.status,
                current_retirement_capture.ok() ? "retired unfinished data changed before removal"
                                                : current_retirement_capture.detail_utf8,
                current_retirement_capture.error, current_retirement_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = retirement;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_before_cleanup_quarantine_when_requested();
#endif
        auto quarantine = retirement;
        auto quarantined_capture = current_retirement_capture;
        if (retirement != quarantine_path) {
            auto quarantined = quarantine_owned_tree(retirement, current_retirement_capture,
                                                     quarantine, quarantined_capture);
            if (!quarantined.ok()) {
                quarantined.recovery_available = true;
                quarantined.staging_destination = quarantine.empty() ? retirement : quarantine;
                quarantined.destination = manifest.destination;
                quarantined.manifest_path = loaded.manifest_path;
                return quarantined;
            }
        }
        const auto final_source_capture = capture_tree(manifest.source);
        if (!same_tree_capture(current_source_capture, final_source_capture)) {
            auto result =
                failure(final_source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                  : final_source_capture.status,
                        final_source_capture.ok()
                            ? "source changed after unfinished data entered cleanup quarantine"
                            : final_source_capture.detail_utf8,
                        final_source_capture.error, final_source_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = quarantine;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        auto removed = remove_captured_owned_tree(quarantine, quarantined_capture);
        if (!removed.ok()) {
            removed.recovery_available = true;
            removed.staging_destination = quarantine;
            removed.destination = manifest.destination;
            removed.manifest_path = loaded.manifest_path;
            return removed;
        }
    }
    if (manifest.kind == FileTransferKind::move) {
        const auto proof_path = publication_proof_path(manifest);
        std::filesystem::remove(proof_path, error);
        if (error) {
            BasicDirectoryTransferResult result;
            result.status = map_transfer_error(error, destination_cifs);
            result.error = error;
            result.platform_code = error.value();
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            result.detail_utf8 = "stale publication proof could not be cleared before move restart";
            return result;
        }
    }
    if (progress) {
        try {
            progress({.phase = BasicDirectoryTransferPhase::enumerating,
                      .current_path = manifest.source});
        } catch (...) {
            static_cast<void>(std::current_exception());
        }
    }
    ExecutionBoundaryPhase boundary_phase{ExecutionBoundaryPhase::manifest_persisted};
    return release_and_finalize_success(
        execute_basic_directory_copy_impl(manifest, progress, boundary_phase, true, &lease,
                                          manifest.kind == FileTransferKind::move),
        true);
}

BasicDirectoryTransferResult resume_exception_result(
    const BasicDirectoryTransferManifest &manifest, const std::filesystem::path &manifest_path,
    const BasicDirectoryTransferStatus fallback_status, const std::error_code error = {},
    const std::int64_t platform_code = 0) noexcept {
    const auto phase = manifest.source.empty() ? ExecutionBoundaryPhase::none
                                               : ExecutionBoundaryPhase::manifest_persisted;
    auto result =
        execution_exception_result(manifest, phase, fallback_status, error, platform_code);
    if (result.manifest_path.empty()) {
        try {
            result.manifest_path = manifest_path;
        } catch (...) {
            static_cast<void>(std::current_exception());
        }
    }
    result.recovery_available = !manifest.source.empty();
    return result;
}

BasicDirectoryTransferResult discard_basic_directory_transfer_impl(
    const std::filesystem::path &manifest_path,
    const BasicDirectoryTransferProgressCallback &progress,
    BasicDirectoryTransferManifest &recovery_context,
    const BasicDirectoryTransferRecoveryIdentity *expected_identity) {
    if (!canonical_absolute_path(manifest_path)) {
        return failure(BasicDirectoryTransferStatus::invalid_request,
                       "directory discard requires an absolute manifest path");
    }
    auto loaded = load_basic_directory_transfer_manifest(manifest_path);
    if (!loaded.ok()) {
        return loaded;
    }
    recovery_context = loaded.manifest;
    const auto control_directory = loaded.manifest_path.parent_path();
    const auto destination_cifs = path_is_cifs(control_directory);
    std::error_code error;
    RecoveryManifestLease lease;
    bool lease_busy{};
    if (!lease.acquire(control_directory / "lease", error, lease_busy)) {
        auto result = failure(lease_busy ? BasicDirectoryTransferStatus::recovery_required
                                         : map_transfer_error(error, destination_cifs),
                              lease_busy ? "directory recovery is already active"
                                         : "directory recovery lease could not be acquired",
                              error, error.value());
        result.recovery_available = true;
        result.manifest_path = loaded.manifest_path;
        return result;
    }
    loaded = load_basic_directory_transfer_manifest(manifest_path);
    if (!loaded.ok()) {
        loaded.recovery_available = true;
        return loaded;
    }
    const auto &manifest = loaded.manifest;
    recovery_context = manifest;
    if (expected_identity != nullptr &&
        (expected_identity->operation_id == 0U || !expected_identity->source.is_absolute() ||
         !expected_identity->destination.is_absolute() ||
         expected_identity->source_revision_utf8.empty() ||
         manifest.operation_id != expected_identity->operation_id ||
         manifest.source != expected_identity->source ||
         manifest.destination != expected_identity->destination ||
         manifest.source_revision_utf8 != expected_identity->source_revision_utf8)) {
        auto result = failure(BasicDirectoryTransferStatus::recovery_required,
                              "recovery manifest identity does not match the requested operation");
        result.recovery_available = true;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        return result;
    }

    bool source_retirement_present{};
    auto observed = path_presence(source_retirement_path(manifest), path_is_cifs(manifest.source),
                                  source_retirement_present);
    if (!observed.ok()) {
        observed.recovery_available = true;
        observed.manifest_path = loaded.manifest_path;
        return observed;
    }
    bool source_present{};
    observed = path_presence(manifest.source, path_is_cifs(manifest.source), source_present);
    if (!observed.ok()) {
        observed.recovery_available = true;
        observed.manifest_path = loaded.manifest_path;
        return observed;
    }
    if (!source_present || source_retirement_present) {
        auto result = failure(
            BasicDirectoryTransferStatus::recovery_required,
            "unfinished data may be the only complete MOVE copy and cannot be discarded safely");
        result.recovery_available = true;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        return result;
    }

    bool atomic_marker_present{};
    observed = path_presence(atomic_move_marker_path(manifest.source),
                             path_is_cifs(manifest.source), atomic_marker_present);
    if (!observed.ok()) {
        observed.recovery_available = true;
        observed.manifest_path = loaded.manifest_path;
        return observed;
    }
    if (atomic_marker_present) {
        auto marker = load_atomic_move_evidence(atomic_move_marker_path(manifest.source), manifest,
                                                "atomic move ownership marker");
        if (!marker.ok()) {
            marker.recovery_available = true;
            marker.manifest_path = loaded.manifest_path;
            return marker;
        }
        marker = remove_atomic_move_marker(manifest.source, manifest);
        if (!marker.ok()) {
            marker.recovery_available = true;
            marker.manifest_path = loaded.manifest_path;
            return marker;
        }
    }
    const auto source_capture = capture_tree(manifest.source);
    if (!same_owned_tree_ignoring_root_metadata(manifest, source_capture)) {
        auto result =
            failure(source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                        : source_capture.status,
                    source_capture.ok() ? "source changed before unfinished data could be discarded"
                                        : source_capture.detail_utf8,
                    source_capture.error, source_capture.platform_code);
        result.recovery_available = true;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        return result;
    }

    const auto retirement_path = basic_directory_transfer_retirement_path(manifest);
    const auto quarantine_path = cleanup_quarantine_path(retirement_path);
    bool staging_present{};
    bool retirement_present{};
    observed = path_presence(manifest.staging_destination, destination_cifs, staging_present);
    if (observed.ok()) {
        observed = path_presence(retirement_path, destination_cifs, retirement_present);
    }
    bool quarantine_present{};
    if (observed.ok()) {
        observed = path_presence(quarantine_path, destination_cifs, quarantine_present);
    }
    if (!observed.ok()) {
        observed.recovery_available = true;
        observed.manifest_path = loaded.manifest_path;
        return observed;
    }
    if ((retirement_present && quarantine_present) ||
        (staging_present && (retirement_present || quarantine_present))) {
        auto result = failure(BasicDirectoryTransferStatus::staging_changed,
                              "multiple active, retired, or quarantined unfinished roots exist");
        result.recovery_available = true;
        result.staging_destination = manifest.staging_destination;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        return result;
    }
    const auto retirement = quarantine_present ? quarantine_path : retirement_path;
    retirement_present = retirement_present || quarantine_present;
    if (staging_present) {
        auto audited = audit_staging_for_restart(manifest);
        if (!audited.ok()) {
            audited.recovery_available = true;
            audited.staging_destination = manifest.staging_destination;
            audited.destination = manifest.destination;
            audited.manifest_path = loaded.manifest_path;
            return audited;
        }
        auto retired =
            rename_no_replace(manifest.staging_destination, retirement_path, destination_cifs);
        if (!retired.ok()) {
            auto result =
                failure(retired.status, retired.detail_utf8, retired.error, retired.platform_code);
            result.recovery_available = true;
            result.staging_destination = manifest.staging_destination;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        retirement_present = true;
    }
    if (retirement_present) {
        auto retired_manifest = manifest;
        retired_manifest.staging_destination = retirement;
        auto audited = audit_staging_for_restart(retired_manifest);
        if (!audited.ok()) {
            audited.recovery_available = true;
            audited.staging_destination = retirement;
            audited.destination = manifest.destination;
            audited.manifest_path = loaded.manifest_path;
            return audited;
        }
        const auto retirement_capture = capture_owned_staging_tree(retirement);
        if (!retirement_capture.ok()) {
            auto result = failure(retirement_capture.status,
                                  "retired unfinished data could not be captured before removal",
                                  retirement_capture.error, retirement_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = retirement;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_after_source_cleanup_validation_when_requested();
        pause_after_staging_retirement_when_requested();
#endif
        const auto current_source_capture = capture_tree(manifest.source);
        if (!same_tree_capture(source_capture, current_source_capture)) {
            auto result =
                failure(current_source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                    : current_source_capture.status,
                        current_source_capture.ok()
                            ? "source changed before retired unfinished data could be removed"
                            : current_source_capture.detail_utf8,
                        current_source_capture.error, current_source_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = retirement;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        const auto current_retirement_capture = capture_owned_staging_tree(retirement);
        if (!same_tree_capture(retirement_capture, current_retirement_capture)) {
            auto result = failure(
                current_retirement_capture.ok() ? BasicDirectoryTransferStatus::staging_changed
                                                : current_retirement_capture.status,
                current_retirement_capture.ok() ? "retired unfinished data changed before removal"
                                                : current_retirement_capture.detail_utf8,
                current_retirement_capture.error, current_retirement_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = retirement;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
        pause_before_cleanup_quarantine_when_requested();
#endif
        auto quarantine = retirement;
        auto quarantined_capture = current_retirement_capture;
        if (retirement != quarantine_path) {
            auto quarantined = quarantine_owned_tree(retirement, current_retirement_capture,
                                                     quarantine, quarantined_capture);
            if (!quarantined.ok()) {
                quarantined.recovery_available = true;
                quarantined.staging_destination = quarantine.empty() ? retirement : quarantine;
                quarantined.destination = manifest.destination;
                quarantined.manifest_path = loaded.manifest_path;
                return quarantined;
            }
        }
        const auto final_source_capture = capture_tree(manifest.source);
        if (!same_tree_capture(current_source_capture, final_source_capture)) {
            auto result =
                failure(final_source_capture.ok() ? BasicDirectoryTransferStatus::source_changed
                                                  : final_source_capture.status,
                        final_source_capture.ok()
                            ? "source changed after unfinished data entered cleanup quarantine"
                            : final_source_capture.detail_utf8,
                        final_source_capture.error, final_source_capture.platform_code);
            result.recovery_available = true;
            result.staging_destination = quarantine;
            result.destination = manifest.destination;
            result.manifest_path = loaded.manifest_path;
            return result;
        }
        auto removed = remove_captured_owned_tree(quarantine, quarantined_capture);
        if (!removed.ok()) {
            removed.recovery_available = true;
            removed.staging_destination = quarantine;
            removed.destination = manifest.destination;
            removed.manifest_path = loaded.manifest_path;
            return removed;
        }
    }

    report_progress(progress, BasicDirectoryTransferPhase::completed, manifest, 0U, 0U,
                    manifest.staging_destination);
    if (!retire_manifest_before_lease_release(lease, loaded.manifest_path, error)) {
        auto result = failure(map_transfer_error(error, destination_cifs),
                              "unfinished data was removed, but recovery control remains", error,
                              error.value());
        result.recovery_available = true;
        result.destination = manifest.destination;
        result.manifest_path = loaded.manifest_path;
        return result;
    }
    auto result = failure(BasicDirectoryTransferStatus::success,
                          "verified unfinished directory transfer was discarded");
    result.destination = manifest.destination;
    return result;
}

} // namespace

BasicDirectoryTransferResult resume_basic_directory_transfer_impl(
    const std::filesystem::path &manifest_path,
    const BasicDirectoryTransferProgressCallback &progress,
    const std::optional<FileTransferKind> expected_kind,
    const BasicDirectoryTransferRecoveryIdentity *expected_identity) {
    BasicDirectoryTransferManifest recovery_context;
    const auto destination_cifs = path_is_cifs(manifest_path.parent_path());
    try {
        auto result = resume_basic_directory_copy_impl(manifest_path, progress, recovery_context,
                                                       expected_kind, expected_identity);
        if (!result.ok() && !recovery_context.source.empty()) {
            result.recovery_available = true;
        }
        return result;
    } catch (const std::bad_alloc &) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::unsupported);
    } catch (const std::length_error &) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::unsupported);
    } catch (const std::filesystem::filesystem_error &exception) {
        return resume_exception_result(recovery_context, manifest_path,
                                       map_transfer_error(exception.code(), destination_cifs),
                                       exception.code(), exception.code().value());
    } catch (...) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::io_error);
    }
}

BasicDirectoryTransferResult
resume_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                const BasicDirectoryTransferProgressCallback &progress) {
    return resume_basic_directory_transfer_impl(manifest_path, progress, std::nullopt, nullptr);
}

BasicDirectoryTransferResult
resume_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                const BasicDirectoryTransferRecoveryIdentity &expected_identity,
                                const BasicDirectoryTransferProgressCallback &progress) {
    return resume_basic_directory_transfer_impl(manifest_path, progress, std::nullopt,
                                                &expected_identity);
}

BasicDirectoryTransferResult
resume_basic_directory_copy(const std::filesystem::path &manifest_path,
                            const BasicDirectoryTransferProgressCallback &progress) {
    return resume_basic_directory_transfer_impl(manifest_path, progress, FileTransferKind::copy,
                                                nullptr);
}

BasicDirectoryTransferResult
resume_basic_directory_move(const std::filesystem::path &manifest_path,
                            const BasicDirectoryTransferProgressCallback &progress) {
    return resume_basic_directory_transfer_impl(manifest_path, progress, FileTransferKind::move,
                                                nullptr);
}

BasicDirectoryTransferResult
discard_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                 const BasicDirectoryTransferProgressCallback &progress) {
    BasicDirectoryTransferManifest recovery_context;
    const auto destination_cifs = path_is_cifs(manifest_path.parent_path());
    try {
        return discard_basic_directory_transfer_impl(manifest_path, progress, recovery_context,
                                                     nullptr);
    } catch (const std::bad_alloc &) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::unsupported);
    } catch (const std::length_error &) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::unsupported);
    } catch (const std::filesystem::filesystem_error &exception) {
        return resume_exception_result(recovery_context, manifest_path,
                                       map_transfer_error(exception.code(), destination_cifs),
                                       exception.code(), exception.code().value());
    } catch (...) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::io_error);
    }
}

BasicDirectoryTransferResult
discard_basic_directory_transfer(const std::filesystem::path &manifest_path,
                                 const BasicDirectoryTransferRecoveryIdentity &expected_identity,
                                 const BasicDirectoryTransferProgressCallback &progress) {
    BasicDirectoryTransferManifest recovery_context;
    const auto destination_cifs = path_is_cifs(manifest_path.parent_path());
    try {
        return discard_basic_directory_transfer_impl(manifest_path, progress, recovery_context,
                                                     &expected_identity);
    } catch (const std::bad_alloc &) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::unsupported);
    } catch (const std::length_error &) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::unsupported);
    } catch (const std::filesystem::filesystem_error &exception) {
        return resume_exception_result(recovery_context, manifest_path,
                                       map_transfer_error(exception.code(), destination_cifs),
                                       exception.code(), exception.code().value());
    } catch (...) {
        return resume_exception_result(recovery_context, manifest_path,
                                       BasicDirectoryTransferStatus::io_error);
    }
}

#ifdef VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS
void basic_directory_transfer_test_fail_once(
    const BasicDirectoryTransferTestFailurePoint point) noexcept {
    failurePointOnce = point;
}

BasicDirectoryTransferResult execute_basic_directory_move_strict_for_test(
    const BasicDirectoryTransferManifest &manifest,
    const BasicDirectoryTransferProgressCallback &progress) {
    return execute_basic_directory_move_mode(manifest, progress, false);
}

void basic_directory_transfer_test_lose_staging_reply_once() noexcept {
    loseStagingReplyOnce = true;
}

void basic_directory_transfer_test_lose_final_reply_once() noexcept {
    loseFinalReplyOnce = true;
}

void basic_directory_transfer_test_lose_source_retirement_reply_once() noexcept {
    loseSourceRetirementReplyOnce = true;
}

void basic_directory_transfer_test_pause_after_source_cleanup_validation_once() noexcept {
    releaseSourceCleanupValidation = false;
    sourceCleanupValidationPaused = false;
    pauseAfterSourceCleanupValidationOnce = true;
}

bool basic_directory_transfer_test_source_cleanup_validation_is_paused() noexcept {
    return sourceCleanupValidationPaused.load();
}

void basic_directory_transfer_test_release_source_cleanup_validation() noexcept {
    releaseSourceCleanupValidation = true;
}

void basic_directory_transfer_test_pause_before_cleanup_quarantine_once() noexcept {
    releaseCleanupQuarantine = false;
    cleanupQuarantinePaused = false;
    pauseBeforeCleanupQuarantineOnce = true;
}

bool basic_directory_transfer_test_cleanup_quarantine_is_paused() noexcept {
    return cleanupQuarantinePaused.load();
}

void basic_directory_transfer_test_release_cleanup_quarantine() noexcept {
    releaseCleanupQuarantine = true;
}

void basic_directory_transfer_test_pause_after_atomic_move_marker_check_once() noexcept {
    releaseAtomicMoveMarkerCheck = false;
    atomicMoveMarkerCheckPaused = false;
    pauseAfterAtomicMoveMarkerCheckOnce = true;
}

bool basic_directory_transfer_test_atomic_move_marker_check_is_paused() noexcept {
    return atomicMoveMarkerCheckPaused.load();
}

void basic_directory_transfer_test_release_atomic_move_marker_check() noexcept {
    releaseAtomicMoveMarkerCheck = true;
}

void basic_directory_transfer_test_pause_after_source_entry_capture_once() noexcept {
    releaseSourceEntryCapture = false;
    sourceEntryCapturePaused = false;
    pauseAfterSourceEntryCaptureOnce = true;
}

bool basic_directory_transfer_test_source_entry_capture_is_paused() noexcept {
    return sourceEntryCapturePaused.load();
}

void basic_directory_transfer_test_release_source_entry_capture() noexcept {
    releaseSourceEntryCapture = true;
}

void basic_directory_transfer_test_pause_after_staging_retirement_once() noexcept {
    releaseStagingRetirement = false;
    stagingRetirementPaused = false;
    pauseAfterStagingRetirementOnce = true;
}

bool basic_directory_transfer_test_staging_retirement_is_paused() noexcept {
    return stagingRetirementPaused.load();
}

void basic_directory_transfer_test_release_staging_retirement() noexcept {
    releaseStagingRetirement = true;
}

void basic_directory_transfer_test_pause_after_manifest_retirement_once() noexcept {
    releaseManifestRetirement = false;
    manifestRetirementPaused = false;
    pauseAfterManifestRetirementOnce = true;
}

bool basic_directory_transfer_test_manifest_retirement_is_paused() noexcept {
    return manifestRetirementPaused.load();
}

void basic_directory_transfer_test_release_manifest_retirement() noexcept {
    releaseManifestRetirement = true;
}
#endif

} // namespace vove::fileops
