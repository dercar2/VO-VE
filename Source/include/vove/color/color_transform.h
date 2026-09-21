#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vove::color {

// Preserved through worker IPC and the thumbnail cache; this is not an ICC profile name.
inline constexpr char kUnprofiledCmykApproximation[] = "unprofiled; approximate colors";

enum class PixelFormat {
    gray8,
    rgb8,
    rgba8,
    cmyk8,
    lab8,
};

enum class RgbColorSpace {
    srgb,
    adobe_rgb_1998,
};

enum class TransformError {
    none,
    invalid_dimensions,
    invalid_buffer,
    resource_limit,
    profile_required,
    invalid_profile,
    profile_mismatch,
    transform_failed,
};

inline constexpr std::uint64_t kMaximumTransformPixels = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaximumIccProfileBytes = 16U * 1024U * 1024U;

struct TransformRequest {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t source_stride{};
    PixelFormat source_format{PixelFormat::rgb8};
    RgbColorSpace source_rgb_space{RgbColorSpace::srgb};
    std::span<const std::byte> source_pixels;
    std::span<const std::byte> embedded_icc;
    std::span<const std::byte> fallback_cmyk_icc;
};

struct MonitorTransformRequest {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t canonical_stride{};
    std::span<const std::byte> canonical_srgb_rgba;
    std::span<const std::byte> monitor_icc;
};

struct ColorMetadata {
    std::string source_model;
    std::string source_profile;
    std::string output_profile{"sRGB IEC61966-2.1"};
    bool used_embedded_profile{};
};

struct TransformResult {
    TransformError error{TransformError::none};
    std::string detail;
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t rgba_stride{};
    std::vector<std::byte> rgba_pixels;
    ColorMetadata metadata;

    [[nodiscard]] bool ok() const noexcept {
        return error == TransformError::none;
    }
};

[[nodiscard]] TransformResult to_srgb_rgba8(const TransformRequest &request);
[[nodiscard]] TransformResult to_monitor_rgba8(const MonitorTransformRequest &request);

struct ScreenProfileCacheLimits {
    std::size_t maximum_entries{64};
    std::size_t maximum_bytes{64U * 1024U * 1024U};
};

struct ScreenProfileCacheStats {
    std::size_t entries{};
    std::size_t accounted_bytes{};
};

class ScreenProfileCache final {
  public:
    explicit ScreenProfileCache(ScreenProfileCacheLimits limits = {});
    ~ScreenProfileCache();

    ScreenProfileCache(const ScreenProfileCache &) = delete;
    ScreenProfileCache &operator=(const ScreenProfileCache &) = delete;
    ScreenProfileCache(ScreenProfileCache &&) noexcept;
    ScreenProfileCache &operator=(ScreenProfileCache &&) noexcept;

    [[nodiscard]] std::shared_ptr<const TransformResult> find(std::string_view canonical_artifact,
                                                              std::string_view monitor_fingerprint);
    [[nodiscard]] bool store(std::string canonical_artifact, std::string monitor_fingerprint,
                             TransformResult result);
    void clear() noexcept;
    [[nodiscard]] ScreenProfileCacheStats stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vove::color
