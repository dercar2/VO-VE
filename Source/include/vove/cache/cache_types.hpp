#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vove::cache {

inline constexpr std::uint32_t kCacheKeySchemaVersion = 7;
inline constexpr std::uint16_t kVvt1Version = 5;
inline constexpr std::size_t kVvt1HeaderBytes = 72;
inline constexpr std::uint32_t kThumbnailCanonicalEdge = 512;
inline constexpr std::uint32_t kMaxArtifactDimension = 2048;
inline constexpr std::size_t kMaxArtifactBytes = std::size_t{24} * 1024U * 1024U;
inline constexpr std::uint32_t kMaxArtifactPageCount = 1'000'000;
inline constexpr std::size_t kMaxSourceProfileNameBytes = 256;
inline constexpr std::size_t kMaxSourceProfileFingerprintBytes = 128;

struct SourceStamp {
    std::string source_identity_utf8;
    std::uint64_t size_bytes{};
    std::int64_t modified_unix_ns{};
    std::optional<std::string> stable_file_id;

    friend bool operator==(const SourceStamp &, const SourceStamp &) = default;
};

struct ColorPolicyIdentity {
    std::uint32_t version{};
    std::string working_profile_fingerprint;

    ColorPolicyIdentity() = default;
    ColorPolicyIdentity(const std::uint32_t initial_version) noexcept : version(initial_version) {}

    ColorPolicyIdentity &operator=(const std::uint32_t new_version) noexcept {
        version = new_version;
        return *this;
    }

    friend bool operator==(const ColorPolicyIdentity &, const ColorPolicyIdentity &) = default;
};

struct CacheKey {
    SourceStamp source;
    std::string handler_id;
    std::uint32_t handler_version{};
    std::uint32_t render_policy_version{};
    ColorPolicyIdentity color_policy_version{};
    std::uint32_t canonical_edge{kThumbnailCanonicalEdge};
    std::uint32_t page_index{};
    friend bool operator==(const CacheKey &, const CacheKey &) = default;
};

// A locator is intentionally weaker than CacheKey: it describes the last catalogued object
// without guessing the decoder selected after opening the source. It may only lead to an
// explicitly unverified offline preview; reconnect always resolves a fresh exact CacheKey.
struct CacheLocatorKey {
    SourceStamp catalog_source;
    std::uint32_t routing_policy_version{};
    std::uint32_t render_policy_version{};
    ColorPolicyIdentity color_policy_version{};
    std::uint32_t page_index{};

    std::uint32_t canonical_edge{kThumbnailCanonicalEdge};

    friend bool operator==(const CacheLocatorKey &, const CacheLocatorKey &) = default;
};

struct CacheDigest {
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] std::string hex() const;
    friend bool operator==(const CacheDigest &, const CacheDigest &) = default;
};

enum class ArtifactEncoding : std::uint8_t {
    qoi_rgba8 = 1,
};

enum class ColorModel : std::uint8_t {
    unknown,
    gray,
    rgb,
    cmyk,
    lab,
    mixed,
    indexed,
};

enum class PreviewProvenance : std::uint8_t {
    primary_render = 0,
    embedded_preview = 1,
};

struct ArtifactMetadata {
    ArtifactEncoding encoding{ArtifactEncoding::qoi_rgba8};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t payload_bytes{};
    std::uint32_t payload_crc32{};
    std::uint32_t page_count{1};
    ColorModel source_color_model{ColorModel::unknown};
    PreviewProvenance provenance{PreviewProvenance::primary_render};

    friend bool operator==(const ArtifactMetadata &, const ArtifactMetadata &) = default;
};

struct ArtifactColorIdentity {
    std::string source_profile_name;
    std::string source_profile_fingerprint;

    friend bool operator==(const ArtifactColorIdentity &, const ArtifactColorIdentity &) = default;
};

} // namespace vove::cache
