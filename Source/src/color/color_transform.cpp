#include "vove/color/color_transform.h"

#include <lcms2.h>

#include <array>
#include <exception>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace vove::color {
namespace {

struct ProfileCloser {
    void operator()(cmsHPROFILE profile) const noexcept {
        if (profile != nullptr) {
            cmsCloseProfile(profile);
        }
    }
};

struct TransformCloser {
    void operator()(cmsHTRANSFORM transform) const noexcept {
        if (transform != nullptr) {
            cmsDeleteTransform(transform);
        }
    }
};

struct ToneCurveCloser {
    void operator()(cmsToneCurve *curve) const noexcept {
        if (curve != nullptr) {
            cmsFreeToneCurve(curve);
        }
    }
};

using Profile = std::unique_ptr<void, ProfileCloser>;
using Transform = std::unique_ptr<void, TransformCloser>;
using ToneCurve = std::unique_ptr<cmsToneCurve, ToneCurveCloser>;

constexpr std::size_t kMaximumCacheKeyBytes = 256;
constexpr std::size_t kEntryAccountingOverhead = 128;
constexpr cmsUInt32Number kMaximumProfileNameBytes = 512;

[[nodiscard]] std::size_t channels(const PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::gray8:
        return 1;
    case PixelFormat::rgb8:
    case PixelFormat::lab8:
        return 3;
    case PixelFormat::rgba8:
    case PixelFormat::cmyk8:
        return 4;
    }
    return 0;
}

[[nodiscard]] cmsUInt32Number lcms_format(const PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::gray8:
        return TYPE_GRAY_8;
    case PixelFormat::rgb8:
        return TYPE_RGB_8;
    case PixelFormat::rgba8:
        return TYPE_RGBA_8;
    case PixelFormat::cmyk8:
        return TYPE_CMYK_8;
    case PixelFormat::lab8:
        return TYPE_Lab_8;
    }
    return 0;
}

[[nodiscard]] cmsColorSpaceSignature expected_space(const PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::gray8:
        return cmsSigGrayData;
    case PixelFormat::rgb8:
    case PixelFormat::rgba8:
        return cmsSigRgbData;
    case PixelFormat::cmyk8:
        return cmsSigCmykData;
    case PixelFormat::lab8:
        return cmsSigLabData;
    }
    return cmsSigRgbData;
}

[[nodiscard]] std::string model_name(const PixelFormat format) {
    switch (format) {
    case PixelFormat::gray8:
        return "GRAY";
    case PixelFormat::rgb8:
    case PixelFormat::rgba8:
        return "RGB";
    case PixelFormat::cmyk8:
        return "CMYK";
    case PixelFormat::lab8:
        return "Lab";
    }
    return "Unknown";
}

[[nodiscard]] std::string profile_name(cmsHPROFILE profile) {
    const auto required =
        cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", nullptr, 0);
    if (required == 0) {
        return "Unnamed ICC";
    }
    if (required > kMaximumProfileNameBytes) {
        return "ICC profile";
    }
    std::string value(required, '\0');
    const auto copied =
        cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", value.data(), required);
    if (copied == 0 || copied > required) {
        return "Unnamed ICC";
    }
    value.resize(copied);
    // LCMS can include padding after the first terminator in its byte count.
    const auto terminator = value.find('\0');
    if (terminator == std::string::npos) {
        return "Unnamed ICC";
    }
    value.resize(terminator);
    return value.empty() ? "Unnamed ICC" : value;
}

[[nodiscard]] TransformResult failure(const TransformError error, std::string detail,
                                      const PixelFormat format) {
    TransformResult result;
    result.error = error;
    result.detail = std::move(detail);
    result.metadata.source_model = model_name(format);
    return result;
}

[[nodiscard]] TransformResult resource_failure(const PixelFormat format) {
    static_cast<void>(format);
    TransformResult result;
    result.error = TransformError::resource_limit;
    return result;
}

[[nodiscard]] bool exceeds_pixel_limit(const std::uint32_t width,
                                       const std::uint32_t height) noexcept {
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
           kMaximumTransformPixels;
}

[[nodiscard]] bool profile_size_is_valid(const std::span<const std::byte> profile) noexcept {
    return profile.size() <= kMaximumIccProfileBytes &&
           profile.size() <= std::numeric_limits<cmsUInt32Number>::max();
}

[[nodiscard]] Profile create_adobe_rgb_1998_profile() {
    cmsCIExyY white_point{};
    if (!cmsWhitePointFromTemp(&white_point, 6504.0)) {
        return Profile(nullptr);
    }
    constexpr cmsCIExyYTRIPLE primaries{
        .Red = {.x = 0.6400, .y = 0.3300, .Y = 1.0},
        .Green = {.x = 0.2100, .y = 0.7100, .Y = 1.0},
        .Blue = {.x = 0.1500, .y = 0.0600, .Y = 1.0},
    };
    constexpr double gamma = 563.0 / 256.0;
    std::array<ToneCurve, 3> curves{ToneCurve(cmsBuildGamma(nullptr, gamma)),
                                    ToneCurve(cmsBuildGamma(nullptr, gamma)),
                                    ToneCurve(cmsBuildGamma(nullptr, gamma))};
    if (!curves[0] || !curves[1] || !curves[2]) {
        return Profile(nullptr);
    }
    const std::array<cmsToneCurve *, 3> raw_curves{curves[0].get(), curves[1].get(),
                                                   curves[2].get()};
    return Profile(cmsCreateRGBProfile(&white_point, &primaries, raw_curves.data()));
}

[[nodiscard]] TransformResult to_srgb_rgba8_impl(const TransformRequest &request) {
    const auto source_channels = channels(request.source_format);
    if (request.width == 0 || request.height == 0 || source_channels == 0) {
        return failure(TransformError::invalid_dimensions, "empty image", request.source_format);
    }
    if (exceeds_pixel_limit(request.width, request.height)) {
        return resource_failure(request.source_format);
    }
    if (request.width > std::numeric_limits<std::size_t>::max() / source_channels) {
        return failure(TransformError::invalid_dimensions, "source row overflows",
                       request.source_format);
    }
    const auto minimum_stride = static_cast<std::size_t>(request.width) * source_channels;
    if (request.source_stride < minimum_stride ||
        request.height > std::numeric_limits<std::size_t>::max() / request.source_stride ||
        request.source_pixels.size() < request.source_stride * request.height) {
        return failure(TransformError::invalid_buffer, "source buffer is truncated",
                       request.source_format);
    }
    const auto maximum_output_width = std::numeric_limits<std::size_t>::max() / 4U;
    if (static_cast<std::uint64_t>(request.width) >
        static_cast<std::uint64_t>(maximum_output_width)) {
        return failure(TransformError::invalid_dimensions, "output row overflows",
                       request.source_format);
    }
    const auto output_stride = static_cast<std::size_t>(request.width) * 4U;
    if (request.height > std::numeric_limits<std::size_t>::max() / output_stride) {
        return failure(TransformError::invalid_dimensions, "output image overflows",
                       request.source_format);
    }

    Profile source_profile(nullptr);
    bool used_embedded = false;
    if (!request.embedded_icc.empty()) {
        if (!profile_size_is_valid(request.embedded_icc)) {
            return resource_failure(request.source_format);
        }
        source_profile.reset(
            cmsOpenProfileFromMem(request.embedded_icc.data(),
                                  static_cast<cmsUInt32Number>(request.embedded_icc.size())));
        used_embedded = true;
    } else if (request.source_format == PixelFormat::gray8) {
        ToneCurve gamma(cmsBuildGamma(nullptr, 2.2));
        if (gamma) {
            source_profile.reset(cmsCreateGrayProfile(cmsD50_xyY(), gamma.get()));
        }
    } else if (request.source_format == PixelFormat::rgb8 ||
               request.source_format == PixelFormat::rgba8) {
        switch (request.source_rgb_space) {
        case RgbColorSpace::srgb:
            source_profile.reset(cmsCreate_sRGBProfile());
            break;
        case RgbColorSpace::adobe_rgb_1998:
            source_profile = create_adobe_rgb_1998_profile();
            break;
        }
    } else if (request.source_format == PixelFormat::lab8) {
        source_profile.reset(cmsCreateLab4Profile(nullptr));
    } else if (!request.fallback_cmyk_icc.empty()) {
        if (!profile_size_is_valid(request.fallback_cmyk_icc)) {
            return resource_failure(request.source_format);
        }
        source_profile.reset(
            cmsOpenProfileFromMem(request.fallback_cmyk_icc.data(),
                                  static_cast<cmsUInt32Number>(request.fallback_cmyk_icc.size())));
    } else {
        return failure(TransformError::profile_required,
                       "untagged CMYK requires a configured ICC profile", request.source_format);
    }

    if (!source_profile) {
        return failure(TransformError::invalid_profile, "cannot open source ICC profile",
                       request.source_format);
    }
    if (cmsGetColorSpace(source_profile.get()) != expected_space(request.source_format)) {
        return failure(TransformError::profile_mismatch,
                       "ICC color space does not match source pixels", request.source_format);
    }

    Profile output_profile(cmsCreate_sRGBProfile());
    if (!output_profile) {
        return failure(TransformError::transform_failed, "cannot create sRGB profile",
                       request.source_format);
    }
    Transform transform(cmsCreateTransform(
        source_profile.get(), lcms_format(request.source_format), output_profile.get(), TYPE_RGB_8,
        INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION));
    if (!transform) {
        return failure(TransformError::transform_failed, "cannot create ICC transform",
                       request.source_format);
    }

    TransformResult result;
    result.width = request.width;
    result.height = request.height;
    result.rgba_stride = output_stride;
    result.rgba_pixels.resize(output_stride * request.height);
    result.metadata.source_model = model_name(request.source_format);
    result.metadata.source_profile =
        !used_embedded && request.source_format != PixelFormat::gray8 &&
                request.source_format != PixelFormat::cmyk8 &&
                request.source_format != PixelFormat::lab8 &&
                request.source_rgb_space == RgbColorSpace::adobe_rgb_1998
            ? "Adobe RGB (1998)"
            : profile_name(source_profile.get());
    result.metadata.used_embedded_profile = used_embedded;

    std::vector<std::byte> transformed_row(static_cast<std::size_t>(request.width) * 3U);
    for (std::uint32_t row = 0; row < request.height; ++row) {
        const auto *source = request.source_pixels.data() + request.source_stride * row;
        auto *destination = result.rgba_pixels.data() + output_stride * row;
        cmsDoTransform(transform.get(), source, transformed_row.data(), request.width);
        for (std::uint32_t column = 0; column < request.width; ++column) {
            const auto source_offset = static_cast<std::size_t>(column) * 3U;
            const auto destination_offset = static_cast<std::size_t>(column) * 4U;
            destination[destination_offset] = transformed_row[source_offset];
            destination[destination_offset + 1U] = transformed_row[source_offset + 1U];
            destination[destination_offset + 2U] = transformed_row[source_offset + 2U];
            destination[destination_offset + 3U] =
                request.source_format == PixelFormat::rgba8
                    ? source[static_cast<std::size_t>(column) * 4U + 3U]
                    : std::byte{255};
        }
    }
    return result;
}

[[nodiscard]] TransformResult to_monitor_rgba8_impl(const MonitorTransformRequest &request) {
    constexpr auto source_format = PixelFormat::rgba8;
    if (request.width == 0 || request.height == 0) {
        return failure(TransformError::invalid_dimensions, "empty image", source_format);
    }
    if (exceeds_pixel_limit(request.width, request.height)) {
        return resource_failure(source_format);
    }
    if (request.monitor_icc.empty()) {
        return failure(TransformError::profile_required, "monitor ICC profile is required",
                       source_format);
    }
    if (!profile_size_is_valid(request.monitor_icc)) {
        return resource_failure(source_format);
    }
    if (static_cast<std::uint64_t>(request.width) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4U)) {
        return failure(TransformError::invalid_dimensions, "canonical row overflows",
                       source_format);
    }
    const auto minimum_stride = static_cast<std::size_t>(request.width) * 4U;
    if (request.canonical_stride < minimum_stride ||
        request.height > std::numeric_limits<std::size_t>::max() / request.canonical_stride ||
        request.canonical_srgb_rgba.size() < request.canonical_stride * request.height) {
        return failure(TransformError::invalid_buffer, "canonical buffer is truncated",
                       source_format);
    }

    Profile source_profile(cmsCreate_sRGBProfile());
    Profile monitor_profile(cmsOpenProfileFromMem(
        request.monitor_icc.data(), static_cast<cmsUInt32Number>(request.monitor_icc.size())));
    if (!source_profile || !monitor_profile) {
        return failure(TransformError::invalid_profile, "cannot open monitor ICC profile",
                       source_format);
    }
    if (cmsGetColorSpace(monitor_profile.get()) != cmsSigRgbData) {
        return failure(TransformError::profile_mismatch, "monitor ICC profile is not RGB",
                       source_format);
    }

    Transform transform(cmsCreateTransform(source_profile.get(), TYPE_RGBA_8, monitor_profile.get(),
                                           TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                           cmsFLAGS_BLACKPOINTCOMPENSATION));
    if (!transform) {
        return failure(TransformError::transform_failed, "cannot create monitor transform",
                       source_format);
    }

    const auto output_stride = minimum_stride;
    TransformResult result;
    result.width = request.width;
    result.height = request.height;
    result.rgba_stride = output_stride;
    result.rgba_pixels.resize(output_stride * request.height);
    result.metadata.source_model = "RGB";
    result.metadata.source_profile = "sRGB IEC61966-2.1";
    result.metadata.output_profile = profile_name(monitor_profile.get());

    std::vector<std::byte> transformed_row(static_cast<std::size_t>(request.width) * 3U);
    for (std::uint32_t row = 0; row < request.height; ++row) {
        const auto *source = request.canonical_srgb_rgba.data() + request.canonical_stride * row;
        auto *destination = result.rgba_pixels.data() + output_stride * row;
        cmsDoTransform(transform.get(), source, transformed_row.data(), request.width);
        for (std::uint32_t column = 0; column < request.width; ++column) {
            const auto source_offset = static_cast<std::size_t>(column) * 3U;
            const auto destination_offset = static_cast<std::size_t>(column) * 4U;
            destination[destination_offset] = transformed_row[source_offset];
            destination[destination_offset + 1U] = transformed_row[source_offset + 1U];
            destination[destination_offset + 2U] = transformed_row[source_offset + 2U];
            destination[destination_offset + 3U] = source[destination_offset + 3U];
        }
    }
    return result;
}

} // namespace

TransformResult to_srgb_rgba8(const TransformRequest &request) {
    try {
        return to_srgb_rgba8_impl(request);
    } catch (const std::bad_alloc &) {
        return resource_failure(request.source_format);
    } catch (const std::length_error &) {
        return resource_failure(request.source_format);
    }
}

TransformResult to_monitor_rgba8(const MonitorTransformRequest &request) {
    try {
        return to_monitor_rgba8_impl(request);
    } catch (const std::bad_alloc &) {
        return resource_failure(PixelFormat::rgba8);
    } catch (const std::length_error &) {
        return resource_failure(PixelFormat::rgba8);
    }
}

struct ScreenProfileCache::Impl {
    struct Entry {
        std::string canonical_artifact;
        std::string monitor_fingerprint;
        std::shared_ptr<const TransformResult> result;
        std::size_t accounted_bytes{};
    };

    explicit Impl(const ScreenProfileCacheLimits cache_limits) : limits(cache_limits) {}

    ScreenProfileCacheLimits limits;
    mutable std::mutex mutex;
    std::list<Entry> entries;
    std::size_t accounted_bytes{};
};

ScreenProfileCache::ScreenProfileCache(const ScreenProfileCacheLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}

ScreenProfileCache::~ScreenProfileCache() = default;
ScreenProfileCache::ScreenProfileCache(ScreenProfileCache &&) noexcept = default;
ScreenProfileCache &ScreenProfileCache::operator=(ScreenProfileCache &&) noexcept = default;

std::shared_ptr<const TransformResult>
ScreenProfileCache::find(const std::string_view canonical_artifact,
                         const std::string_view monitor_fingerprint) {
    if (!impl_ || canonical_artifact.size() > kMaximumCacheKeyBytes ||
        monitor_fingerprint.size() > kMaximumCacheKeyBytes) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    for (auto iterator = impl_->entries.begin(); iterator != impl_->entries.end(); ++iterator) {
        if (iterator->canonical_artifact == canonical_artifact &&
            iterator->monitor_fingerprint == monitor_fingerprint) {
            const auto result = iterator->result;
            impl_->entries.splice(impl_->entries.begin(), impl_->entries, iterator);
            return result;
        }
    }
    return {};
}

bool ScreenProfileCache::store(std::string canonical_artifact, std::string monitor_fingerprint,
                               TransformResult result) {
    if (!impl_ || !result.ok() || canonical_artifact.empty() || monitor_fingerprint.empty() ||
        canonical_artifact.size() > kMaximumCacheKeyBytes ||
        monitor_fingerprint.size() > kMaximumCacheKeyBytes || impl_->limits.maximum_entries == 0 ||
        impl_->limits.maximum_bytes == 0) {
        return false;
    }

    try {
        std::size_t entry_bytes = kEntryAccountingOverhead;
        const std::array<std::size_t, 7> allocations{
            result.rgba_pixels.capacity(),
            result.detail.capacity(),
            result.metadata.source_model.capacity(),
            result.metadata.source_profile.capacity(),
            result.metadata.output_profile.capacity(),
            canonical_artifact.capacity(),
            monitor_fingerprint.capacity(),
        };
        for (const auto allocation : allocations) {
            if (allocation > std::numeric_limits<std::size_t>::max() - entry_bytes) {
                return false;
            }
            entry_bytes += allocation;
        }
        if (entry_bytes > impl_->limits.maximum_bytes) {
            return false;
        }
        auto stored_result = std::make_shared<const TransformResult>(std::move(result));

        std::lock_guard lock(impl_->mutex);
        for (auto iterator = impl_->entries.begin(); iterator != impl_->entries.end();) {
            if (iterator->canonical_artifact == canonical_artifact &&
                iterator->monitor_fingerprint == monitor_fingerprint) {
                impl_->accounted_bytes -= iterator->accounted_bytes;
                iterator = impl_->entries.erase(iterator);
            } else {
                ++iterator;
            }
        }

        impl_->entries.push_front(Impl::Entry{
            .canonical_artifact = std::move(canonical_artifact),
            .monitor_fingerprint = std::move(monitor_fingerprint),
            .result = std::move(stored_result),
            .accounted_bytes = entry_bytes,
        });
        impl_->accounted_bytes += entry_bytes;
        while (impl_->entries.size() > impl_->limits.maximum_entries ||
               impl_->accounted_bytes > impl_->limits.maximum_bytes) {
            impl_->accounted_bytes -= impl_->entries.back().accounted_bytes;
            impl_->entries.pop_back();
        }
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    } catch (const std::length_error &) {
        return false;
    }
}

void ScreenProfileCache::clear() noexcept {
    if (!impl_) {
        return;
    }
    std::lock_guard lock(impl_->mutex);
    impl_->entries.clear();
    impl_->accounted_bytes = 0;
}

ScreenProfileCacheStats ScreenProfileCache::stats() const noexcept {
    if (!impl_) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    return {
        .entries = impl_->entries.size(),
        .accounted_bytes = impl_->accounted_bytes,
    };
}

} // namespace vove::color
