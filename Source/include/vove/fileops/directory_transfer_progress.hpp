#pragma once

#include "vove/fileops/directory_transfer_control.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kDirectoryTransferProgressVersion = 1U;
inline constexpr std::size_t kDirectoryTransferProgressDigestBytes = 32U;
inline constexpr std::size_t kDirectoryTransferProgressHeaderBytes = 128U;
inline constexpr std::size_t kDirectoryTransferProgressRecordBytes = 176U;
inline constexpr std::uint32_t kDirectoryTransferProgressNoEntry = 0xffffffffU;
inline constexpr std::size_t kMaximumDirectoryTransferProgressRecords =
    kMaximumDirectoryTransferEntries + 2U * kMaximumDirectoryTransferRoots;
inline constexpr std::size_t kMaximumDirectoryTransferProgressBytes =
    kDirectoryTransferProgressHeaderBytes +
    kMaximumDirectoryTransferProgressRecords * kDirectoryTransferProgressRecordBytes;

using DirectoryTransferProgressDigest =
    std::array<std::uint8_t, kDirectoryTransferProgressDigestBytes>;
using DirectoryTransferProgressOwnershipToken = DirectoryTransferOwnershipToken;

enum class DirectoryTransferProgressEventKind : std::uint8_t {
    root_staging_owned = 1U,
    entry_materialized = 2U,
    root_published = 3U,
};

enum class DirectoryTransferProgressObjectKind : std::uint8_t {
    root = 1U,
    directory = 2U,
    regular_file = 3U,
};

enum class DirectoryTransferRootProgressPhase : std::uint8_t {
    awaiting_staging,
    materializing,
    complete,
};

struct DirectoryTransferProgressHeader {
    std::uint32_t version{kDirectoryTransferProgressVersion};
    std::uint16_t header_bytes{kDirectoryTransferProgressHeaderBytes};
    std::uint16_t record_bytes{kDirectoryTransferProgressRecordBytes};
    std::uint8_t hash_algorithm{1U};
    FileTransferKind kind{FileTransferKind::copy};
    std::uint16_t flags{};
    std::uint32_t root_count{};
    std::uint32_t entry_count{};
    std::uint64_t operation_id{};
    std::uint64_t manifest_encoded_size{};
    DirectoryTransferManifestDigest manifest_digest{};
    std::uint32_t maximum_record_count{};
    DirectoryTransferProgressDigest header_sha256{};
};

struct DirectoryTransferProgressRecord {
    std::uint8_t version{static_cast<std::uint8_t>(kDirectoryTransferProgressVersion)};
    DirectoryTransferProgressEventKind kind{DirectoryTransferProgressEventKind::root_staging_owned};
    DirectoryTransferProgressObjectKind object_kind{DirectoryTransferProgressObjectKind::root};
    std::uint64_t sequence{};
    std::uint32_t root_index{};
    std::uint32_t entry_index{kDirectoryTransferProgressNoEntry};
    std::uint64_t content_bytes{};
    DirectoryTransferProgressOwnershipToken ownership_token{};
    DirectoryTransferProgressDigest object_identity_sha256{};
    DirectoryTransferProgressDigest content_sha256{};
    DirectoryTransferProgressDigest previous_record_sha256{};
    DirectoryTransferProgressDigest record_sha256{};
};

struct DirectoryTransferRootStagingEvidence {
    std::uint32_t root_index{};
    DirectoryTransferProgressOwnershipToken ownership_token{};
    std::string object_revision_utf8;
};

struct DirectoryTransferEntryEvidence {
    std::uint32_t root_index{};
    std::uint32_t entry_index{};
    DirectoryTransferProgressObjectKind object_kind{DirectoryTransferProgressObjectKind::directory};
    std::uint64_t content_bytes{};
    std::string object_revision_utf8;
    // SHA-256 of the unmodified file bytes; zero for a directory.
    DirectoryTransferProgressDigest content_sha256{};
};

struct DirectoryTransferRootPublicationEvidence {
    std::uint32_t root_index{};
    std::uint64_t content_bytes{};
    DirectoryTransferProgressOwnershipToken ownership_token{};
    std::string destination_revision_utf8;
    // Produced by a fresh full-tree audit after materialization, never by the progress reducer.
    DirectoryTransferProgressDigest audited_tree_sha256{};
};

class DirectoryTransferProgressPlan final {
  public:
    DirectoryTransferProgressPlan(const DirectoryTransferProgressPlan &) = default;
    DirectoryTransferProgressPlan &operator=(const DirectoryTransferProgressPlan &) = default;
    DirectoryTransferProgressPlan(DirectoryTransferProgressPlan &&) noexcept = default;
    DirectoryTransferProgressPlan &operator=(DirectoryTransferProgressPlan &&) noexcept = default;

    [[nodiscard]] const DirectoryTransferControlRecord &control() const noexcept;
    [[nodiscard]] const DirectoryTransferManifest &manifest() const noexcept;
    [[nodiscard]] const DirectoryTransferProgressHeader &header() const noexcept;

  private:
    DirectoryTransferProgressPlan(DirectoryTransferControlRecord control,
                                  DirectoryTransferManifest manifest,
                                  DirectoryTransferProgressHeader header);

    DirectoryTransferControlRecord control_;
    DirectoryTransferManifest manifest_;
    DirectoryTransferProgressHeader header_;

    friend DirectoryTransferProgressPlan
    prepare_directory_transfer_progress_plan(const DirectoryTransferControlRecord &,
                                             DirectoryTransferManifest);
    friend std::optional<DirectoryTransferProgressPlan>
    bind_directory_transfer_progress_plan(const DirectoryTransferProgressHeader &,
                                          const DirectoryTransferControlRecord &,
                                          DirectoryTransferManifest, std::string &);
};

class DirectoryTransferProgressState final {
  public:
    DirectoryTransferProgressState(const DirectoryTransferProgressState &) = default;
    DirectoryTransferProgressState &operator=(const DirectoryTransferProgressState &) = default;
    DirectoryTransferProgressState(DirectoryTransferProgressState &&) noexcept = default;
    DirectoryTransferProgressState &operator=(DirectoryTransferProgressState &&) noexcept = default;

    [[nodiscard]] std::uint64_t next_sequence() const noexcept;
    [[nodiscard]] std::uint32_t completed_roots() const noexcept;
    [[nodiscard]] std::uint32_t next_manifest_entry() const noexcept;
    [[nodiscard]] DirectoryTransferRootProgressPhase phase() const noexcept;
    [[nodiscard]] const DirectoryTransferProgressOwnershipToken &
    active_ownership_token() const noexcept;
    [[nodiscard]] const DirectoryTransferProgressDigest &
    active_root_identity_sha256() const noexcept;
    [[nodiscard]] const DirectoryTransferProgressDigest &rolling_tree_sha256() const noexcept;
    [[nodiscard]] bool complete() const noexcept;

  private:
    DirectoryTransferProgressState() = default;

    std::uint64_t operation_id_{};
    std::uint32_t root_count_{};
    std::uint32_t entry_count_{};
    std::uint64_t next_sequence_{1U};
    std::uint32_t completed_roots_{};
    std::uint32_t next_manifest_entry_{};
    std::uint64_t active_content_bytes_{};
    DirectoryTransferRootProgressPhase phase_{DirectoryTransferRootProgressPhase::awaiting_staging};
    DirectoryTransferProgressDigest last_record_sha256_{};
    DirectoryTransferManifestDigest manifest_digest_{};
    DirectoryTransferProgressOwnershipToken active_ownership_token_{};
    DirectoryTransferProgressDigest active_root_identity_sha256_{};
    DirectoryTransferProgressDigest rolling_tree_sha256_{};

    friend struct DirectoryTransferProgressStateAccess;
    friend DirectoryTransferProgressState
    initialize_directory_transfer_progress(const DirectoryTransferProgressPlan &);
    friend bool advance_directory_transfer_progress(const DirectoryTransferProgressPlan &,
                                                    const DirectoryTransferRootStagingEvidence &,
                                                    DirectoryTransferProgressState &,
                                                    DirectoryTransferProgressRecord &,
                                                    std::string &);
    friend bool advance_directory_transfer_progress(const DirectoryTransferProgressPlan &,
                                                    const DirectoryTransferEntryEvidence &,
                                                    DirectoryTransferProgressState &,
                                                    DirectoryTransferProgressRecord &,
                                                    std::string &);
    friend bool advance_directory_transfer_progress(
        const DirectoryTransferProgressPlan &, const DirectoryTransferRootPublicationEvidence &,
        DirectoryTransferProgressState &, DirectoryTransferProgressRecord &, std::string &);
    friend std::optional<DirectoryTransferProgressState>
    replay_directory_transfer_progress(const DirectoryTransferProgressPlan &,
                                       std::span<const DirectoryTransferProgressRecord>,
                                       std::string &);
    friend bool directory_transfer_progress_state_matches(const DirectoryTransferProgressPlan &,
                                                          const DirectoryTransferProgressState &,
                                                          std::string &);
};

// SHA-256("VOVE-DT-ID-v1" || path-semantics || canonical stable native identity).
[[nodiscard]] DirectoryTransferProgressDigest
directory_transfer_object_identity_sha256(DirectoryPathSemantics semantics,
                                          std::string_view source_revision_utf8);

[[nodiscard]] DirectoryTransferProgressPlan
prepare_directory_transfer_progress_plan(const DirectoryTransferControlRecord &control,
                                         DirectoryTransferManifest manifest);
[[nodiscard]] std::optional<DirectoryTransferProgressPlan>
bind_directory_transfer_progress_plan(const DirectoryTransferProgressHeader &header,
                                      const DirectoryTransferControlRecord &control,
                                      DirectoryTransferManifest manifest, std::string &detail_utf8);

[[nodiscard]] std::vector<std::byte>
encode_directory_transfer_progress_header(const DirectoryTransferProgressHeader &header);
[[nodiscard]] bool
decode_directory_transfer_progress_header(std::span<const std::byte> payload,
                                          DirectoryTransferProgressHeader &header,
                                          std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_directory_transfer_progress_record(const DirectoryTransferProgressRecord &record);
[[nodiscard]] bool
decode_directory_transfer_progress_record(std::span<const std::byte> payload,
                                          DirectoryTransferProgressRecord &record,
                                          std::string &detail_utf8);

[[nodiscard]] DirectoryTransferProgressState
initialize_directory_transfer_progress(const DirectoryTransferProgressPlan &plan);
[[nodiscard]] bool
directory_transfer_progress_state_matches(const DirectoryTransferProgressPlan &plan,
                                          const DirectoryTransferProgressState &state,
                                          std::string &detail_utf8);
[[nodiscard]] bool advance_directory_transfer_progress(
    const DirectoryTransferProgressPlan &plan, const DirectoryTransferRootStagingEvidence &evidence,
    DirectoryTransferProgressState &state, DirectoryTransferProgressRecord &record,
    std::string &detail_utf8);
[[nodiscard]] bool advance_directory_transfer_progress(
    const DirectoryTransferProgressPlan &plan, const DirectoryTransferEntryEvidence &evidence,
    DirectoryTransferProgressState &state, DirectoryTransferProgressRecord &record,
    std::string &detail_utf8);
[[nodiscard]] bool advance_directory_transfer_progress(
    const DirectoryTransferProgressPlan &plan,
    const DirectoryTransferRootPublicationEvidence &evidence, DirectoryTransferProgressState &state,
    DirectoryTransferProgressRecord &record, std::string &detail_utf8);
[[nodiscard]] std::optional<DirectoryTransferProgressState>
replay_directory_transfer_progress(const DirectoryTransferProgressPlan &plan,
                                   std::span<const DirectoryTransferProgressRecord> records,
                                   std::string &detail_utf8);

} // namespace vove::fileops
