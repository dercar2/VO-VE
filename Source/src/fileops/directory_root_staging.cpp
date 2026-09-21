#include "vove/fileops/directory_root_staging.hpp"

#include "catalog_protocol_codec.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::array<std::uint8_t, 8> markerMagic{'V', 'O', 'V', 'E', 'D', 'R', 'O', '1'};
constexpr std::size_t markerReservedBytes = 8U;
static_assert(markerMagic.size() + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
                  kDirectoryTransferManifestDigestBytes + sizeof(std::uint32_t) +
                  kDirectoryTransferOwnershipTokenBytes + markerReservedBytes +
                  detail::kSha256DigestBytes ==
              kDirectoryRootOwnershipMarkerBytes);

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

bool canonical_path(const std::filesystem::path &path) {
    if (!path.is_absolute() || path != path.lexically_normal()) {
        return false;
    }
    return std::ranges::none_of(path, [](const auto &part) { return part == "." || part == ".."; });
}

bool parent_anchor_matches(const DirectoryRootStagingBinding &binding,
                           const std::string_view revision) {
    try {
        return directory_transfer_canonical_object_identity(revision, binding.path_semantics) ==
               binding.destination_parent_identity_utf8;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace

bool valid_directory_root_staging_binding(const DirectoryRootStagingBinding &binding,
                                          std::string &detail_utf8) {
    detail_utf8.clear();
    if (binding.version != kDirectoryRootStagingVersion || binding.parent_operation_id == 0U ||
        binding.root_index >= kMaximumDirectoryTransferRoots ||
        bytes_empty(binding.parent_manifest_digest) || bytes_empty(binding.ownership_token)) {
        detail_utf8 = "directory-root staging identity is invalid";
        return false;
    }
#ifdef _WIN32
    constexpr auto native_semantics = DirectoryPathSemantics::windows_ordinal_nfc;
#else
    constexpr auto native_semantics = DirectoryPathSemantics::posix_exact;
#endif
    const auto expected_path = directory_transfer_staging_destination_path(
        binding.staging_root.parent_path() / "manifest-destination", binding.parent_operation_id,
        binding.root_index, binding.ownership_token);
    if (binding.path_semantics != native_semantics || !canonical_path(binding.staging_root) ||
        binding.staging_root != expected_path) {
        detail_utf8 = "directory-root staging path is invalid";
        return false;
    }
    try {
        const auto canonical = directory_transfer_canonical_object_identity(
            binding.destination_parent_identity_utf8, binding.path_semantics);
        if (canonical.empty() || canonical != binding.destination_parent_identity_utf8) {
            detail_utf8 = "directory-root staging parent identity is not canonical";
            return false;
        }
    } catch (const std::exception &) {
        detail_utf8 = "directory-root staging parent identity is invalid";
        return false;
    }
    return true;
}

DirectoryRootStagingRequest
prepare_directory_root_staging_request(const DirectoryTransferProgressPlan &plan,
                                       const DirectoryTransferProgressState &state) {
    std::string state_detail;
    if (!directory_transfer_progress_state_matches(plan, state, state_detail) || state.complete() ||
        state.phase() != DirectoryTransferRootProgressPhase::awaiting_staging ||
        state.completed_roots() >= plan.manifest().roots.size()) {
        throw std::invalid_argument(state_detail.empty()
                                        ? "directory-root staging is not at the parent frontier"
                                        : state_detail);
    }
    const auto root_index = state.completed_roots();
    const auto &root = plan.manifest().roots[root_index];
    return {.binding = {.parent_operation_id = plan.header().operation_id,
                        .parent_manifest_digest = plan.header().manifest_digest,
                        .root_index = root_index,
                        .path_semantics = plan.manifest().path_semantics,
                        .staging_root = root.staging_destination,
                        .destination_parent_identity_utf8 = root.destination_parent_identity_utf8,
                        .ownership_token = root.ownership_token},
            .destination_parent_revision_utf8 = root.destination_parent_revision_utf8};
}

bool directory_root_staging_request_matches(const DirectoryTransferProgressPlan &plan,
                                            const DirectoryTransferProgressState &state,
                                            const DirectoryRootStagingRequest &request,
                                            std::string &detail_utf8) {
    DirectoryRootStagingRequest expected;
    try {
        expected = prepare_directory_root_staging_request(plan, state);
    } catch (const std::exception &error) {
        detail_utf8 = error.what();
        return false;
    }
    if (!valid_directory_root_staging_binding(request.binding, detail_utf8) ||
        request.binding.version != expected.binding.version ||
        request.binding.parent_operation_id != expected.binding.parent_operation_id ||
        request.binding.parent_manifest_digest != expected.binding.parent_manifest_digest ||
        request.binding.root_index != expected.binding.root_index ||
        request.binding.path_semantics != expected.binding.path_semantics ||
        request.binding.staging_root != expected.binding.staging_root ||
        request.binding.destination_parent_identity_utf8 !=
            expected.binding.destination_parent_identity_utf8 ||
        request.binding.ownership_token != expected.binding.ownership_token ||
        request.destination_parent_revision_utf8 != expected.destination_parent_revision_utf8 ||
        !parent_anchor_matches(request.binding, request.destination_parent_revision_utf8)) {
        detail_utf8 = "directory-root staging request is not bound to the manifest frontier";
        return false;
    }
    return true;
}

DirectoryRootOwnershipMarker
directory_root_ownership_marker_for(const DirectoryRootStagingBinding &binding) {
    return {.parent_operation_id = binding.parent_operation_id,
            .parent_manifest_digest = binding.parent_manifest_digest,
            .root_index = binding.root_index,
            .ownership_token = binding.ownership_token};
}

bool valid_directory_root_ownership_marker(const DirectoryRootOwnershipMarker &marker,
                                           std::string &detail_utf8) {
    detail_utf8.clear();
    if (marker.version != kDirectoryRootOwnershipMarkerVersion ||
        marker.parent_operation_id == 0U || marker.root_index >= kMaximumDirectoryTransferRoots ||
        bytes_empty(marker.parent_manifest_digest) || bytes_empty(marker.ownership_token)) {
        detail_utf8 = "directory-root ownership marker is invalid";
        return false;
    }
    return true;
}

bool directory_root_ownership_marker_matches(const DirectoryRootOwnershipMarker &marker,
                                             const DirectoryRootStagingBinding &binding,
                                             std::string &detail_utf8) {
    if (!valid_directory_root_staging_binding(binding, detail_utf8) ||
        !valid_directory_root_ownership_marker(marker, detail_utf8) ||
        marker.parent_operation_id != binding.parent_operation_id ||
        marker.parent_manifest_digest != binding.parent_manifest_digest ||
        marker.root_index != binding.root_index ||
        marker.ownership_token != binding.ownership_token) {
        detail_utf8 = "directory-root ownership marker does not match its manifest binding";
        return false;
    }
    return true;
}

std::vector<std::byte>
encode_directory_root_ownership_marker(const DirectoryRootOwnershipMarker &marker) {
    std::string detail_utf8;
    if (!valid_directory_root_ownership_marker(marker, detail_utf8)) {
        throw std::invalid_argument(detail_utf8);
    }
    std::vector<std::byte> payload;
    payload.reserve(kDirectoryRootOwnershipMarkerBytes);
    append_bytes(payload, markerMagic);
    append_integer(payload, marker.version);
    append_integer(payload, marker.parent_operation_id);
    append_bytes(payload, marker.parent_manifest_digest);
    append_integer(payload, marker.root_index);
    append_bytes(payload, marker.ownership_token);
    payload.insert(payload.end(), markerReservedBytes, std::byte{});
    append_bytes(payload, detail::sha256(payload));
    return payload;
}

bool decode_directory_root_ownership_marker(const std::span<const std::byte> payload,
                                            DirectoryRootOwnershipMarker &marker,
                                            std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() != kDirectoryRootOwnershipMarkerBytes) {
        detail_utf8 = "directory-root ownership marker size is invalid";
        return false;
    }
    const auto body = payload.first(payload.size() - detail::kSha256DigestBytes);
    const auto digest = detail::sha256(body);
    if (!std::ranges::equal(digest, payload.last(digest.size()), {},
                            [](const auto value) { return static_cast<std::byte>(value); })) {
        detail_utf8 = "directory-root ownership marker digest is invalid";
        return false;
    }
    Cursor cursor(body);
    std::array<std::uint8_t, markerMagic.size()> magic{};
    DirectoryRootOwnershipMarker decoded;
    if (!read_bytes(cursor, magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.parent_operation_id) ||
        !read_bytes(cursor, decoded.parent_manifest_digest) || !cursor.read(decoded.root_index) ||
        !read_bytes(cursor, decoded.ownership_token) || magic != markerMagic) {
        detail_utf8 = "directory-root ownership marker is invalid or truncated";
        return false;
    }
    for (std::size_t index{}; index < markerReservedBytes; ++index) {
        std::uint8_t reserved{};
        if (!cursor.read(reserved) || reserved != 0U) {
            detail_utf8 = "directory-root ownership marker reserved bytes are invalid";
            return false;
        }
    }
    if (cursor.remaining() != 0U || !valid_directory_root_ownership_marker(decoded, detail_utf8)) {
        return false;
    }
    marker = decoded;
    return true;
}

} // namespace vove::fileops
