#include "vove/preview/folder_mosaic.hpp"

#include <algorithm>

namespace vove::preview {
namespace {

FolderMosaicLayout make_layout(const std::size_t candidate_count) noexcept {
    FolderMosaicLayout layout;
    layout.tile_count = static_cast<std::uint8_t>(candidate_count);

    switch (candidate_count) {
    case 1:
        layout.tiles[0] = {
            .candidate_index = 0, .row = 0, .column = 0, .row_span = 2, .column_span = 2};
        break;
    case 2:
        layout.tiles[0] = {
            .candidate_index = 0, .row = 0, .column = 0, .row_span = 2, .column_span = 1};
        layout.tiles[1] = {
            .candidate_index = 1, .row = 0, .column = 1, .row_span = 2, .column_span = 1};
        break;
    case 3:
        layout.tiles[0] = {
            .candidate_index = 0, .row = 0, .column = 0, .row_span = 2, .column_span = 1};
        layout.tiles[1] = {
            .candidate_index = 1, .row = 0, .column = 1, .row_span = 1, .column_span = 1};
        layout.tiles[2] = {
            .candidate_index = 2, .row = 1, .column = 1, .row_span = 1, .column_span = 1};
        break;
    case 4:
        for (std::uint8_t index = 0; index < 4; ++index) {
            layout.tiles[index] = {.candidate_index = index,
                                   .row = static_cast<std::uint8_t>(index / 2),
                                   .column = static_cast<std::uint8_t>(index % 2),
                                   .row_span = 1,
                                   .column_span = 1};
        }
        break;
    default:
        break;
    }
    return layout;
}

} // namespace

FolderMosaicBuilder::FolderMosaicBuilder(const TimePoint deadline) noexcept : deadline_(deadline) {}

FolderMosaicChildDisposition FolderMosaicBuilder::push_child(const FolderMosaicChild child,
                                                             const TimePoint now) noexcept {
    if (result_) {
        return result_->outcome == FolderMosaicOutcome::deadline
                   ? FolderMosaicChildDisposition::deadline
                   : FolderMosaicChildDisposition::finished;
    }
    if (expire(now)) {
        return FolderMosaicChildDisposition::deadline;
    }
    if (childrenFinished_) {
        return FolderMosaicChildDisposition::limit_reached;
    }

    const bool is_duplicate = duplicate(child.id);
    observedChildIds_[entriesExamined_] = child.id;
    ++entriesExamined_;
    if (entriesExamined_ == kMaximumFolderMosaicEntries) {
        childrenFinished_ = true;
    }

    FolderMosaicChildDisposition disposition;
    if (is_duplicate) {
        disposition = FolderMosaicChildDisposition::ignored_duplicate;
    } else if (child.kind != FolderMosaicChildKind::regular_file) {
        disposition = FolderMosaicChildDisposition::ignored_non_file;
    } else {
        probeChildren_[probeChildCount_] = {.id = child.id, .state = ProbeState::available};
        ++probeChildCount_;
        disposition = FolderMosaicChildDisposition::accepted;
    }

    settle();
    return disposition;
}

void FolderMosaicBuilder::finish_children(const TimePoint now) noexcept {
    if (result_ || expire(now)) {
        return;
    }
    childrenFinished_ = true;
    settle();
}

std::optional<FolderMosaicProbeRequest>
FolderMosaicBuilder::next_probe(const TimePoint now) noexcept {
    if (result_ || expire(now)) {
        return std::nullopt;
    }

    std::size_t images{};
    std::size_t pending{};
    for (std::size_t index = 0; index < probeChildCount_; ++index) {
        images += probeChildren_[index].state == ProbeState::image ? 1U : 0U;
        pending += probeChildren_[index].state == ProbeState::pending ? 1U : 0U;
    }
    if (images >= kMaximumFolderMosaicCandidates ||
        pending >= kMaximumFolderMosaicCandidates - images) {
        return std::nullopt;
    }

    const auto end = probeChildren_.begin() + static_cast<std::ptrdiff_t>(probeChildCount_);
    const auto candidate = std::find_if(probeChildren_.begin(), end, [](const ProbeChild &child) {
        return child.state == ProbeState::available;
    });
    if (candidate == end) {
        settle();
        return std::nullopt;
    }

    candidate->state = ProbeState::pending;
    ++probesStarted_;
    return FolderMosaicProbeRequest{.child_id = candidate->id, .priority = kFolderMosaicPriority};
}

FolderMosaicProbeDisposition FolderMosaicBuilder::submit_probe(const FolderMosaicProbeResult probe,
                                                               const TimePoint now) noexcept {
    if (result_) {
        return result_->outcome == FolderMosaicOutcome::deadline
                   ? FolderMosaicProbeDisposition::deadline
                   : FolderMosaicProbeDisposition::finished;
    }
    if (expire(now)) {
        return FolderMosaicProbeDisposition::deadline;
    }

    const auto end = probeChildren_.begin() + static_cast<std::ptrdiff_t>(probeChildCount_);
    const auto child =
        std::find_if(probeChildren_.begin(), end, [probe](const ProbeChild &candidate) {
            return candidate.id == probe.child_id;
        });
    if (child == end || child->state != ProbeState::pending) {
        return FolderMosaicProbeDisposition::unexpected_child;
    }

    child->state =
        probe.outcome == FolderMosaicProbeOutcome::image ? ProbeState::image : ProbeState::rejected;
    settle();
    return FolderMosaicProbeDisposition::accepted;
}

std::optional<FolderMosaicResult> FolderMosaicBuilder::result(const TimePoint now) noexcept {
    if (!result_) {
        static_cast<void>(expire(now));
        settle();
    }
    return result_;
}

std::size_t FolderMosaicBuilder::entries_examined() const noexcept {
    return entriesExamined_;
}

std::size_t FolderMosaicBuilder::probes_started() const noexcept {
    return probesStarted_;
}

FolderMosaicPendingProbes FolderMosaicBuilder::pending_probes() const noexcept {
    FolderMosaicPendingProbes pending;
    for (std::size_t index = 0;
         index < probeChildCount_ && pending.count < pending.child_ids.size(); ++index) {
        if (probeChildren_[index].state == ProbeState::pending) {
            pending.child_ids[pending.count] = probeChildren_[index].id;
            ++pending.count;
        }
    }
    return pending;
}

bool FolderMosaicBuilder::expire(const TimePoint now) noexcept {
    if (!result_ && now >= deadline_) {
        finalize(FolderMosaicOutcome::deadline);
    }
    return result_ && result_->outcome == FolderMosaicOutcome::deadline;
}

bool FolderMosaicBuilder::duplicate(const FolderMosaicChildId child_id) const noexcept {
    const auto end = observedChildIds_.begin() + static_cast<std::ptrdiff_t>(entriesExamined_);
    return std::find(observedChildIds_.begin(), end, child_id) != end;
}

void FolderMosaicBuilder::settle() noexcept {
    if (result_) {
        return;
    }

    std::size_t candidate_count{};
    bool unresolved{};
    for (std::size_t index = 0; index < probeChildCount_; ++index) {
        switch (probeChildren_[index].state) {
        case ProbeState::available:
        case ProbeState::pending:
            unresolved = true;
            break;
        case ProbeState::image:
            ++candidate_count;
            if (candidate_count == kMaximumFolderMosaicCandidates) {
                finalize(FolderMosaicOutcome::complete);
                return;
            }
            break;
        case ProbeState::rejected:
            break;
        }
    }

    if (!childrenFinished_ || unresolved) {
        return;
    }
    finalize(candidate_count == 0 ? FolderMosaicOutcome::no_candidates
                                  : FolderMosaicOutcome::partial);
}

void FolderMosaicBuilder::finalize(const FolderMosaicOutcome outcome) noexcept {
    FolderMosaicResult final_result;
    final_result.outcome = outcome;
    final_result.entries_examined = entriesExamined_;

    for (std::size_t index = 0; index < probeChildCount_ &&
                                final_result.candidate_count < kMaximumFolderMosaicCandidates;
         ++index) {
        if (probeChildren_[index].state == ProbeState::image) {
            final_result.candidates[final_result.candidate_count] = probeChildren_[index].id;
            ++final_result.candidate_count;
        }
    }

    final_result.layout = make_layout(final_result.candidate_count);
    result_ = final_result;
}

} // namespace vove::preview
