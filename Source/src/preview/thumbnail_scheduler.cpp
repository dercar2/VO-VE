#include "vove/preview/thumbnail_scheduler.hpp"

#include <algorithm>

namespace vove::preview {

ThumbnailScheduler::ThumbnailScheduler(const std::size_t capacity) : capacity_(capacity) {
    pending_.reserve(capacity_);
    pendingJobIds_.reserve(capacity_);
}

SubmitResult ThumbnailScheduler::submit(ThumbnailRequest request) {
    if (!valid_request(request)) {
        return {.status = SubmitStatus::invalid_request, .evicted_job = std::nullopt};
    }
    if (cancelledGenerations_.contains(request.generation)) {
        return {.status = SubmitStatus::cancelled_generation, .evicted_job = std::nullopt};
    }
    if (pendingJobIds_.contains(request.job_id)) {
        return {.status = SubmitStatus::duplicate_job, .evicted_job = std::nullopt};
    }
    if (capacity_ == 0) {
        return {.status = SubmitStatus::queue_full, .evicted_job = std::nullopt};
    }

    std::optional<JobId> evicted_job;
    if (pending_.size() == capacity_) {
        const auto index = worst_index();
        if (priority_rank(request.priority) >= priority_rank(pending_[index].request.priority)) {
            return {.status = SubmitStatus::queue_full, .evicted_job = std::nullopt};
        }
        evicted_job = pending_[index].request.job_id;
        erase_at(index);
    }

    pendingJobIds_.insert(request.job_id);
    pending_.push_back({.request = request,
                        .sequence = nextSequence_++,
                        .dispatch_count_at_submit = dispatchCount_});
    return {.status = evicted_job ? SubmitStatus::queued_after_eviction : SubmitStatus::queued,
            .evicted_job = evicted_job};
}

std::optional<ThumbnailRequest> ThumbnailScheduler::take_next() {
    if (pending_.empty()) {
        return std::nullopt;
    }
    const auto index = next_index();
    const auto request = pending_[index].request;
    erase_at(index);
    ++dispatchCount_;
    return request;
}

std::size_t ThumbnailScheduler::cancel_generation(const RequestGeneration generation) {
    if (cancelledGenerations_.insert(generation).second) {
        cancelledGenerationOrder_.push_back(generation);
        if (cancelledGenerationOrder_.size() > kMaximumRememberedCancelledGenerations) {
            const auto expired = cancelledGenerationOrder_.front();
            cancelledGenerationOrder_.pop_front();
            cancelledGenerations_.erase(expired);
        }
    }
    const auto previous_size = pending_.size();
    std::erase_if(pending_, [this, generation](const PendingRequest &pending) {
        if (pending.request.generation != generation) {
            return false;
        }
        pendingJobIds_.erase(pending.request.job_id);
        return true;
    });
    return previous_size - pending_.size();
}

void ThumbnailScheduler::release_generation(const RequestGeneration generation) {
    cancelledGenerations_.erase(generation);
    std::erase(cancelledGenerationOrder_, generation);
}

bool ThumbnailScheduler::is_generation_cancelled(const RequestGeneration generation) const {
    return cancelledGenerations_.contains(generation);
}

std::size_t ThumbnailScheduler::remembered_cancelled_generation_count() const noexcept {
    return cancelledGenerations_.size();
}

std::size_t ThumbnailScheduler::size() const noexcept {
    return pending_.size();
}

std::size_t ThumbnailScheduler::capacity() const noexcept {
    return capacity_;
}

bool ThumbnailScheduler::empty() const noexcept {
    return pending_.empty();
}

bool ThumbnailScheduler::valid_request(const ThumbnailRequest &request) noexcept {
    const auto valid_priority = [&request] {
        switch (request.priority) {
        case ThumbnailPriority::visible_selected:
        case ThumbnailPriority::visible:
        case ThumbnailPriority::folder:
        case ThumbnailPriority::nearby:
        case ThumbnailPriority::rest:
            return true;
        }
        return false;
    };
    return request.job_id != 0 && request.generation != 0 && request.source_id != 0 &&
           request.canonical_edge != 0 && request.canonical_edge <= kMaximumCanonicalEdge &&
           valid_priority();
}

std::uint8_t ThumbnailScheduler::priority_rank(const ThumbnailPriority priority) noexcept {
    return static_cast<std::uint8_t>(priority);
}

std::size_t ThumbnailScheduler::next_index() const noexcept {
    std::optional<std::size_t> starved;
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        const auto &candidate = pending_[index];
        const auto bypass_limit = candidate.request.priority == ThumbnailPriority::folder
                                      ? kMaximumFolderPriorityBypasses
                                      : kMaximumPriorityBypasses;
        if (dispatchCount_ - candidate.dispatch_count_at_submit < bypass_limit) {
            continue;
        }
        if (!starved ||
            candidate.dispatch_count_at_submit < pending_[*starved].dispatch_count_at_submit ||
            (candidate.dispatch_count_at_submit == pending_[*starved].dispatch_count_at_submit &&
             candidate.sequence < pending_[*starved].sequence)) {
            starved = index;
        }
    }
    if (starved) {
        return *starved;
    }

    std::size_t best{};
    for (std::size_t index = 1; index < pending_.size(); ++index) {
        const auto &candidate = pending_[index];
        const auto &current = pending_[best];
        const auto candidate_rank = priority_rank(candidate.request.priority);
        const auto current_rank = priority_rank(current.request.priority);
        if (candidate_rank < current_rank ||
            (candidate_rank == current_rank && candidate.sequence < current.sequence)) {
            best = index;
        }
    }
    return best;
}

std::size_t ThumbnailScheduler::worst_index() const noexcept {
    std::size_t worst{};
    for (std::size_t index = 1; index < pending_.size(); ++index) {
        const auto &candidate = pending_[index];
        const auto &current = pending_[worst];
        const auto candidate_rank = priority_rank(candidate.request.priority);
        const auto current_rank = priority_rank(current.request.priority);
        if (candidate_rank > current_rank ||
            (candidate_rank == current_rank && candidate.sequence > current.sequence)) {
            worst = index;
        }
    }
    return worst;
}

void ThumbnailScheduler::erase_at(const std::size_t index) {
    pendingJobIds_.erase(pending_[index].request.job_id);
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
}

} // namespace vove::preview
