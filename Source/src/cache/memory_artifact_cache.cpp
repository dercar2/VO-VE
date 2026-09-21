#include "vove/cache/memory_artifact_cache.hpp"

#include <algorithm>

namespace vove::cache {

MemoryArtifactCache::MemoryArtifactCache(const MemoryCacheLimits limits) : limits_(limits) {
    by_digest_.reserve(limits_.maximum_entries);
}

std::shared_ptr<const ArtifactData> MemoryArtifactCache::find(const std::string_view digest_hex) {
    std::scoped_lock lock(mutex_);
    const auto found = by_digest_.find(std::string(digest_hex));
    if (found == by_digest_.end()) {
        ++stats_.misses;
        return {};
    }
    entries_.splice(entries_.begin(), entries_, found->second);
    ++stats_.hits;
    return found->second->artifact;
}

bool MemoryArtifactCache::insert(std::string digest_hex,
                                 std::shared_ptr<const ArtifactData> artifact,
                                 const std::int64_t persisted_access_unix_ns) {
    if (!artifact || !valid_digest(digest_hex)) {
        return false;
    }
    const auto weight = artifact_weight(*artifact);
    if (limits_.maximum_entries == 0 || weight > limits_.maximum_bytes) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    if (const auto found = by_digest_.find(digest_hex); found != by_digest_.end()) {
        erase_entry(found->second);
    }
    entries_.push_front(Entry{.digest_hex = std::move(digest_hex),
                              .artifact = std::move(artifact),
                              .weight = weight,
                              .persisted_access_unix_ns = persisted_access_unix_ns});
    by_digest_.emplace(entries_.front().digest_hex, entries_.begin());
    stats_.entries = entries_.size();
    stats_.bytes += weight;
    evict_to_limits();
    return true;
}

std::optional<std::int64_t>
MemoryArtifactCache::persisted_access(const std::string_view digest_hex) const {
    std::scoped_lock lock(mutex_);
    const auto found = by_digest_.find(std::string(digest_hex));
    if (found == by_digest_.end()) {
        return std::nullopt;
    }
    return found->second->persisted_access_unix_ns;
}

bool MemoryArtifactCache::compare_exchange_persisted_access(
    const std::string_view digest_hex, const PersistedAccessExchange exchange) {
    std::scoped_lock lock(mutex_);
    const auto found = by_digest_.find(std::string(digest_hex));
    if (found == by_digest_.end() ||
        found->second->persisted_access_unix_ns != exchange.expected_unix_ns) {
        return false;
    }
    found->second->persisted_access_unix_ns = exchange.desired_unix_ns;
    return true;
}

bool MemoryArtifactCache::erase(const std::string_view digest_hex) {
    std::scoped_lock lock(mutex_);
    const auto found = by_digest_.find(std::string(digest_hex));
    if (found == by_digest_.end()) {
        return false;
    }
    erase_entry(found->second);
    return true;
}

void MemoryArtifactCache::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    by_digest_.clear();
    stats_.entries = 0;
    stats_.bytes = 0;
}

MemoryCacheStats MemoryArtifactCache::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}

MemoryCacheLimits MemoryArtifactCache::limits() const noexcept {
    return limits_;
}

bool MemoryArtifactCache::valid_digest(const std::string_view digest_hex) noexcept {
    return digest_hex.size() == 64U && std::ranges::all_of(digest_hex, [](const char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

std::size_t MemoryArtifactCache::artifact_weight(const ArtifactData &artifact) noexcept {
    return kVvt1HeaderBytes + artifact.payload.size();
}

void MemoryArtifactCache::evict_to_limits() {
    while (entries_.size() > limits_.maximum_entries || stats_.bytes > limits_.maximum_bytes) {
        erase_entry(std::prev(entries_.end()));
        ++stats_.evictions;
    }
}

void MemoryArtifactCache::erase_entry(const Iterator entry) {
    stats_.bytes -= entry->weight;
    by_digest_.erase(entry->digest_hex);
    entries_.erase(entry);
    stats_.entries = entries_.size();
}

} // namespace vove::cache
