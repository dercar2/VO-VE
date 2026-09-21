#include "raster_executor.hpp"

#include "vove/cache/qoi_codec.hpp"
#include "vove/color/color_transform.h"
#include "vove/handlers/raster/heic_decoder.hpp"
#include "vove/handlers/raster/jpegxl_decoder.hpp"
#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/raster/psd_decoder.hpp"
#include "vove/handlers/raster/qt_raster_decoder.hpp"
#include "vove/handlers/raster/raw_preview_decoder.hpp"
#include "vove/handlers/raster/signature_probe.hpp"
#include "vove/handlers/raster/webp_decoder.hpp"
#ifdef _WIN32
#include "vove/handlers/raster/wic_raster_decoder.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vove::worker {
namespace {

[[nodiscard]] WorkerResult failure(const WorkerJob &job, const ResultStatus status,
                                   std::string diagnostic) {
    if (diagnostic.size() > kMaximumDiagnosticBytes) {
        diagnostic.resize(kMaximumDiagnosticBytes);
    }
    return {.job_id = job.job_id,
            .status = status,
            .width = 0,
            .height = 0,
            .color_model = ColorModel::unknown,
            .color_profile_utf8 = {},
            .source_profile_fingerprint = {},
            .page_index = 0,
            .page_count = 0,
            .bytes_written = 0,
            .diagnostic_utf8 = std::move(diagnostic)};
}

[[nodiscard]] ResultStatus
source_error_status(const handlers::raster::NativeSourceErrorCode code) noexcept {
    using Error = handlers::raster::NativeSourceErrorCode;
    switch (code) {
    case Error::size_limit_exceeded:
    case Error::offset_out_of_range:
    case Error::offset_overflow:
        return ResultStatus::resource_limit;
    case Error::io_error:
        return ResultStatus::source_unavailable;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::disconnected:
        return ResultStatus::disconnected;
    case Error::none:
    case Error::invalid_handle:
    case Error::not_regular_file:
        return ResultStatus::malformed_source;
    }
    return ResultStatus::internal_error;
}

struct FallbackProfile {
    std::optional<handlers::raster::NativeSource> source;
    std::vector<std::byte> bytes;
    ResultStatus error_status{ResultStatus::success};
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] bool profile_handle_is_present(const NativeIoHandle handle) noexcept {
#ifdef _WIN32
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
#else
    return handle >= 0;
#endif
}

[[nodiscard]] FallbackProfile load_fallback_profile(const NativeIoHandle handle) {
    FallbackProfile loaded;
    if (!profile_handle_is_present(handle)) {
        return loaded;
    }
    auto source = handlers::raster::make_native_source(handle, color::kMaximumIccProfileBytes);
    if (!source.ok() || !source.source.has_value()) {
        loaded.error_status = source_error_status(source.error.code);
        loaded.error = "fallback CMYK profile could not be opened";
        return loaded;
    }
    if (source.source->size() == 0) {
        loaded.error_status = ResultStatus::malformed_source;
        loaded.error = "fallback CMYK profile is empty";
        return loaded;
    }
    loaded.bytes.resize(static_cast<std::size_t>(source.source->size()));
    const auto read = source.source->read_at(0, loaded.bytes);
    if (!read.ok() || read.bytes_read != loaded.bytes.size()) {
        loaded.error_status = source_error_status(read.error.code);
        loaded.error = "fallback CMYK profile could not be read";
        return loaded;
    }
    loaded.source = std::move(source.source);
    return loaded;
}

[[nodiscard]] bool fallback_profile_unchanged(const FallbackProfile &profile) noexcept {
    return !profile.source.has_value() || profile.source->validate_unchanged().unchanged;
}

#ifdef _WIN32
[[nodiscard]] ResultStatus decode_error_status(
    const handlers::raster::WicRasterDecodeErrorCode code) noexcept {
    using Error = handlers::raster::WicRasterDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::unsupported_format:
    case Error::unsupported_webp:
    case Error::unsupported_heif:
    case Error::unsupported_avif:
    case Error::decoder_unavailable:
    case Error::unsupported_color:
    case Error::color_transform_failed:
        return ResultStatus::unsupported;
    case Error::color_profile_required:
        return ResultStatus::color_profile_required;
    case Error::invalid_color_profile:
    case Error::color_profile_mismatch:
        return ResultStatus::malformed_source;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}
#endif

[[nodiscard]] ResultStatus decode_error_status(
    const handlers::raster::QtRasterDecodeErrorCode code) noexcept {
    using Error = handlers::raster::QtRasterDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::unsupported_format:
    case Error::unsupported_heif:
    case Error::decoder_unavailable:
    case Error::unsupported_color:
        return ResultStatus::unsupported;
    case Error::color_profile_required:
        return ResultStatus::color_profile_required;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] bool write_all(const NativeIoHandle output,
                             const std::span<const std::byte> bytes) noexcept {
    std::size_t offset{};
    while (offset < bytes.size()) {
#ifdef _WIN32
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        DWORD written{};
        if (WriteFile(static_cast<HANDLE>(output), bytes.data() + offset, chunk, &written,
                      nullptr) == FALSE ||
            written == 0) {
            return false;
        }
        offset += written;
#else
        const auto written = ::write(output, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
#endif
    }
    return true;
}

[[nodiscard]] std::string diagnostic_text(const QString &text, const char *fallback) {
    auto bytes = text.toUtf8();
    if (bytes.isEmpty()) {
        return fallback;
    }
    if (bytes.size() > static_cast<qsizetype>(kMaximumDiagnosticBytes)) {
        bytes.truncate(static_cast<qsizetype>(kMaximumDiagnosticBytes));
    }
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] ColorModel source_color_model(const std::string_view summary) noexcept {
    if (summary.starts_with("CMYK")) {
        return ColorModel::cmyk;
    }
    if (summary.starts_with("GRAY")) {
        return ColorModel::grayscale;
    }
    if (summary.starts_with("LAB")) {
        return ColorModel::lab;
    }
    return summary.empty() ? ColorModel::unknown : ColorModel::rgb;
}

[[nodiscard]] ColorModel source_color_model(const handlers::raster::HeicColorModel model) noexcept {
    switch (model) {
    case handlers::raster::HeicColorModel::unknown:
        return ColorModel::unknown;
    case handlers::raster::HeicColorModel::grayscale:
        return ColorModel::grayscale;
    case handlers::raster::HeicColorModel::rgb:
        return ColorModel::rgb;
    }
    return ColorModel::unknown;
}

[[nodiscard]] ColorModel source_color_model(const handlers::raster::PsdColorModel model) noexcept {
    switch (model) {
    case handlers::raster::PsdColorModel::grayscale:
        return ColorModel::grayscale;
    case handlers::raster::PsdColorModel::rgb:
        return ColorModel::rgb;
    case handlers::raster::PsdColorModel::cmyk:
        return ColorModel::cmyk;
    case handlers::raster::PsdColorModel::lab:
        return ColorModel::lab;
    }
    return ColorModel::unknown;
}

[[nodiscard]] std::string source_profile_name(const std::string_view summary) {
    const auto separator = summary.find(':');
    if (separator == std::string_view::npos) {
        return std::string(summary);
    }
    auto profile = summary.substr(separator + 1);
    while (!profile.empty() && profile.front() == ' ') {
        profile.remove_prefix(1);
    }
    return std::string(profile);
}

[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::HeicDecodeErrorCode code) noexcept {
    using Error = handlers::raster::HeicDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::resource_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::unsupported_format:
    case Error::unsupported_sequence:
    case Error::decoder_unavailable:
    case Error::unsupported_color:
        return ResultStatus::unsupported;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::WebpDecodeErrorCode code) noexcept {
    using Error = handlers::raster::WebpDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::resource_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::unsupported_format:
    case Error::decoder_unavailable:
    case Error::unsupported_color:
        return ResultStatus::unsupported;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::PsdDecodeErrorCode code) noexcept {
    using Error = handlers::raster::PsdDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::resource_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::invalid_color_profile:
        return ResultStatus::malformed_source;
    case Error::unsupported_version:
    case Error::unsupported_depth:
    case Error::unsupported_color_mode:
    case Error::unsupported_compression:
        return ResultStatus::unsupported;
    case Error::color_profile_required:
        return ResultStatus::color_profile_required;
    case Error::color_transform_failed:
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::RawPreviewErrorCode code) noexcept {
    using Error = handlers::raster::RawPreviewErrorCode;
    switch (code) {
    case Error::not_raw:
    case Error::unsupported_format:
        return ResultStatus::unsupported;
    case Error::embedded_preview_unavailable:
        return ResultStatus::embedded_preview_unavailable;
    case Error::malformed_input:
        return ResultStatus::malformed_source;
    case Error::resource_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ResultStatus transform_error_status(const color::TransformError error) noexcept {
    switch (error) {
    case color::TransformError::resource_limit:
        return ResultStatus::resource_limit;
    case color::TransformError::profile_required:
        return ResultStatus::color_profile_required;
    case color::TransformError::invalid_dimensions:
    case color::TransformError::invalid_buffer:
    case color::TransformError::invalid_profile:
    case color::TransformError::profile_mismatch:
        return ResultStatus::malformed_source;
    case color::TransformError::transform_failed:
    case color::TransformError::none:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::JpegxlDecodeErrorCode code) noexcept {
    using Error = handlers::raster::JpegxlDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::resource_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::unsupported_format:
    case Error::unsupported_color:
    case Error::decoder_unavailable:
        return ResultStatus::unsupported;
    case Error::invalid_limits:
    case Error::none:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

struct DecodedRawPreview {
    std::vector<std::byte> rgba8;
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t stride{};
    ColorModel color_model{ColorModel::rgb};
    std::string profile_name;
    std::string profile_fingerprint;
};

[[nodiscard]] bool apply_orientation(DecodedRawPreview &image, const std::uint16_t orientation) {
    if (orientation <= 1 || orientation > 8) {
        return true;
    }
    const auto source_width = image.width;
    const auto source_height = image.height;
    if (source_width == 0 || source_height == 0 ||
        image.stride < static_cast<std::size_t>(source_width) * 4U ||
        image.rgba8.size() < image.stride * source_height) {
        return false;
    }
    const auto swaps_axes = orientation >= 5;
    const auto output_width = swaps_axes ? source_height : source_width;
    const auto output_height = swaps_axes ? source_width : source_height;
    const auto output_stride = static_cast<std::size_t>(output_width) * 4U;
    std::vector<std::byte> oriented(output_stride * output_height);
    for (std::uint32_t y{}; y < source_height; ++y) {
        for (std::uint32_t x{}; x < source_width; ++x) {
            std::uint32_t destination_x{};
            std::uint32_t destination_y{};
            switch (orientation) {
            case 2:
                destination_x = source_width - 1U - x;
                destination_y = y;
                break;
            case 3:
                destination_x = source_width - 1U - x;
                destination_y = source_height - 1U - y;
                break;
            case 4:
                destination_x = x;
                destination_y = source_height - 1U - y;
                break;
            case 5:
                destination_x = y;
                destination_y = x;
                break;
            case 6:
                destination_x = source_height - 1U - y;
                destination_y = x;
                break;
            case 7:
                destination_x = source_height - 1U - y;
                destination_y = source_width - 1U - x;
                break;
            case 8:
                destination_x = y;
                destination_y = source_width - 1U - x;
                break;
            default:
                return false;
            }
            const auto source_offset =
                static_cast<std::size_t>(y) * image.stride + static_cast<std::size_t>(x) * 4U;
            const auto destination_offset =
                static_cast<std::size_t>(destination_y) * output_stride +
                static_cast<std::size_t>(destination_x) * 4U;
            std::memcpy(oriented.data() + destination_offset, image.rgba8.data() + source_offset,
                        4U);
        }
    }
    image.rgba8 = std::move(oriented);
    image.width = output_width;
    image.height = output_height;
    image.stride = output_stride;
    return true;
}

#ifdef _WIN32
class UniqueHandle final {
  public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            static_cast<void>(CloseHandle(handle_));
        }
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
                static_cast<void>(CloseHandle(handle_));
            }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_;
};

[[nodiscard]] std::optional<std::wstring> private_temporary_directory() {
    std::array<wchar_t, 32'768> supplied{};
    const auto supplied_length = GetEnvironmentVariableW(L"VOVE_SANDBOX_TEMP", supplied.data(),
                                                         static_cast<DWORD>(supplied.size()));
    if (supplied_length != 0 && supplied_length < supplied.size()) {
        return std::wstring{supplied.data(), supplied_length};
    }
    std::array<wchar_t, MAX_PATH + 1> fallback{};
    const auto length = GetTempPathW(static_cast<DWORD>(fallback.size()), fallback.data());
    if (length == 0 || length >= fallback.size()) {
        return std::nullopt;
    }
    auto path = std::wstring{fallback.data(), length};
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}
#endif

[[nodiscard]] std::optional<DecodedRawPreview>
decode_raw_jpeg(const handlers::raster::RawEmbeddedPreview &preview, const WorkerJob &job,
                ResultStatus &status, std::string &diagnostic) {
    auto deferred_orientation = preview.orientation;
#ifdef _WIN32
    const auto temporary_directory = private_temporary_directory();
    if (!temporary_directory) {
        status = ResultStatus::internal_error;
        diagnostic = "RAW sandbox temporary directory is unavailable";
        return std::nullopt;
    }
    UniqueHandle file;
    for (std::uint32_t attempt{}; attempt < 4 && file.get() == INVALID_HANDLE_VALUE; ++attempt) {
        const auto name = *temporary_directory + L"\\vove-raw-" +
                          std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(job.job_id) + L"-" + std::to_wstring(attempt) + L".tmp";
        file = UniqueHandle(CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
    }
    if (file.get() == INVALID_HANDLE_VALUE || !write_all(file.get(), preview.jpeg)) {
        status = ResultStatus::internal_error;
        diagnostic = "RAW embedded preview could not be staged";
        return std::nullopt;
    }
    LARGE_INTEGER beginning{};
    if (SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN) == FALSE) {
        status = ResultStatus::internal_error;
        diagnostic = "RAW staged preview could not be rewound";
        return std::nullopt;
    }
    auto staged = handlers::raster::make_native_source(file.get(), preview.jpeg.size());
    if (!staged.ok() || !staged.source.has_value()) {
        status = source_error_status(staged.error.code);
        diagnostic = "RAW staged preview could not be bounded";
        return std::nullopt;
    }
    const handlers::raster::WicRasterDecodeLimits limits{
        .maximum_source_bytes = handlers::raster::kMaximumRawPreviewBytes,
        .maximum_source_pixels = handlers::raster::kMaximumWicRasterSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::raster::kMaximumWicRasterEdge),
        .maximum_output_bytes = std::min(job.limits.maximum_output_bytes,
                                         handlers::raster::kMaximumWicRasterOutputBytes),
        .fallback_cmyk_icc = {},
        // WIC's rotate-after-convert path is not reliable for every camera JPEG. Decode pixels
        // unchanged, retain its EXIF value, then use the bounded in-process transform below.
        .apply_orientation = false,
        .allow_external_rgb_color_space = preview.adobe_rgb,
    };
    auto decoded = handlers::raster::decode_wic_raster(*staged.source, limits);
    if (!decoded.ok()) {
        status = decode_error_status(decoded.error.code);
        diagnostic = decoded.error.detail.empty() ? "RAW embedded JPEG decode failed"
                                                  : std::move(decoded.error.detail);
        return std::nullopt;
    }
    if (deferred_orientation == 0 && decoded.image.metadata.exif_orientation > 1) {
        deferred_orientation = decoded.image.metadata.exif_orientation;
    }
    const auto summary = decoded.image.metadata.color_summary.empty()
                             ? std::string{"RGB: sRGB (assumed)"}
                             : decoded.image.metadata.color_summary;
    DecodedRawPreview result{
        .rgba8 = std::move(decoded.image.rgba8),
        .width = decoded.image.width,
        .height = decoded.image.height,
        .stride = static_cast<std::size_t>(decoded.image.width) * 4U,
        .color_model = source_color_model(summary),
        .profile_name = source_profile_name(summary),
        .profile_fingerprint = std::move(decoded.image.metadata.source_profile_fingerprint),
    };
#else
    const handlers::raster::QtRasterDecodeLimits limits{
        .maximum_source_bytes = handlers::raster::kMaximumRawPreviewBytes,
        .maximum_source_pixels = handlers::raster::kMaximumQtRasterSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::raster::kMaximumCanonicalRasterEdge),
        .maximum_output_bytes = std::min<std::uint64_t>(
            job.limits.maximum_output_bytes,
            static_cast<std::uint64_t>(handlers::raster::kMaximumCanonicalRasterEdge) *
                handlers::raster::kMaximumCanonicalRasterEdge * 4ULL),
        .fallback_cmyk_icc = {},
        .apply_orientation = preview.orientation == 0,
    };
    auto decoded = handlers::raster::decode_qt_raster_bytes(preview.jpeg, limits);
    if (!decoded.ok()) {
        status = decode_error_status(decoded.error.code);
        diagnostic = decoded.error.detail.isEmpty() ? "RAW embedded JPEG decode failed"
                                                    : decoded.error.detail.toUtf8().toStdString();
        return std::nullopt;
    }
    if (preview.orientation == 0) {
        deferred_orientation = 0;
    }
    const auto &image = decoded.image.rgba8;
    std::vector<std::byte> pixels(static_cast<std::size_t>(image.sizeInBytes()));
    std::memcpy(pixels.data(), image.constBits(), pixels.size());
    const auto summary = decoded.image.metadata.color_summary.isEmpty()
                             ? std::string{"RGB: sRGB (assumed)"}
                             : decoded.image.metadata.color_summary.toUtf8().toStdString();
    DecodedRawPreview result{
        .rgba8 = std::move(pixels),
        .width = static_cast<std::uint32_t>(image.width()),
        .height = static_cast<std::uint32_t>(image.height()),
        .stride = static_cast<std::size_t>(image.bytesPerLine()),
        .color_model = source_color_model(summary),
        .profile_name = source_profile_name(summary),
        .profile_fingerprint = std::move(decoded.image.metadata.source_profile_fingerprint),
    };
#endif
    if (preview.adobe_rgb && result.profile_name.find("assumed") != std::string::npos) {
        auto transformed = color::to_srgb_rgba8({
            .width = result.width,
            .height = result.height,
            .source_stride = result.stride,
            .source_format = color::PixelFormat::rgba8,
            .source_rgb_space = color::RgbColorSpace::adobe_rgb_1998,
            .source_pixels = result.rgba8,
            .embedded_icc = {},
            .fallback_cmyk_icc = {},
        });
        if (!transformed.ok()) {
            status = transform_error_status(transformed.error);
            diagnostic = transformed.detail.empty() ? "Adobe RGB RAW preview conversion failed"
                                                    : std::move(transformed.detail);
            return std::nullopt;
        }
        result.rgba8 = std::move(transformed.rgba_pixels);
        result.stride = transformed.rgba_stride;
        result.profile_name = std::move(transformed.metadata.source_profile);
    }
    if (deferred_orientation != 0 && !apply_orientation(result, deferred_orientation)) {
        status = ResultStatus::internal_error;
        diagnostic = "RAW embedded preview orientation could not be applied";
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool retryable_raw_candidate_failure(const ResultStatus status) noexcept {
    switch (status) {
    case ResultStatus::malformed_source:
    case ResultStatus::unsupported:
    case ResultStatus::resource_limit:
    case ResultStatus::color_profile_required:
        return true;
    default:
        return false;
    }
}

struct EncodedThumbnail {
    std::span<const std::byte> rgba;
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t stride{};
    std::uint32_t page_count{};
    ColorModel color_model{ColorModel::unknown};
    PreviewProvenance provenance{PreviewProvenance::primary_render};
    std::string color_profile;
    std::string source_profile_fingerprint;
};

[[nodiscard]] WorkerResult encode_success(const NativeIoHandle output_handle, const WorkerJob &job,
                                          EncodedThumbnail thumbnail) {
    const auto encoded = cache::encode_qoi_rgba8(thumbnail.rgba, thumbnail.width, thumbnail.height,
                                                 thumbnail.stride);
    if (!encoded.ok()) {
        return failure(job, ResultStatus::internal_error, encoded.error);
    }
    if (encoded.bytes.empty() || encoded.bytes.size() > job.limits.maximum_output_bytes) {
        return failure(job, ResultStatus::resource_limit,
                       "encoded thumbnail exceeds the output limit");
    }
    if (!write_all(output_handle, encoded.bytes)) {
        return failure(job, ResultStatus::internal_error, "thumbnail output write failed");
    }
    if (thumbnail.color_profile.size() > kMaximumProfileNameBytes) {
        thumbnail.color_profile.resize(kMaximumProfileNameBytes);
    }
    return {.job_id = job.job_id,
            .status = ResultStatus::success,
            .width = thumbnail.width,
            .height = thumbnail.height,
            .color_model = thumbnail.color_model,
            .provenance = thumbnail.provenance,
            .color_profile_utf8 = std::move(thumbnail.color_profile),
            .source_profile_fingerprint = std::move(thumbnail.source_profile_fingerprint),
            .page_index = 0,
            .page_count = std::max(1U, thumbnail.page_count),
            .bytes_written = encoded.bytes.size(),
            .diagnostic_utf8 = {}};
}

} // namespace

WorkerResult execute_raster_job_native(const NativeJobIo io, const WorkerJob &job) {
#ifdef _WIN32
    const auto platform_source_limit = job.source_format_hint == SourceFormatHint::camera_raw
                                           ? handlers::raster::kMaximumRawSourceBytes
                                           : handlers::raster::kMaximumWicRasterSourceBytes;
#else
    const auto platform_source_limit = job.source_format_hint == SourceFormatHint::camera_raw
                                           ? handlers::raster::kMaximumRawSourceBytes
                                           : handlers::raster::kMaximumQtRasterSourceBytes;
#endif
    auto maximum_source = std::min<std::uint64_t>(
        job.limits.maximum_input_bytes,
        job.source_format_hint == SourceFormatHint::camera_raw
            ? platform_source_limit
                : std::max(platform_source_limit, handlers::raster::kMaximumPsbSourceBytes));
    auto source_result = handlers::raster::make_native_source(io.source, maximum_source);
    if (!source_result.ok() || !source_result.source.has_value()) {
        return failure(job, source_error_status(source_result.error.code),
                       "source could not be opened");
    }
    auto fallback_profile = load_fallback_profile(io.profile);
    if (!fallback_profile.ok()) {
        return failure(job, fallback_profile.error_status, std::move(fallback_profile.error));
    }
    std::array<std::byte, handlers::raster::kMaxSignatureProbeBytes> signature{};
    const auto signature_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(source_result.source->size(), signature.size()));
    const auto signature_read =
        source_result.source->read_at(0, std::span<std::byte>(signature).first(signature_size));
    if (!signature_read.ok() || signature_read.bytes_read != signature_size) {
        return failure(job, ResultStatus::source_unavailable,
                       "source signature became unavailable");
    }
    const auto probe = handlers::raster::probe_raster_signature(
        std::span<const std::byte>(signature).first(signature_size));
    if (probe.format != handlers::raster::RasterFormat::Psd) {
        maximum_source = std::min(maximum_source, platform_source_limit);
        if (source_result.source->size() > maximum_source) {
            return failure(job, ResultStatus::resource_limit, "source exceeds its format limit");
        }
    }
    if (job.source_format_hint == SourceFormatHint::camera_raw) {
        const handlers::raster::RawPreviewLimits limits{
            .maximum_source_bytes = maximum_source,
            .maximum_metadata_bytes = handlers::raster::kMaximumRawMetadataBytes,
            .maximum_preview_bytes = handlers::raster::kMaximumRawPreviewBytes,
            .maximum_metadata_reads = handlers::raster::kMaximumRawMetadataReads,
        };
        auto extracted =
            handlers::raster::extract_raw_embedded_preview(*source_result.source, limits);
        if (!extracted.ok()) {
            return failure(job, decode_error_status(extracted.error.code),
                           extracted.error.detail.empty() ? "RAW preview extraction failed"
                                                          : std::move(extracted.error.detail));
        }
        std::optional<DecodedRawPreview> decoded;
        ResultStatus first_failure_status{ResultStatus::internal_error};
        std::string first_failure_diagnostic;
        bool recorded_failure{};
        for (const auto &candidate : extracted.previews) {
            ResultStatus status{ResultStatus::success};
            std::string diagnostic;
            auto current = decode_raw_jpeg(candidate, job, status, diagnostic);
            if (current) {
                decoded = std::move(current);
                break;
            }
            if (!recorded_failure) {
                first_failure_status = status;
                first_failure_diagnostic = diagnostic;
                recorded_failure = true;
            }
            if (!retryable_raw_candidate_failure(status)) {
                return failure(job, status, std::move(diagnostic));
            }
        }
        if (!decoded) {
            return failure(job, first_failure_status,
                           first_failure_diagnostic.empty() ? "RAW embedded JPEG decode failed"
                                                            : std::move(first_failure_diagnostic));
        }
        const auto validation = source_result.source->validate_unchanged();
        if (!validation.unchanged) {
            return failure(job, source_error_status(validation.error.code),
                           "RAW source changed or became unavailable while decoding");
        }
        return encode_success(
            io.output, job,
            {.rgba = decoded->rgba8,
             .width = decoded->width,
             .height = decoded->height,
             .stride = decoded->stride,
             .page_count = 1,
             .color_model = decoded->color_model,
             .provenance = PreviewProvenance::embedded_preview,
             .color_profile = std::move(decoded->profile_name),
             .source_profile_fingerprint = std::move(decoded->profile_fingerprint)});
    }
    if (probe.format == handlers::raster::RasterFormat::Psd) {
        const handlers::raster::PsdDecodeLimits limits{
            .maximum_source_bytes = maximum_source,
            .maximum_source_pixels = handlers::raster::kMaximumPsdSourcePixels,
            .maximum_dimension = handlers::raster::kMaximumPsbDimension,
            .maximum_output_edge =
                std::min(job.limits.canonical_edge, handlers::raster::kMaximumPsdOutputEdge),
            .maximum_output_bytes = std::min<std::uint64_t>(
                job.limits.maximum_output_bytes, handlers::raster::kMaximumPsdOutputBytes),
            .maximum_zip_bytes = handlers::raster::kMaximumPsdZipBytes,
            .fallback_cmyk_icc = fallback_profile.bytes,
        };
        const auto decoded = handlers::raster::decode_psd_composite(*source_result.source, limits);
        if (!decoded.ok()) {
            return failure(job, decode_error_status(decoded.error.code),
                           decoded.error.detail.empty() ? "PSD decode failed"
                                                        : decoded.error.detail);
        }
        if (!fallback_profile_unchanged(fallback_profile)) {
            return failure(job, ResultStatus::source_changed,
                           "fallback CMYK profile changed while decoding");
        }
        return encode_success(
            io.output, job,
            {.rgba = decoded.image.rgba8,
             .width = decoded.image.width,
             .height = decoded.image.height,
             .stride = static_cast<std::size_t>(decoded.image.width) * 4U,
             .page_count = 1,
             .color_model = source_color_model(decoded.image.metadata.source_color_model),
             .color_profile = decoded.image.metadata.source_color_profile,
             .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
    }
    if (probe.format == handlers::raster::RasterFormat::Jpegxl) {
        const handlers::raster::JpegxlDecodeLimits limits{
            .maximum_source_bytes = maximum_source,
            .maximum_source_pixels = handlers::raster::kMaximumJpegxlSourcePixels,
            .maximum_output_edge =
                std::min(job.limits.canonical_edge, handlers::raster::kMaximumJpegxlOutputEdge),
            .maximum_output_bytes = std::min<std::uint64_t>(
                job.limits.maximum_output_bytes, handlers::raster::kMaximumJpegxlOutputBytes)};
        const auto decoded = handlers::raster::decode_jpegxl(*source_result.source, limits);
        if (!decoded.ok()) {
            return failure(job, decode_error_status(decoded.error.code), decoded.error.detail);
        }
        return encode_success(
            io.output, job,
            {.rgba = decoded.image.rgba8,
             .width = decoded.image.width,
             .height = decoded.image.height,
             .stride = static_cast<std::size_t>(decoded.image.width) * 4U,
             .page_count = decoded.image.metadata.page_count,
             .color_model = source_color_model(decoded.image.metadata.source_color_model),
             .color_profile = decoded.image.metadata.source_color_profile,
             .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
    }
    if (probe.format == handlers::raster::RasterFormat::Heif ||
        probe.format == handlers::raster::RasterFormat::Avif) {
        const handlers::raster::HeicDecodeLimits limits{
            .maximum_source_bytes = maximum_source,
            .maximum_source_pixels = handlers::raster::kMaximumHeicSourcePixels,
            .maximum_output_edge =
                std::min(job.limits.canonical_edge, handlers::raster::kMaximumHeicOutputEdge),
            .maximum_output_bytes = std::min<std::uint64_t>(
                job.limits.maximum_output_bytes, handlers::raster::kMaximumHeicOutputBytes),
        };
        const auto decoded = handlers::raster::decode_heic(*source_result.source, limits);
        if (!decoded.ok()) {
            return failure(job, decode_error_status(decoded.error.code),
                           decoded.error.detail.empty() ? "HEIC decode failed"
                                                        : decoded.error.detail);
        }
        const auto validation = source_result.source->validate_unchanged();
        if (!validation.unchanged) {
            return failure(job, source_error_status(validation.error.code),
                           "source changed or became unavailable while decoding");
        }
        const auto color_model = source_color_model(decoded.image.metadata.source_color_model);
        return encode_success(
            io.output, job,
            {.rgba = decoded.image.rgba8,
             .width = decoded.image.width,
             .height = decoded.image.height,
             .stride = static_cast<std::size_t>(decoded.image.width) * 4U,
             .page_count = decoded.image.metadata.page_count,
             .color_model = color_model,
             .color_profile = decoded.image.metadata.source_color_profile,
             .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
    }
    if (probe.format == handlers::raster::RasterFormat::Webp) {
        const handlers::raster::WebpDecodeLimits limits{
            .maximum_source_bytes =
                std::min<std::uint64_t>(maximum_source, handlers::raster::kMaximumWebpSourceBytes),
            .maximum_source_pixels = handlers::raster::kMaximumWebpSourcePixels,
            .maximum_output_edge =
                std::min(job.limits.canonical_edge, handlers::raster::kMaximumWebpOutputEdge),
            .maximum_output_bytes = std::min<std::uint64_t>(
                job.limits.maximum_output_bytes, handlers::raster::kMaximumWebpOutputBytes),
        };
        const auto decoded = handlers::raster::decode_webp(*source_result.source, limits);
        if (!decoded.ok()) {
            return failure(job, decode_error_status(decoded.error.code),
                           decoded.error.detail.empty() ? "WebP decode failed"
                                                        : decoded.error.detail);
        }
        const auto validation = source_result.source->validate_unchanged();
        if (!validation.unchanged) {
            return failure(job, source_error_status(validation.error.code),
                           "source changed or became unavailable while decoding");
        }
        const auto color_model = source_color_model(decoded.image.metadata.source_color_model);
        return encode_success(
            io.output, job,
            {.rgba = decoded.image.rgba8,
             .width = decoded.image.width,
             .height = decoded.image.height,
             .stride = static_cast<std::size_t>(decoded.image.width) * 4U,
             .page_count = decoded.image.metadata.page_count,
             .color_model = color_model,
             .color_profile = source_profile_name(decoded.image.metadata.source_color_profile),
             .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
    }

#ifdef _WIN32
    if (probe.format == handlers::raster::RasterFormat::Jpeg) {
        const handlers::raster::QtRasterDecodeLimits limits{
            .maximum_source_bytes = maximum_source,
            .maximum_source_pixels = handlers::raster::kMaximumQtRasterSourcePixels,
            .maximum_output_edge =
                std::min(job.limits.canonical_edge,
                         handlers::raster::kMaximumCanonicalRasterEdge),
            .maximum_output_bytes = std::min<std::uint64_t>(
                job.limits.maximum_output_bytes,
                static_cast<std::uint64_t>(handlers::raster::kMaximumCanonicalRasterEdge) *
                    handlers::raster::kMaximumCanonicalRasterEdge * 4ULL),
            .fallback_cmyk_icc = fallback_profile.bytes,
        };
        const auto decoded = handlers::raster::decode_qt_raster(*source_result.source, limits);
        if (!decoded.ok()) {
            return failure(job, decode_error_status(decoded.error.code),
                           diagnostic_text(decoded.error.detail, "JPEG decode failed"));
        }
        const auto validation = source_result.source->validate_unchanged();
        if (!validation.unchanged) {
            return failure(job, source_error_status(validation.error.code),
                           "source changed or became unavailable while decoding");
        }
        if (!fallback_profile_unchanged(fallback_profile)) {
            return failure(job, ResultStatus::source_changed,
                           "fallback CMYK profile changed while decoding");
        }

        const auto &image = decoded.image.rgba8;
        const auto rgba =
            std::span<const std::byte>(reinterpret_cast<const std::byte *>(image.constBits()),
                                       static_cast<std::size_t>(image.sizeInBytes()));
        const auto profile = diagnostic_text(decoded.image.metadata.color_summary, "RGB: sRGB");
        return encode_success(
            io.output, job,
            {.rgba = rgba,
             .width = static_cast<std::uint32_t>(image.width()),
             .height = static_cast<std::uint32_t>(image.height()),
             .stride = static_cast<std::size_t>(image.bytesPerLine()),
             .page_count = decoded.image.metadata.page_count.value_or(1U),
             .color_model = source_color_model(profile),
             .color_profile = source_profile_name(profile),
             .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
    }

    const handlers::raster::WicRasterDecodeLimits limits{
        .maximum_source_bytes = maximum_source,
        .maximum_source_pixels = handlers::raster::kMaximumWicRasterSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::raster::kMaximumWicRasterEdge),
        .maximum_output_bytes = std::min<std::uint64_t>(
            job.limits.maximum_output_bytes, handlers::raster::kMaximumWicRasterOutputBytes),
        .fallback_cmyk_icc = fallback_profile.bytes,
    };
    const auto decoded = handlers::raster::decode_wic_raster(*source_result.source, limits);
#else
    const handlers::raster::QtRasterDecodeLimits limits{
        .maximum_source_bytes = maximum_source,
        .maximum_source_pixels = handlers::raster::kMaximumQtRasterSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::raster::kMaximumCanonicalRasterEdge),
        .maximum_output_bytes = std::min<std::uint64_t>(
            job.limits.maximum_output_bytes,
            static_cast<std::uint64_t>(handlers::raster::kMaximumCanonicalRasterEdge) *
                handlers::raster::kMaximumCanonicalRasterEdge * 4ULL),
        .fallback_cmyk_icc = fallback_profile.bytes,
    };
    const auto decoded = handlers::raster::decode_qt_raster(*source_result.source, limits);
#endif
    if (!decoded.ok()) {
#ifdef _WIN32
        return failure(job, decode_error_status(decoded.error.code),
                       decoded.error.detail.empty() ? "raster decode failed"
                                                    : decoded.error.detail);
#else
        return failure(job, decode_error_status(decoded.error.code),
                       diagnostic_text(decoded.error.detail, "raster decode failed"));
#endif
    }

    const auto validation = source_result.source->validate_unchanged();
    if (!validation.unchanged) {
        return failure(job, source_error_status(validation.error.code),
                       "source changed or became unavailable while decoding");
    }
    if (!fallback_profile_unchanged(fallback_profile)) {
        return failure(job, ResultStatus::source_changed,
                       "fallback CMYK profile changed while decoding");
    }

#ifdef _WIN32
    const auto &image = decoded.image.rgba8;
    const auto width = decoded.image.width;
    const auto height = decoded.image.height;
    const auto profile = decoded.image.metadata.color_summary.empty()
                             ? std::string{"RGB: sRGB"}
                             : decoded.image.metadata.color_summary;
    return encode_success(
        io.output, job,
        {.rgba = image,
         .width = width,
         .height = height,
         .stride = static_cast<std::size_t>(width) * 4U,
         .page_count = decoded.image.metadata.page_count.value_or(1U),
         .color_model = source_color_model(profile),
         .color_profile = source_profile_name(profile),
         .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
#else
    const auto &image = decoded.image.rgba8;
    const auto rgba =
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(image.constBits()),
                                   static_cast<std::size_t>(image.sizeInBytes()));
    const auto profile = diagnostic_text(decoded.image.metadata.color_summary, "RGB: sRGB");
    return encode_success(
        io.output, job,
        {.rgba = rgba,
         .width = static_cast<std::uint32_t>(image.width()),
         .height = static_cast<std::uint32_t>(image.height()),
         .stride = static_cast<std::size_t>(image.bytesPerLine()),
         .page_count = decoded.image.metadata.page_count.value_or(1U),
         .color_model = source_color_model(profile),
         .color_profile = source_profile_name(profile),
         .source_profile_fingerprint = decoded.image.metadata.source_profile_fingerprint});
#endif
}

} // namespace vove::worker
