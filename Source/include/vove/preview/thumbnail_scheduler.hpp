#pragma once

#include "vove/preview/thumbnail_types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_set>
#include <vector>

namespace vove::preview {

inline constexpr std::size_t kDefaultThumbnailQueueCapacity = 4'096;
inline constexpr std::size_t kMaximumRememberedCancelledGenerations = 4'096;
inline constexpr std::uint64_t kMaximumPriorityBypasses = 16;
inline constexpr std::uint64_t kMaximumFolderPriorityBypasses = 4;

enum class SubmitStatus : std::uint8_t {
    queued,
    queued_after_eviction,
    invalid_request,
    duplicate_job,
    cancelled_generation,
    queue_full,
};

struct SubmitResult {
    SubmitStatus status{SubmitStatus::invalid_request};
    std::optional<JobId> evicted_job;

    [[nodiscard]] bool accepted() const noexcept {
        return status == SubmitStatus::queued || status == SubmitStatus::queued_after_eviction;
    }
};

class ThumbnailScheduler {
  public:
    explicit ThumbnailScheduler(std::size_t capacity = kDefaultThumbnailQueueCapacity);

    [[nodiscard]] SubmitResult submit(ThumbnailRequest request);
    [[nodiscard]] std::optional<ThumbnailRequest> take_next();

    [[nodiscard]] std::size_t cancel_generation(RequestGeneration generation);
    void release_generation(RequestGeneration generation);

    [[nodiscard]] bool is_generation_cancelled(RequestGeneration generation) const;
    [[nodiscard]] std::size_t remembered_cancelled_generation_count() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    struct PendingRequest {
        ThumbnailRequest request;
        std::uint64_t sequence{};
        std::uint64_t dispatch_count_at_submit{};
    };

    [[nodiscard]] static bool valid_request(const ThumbnailRequest &request) noexcept;
    [[nodiscard]] static std::uint8_t priority_rank(ThumbnailPriority priority) noexcept;
    [[nodiscard]] std::size_t next_index() const noexcept;
    [[nodiscard]] std::size_t worst_index() const noexcept;
    void erase_at(std::size_t index);

    std::size_t capacity_{};
    std::uint64_t nextSequence_{};
    std::uint64_t dispatchCount_{};
    std::vector<PendingRequest> pending_;
    std::unordered_set<JobId> pendingJobIds_;
    std::unordered_set<RequestGeneration> cancelledGenerations_;
    std::deque<RequestGeneration> cancelledGenerationOrder_;
};

} // namespace vove::preview
