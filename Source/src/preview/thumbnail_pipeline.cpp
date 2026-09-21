#include "vove/preview/thumbnail_pipeline.hpp"

#include "vove/cache/qoi_codec.hpp"
#include "vove/preview/filename_policy.hpp"
#include "vove/preview/rgba_scaler.hpp"

#include <utility>

namespace vove::preview {
namespace {

[[nodiscard]] std::uint32_t cache_edge(const std::uint32_t presentation_edge) noexcept {
    auto edge = cache::kThumbnailCanonicalEdge;
    while (edge < presentation_edge && edge < cache::kMaxArtifactDimension) {
        edge *= 2U;
    }
    return edge;
}

[[nodiscard]] ThumbnailPipelineResult
failure(const ThumbnailPipelineStatus status, std::string diagnostic, const bool from_cache = false,
        const PreviewFreshness freshness = PreviewFreshness::verified) {
    ThumbnailPipelineResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    result.from_cache = from_cache;
    result.freshness = freshness;
    return result;
}

struct MaterializeOptions {
    std::uint32_t presentation_edge{};
    std::uint32_t page_index{};
    bool from_cache{};
    PreviewFreshness freshness{PreviewFreshness::verified};
};

[[nodiscard]] ThumbnailPipelineResult materialize(const cache::ArtifactData &artifact,
                                                  const MaterializeOptions options) {
    auto decoded = cache::decode_qoi_rgba8(artifact.payload);
    if (!decoded.ok() || decoded.width != artifact.metadata.width ||
        decoded.height != artifact.metadata.height) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       decoded.error.empty() ? "cached QOI dimensions do not match metadata"
                                             : decoded.error,
                       options.from_cache);
    }
    auto width = decoded.width;
    auto height = decoded.height;
    auto pixels = std::move(decoded.rgba8);
    if (width > options.presentation_edge || height > options.presentation_edge) {
        auto scaled = scale_rgba8_to_edge(pixels, width, height, options.presentation_edge);
        if (!scaled.ok()) {
            return failure(ThumbnailPipelineStatus::internal_error, std::move(scaled.error),
                           options.from_cache);
        }
        width = scaled.width;
        height = scaled.height;
        pixels = std::move(scaled.rgba8);
    }
    return {.status = ThumbnailPipelineStatus::success,
            .width = width,
            .height = height,
            .page_index = options.page_index,
            .page_count = artifact.metadata.page_count,
            .source_color_model = artifact.metadata.source_color_model,
            .provenance = artifact.metadata.provenance,
            .source_profile_name = artifact.color_identity.source_profile_name,
            .source_profile_fingerprint = artifact.color_identity.source_profile_fingerprint,
            .rgba8 = std::move(pixels),
            .diagnostic = {},
            .from_cache = options.from_cache,
            .freshness = options.freshness};
}

[[nodiscard]] bool permits_offline_fallback(const ThumbnailPipelineStatus status) noexcept {
    return status == ThumbnailPipelineStatus::disconnected ||
           status == ThumbnailPipelineStatus::timed_out;
}

[[nodiscard]] bool revokes_offline_locator(const ThumbnailPipelineStatus status) noexcept {
    return status == ThumbnailPipelineStatus::source_changed ||
           status == ThumbnailPipelineStatus::source_not_found ||
           status == ThumbnailPipelineStatus::permission_denied ||
           status == ThumbnailPipelineStatus::authentication_failed;
}

} // namespace

ThumbnailPipeline::ThumbnailPipeline(cache::PersistentThumbnailCache &cache,
                                     ThumbnailRenderer renderer,
                                     ThumbnailSourceResolver source_resolver,
                                     ThumbnailColorPolicyResolver color_policy_resolver)
    : cache_(cache), renderer_(std::move(renderer)), source_resolver_(std::move(source_resolver)),
      color_policy_resolver_(std::move(color_policy_resolver)) {}

cache::CacheKey ThumbnailPipeline::make_cache_key(const ThumbnailPipelineRequest &request) {
    cache::ColorPolicyIdentity color_policy{kThumbnailColorPolicyVersion};
    color_policy.working_profile_fingerprint = request.color_policy_fingerprint;
    return {.source = request.source,
            .handler_id = request.handler_id,
            .handler_version = request.handler_version,
            .render_policy_version = kThumbnailRenderPolicyVersion,
            .color_policy_version = std::move(color_policy),
            .canonical_edge = cache_edge(request.canonical_edge),
            .page_index = request.page_index};
}

cache::CacheLocatorKey
ThumbnailPipeline::make_cache_locator(const ThumbnailPipelineRequest &request) {
    cache::ColorPolicyIdentity color_policy{kThumbnailColorPolicyVersion};
    color_policy.working_profile_fingerprint = request.color_policy_fingerprint;
    return {.catalog_source = request.source,
            .routing_policy_version = kThumbnailRoutingPolicyVersion,
            .render_policy_version = kThumbnailRenderPolicyVersion,
            .color_policy_version = std::move(color_policy),
            .page_index = request.page_index,
            .canonical_edge = cache_edge(request.canonical_edge)};
}

ThumbnailPipelineResult ThumbnailPipeline::fetch(const ThumbnailPipelineRequest &request) {
    if (request.source.source_identity_utf8.empty() || request.handler_id.empty() ||
        request.handler_version == 0 || request.page_index >= cache::kMaxArtifactPageCount ||
        request.canonical_edge == 0 || request.canonical_edge > cache::kMaxArtifactDimension) {
        return failure(ThumbnailPipelineStatus::internal_error, "thumbnail request is invalid");
    }
    // Temporary documents stay visible as files, but never reuse or generate graphic previews.
    if (is_temporary_preview_source(request.source.source_identity_utf8)) {
        return failure(ThumbnailPipelineStatus::unsupported, "temporary files have no preview");
    }

    auto resolved_request = request;
    if (color_policy_resolver_) {
        auto color_policy = color_policy_resolver_(request);
        if (!color_policy.empty()) {
            resolved_request.color_policy_fingerprint = std::move(color_policy);
        }
    }
    auto locator = make_cache_locator(resolved_request);
    const auto revoke_locators = [&] {
        auto revoked = locator;
        for (auto edge = cache::kThumbnailCanonicalEdge; edge <= cache::kMaxArtifactDimension;
             edge *= 2U) {
            revoked.canonical_edge = edge;
            static_cast<void>(cache_.erase_locator(revoked));
        }
    };
    const auto materialize_offline = [&]() -> ThumbnailPipelineResult {
        auto offline_locator = locator;
        auto cached = cache_.lookup_locator(offline_locator, request.access_unix_ns);
        for (auto edge = locator.canonical_edge * 2U;
             cached.kind == cache::CacheLookupKind::miss && edge <= cache::kMaxArtifactDimension;
             edge *= 2U) {
            offline_locator.canonical_edge = edge;
            cached = cache_.lookup_locator(offline_locator, request.access_unix_ns);
        }
        for (auto edge = locator.canonical_edge / 2U;
             cached.kind == cache::CacheLookupKind::miss && edge >= cache::kThumbnailCanonicalEdge;
             edge /= 2U) {
            offline_locator.canonical_edge = edge;
            cached = cache_.lookup_locator(offline_locator, request.access_unix_ns);
        }
        if (cached.kind == cache::CacheLookupKind::error) {
            return failure(ThumbnailPipelineStatus::internal_error,
                           cached.error.empty() ? "offline cache lookup failed" : cached.error);
        }
        if ((cached.kind != cache::CacheLookupKind::memory_hit &&
             cached.kind != cache::CacheLookupKind::disk_hit) ||
            !cached.thumbnail || !cached.thumbnail->artifact) {
            return failure(ThumbnailPipelineStatus::disconnected,
                           "verified source is offline and no local preview is available");
        }
        auto result = materialize(*cached.thumbnail->artifact,
                                  {.presentation_edge = request.canonical_edge,
                                   .page_index = request.page_index,
                                   .from_cache = true,
                                   .freshness = PreviewFreshness::offline_unverified});
        if (!result.ok()) {
            static_cast<void>(cache_.erase_locator(offline_locator));
        }
        return result;
    };

    if (request.offline_cache_only) {
        if (!request.password_utf8.empty()) {
            return failure(ThumbnailPipelineStatus::disconnected,
                           "password-protected previews are not persisted for offline use");
        }
        return materialize_offline();
    }

    if (source_resolver_) {
        auto resolved = source_resolver_(resolved_request);
        if (!resolved.ok()) {
            if (request.allow_offline_fallback && request.password_utf8.empty() &&
                permits_offline_fallback(resolved.status)) {
                auto offline = materialize_offline();
                if (offline.ok()) {
                    return offline;
                }
            } else if (revokes_offline_locator(resolved.status)) {
                revoke_locators();
            }
            return failure(resolved.status, std::move(resolved.diagnostic));
        }
        resolved_request.source = std::move(resolved.source);
        resolved_request.handler_id = std::move(resolved.handler_id);
        resolved_request.handler_version = resolved.handler_version;
        resolved_request.color_policy_fingerprint = std::move(resolved.color_policy_fingerprint);
        if (resolved_request.handler_id.empty() || resolved_request.handler_version == 0) {
            return failure(ThumbnailPipelineStatus::internal_error,
                           "thumbnail source resolver returned an invalid handler identity");
        }
    }

    const auto key = make_cache_key(resolved_request);
    if (!resolved_request.password_utf8.empty()) {
        return render_and_publish(resolved_request, key, locator, false);
    }
    const auto finish = [&](ThumbnailPipelineResult result) {
        if (!result.ok() || !resolved_request.extended_limits ||
            key.canonical_edge <= cache::kThumbnailCanonicalEdge) {
            return result;
        }
        // Derive the grid and smaller pane tiers from these pixels, including a large cache hit.
        for (auto edge = cache::kThumbnailCanonicalEdge; edge < key.canonical_edge; edge *= 2U) {
            auto scaled = scale_rgba8_to_edge(result.rgba8, result.width, result.height, edge);
            if (!scaled.ok()) {
                return failure(ThumbnailPipelineStatus::internal_error, std::move(scaled.error));
            }
            auto encoded = cache::encode_qoi_rgba8(scaled.rgba8, scaled.width, scaled.height,
                                                   static_cast<std::size_t>(scaled.width) * 4U);
            if (!encoded.ok()) {
                return failure(ThumbnailPipelineStatus::internal_error, std::move(encoded.error));
            }
            auto canonical_key = key;
            canonical_key.canonical_edge = edge;
            auto canonical_locator = locator;
            canonical_locator.canonical_edge = edge;
            const auto published = cache_.publish(
                {.key = canonical_key,
                 .metadata = {.encoding = cache::ArtifactEncoding::qoi_rgba8,
                              .width = scaled.width,
                              .height = scaled.height,
                              .payload_bytes = 0,
                              .payload_crc32 = 0,
                              .page_count = result.page_count,
                              .source_color_model = result.source_color_model,
                              .provenance = result.provenance},
                 .payload = encoded.bytes,
                 .source_profile_name = result.source_profile_name,
                 .source_profile_fingerprint = result.source_profile_fingerprint,
                 .access_unix_ns = resolved_request.access_unix_ns});
            if (!published.published || !published.index_published ||
                !cache_.bind_locator(canonical_locator, canonical_key)) {
                return failure(ThumbnailPipelineStatus::internal_error,
                               published.error.empty() ? "canonical thumbnail publication failed"
                                                       : published.error);
            }
        }
        return result;
    };
    const auto cached = cache_.lookup(key, resolved_request.access_unix_ns);
    if (cached.kind == cache::CacheLookupKind::memory_hit ||
        cached.kind == cache::CacheLookupKind::disk_hit) {
        if (!cached.thumbnail || !cached.thumbnail->artifact) {
            return failure(ThumbnailPipelineStatus::internal_error,
                           "cache hit did not contain an artifact");
        }
        auto result = materialize(*cached.thumbnail->artifact,
                                  {.presentation_edge = resolved_request.canonical_edge,
                                   .page_index = resolved_request.page_index,
                                   .from_cache = true});
        if (result.ok()) {
            if (!cache_.bind_locator(locator, key)) {
                return failure(ThumbnailPipelineStatus::internal_error,
                               "thumbnail cache locator publication failed");
            }
            return finish(std::move(result));
        }
        static_cast<void>(cache_.erase(key));
    } else if (cached.kind == cache::CacheLookupKind::error) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       cached.error.empty() ? "thumbnail cache lookup failed" : cached.error);
    }
    auto rendered = render_and_publish(resolved_request, key, locator, true);
    if (revokes_offline_locator(rendered.status)) {
        revoke_locators();
    }
    return finish(std::move(rendered));
}

ThumbnailPipelineResult ThumbnailPipeline::render_and_publish(
    const ThumbnailPipelineRequest &request, const cache::CacheKey &key,
    const cache::CacheLocatorKey &locator, const bool allow_persistent_cache) {
    if (!renderer_) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "thumbnail renderer is unavailable");
    }
    auto render_request = request;
    render_request.canonical_edge = key.canonical_edge;
    auto rendered = renderer_(render_request);
    if (rendered.status != ThumbnailPipelineStatus::success) {
        auto result = failure(rendered.status, std::move(rendered.diagnostic));
        if (rendered.status == ThumbnailPipelineStatus::worker_start_failed) {
            result.startup_diagnostic = rendered.startup_diagnostic;
        }
        return result;
    }

    auto decoded = cache::decode_qoi_rgba8(rendered.qoi_payload);
    if (!decoded.ok() || decoded.width != rendered.metadata.width ||
        decoded.height != rendered.metadata.height || rendered.metadata.page_count == 0) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       decoded.error.empty() ? "renderer returned inconsistent metadata"
                                             : decoded.error);
    }

    if (allow_persistent_cache) {
        const auto published =
            cache_.publish({.key = key,
                            .metadata = rendered.metadata,
                            .payload = rendered.qoi_payload,
                            .source_profile_name = rendered.source_profile_name,
                            .source_profile_fingerprint = rendered.source_profile_fingerprint,
                            .access_unix_ns = request.access_unix_ns});
        if (!published.published || !published.index_published) {
            return failure(ThumbnailPipelineStatus::internal_error,
                           published.error.empty() ? "thumbnail cache publication failed"
                                                   : published.error);
        }
        if (!cache_.bind_locator(locator, key)) {
            return failure(ThumbnailPipelineStatus::internal_error,
                           "thumbnail cache locator publication failed");
        }
    }

    auto width = decoded.width;
    auto height = decoded.height;
    auto pixels = std::move(decoded.rgba8);
    if (width > request.canonical_edge || height > request.canonical_edge) {
        auto scaled = scale_rgba8_to_edge(pixels, width, height, request.canonical_edge);
        if (!scaled.ok()) {
            return failure(ThumbnailPipelineStatus::internal_error, std::move(scaled.error));
        }
        width = scaled.width;
        height = scaled.height;
        pixels = std::move(scaled.rgba8);
    }
    return {.status = ThumbnailPipelineStatus::success,
            .width = width,
            .height = height,
            .page_index = request.page_index,
            .page_count = rendered.metadata.page_count,
            .source_color_model = rendered.metadata.source_color_model,
            .provenance = rendered.metadata.provenance,
            .source_profile_name = std::move(rendered.source_profile_name),
            .source_profile_fingerprint = std::move(rendered.source_profile_fingerprint),
            .rgba8 = std::move(pixels),
            .diagnostic = {},
            .from_cache = false,
            .freshness = PreviewFreshness::verified};
}

} // namespace vove::preview
