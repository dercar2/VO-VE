#pragma once

#include "vove/cache/artifact_index.hpp"
#include "vove/cache/artifact_store.hpp"
#include "vove/cache/cache_maintenance.hpp"
#include "vove/cache/memory_artifact_cache.hpp"
#include "vove/cache/qoi_codec.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace vove::cache {

inline constexpr std::int64_t kAccessTimestampGranularityNs = 60'000'000'000LL;

enum class CacheLookupKind : std::uint8_t {
    memory_hit,
    disk_hit,
    miss,
    error,
};

struct CachedThumbnail {
    std::shared_ptr<const ArtifactData> artifact;
    std::optional<ArtifactIndexRecord> index_record{};
    // Request-scoped pixels, never retained by the encoded-artifact memory cache.
    std::optional<QoiDecodeResult> decoded{};
};

struct CacheLookupResult {
    CacheLookupKind kind{CacheLookupKind::miss};
    std::optional<CachedThumbnail> thumbnail;
    std::string error;
};

struct CachePublishRequest {
    CacheKey key;
    ArtifactMetadata metadata;
    std::span<const std::byte> payload;
    std::string source_profile_name;
    std::string source_profile_fingerprint;
    std::int64_t access_unix_ns{};
};

struct CachePublishResult {
    bool published{};
    bool index_published{};
    std::string error;
    // A valid decode remains available even when persistence fails.
    std::optional<QoiDecodeResult> decoded{};
};

struct DiskCacheStats {
    std::uint64_t entries{};
    std::uint64_t bytes{};
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

class PersistentThumbnailCache {
  public:
    PersistentThumbnailCache(ArtifactStore store, ArtifactIndex &index,
                             MemoryCacheLimits memory_limits = {});

    [[nodiscard]] CacheLookupResult lookup(const CacheKey &key, std::int64_t access_unix_ns);
    [[nodiscard]] CacheLookupResult lookup_locator(const CacheLocatorKey &locator,
                                                   std::int64_t access_unix_ns);
    [[nodiscard]] CachePublishResult publish(const CachePublishRequest &request);
    [[nodiscard]] bool bind_locator(const CacheLocatorKey &locator, const CacheKey &key);
    [[nodiscard]] bool erase_locator(const CacheLocatorKey &locator);
    [[nodiscard]] bool erase(const CacheKey &key);

    [[nodiscard]] const ArtifactStore &store() const noexcept;
    [[nodiscard]] MemoryCacheStats memory_stats() const;
    [[nodiscard]] DiskCacheStats disk_stats();
    [[nodiscard]] PruneResult prune(DiskCacheLimits limits, bool scan_orphans = false);

  private:
    [[nodiscard]] ArtifactIndexRecord make_record(const CachePublishRequest &request,
                                                  const CacheDigest &digest,
                                                  const CommitResult &commit,
                                                  std::uint64_t artifact_bytes) const;

    ArtifactStore store_;
    ArtifactIndex &index_;
    MemoryArtifactCache memory_;
    // Last-resort guard if neither a durable fence nor the SQLite revocation can be written.
    std::atomic_bool offline_lookup_disabled_{};
};

} // namespace vove::cache
