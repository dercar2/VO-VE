#include "vove/cache/persistent_thumbnail_cache.hpp"

#include "vove/cache/qoi_codec.hpp"

#include <filesystem>
#include <utility>

namespace vove::cache {
namespace {

[[nodiscard]] std::uint64_t artifact_bytes(const ArtifactData &artifact) {
    return static_cast<std::uint64_t>(kVvt1HeaderBytes) + artifact.payload.size() +
           artifact.color_identity.source_profile_name.size() +
           artifact.color_identity.source_profile_fingerprint.size();
}

[[nodiscard]] bool record_matches(const ArtifactIndexRecord &record, const CacheKey &key,
                                  const CacheDigest &digest, const ArtifactData &artifact,
                                  const std::string_view relative_path) {
    return record.digest_hex == digest.hex() && record.source == key.source &&
           record.metadata == artifact.metadata &&
           record.source_profile_name == artifact.color_identity.source_profile_name &&
           record.source_profile_fingerprint ==
               artifact.color_identity.source_profile_fingerprint &&
           record.relative_artifact_path_utf8 == relative_path &&
           record.artifact_bytes == artifact_bytes(artifact);
}

[[nodiscard]] ArtifactIndexRecord
record_from_artifact(const CacheKey &key, const CacheDigest &digest, const ArtifactData &artifact,
                     const std::string_view relative_path, const std::int64_t access_unix_ns) {
    return {
        .digest_hex = digest.hex(),
        .source = key.source,
        .metadata = artifact.metadata,
        .source_profile_name = artifact.color_identity.source_profile_name,
        .source_profile_fingerprint = artifact.color_identity.source_profile_fingerprint,
        .relative_artifact_path_utf8 = std::string(relative_path),
        .artifact_bytes = artifact_bytes(artifact),
        .last_access_unix_ns = access_unix_ns,
    };
}

} // namespace

PersistentThumbnailCache::PersistentThumbnailCache(ArtifactStore store, ArtifactIndex &index,
                                                   const MemoryCacheLimits memory_limits)
    : store_(std::move(store)), index_(index), memory_(memory_limits) {}

CacheLookupResult PersistentThumbnailCache::lookup(const CacheKey &key,
                                                   const std::int64_t access_unix_ns) {
    const auto digest = hash_cache_key(key);
    const auto digest_hex = digest.hex();
    if (auto resident = memory_.find(digest_hex)) {
        auto record = index_.find(digest_hex);
        if (!record) {
            const auto stored = store_.lookup(key);
            if (stored.status != LookupStatus::hit || !stored.artifact || stored.error) {
                static_cast<void>(memory_.erase(digest_hex));
                return stored.error ? CacheLookupResult{.kind = CacheLookupKind::error,
                                                        .thumbnail = std::nullopt,
                                                        .error = stored.error.message}
                                    : CacheLookupResult{};
            }
            const auto relative_path =
                store_.artifact_path(key).lexically_relative(store_.root()).generic_string();
            auto recovered =
                record_from_artifact(key, digest, *stored.artifact, relative_path, access_unix_ns);
            if (!index_.publish(recovered)) {
                return {.kind = CacheLookupKind::error,
                        .thumbnail = std::nullopt,
                        .error = "failed to reconstruct the cache index for a resident artifact"};
            }
            record = std::move(recovered);
        }
        const auto persisted_access = memory_.persisted_access(digest_hex);
        if (persisted_access && access_unix_ns >= *persisted_access &&
            access_unix_ns - *persisted_access >= kAccessTimestampGranularityNs &&
            memory_.compare_exchange_persisted_access(
                digest_hex,
                {.expected_unix_ns = *persisted_access, .desired_unix_ns = access_unix_ns})) {
            const auto touched = index_.touch(digest_hex, access_unix_ns);
            if (touched != TouchResult::updated) {
                static_cast<void>(memory_.compare_exchange_persisted_access(
                    digest_hex,
                    {.expected_unix_ns = access_unix_ns, .desired_unix_ns = *persisted_access}));
            }
        }
        return {.kind = CacheLookupKind::memory_hit,
                .thumbnail = CachedThumbnail{.artifact = std::move(resident),
                                             .index_record = std::move(record)},
                .error = {}};
    }

    auto record = index_.find(digest_hex);
    auto stored = store_.lookup(key);
    if (stored.error) {
        return {.kind = CacheLookupKind::error,
                .thumbnail = std::nullopt,
                .error = stored.error.message};
    }
    if (stored.status == LookupStatus::miss || !stored.artifact) {
        if (record) {
            if (!index_.erase(digest_hex)) {
                return {.kind = CacheLookupKind::error,
                        .thumbnail = std::nullopt,
                        .error = "failed to erase index for a missing cache artifact"};
            }
        }
        return {};
    }
    if (stored.artifact->metadata.encoding == ArtifactEncoding::qoi_rgba8) {
        const auto decoded = decode_qoi_rgba8(stored.artifact->payload);
        if (!decoded.ok() || decoded.width != stored.artifact->metadata.width ||
            decoded.height != stored.artifact->metadata.height) {
            const auto removal = store_.remove_digest_checked(digest_hex);
            if (!removal.ok()) {
                return {.kind = CacheLookupKind::error,
                        .thumbnail = std::nullopt,
                        .error = removal.error.message};
            }
            if (record && !index_.erase(digest_hex)) {
                return {.kind = CacheLookupKind::error,
                        .thumbnail = std::nullopt,
                        .error = "failed to erase index for malformed cached QOI"};
            }
            return {};
        }
    }

    auto artifact = std::make_shared<ArtifactData>(std::move(*stored.artifact));
    const auto relative_path =
        store_.artifact_path(key).lexically_relative(store_.root()).generic_string();
    if (!record || !record_matches(*record, key, digest, *artifact, relative_path)) {
        auto recovered =
            record_from_artifact(key, digest, *artifact, relative_path, access_unix_ns);
        if (!index_.publish(recovered)) {
            return {.kind = CacheLookupKind::error,
                    .thumbnail = std::nullopt,
                    .error = "failed to reconstruct the cache index from its artifact"};
        }
        record = std::move(recovered);
    } else if (access_unix_ns >= record->last_access_unix_ns &&
               access_unix_ns - record->last_access_unix_ns >= kAccessTimestampGranularityNs &&
               index_.touch(digest_hex, access_unix_ns) == TouchResult::updated) {
        record->last_access_unix_ns = access_unix_ns;
    }
    static_cast<void>(memory_.insert(digest_hex, artifact, record->last_access_unix_ns));
    return {.kind = CacheLookupKind::disk_hit,
            .thumbnail =
                CachedThumbnail{.artifact = std::move(artifact), .index_record = std::move(record)},
            .error = {}};
}

CacheLookupResult PersistentThumbnailCache::lookup_locator(const CacheLocatorKey &locator,
                                                           const std::int64_t access_unix_ns) {
    const auto locator_digest = hash_cache_locator(locator).hex();
    const auto artifact_digest = index_.find_locator(locator_digest);
    if (!artifact_digest) {
        return {};
    }
    auto record = index_.find(*artifact_digest);
    if (!record) {
        static_cast<void>(index_.erase_locator(locator_digest));
        return {};
    }
    if (auto resident = memory_.find(*artifact_digest)) {
        if (access_unix_ns >= record->last_access_unix_ns &&
            access_unix_ns - record->last_access_unix_ns >= kAccessTimestampGranularityNs &&
            index_.touch(*artifact_digest, access_unix_ns) == TouchResult::updated) {
            record->last_access_unix_ns = access_unix_ns;
        }
        return {.kind = CacheLookupKind::memory_hit,
                .thumbnail = CachedThumbnail{.artifact = std::move(resident),
                                             .index_record = std::move(record)},
                .error = {}};
    }

    auto stored = store_.lookup_digest(*artifact_digest);
    if (stored.error) {
        static_cast<void>(index_.erase(*artifact_digest));
        static_cast<void>(index_.erase_locator(locator_digest));
        return {.kind = CacheLookupKind::error,
                .thumbnail = std::nullopt,
                .error = stored.error.message};
    }
    if (stored.status == LookupStatus::miss || !stored.artifact) {
        static_cast<void>(index_.erase(*artifact_digest));
        static_cast<void>(index_.erase_locator(locator_digest));
        return {};
    }
    if (stored.artifact->metadata.encoding == ArtifactEncoding::qoi_rgba8) {
        const auto decoded = decode_qoi_rgba8(stored.artifact->payload);
        if (!decoded.ok() || decoded.width != stored.artifact->metadata.width ||
            decoded.height != stored.artifact->metadata.height) {
            static_cast<void>(store_.remove_digest(*artifact_digest));
            static_cast<void>(index_.erase(*artifact_digest));
            static_cast<void>(index_.erase_locator(locator_digest));
            return {};
        }
    }
    const auto relative_path = store_.artifact_path_from_digest(*artifact_digest)
                                   ->lexically_relative(store_.root())
                                   .generic_string();
    if (record->digest_hex != *artifact_digest || record->metadata != stored.artifact->metadata ||
        record->source_profile_name != stored.artifact->color_identity.source_profile_name ||
        record->source_profile_fingerprint !=
            stored.artifact->color_identity.source_profile_fingerprint ||
        record->relative_artifact_path_utf8 != relative_path ||
        record->artifact_bytes != artifact_bytes(*stored.artifact)) {
        static_cast<void>(store_.remove_digest(*artifact_digest));
        static_cast<void>(index_.erase(*artifact_digest));
        static_cast<void>(index_.erase_locator(locator_digest));
        return {};
    }
    auto artifact = std::make_shared<ArtifactData>(std::move(*stored.artifact));
    if (access_unix_ns >= record->last_access_unix_ns &&
        access_unix_ns - record->last_access_unix_ns >= kAccessTimestampGranularityNs &&
        index_.touch(*artifact_digest, access_unix_ns) == TouchResult::updated) {
        record->last_access_unix_ns = access_unix_ns;
    }
    static_cast<void>(memory_.insert(*artifact_digest, artifact, record->last_access_unix_ns));
    return {.kind = CacheLookupKind::disk_hit,
            .thumbnail =
                CachedThumbnail{.artifact = std::move(artifact), .index_record = std::move(record)},
            .error = {}};
}

CachePublishResult PersistentThumbnailCache::publish(const CachePublishRequest &request) {
    if (request.metadata.encoding == ArtifactEncoding::qoi_rgba8) {
        const auto decoded = decode_qoi_rgba8(request.payload);
        if (!decoded.ok() || decoded.width != request.metadata.width ||
            decoded.height != request.metadata.height) {
            return {.published = false,
                    .index_published = false,
                    .error = decoded.ok() ? "QOI dimensions do not match artifact metadata"
                                          : decoded.error};
        }
    }
    const auto digest = hash_cache_key(request.key);
    const ArtifactColorIdentity color_identity{
        .source_profile_name = request.source_profile_name,
        .source_profile_fingerprint = request.source_profile_fingerprint,
    };
    const auto encoded = encode_vvt1(request.metadata, request.payload, digest, color_identity);
    if (encoded.error) {
        return {.published = false, .index_published = false, .error = encoded.error.message};
    }
    auto started = store_.begin_write(request.key);
    if (!started.staging) {
        return {.published = false, .index_published = false, .error = started.error.message};
    }
    auto staging = std::move(*started.staging);
    if (!staging.write(encoded.bytes)) {
        return {.published = false, .index_published = false, .error = staging.error().message};
    }
    const auto commit = store_.commit(std::move(staging));
    if (!commit.committed) {
        return {.published = false, .index_published = false, .error = commit.error.message};
    }

    const auto record =
        make_record(request, digest, commit, static_cast<std::uint64_t>(encoded.bytes.size()));
    const auto indexed = index_.publish(record);
    if (!indexed) {
        const auto cleanup = store_.remove_digest_checked(digest.hex());
        return {.published = false,
                .index_published = false,
                .error = cleanup.ok() ? "cache index update failed; unindexed artifact was removed"
                                      : "cache index update failed and artifact cleanup failed: " +
                                            cleanup.error.message};
    }
    auto artifact = std::make_shared<ArtifactData>(ArtifactData{
        .metadata = encoded.metadata,
        .cache_key_digest = encoded.cache_key_digest,
        .color_identity = encoded.color_identity,
        .payload = std::vector<std::byte>(request.payload.begin(), request.payload.end())});
    static_cast<void>(memory_.insert(digest.hex(), std::move(artifact), request.access_unix_ns));
    return {.published = true, .index_published = true, .error = {}};
}

bool PersistentThumbnailCache::bind_locator(const CacheLocatorKey &locator, const CacheKey &key) {
    return index_.publish_locator(hash_cache_locator(locator).hex(), hash_cache_key(key).hex());
}

bool PersistentThumbnailCache::erase_locator(const CacheLocatorKey &locator) {
    return index_.erase_locator(hash_cache_locator(locator).hex());
}

bool PersistentThumbnailCache::erase(const CacheKey &key) {
    const auto digest = hash_cache_key(key).hex();
    const auto memory_removed = memory_.erase(digest);
    const auto artifact_removal = store_.remove_digest_checked(digest);
    if (!artifact_removal.ok()) {
        return memory_removed;
    }
    const auto index_removed = index_.erase(digest);
    return memory_removed || artifact_removal.status == RemoveStatus::removed || index_removed;
}

const ArtifactStore &PersistentThumbnailCache::store() const noexcept {
    return store_;
}

MemoryCacheStats PersistentThumbnailCache::memory_stats() const {
    return memory_.stats();
}

DiskCacheStats PersistentThumbnailCache::disk_stats() {
    if (!index_.ready()) {
        return {.error = index_.last_error().empty() ? "cache index is unavailable"
                                                     : index_.last_error()};
    }
    const auto entries = index_.count();
    const auto bytes = index_.total_bytes();
    if (!index_.last_error().empty()) {
        return {.error = index_.last_error()};
    }
    return {.entries = entries, .bytes = bytes, .error = {}};
}

PruneResult PersistentThumbnailCache::prune(const DiskCacheLimits limits, const bool scan_orphans) {
    auto result = prune_disk_cache(store_, index_, limits, 256, {}, scan_orphans);
    if (result.index_records_removed != 0 || result.orphan_artifacts_removed != 0) {
        memory_.clear();
    }
    return result;
}

ArtifactIndexRecord
PersistentThumbnailCache::make_record(const CachePublishRequest &request, const CacheDigest &digest,
                                      const CommitResult &commit,
                                      const std::uint64_t artifact_bytes) const {
    return {
        .digest_hex = digest.hex(),
        .source = request.key.source,
        .metadata = commit.metadata,
        .source_profile_name = commit.color_identity.source_profile_name,
        .source_profile_fingerprint = commit.color_identity.source_profile_fingerprint,
        .relative_artifact_path_utf8 =
            commit.path.lexically_relative(store_.root()).generic_string(),
        .artifact_bytes = artifact_bytes,
        .last_access_unix_ns = request.access_unix_ns,
    };
}

} // namespace vove::cache
