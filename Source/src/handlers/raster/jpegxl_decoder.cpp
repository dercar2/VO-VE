#include "vove/handlers/raster/jpegxl_decoder.hpp"

#include "vove/color/color_transform.h"
#include "vove/handlers/raster/profile_fingerprint.hpp"
#include "vove/preview/rgba_scaler.hpp"

#include <jxl/cms.h>
#include <jxl/decode.h>
#include <lcms2.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>

namespace vove::handlers::raster {
namespace {

using Error = JpegxlDecodeErrorCode;
constexpr std::size_t kMaximumDecoderBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaximumInputWindow = 16U * 1024U * 1024U;

struct MemoryBudget {
    std::size_t used{};
    std::size_t limit{kMaximumDecoderBytes};
    bool exhausted{};
};

struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};

void *allocate(void *opaque, const std::size_t size) noexcept {
    auto &budget = *static_cast<MemoryBudget *>(opaque);
    if (budget.used > budget.limit || size > budget.limit - budget.used ||
        size > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) {
        budget.exhausted = true;
        return nullptr;
    }
    auto *block = static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
    if (!block) {
        budget.exhausted = true;
        return nullptr;
    }
    block->size = size;
    budget.used += size;
    return block + 1;
}

void deallocate(void *opaque, void *address) noexcept {
    if (address) {
        auto *block = static_cast<Allocation *>(address) - 1;
        static_cast<MemoryBudget *>(opaque)->used -= block->size;
        std::free(block);
    }
}

JpegxlDecodeResult failure(const Error code, const std::string_view detail) {
    return {.image = {}, .error = {code, std::string(detail.substr(0, 256))}};
}

Error source_error(const NativeSourceError &error) noexcept {
    if (error.code == NativeSourceErrorCode::disconnected) {
        return Error::source_disconnected;
    }
    return error.code == NativeSourceErrorCode::source_changed ? Error::source_changed
                                                              : Error::source_io_error;
}

bool valid_limits(const JpegxlDecodeLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 && limits.maximum_source_bytes <= kMaximumJpegxlSourceBytes &&
           limits.maximum_source_pixels != 0 && limits.maximum_source_pixels <= kMaximumJpegxlSourcePixels &&
           limits.maximum_output_edge != 0 && limits.maximum_output_edge <= kMaximumJpegxlOutputEdge &&
           limits.maximum_output_bytes != 0 && limits.maximum_output_bytes <= kMaximumJpegxlOutputBytes;
}

std::string profile_description(const std::span<const std::byte> bytes) {
    const std::unique_ptr<void, decltype(&cmsCloseProfile)> profile(
        cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size())), cmsCloseProfile);
    if (!profile) {
        return "Embedded ICC";
    }
    std::array<char, 256> name{};
    const auto size = cmsGetProfileInfoASCII(profile.get(), cmsInfoDescription, "en", "US",
                                           name.data(), static_cast<cmsUInt32Number>(name.size()));
    return size > 0 && size <= name.size() && name[0] != '\0'
               ? std::string(name.data(), std::find(name.begin(), name.end(), '\0'))
               : "Embedded ICC";
}

JpegxlDecodeResult decode_impl(const NativeSource &source, const JpegxlDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(Error::invalid_limits, "invalid JPEG XL limits");
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(Error::source_limit_exceeded, "JPEG XL source exceeds limit");
    }
    std::array<std::byte, 12> prefix{};
    const auto prefix_size = static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), prefix.size()));
    const auto probe = source.read_at(0, std::span(prefix).first(prefix_size));
    if (!probe.ok()) {
        return failure(source_error(probe.error), "cannot read JPEG XL signature");
    }
    const auto signature = JxlSignatureCheck(reinterpret_cast<const std::uint8_t *>(prefix.data()),
                                             probe.bytes_read);
    if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
        return failure(signature == JXL_SIG_NOT_ENOUGH_BYTES ? Error::truncated_input
                                                             : Error::unsupported_format,
                       "input is not a complete JPEG XL signature");
    }

    MemoryBudget budget;
    const JxlMemoryManager memory{&budget, allocate, deallocate};
    const std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder(
        JxlDecoderCreate(&memory), JxlDecoderDestroy);
    if (!decoder) {
        return failure(Error::resource_limit_exceeded, "cannot allocate JPEG XL decoder");
    }
    auto *dec = decoder.get();
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetKeepOrientation(dec, JXL_FALSE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetCoalescing(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetCms(dec, *JxlGetDefaultCms()) != JXL_DEC_SUCCESS) {
        return failure(Error::decoder_unavailable, "cannot configure JPEG XL decoder");
    }

    std::vector<std::byte> input(256U * 1024U);
    std::vector<std::byte> rgba;
    JxlBasicInfo info{};
    JpegxlMetadata metadata;
    bool have_info{}, have_color{}, input_active{}, closed{};
    std::uint64_t position{};
    std::size_t input_size{};
    auto status = JXL_DEC_NEED_MORE_INPUT;
    const JxlPixelFormat output_format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    while (true) {
        if (status == JXL_DEC_NEED_MORE_INPUT) {
            if (closed) {
                return failure(Error::truncated_input, "JPEG XL data ended before the first image");
            }
            const auto remaining = input_active ? JxlDecoderReleaseInput(dec) : 0;
            if (remaining > input_size) {
                return failure(Error::decode_failed, "invalid JPEG XL input state");
            }
            if (remaining != 0) {
                std::memmove(input.data(), input.data() + input_size - remaining, remaining);
            }
            if (remaining == input.size()) {
                if (input.size() == kMaximumInputWindow) {
                    return failure(Error::resource_limit_exceeded, "JPEG XL input window exceeds limit");
                }
                input.resize(std::min(kMaximumInputWindow, input.size() * 2));
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                source.size() - position, input.size() - remaining));
            const auto read = source.read_at(position, std::span(input).subspan(remaining, count));
            if (!read.ok()) {
                return failure(source_error(read.error), "cannot read JPEG XL source");
            }
            if (read.bytes_read != count) {
                return failure(Error::truncated_input, "JPEG XL source was truncated");
            }
            position += count;
            input_size = remaining + count;
            if (JxlDecoderSetInput(dec, reinterpret_cast<const std::uint8_t *>(input.data()), input_size) != JXL_DEC_SUCCESS) {
                return failure(Error::decode_failed, "cannot supply JPEG XL input");
            }
            input_active = true;
            if (position == source.size()) {
                JxlDecoderCloseInput(dec);
                closed = true;
            }
        } else if (status == JXL_DEC_BASIC_INFO) {
            if (have_info || JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
                return failure(Error::malformed_input, "invalid JPEG XL image header");
            }
            if (info.xsize == 0 || info.ysize == 0 ||
                static_cast<std::uint64_t>(info.xsize) * info.ysize > limits.maximum_source_pixels) {
                return failure(Error::dimension_limit_exceeded, "JPEG XL dimensions exceed limit");
            }
            if (info.num_color_channels != 1 && info.num_color_channels != 3) {
                return failure(Error::unsupported_color, "unsupported JPEG XL color model");
            }
            metadata.source_width = info.xsize;
            metadata.source_height = info.ysize;
            metadata.source_color_model = info.num_color_channels == 1 ? "GRAY" : "RGB";
            metadata.has_alpha = info.alpha_bits != 0;
            have_info = true;
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            if (!have_info || have_color) {
                return failure(Error::malformed_input, "invalid JPEG XL color state");
            }
            JxlColorEncoding original{};
            const auto structured = JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                                                      &original) == JXL_DEC_SUCCESS;
            if (structured && (original.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
                               original.transfer_function == JXL_TRANSFER_FUNCTION_HLG)) {
                return failure(Error::unsupported_color, "PQ/HLG JPEG XL requires tone mapping");
            }
            std::size_t profile_size{};
            if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &profile_size) != JXL_DEC_SUCCESS ||
                profile_size == 0 || profile_size > color::kMaximumIccProfileBytes) {
                return failure(Error::unsupported_color, "JPEG XL source profile is unavailable or oversized");
            }
            std::vector<std::byte> profile(profile_size);
            if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                    reinterpret_cast<std::uint8_t *>(profile.data()), profile.size()) != JXL_DEC_SUCCESS) {
                return failure(Error::unsupported_color, "cannot read JPEG XL source profile");
            }
            metadata.source_color_profile = profile_description(profile);
            metadata.source_profile_fingerprint = profile_sha256(profile);
            metadata.used_embedded_icc = !structured;

            JxlColorEncoding output{};
            output.color_space = info.num_color_channels == 1 ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
            output.white_point = JXL_WHITE_POINT_D65;
            output.primaries = JXL_PRIMARIES_SRGB;
            output.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            output.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
            if (JxlDecoderSetOutputColorProfile(dec, &output, nullptr, 0) != JXL_DEC_SUCCESS) {
                return failure(Error::unsupported_color, "JPEG XL profile cannot be converted to sRGB");
            }
            // In libjxl 0.12, TARGET_DATA still returns the original profile for
            // non-XYB images. SetOutputColorProfile with a CMS controls the pixels.
            have_color = true;
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            std::size_t size{};
            const auto expected = static_cast<std::uint64_t>(info.xsize) * info.ysize * 4;
            if (!have_color || !rgba.empty() ||
                JxlDecoderImageOutBufferSize(dec, &output_format, &size) != JXL_DEC_SUCCESS || size != expected) {
                return failure(Error::malformed_input, "invalid JPEG XL output layout");
            }
            if (size > kMaximumDecoderBytes || budget.used > kMaximumDecoderBytes - size) {
                return failure(Error::resource_limit_exceeded, "JPEG XL image exceeds memory limit");
            }
            budget.limit = kMaximumDecoderBytes - size;
            rgba.resize(size);
            if (JxlDecoderSetImageOutBuffer(dec, &output_format, rgba.data(), rgba.size()) != JXL_DEC_SUCCESS) {
                return failure(Error::decode_failed, "cannot configure JPEG XL output");
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            if (!have_color || rgba.empty()) {
                return failure(Error::decode_failed, "JPEG XL produced no pixels");
            }
            const auto validation = source.validate_unchanged();
            if (!validation.unchanged || validation.error) {
                return failure(source_error(validation.error), "JPEG XL source changed during decoding");
            }
            auto scaled = preview::scale_rgba8_to_edge(rgba, info.xsize, info.ysize,
                                                      limits.maximum_output_edge);
            if (!scaled.ok() || scaled.rgba8.size() > limits.maximum_output_bytes) {
                return failure(Error::output_limit_exceeded, "JPEG XL preview exceeds output limit");
            }
            return {.image = {scaled.width, scaled.height, std::move(scaled.rgba8), std::move(metadata)},
                    .error = {}};
        } else {
            return failure(budget.exhausted ? Error::resource_limit_exceeded : Error::malformed_input,
                           budget.exhausted ? "JPEG XL memory limit exceeded" : "JPEG XL image could not be decoded");
        }
        status = JxlDecoderProcessInput(dec);
    }
}

} // namespace

JpegxlDecodeResult decode_jpegxl(const NativeSource &source, const JpegxlDecodeLimits &limits) {
    try {
        return decode_impl(source, limits);
    } catch (const std::bad_alloc &) {
        return failure(Error::resource_limit_exceeded, "JPEG XL allocation failed");
    } catch (const std::length_error &) {
        return failure(Error::resource_limit_exceeded, "JPEG XL allocation overflow");
    }
}

} // namespace vove::handlers::raster
