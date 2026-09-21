#pragma once

#include "vove/cache/persistent_thumbnail_cache.hpp"
#include "vove/preview/thumbnail_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vove::preview {

inline constexpr std::string_view kRasterThumbnailHandlerId = "raster.qt";
inline constexpr std::uint32_t kRasterThumbnailHandlerVersion = 6;
inline constexpr std::string_view kPsdCompositeHandlerId = "raster.psd.composite";
inline constexpr std::uint32_t kPsdCompositeHandlerVersion = 2;
inline constexpr std::string_view kRawEmbeddedPreviewHandlerId = "raster.raw.embedded-preview";
inline constexpr std::uint32_t kRawEmbeddedPreviewHandlerVersion = 1;
inline constexpr std::string_view kMuPdfThumbnailHandlerId = "document.mupdf.pdf";
inline constexpr std::uint32_t kMuPdfThumbnailHandlerVersion = 1;
inline constexpr std::string_view kPostScriptPreviewHandlerId = "document.postscript.preview";
inline constexpr std::uint32_t kPostScriptPreviewHandlerVersion = 2;
inline constexpr std::string_view kCdrEmbeddedPreviewHandlerId = "document.cdr.embedded-preview";
inline constexpr std::uint32_t kCdrEmbeddedPreviewHandlerVersion = 1;
inline constexpr std::string_view kInDesignPreviewHandlerId = "document.indesign.embedded-preview";
inline constexpr std::uint32_t kInDesignPreviewHandlerVersion = 1;
inline constexpr std::string_view kIdmlPreviewHandlerId = "document.idml.embedded-preview";
inline constexpr std::uint32_t kIdmlPreviewHandlerVersion = 1;
inline constexpr std::string_view kAffinityPreviewHandlerId = "document.affinity.embedded-preview";
inline constexpr std::uint32_t kAffinityPreviewHandlerVersion = 1;
inline constexpr std::string_view kArchivePreviewHandlerId = "document.archive.embedded-preview";
inline constexpr std::uint32_t kArchivePreviewHandlerVersion = 1;
inline constexpr std::string_view kSvgThumbnailHandlerId = "document.resvg.svg";
inline constexpr std::uint32_t kSvgThumbnailHandlerVersion = 1;
inline constexpr std::string_view kHpglThumbnailHandlerId = "document.hp2xx.hpgl";
inline constexpr std::uint32_t kHpglThumbnailHandlerVersion = 1;
inline constexpr std::string_view kXcfThumbnailHandlerId = "document.kimageformats.xcf";
inline constexpr std::uint32_t kXcfThumbnailHandlerVersion = 2;
inline constexpr std::string_view kDxfEmbeddedPreviewHandlerId = "document.dxf.embedded-preview";
inline constexpr std::uint32_t kDxfEmbeddedPreviewHandlerVersion = 1;
inline constexpr std::uint32_t kThumbnailRenderPolicyVersion = 1;
inline constexpr std::uint32_t kThumbnailColorPolicyVersion = 1;
inline constexpr std::uint32_t kThumbnailRoutingPolicyVersion = 1;

enum class ThumbnailPipelineStatus : std::uint8_t {
    success,
    unsupported,
    malformed_source,
    timed_out,
    cancelled,
    resource_limit,
    source_unavailable,
    source_changed,
    source_not_found,
    permission_denied,
    authentication_failed,
    disconnected,
    color_profile_required,
    password_required,
    document_password_incorrect,
    ghostscript_required,
    embedded_preview_unavailable,
    internal_error,
    pdf_input_too_large,
    memory_limit,
    processing_timed_out,
    worker_start_failed,
};

enum class PreviewFreshness : std::uint8_t {
    verified,
    offline_unverified,
};

struct ThumbnailPipelineRequest {
    cache::SourceStamp source;
    std::int64_t access_unix_ns{};
    std::uint32_t canonical_edge{cache::kThumbnailCanonicalEdge};
    std::string handler_id{std::string(kRasterThumbnailHandlerId)};
    std::uint32_t handler_version{kRasterThumbnailHandlerVersion};
    std::uint32_t page_index{};
    std::string password_utf8;
    std::string color_policy_fingerprint{"sRGB-v1|cmyk:none"};
    bool offline_cache_only{};
    bool allow_offline_fallback{true};
    bool selected_view{};
    bool extended_limits{};
};

struct RenderedThumbnail {
    ThumbnailPipelineStatus status{ThumbnailPipelineStatus::internal_error};
    cache::ArtifactMetadata metadata;
    std::vector<std::byte> qoi_payload;
    std::string source_profile_name;
    std::string source_profile_fingerprint;
    std::string diagnostic;
    WorkerStartupDiagnostic startup_diagnostic{};
};

struct ThumbnailPipelineResult {
    ThumbnailPipelineStatus status{ThumbnailPipelineStatus::internal_error};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t page_index{};
    std::uint32_t page_count{};
    cache::ColorModel source_color_model{cache::ColorModel::unknown};
    cache::PreviewProvenance provenance{cache::PreviewProvenance::primary_render};
    std::string source_profile_name;
    std::string source_profile_fingerprint;
    std::vector<std::byte> rgba8;
    std::string diagnostic;
    bool from_cache{};
    PreviewFreshness freshness{PreviewFreshness::verified};
    WorkerStartupDiagnostic startup_diagnostic{};

    [[nodiscard]] bool ok() const noexcept {
        return status == ThumbnailPipelineStatus::success;
    }
};

using ThumbnailRenderer = std::function<RenderedThumbnail(const ThumbnailPipelineRequest &)>;

struct ResolvedThumbnailSource {
    ThumbnailPipelineStatus status{ThumbnailPipelineStatus::success};
    cache::SourceStamp source;
    std::string handler_id{std::string(kRasterThumbnailHandlerId)};
    std::uint32_t handler_version{kRasterThumbnailHandlerVersion};
    std::string color_policy_fingerprint{"sRGB-v1|cmyk:none"};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == ThumbnailPipelineStatus::success;
    }
};

using ThumbnailSourceResolver =
    std::function<ResolvedThumbnailSource(const ThumbnailPipelineRequest &)>;
using ThumbnailColorPolicyResolver = std::function<std::string(const ThumbnailPipelineRequest &)>;

class ThumbnailPipeline final {
  public:
    ThumbnailPipeline(cache::PersistentThumbnailCache &cache, ThumbnailRenderer renderer,
                      ThumbnailSourceResolver source_resolver = {},
                      ThumbnailColorPolicyResolver color_policy_resolver = {});

    [[nodiscard]] ThumbnailPipelineResult fetch(const ThumbnailPipelineRequest &request);
    [[nodiscard]] static cache::CacheKey make_cache_key(const ThumbnailPipelineRequest &request);
    [[nodiscard]] static cache::CacheLocatorKey
    make_cache_locator(const ThumbnailPipelineRequest &request);

  private:
    [[nodiscard]] ThumbnailPipelineResult
    render_and_publish(const ThumbnailPipelineRequest &request, const cache::CacheKey &key,
                       const cache::CacheLocatorKey &locator, bool allow_persistent_cache);

    cache::PersistentThumbnailCache &cache_;
    ThumbnailRenderer renderer_;
    ThumbnailSourceResolver source_resolver_;
    ThumbnailColorPolicyResolver color_policy_resolver_;
};

} // namespace vove::preview
