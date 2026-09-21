#pragma once

#include "vove/cache/artifact_store.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vove::cache {

struct MemoryCacheLimits {
    std::size_t maximum_entries{256};
    std::size_t maximum_bytes{128U * 1024U * 1024U};
};

struct MemoryCacheStats {
    std::size_t entries{};
    std::size_t bytes{};
    std::size_t hits{};
    std::size_t misses{};
    std::size_t evictions{};
};

struct PersistedAccessExchange {
    std::int64_t expected_unix_ns{};
    std::int64_t desired_unix_ns{};
};

class MemoryArtifactCache {
  public:
    explicit MemoryArtifactCache(MemoryCacheLimits limits = {});

    [[nodiscard]] std::shared_ptr<const ArtifactData> find(std::string_view digest_hex);
    [[nodiscard]] bool insert(std::string digest_hex, std::shared_ptr<const ArtifactData> artifact,
                              std::int64_t persisted_access_unix_ns = 0);
    [[nodiscard]] std::optional<std::int64_t> persisted_access(std::string_view digest_hex) const;
    [[nodiscard]] bool compare_exchange_persisted_access(std::string_view digest_hex,
                                                         PersistedAccessExchange exchange);
    [[nodiscard]] bool erase(std::string_view digest_hex);
    void clear();

    [[nodiscard]] MemoryCacheStats stats() const;
    [[nodiscard]] MemoryCacheLimits limits() const noexcept;

  private:
    struct Entry {
        std::string digest_hex;
        std::shared_ptr<const ArtifactData> artifact;
        std::size_t weight{};
        std::int64_t persisted_access_unix_ns{};
    };

    using Entries = std::list<Entry>;
    using Iterator = Entries::iterator;

    [[nodiscard]] static bool valid_digest(std::string_view digest_hex) noexcept;
    [[nodiscard]] static std::size_t artifact_weight(const ArtifactData &artifact) noexcept;
    void evict_to_limits();
    void erase_entry(Iterator entry);

    MemoryCacheLimits limits_;
    mutable std::mutex mutex_;
    Entries entries_;
    std::unordered_map<std::string, Iterator> by_digest_;
    MemoryCacheStats stats_;
};

} // namespace vove::cache
