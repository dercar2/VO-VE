#pragma once

#include "vove/preview/thumbnail_types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace vove::preview {

using FolderMosaicChildId = std::uint64_t;
using FolderMosaicClock = std::chrono::steady_clock;

inline constexpr std::size_t kMaximumFolderMosaicEntries = 64;
inline constexpr std::size_t kMaximumFolderMosaicCandidates = 4;
inline constexpr auto kFolderMosaicBudget = std::chrono::milliseconds(2'000);
inline constexpr std::uint8_t kFolderMosaicGridRows = 2;
inline constexpr std::uint8_t kFolderMosaicGridColumns = 2;
inline constexpr ThumbnailPriority kFolderMosaicPriority = ThumbnailPriority::folder;

static_assert(static_cast<std::uint8_t>(ThumbnailPriority::visible) <
                  static_cast<std::uint8_t>(kFolderMosaicPriority) &&
              static_cast<std::uint8_t>(kFolderMosaicPriority) <
                  static_cast<std::uint8_t>(ThumbnailPriority::nearby));

enum class FolderMosaicChildKind : std::uint8_t {
    regular_file,
    directory,
    other,
};

struct FolderMosaicChild {
    FolderMosaicChildId id{};
    FolderMosaicChildKind kind{FolderMosaicChildKind::other};
};

enum class FolderMosaicChildDisposition : std::uint8_t {
    accepted,
    ignored_non_file,
    ignored_duplicate,
    limit_reached,
    finished,
    deadline,
};

enum class FolderMosaicProbeOutcome : std::uint8_t {
    image,
    unsupported,
    error,
};

struct FolderMosaicProbeRequest {
    FolderMosaicChildId child_id{};
    ThumbnailPriority priority{kFolderMosaicPriority};
};

struct FolderMosaicProbeResult {
    FolderMosaicChildId child_id{};
    FolderMosaicProbeOutcome outcome{FolderMosaicProbeOutcome::error};
};

enum class FolderMosaicProbeDisposition : std::uint8_t {
    accepted,
    unexpected_child,
    finished,
    deadline,
};

enum class FolderMosaicOutcome : std::uint8_t {
    complete,
    partial,
    deadline,
    no_candidates,
};

struct FolderMosaicTile {
    std::uint8_t candidate_index{};
    std::uint8_t row{};
    std::uint8_t column{};
    std::uint8_t row_span{1};
    std::uint8_t column_span{1};
};

struct FolderMosaicLayout {
    std::uint8_t rows{kFolderMosaicGridRows};
    std::uint8_t columns{kFolderMosaicGridColumns};
    std::uint8_t tile_count{};
    std::array<FolderMosaicTile, kMaximumFolderMosaicCandidates> tiles{};
};

struct FolderMosaicResult {
    FolderMosaicOutcome outcome{FolderMosaicOutcome::no_candidates};
    std::size_t entries_examined{};
    std::size_t candidate_count{};
    std::array<FolderMosaicChildId, kMaximumFolderMosaicCandidates> candidates{};
    FolderMosaicLayout layout;
};

struct FolderMosaicPendingProbes {
    std::array<FolderMosaicChildId, kMaximumFolderMosaicCandidates> child_ids{};
    std::size_t count{};
};

class FolderMosaicBuilder {
  public:
    using TimePoint = FolderMosaicClock::time_point;

    explicit FolderMosaicBuilder(TimePoint deadline) noexcept;

    [[nodiscard]] FolderMosaicChildDisposition push_child(FolderMosaicChild child,
                                                          TimePoint now) noexcept;
    void finish_children(TimePoint now) noexcept;

    [[nodiscard]] std::optional<FolderMosaicProbeRequest> next_probe(TimePoint now) noexcept;
    [[nodiscard]] FolderMosaicProbeDisposition submit_probe(FolderMosaicProbeResult probe,
                                                            TimePoint now) noexcept;

    [[nodiscard]] std::optional<FolderMosaicResult> result(TimePoint now) noexcept;
    [[nodiscard]] std::size_t entries_examined() const noexcept;
    [[nodiscard]] std::size_t probes_started() const noexcept;
    [[nodiscard]] FolderMosaicPendingProbes pending_probes() const noexcept;

  private:
    enum class ProbeState : std::uint8_t {
        available,
        pending,
        image,
        rejected,
    };

    struct ProbeChild {
        FolderMosaicChildId id{};
        ProbeState state{ProbeState::available};
    };

    [[nodiscard]] bool expire(TimePoint now) noexcept;
    [[nodiscard]] bool duplicate(FolderMosaicChildId child_id) const noexcept;
    void settle() noexcept;
    void finalize(FolderMosaicOutcome outcome) noexcept;

    TimePoint deadline_;
    std::array<FolderMosaicChildId, kMaximumFolderMosaicEntries> observedChildIds_{};
    std::array<ProbeChild, kMaximumFolderMosaicEntries> probeChildren_{};
    std::size_t entriesExamined_{};
    std::size_t probeChildCount_{};
    std::size_t probesStarted_{};
    bool childrenFinished_{};
    std::optional<FolderMosaicResult> result_;
};

} // namespace vove::preview
