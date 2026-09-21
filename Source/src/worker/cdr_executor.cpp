#include "cdr_executor.hpp"

#include "vove/cache/qoi_codec.hpp"
#include "vove/handlers/cdr/embedded_preview.hpp"
#include "vove/handlers/dxf/embedded_preview.hpp"
#include "vove/handlers/archive/embedded_preview.hpp"
#include "vove/handlers/affinity/embedded_preview.hpp"
#include "vove/handlers/indesign/embedded_preview.hpp"
#include "vove/handlers/publishing_signature.hpp"

#ifdef _WIN32
#include "vove/handlers/raster/wic_raster_decoder.hpp"
#else
#include "vove/handlers/raster/qt_raster_decoder.hpp"

#include <QColorSpace>
#include <QImage>
#include <QString>
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
#include <string_view>
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
    case Error::disconnected:
        return ResultStatus::disconnected;
    case Error::io_error:
        return ResultStatus::source_unavailable;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::none:
    case Error::invalid_handle:
    case Error::not_regular_file:
        return ResultStatus::malformed_source;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ResultStatus cdr_error_status(const handlers::cdr::CdrPreviewStatus status) noexcept {
    using Status = handlers::cdr::CdrPreviewStatus;
    switch (status) {
    case Status::success:
        return ResultStatus::success;
    case Status::unsupported_container:
        return ResultStatus::unsupported;
    case Status::no_preview:
        return ResultStatus::embedded_preview_unavailable;
    case Status::malformed:
        return ResultStatus::malformed_source;
    case Status::resource_limit:
        return ResultStatus::resource_limit;
    case Status::io_error:
        return ResultStatus::source_unavailable;
    }
    return ResultStatus::internal_error;
}

#ifdef _WIN32
[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::WicRasterDecodeErrorCode code) noexcept {
    using Error = handlers::raster::WicRasterDecodeErrorCode;
    switch (code) {
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_changed:
        return ResultStatus::source_changed;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::color_profile_required:
        return ResultStatus::color_profile_required;
    case Error::unsupported_format:
    case Error::unsupported_webp:
    case Error::unsupported_heif:
    case Error::unsupported_avif:
    case Error::decoder_unavailable:
    case Error::unsupported_color:
    case Error::invalid_color_profile:
    case Error::color_profile_mismatch:
    case Error::color_transform_failed:
        return ResultStatus::unsupported;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}
#else
[[nodiscard]] ResultStatus
decode_error_status(const handlers::raster::QtRasterDecodeErrorCode code) noexcept {
    using Error = handlers::raster::QtRasterDecodeErrorCode;
    switch (code) {
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::output_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::truncated_input:
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::color_profile_required:
        return ResultStatus::color_profile_required;
    case Error::unsupported_format:
    case Error::unsupported_heif:
    case Error::decoder_unavailable:
    case Error::unsupported_color:
        return ResultStatus::unsupported;
    case Error::none:
    case Error::invalid_limits:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}
#endif

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

[[nodiscard]] bool starts_with_case_insensitive(const std::string_view text,
                                                const std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           std::equal(
               prefix.begin(), prefix.end(), text.begin(), [](const char left, const char right) {
                   const auto normalize = [](const char value) {
                       return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A')
                                                           : value;
                   };
                   return normalize(left) == normalize(right);
               });
}

[[nodiscard]] ColorModel source_color_model(const std::string_view summary) noexcept {
    if (starts_with_case_insensitive(summary, "GRAY")) {
        return ColorModel::grayscale;
    }
    if (starts_with_case_insensitive(summary, "CMYK")) {
        return ColorModel::cmyk;
    }
    if (starts_with_case_insensitive(summary, "LAB")) {
        return ColorModel::lab;
    }
    if (starts_with_case_insensitive(summary, "MIXED")) {
        return ColorModel::mixed;
    }
    return ColorModel::rgb;
}

[[nodiscard]] std::string source_profile_name(std::string summary) {
    const auto separator = summary.find(':');
    if (separator != std::string::npos) {
        summary.erase(0, separator + 1U);
    }
    const auto first = summary.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    summary.erase(0, first);
    const auto last = summary.find_last_not_of(" \t\r\n");
    summary.resize(last + 1U);
    if (summary.size() > kMaximumProfileNameBytes) {
        summary.resize(kMaximumProfileNameBytes);
    }
    return summary;
}

struct DecodedCandidate {
    std::vector<std::byte> rgba8;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::size_t stride{};
    ColorModel color_model{ColorModel::unknown};
    std::string profile_name;
    std::string profile_fingerprint;
    bool page_one{};
};

[[nodiscard]] bool better_candidate(const DecodedCandidate &candidate,
                                    const DecodedCandidate &current) noexcept {
    const auto candidate_area =
        static_cast<std::uint64_t>(candidate.source_width) * candidate.source_height;
    const auto current_area =
        static_cast<std::uint64_t>(current.source_width) * current.source_height;
    return candidate_area > current_area ||
           (candidate_area == current_area && candidate.page_one && !current.page_one);
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

[[nodiscard]] std::optional<DecodedCandidate>
decode_candidate(const handlers::cdr::CdrEmbeddedCandidate &candidate, const WorkerJob &job,
                 ResultStatus &status, std::string &diagnostic) {
    const auto temporary_directory = private_temporary_directory();
    if (!temporary_directory) {
        status = ResultStatus::internal_error;
        diagnostic = "CDR sandbox temporary directory is unavailable";
        return std::nullopt;
    }
    UniqueHandle file;
    for (std::uint32_t attempt{}; attempt < 4 && file.get() == INVALID_HANDLE_VALUE; ++attempt) {
        const auto temporary_name =
            *temporary_directory + L"\\vove-cdr-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(job.job_id) + L"-" + std::to_wstring(attempt) + L".tmp";
        file = UniqueHandle(CreateFileW(
            temporary_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
    }
    if (file.get() == INVALID_HANDLE_VALUE) {
        status = ResultStatus::internal_error;
        diagnostic = "CDR sandbox temporary file could not be opened (" +
                     std::to_string(GetLastError()) + ')';
        return std::nullopt;
    }
    if (!write_all(file.get(), candidate.encoded_image)) {
        status = ResultStatus::internal_error;
        diagnostic = "CDR embedded preview could not be staged";
        return std::nullopt;
    }
    LARGE_INTEGER beginning{};
    if (SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN) == FALSE) {
        status = ResultStatus::internal_error;
        diagnostic = "CDR staged preview could not be rewound";
        return std::nullopt;
    }
    auto source = handlers::raster::make_native_source(
        file.get(), static_cast<std::uint64_t>(candidate.encoded_image.size()));
    if (!source.ok() || !source.source.has_value()) {
        status = source_error_status(source.error.code);
        diagnostic = "CDR staged preview could not be bounded";
        return std::nullopt;
    }
    const handlers::raster::WicRasterDecodeLimits limits{
        .maximum_source_bytes = handlers::cdr::kMaximumCdrPreviewBytes,
        .maximum_source_pixels = handlers::raster::kMaximumWicRasterSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::raster::kMaximumWicRasterEdge),
        .maximum_output_bytes = std::min(job.limits.maximum_output_bytes,
                                         handlers::raster::kMaximumWicRasterOutputBytes),
        .fallback_cmyk_icc = {},
    };
    auto decoded = handlers::raster::decode_wic_raster(*source.source, limits);
    if (!decoded.ok()) {
        status = decode_error_status(decoded.error.code);
        diagnostic = decoded.error.detail.empty() ? "CDR embedded image decode failed"
                                                  : std::move(decoded.error.detail);
        return std::nullopt;
    }
    const auto summary = decoded.image.metadata.color_summary.empty()
                             ? std::string{"RGB: sRGB (assumed)"}
                             : decoded.image.metadata.color_summary;
    return DecodedCandidate{
        .rgba8 = std::move(decoded.image.rgba8),
        .width = decoded.image.width,
        .height = decoded.image.height,
        .source_width = decoded.image.metadata.source_width,
        .source_height = decoded.image.metadata.source_height,
        .stride = static_cast<std::size_t>(decoded.image.width) * 4U,
        .color_model = source_color_model(summary),
        .profile_name = source_profile_name(summary),
        .profile_fingerprint = std::move(decoded.image.metadata.source_profile_fingerprint),
        .page_one = candidate.page_one,
    };
}
#else
[[nodiscard]] std::optional<DecodedCandidate>
decode_candidate(const handlers::cdr::CdrEmbeddedCandidate &candidate, const WorkerJob &job,
                 ResultStatus &status, std::string &diagnostic) {
    const handlers::raster::QtRasterDecodeLimits limits{
        .maximum_source_bytes = handlers::cdr::kMaximumCdrPreviewBytes,
        .maximum_source_pixels = handlers::raster::kMaximumQtRasterSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::raster::kMaximumCanonicalRasterEdge),
        .maximum_output_bytes = std::min<std::uint64_t>(
            job.limits.maximum_output_bytes,
            static_cast<std::uint64_t>(handlers::raster::kMaximumCanonicalRasterEdge) *
                handlers::raster::kMaximumCanonicalRasterEdge * 4ULL),
        .fallback_cmyk_icc = {},
    };
    auto decoded = handlers::raster::decode_qt_raster_bytes(candidate.encoded_image, limits);
    if (!decoded.ok()) {
        status = decode_error_status(decoded.error.code);
        diagnostic = decoded.error.detail.isEmpty() ? "CDR embedded image decode failed"
                                                    : decoded.error.detail.toUtf8().toStdString();
        return std::nullopt;
    }
    const auto &image = decoded.image.rgba8;
    std::vector<std::byte> pixels(static_cast<std::size_t>(image.sizeInBytes()));
    std::memcpy(pixels.data(), image.constBits(), pixels.size());
    const auto summary = decoded.image.metadata.color_summary.isEmpty()
                             ? std::string{"RGB: sRGB (assumed)"}
                             : decoded.image.metadata.color_summary.toUtf8().toStdString();
    return DecodedCandidate{
        .rgba8 = std::move(pixels),
        .width = static_cast<std::uint32_t>(image.width()),
        .height = static_cast<std::uint32_t>(image.height()),
        .source_width = decoded.image.metadata.source_width,
        .source_height = decoded.image.metadata.source_height,
        .stride = static_cast<std::size_t>(image.bytesPerLine()),
        .color_model = source_color_model(summary),
        .profile_name = source_profile_name(summary),
        .profile_fingerprint = std::move(decoded.image.metadata.source_profile_fingerprint),
        .page_one = candidate.page_one,
    };
}
#endif

} // namespace

WorkerResult execute_cdr_job_native(const NativeJobIo io, const WorkerJob &job) {
    if (job.page_index != 0) {
        return failure(job, ResultStatus::unsupported,
                       "embedded document preview exposes only the first saved view");
    }
    auto source_result =
        handlers::raster::make_native_source(io.source, job.limits.maximum_input_bytes);
    if (!source_result.ok() || !source_result.source.has_value()) {
        return failure(job, source_error_status(source_result.error.code),
                       "embedded document source could not be opened");
    }

    std::array<std::byte, 16> signature{};
    const auto signature_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(signature.size(), source_result.source->size()));
    const auto read = source_result.source->read_at(0, std::span(signature).first(signature_size));
    if (!read.ok() || read.bytes_read != signature_size) {
        return failure(job, ResultStatus::source_unavailable, "document signature read failed");
    }
    // Keep the existing small worker and raster/ICC path for all saved document previews.
    auto extracted = handlers::cdr::CdrPreviewResult{};
    bool archive_fallback{};
    if (job.source_format_hint == SourceFormatHint::idml) {
        extracted = handlers::indesign::extract_idml_embedded_previews(*source_result.source);
    } else if (job.source_format_hint == SourceFormatHint::dxf) {
        auto saved = handlers::dxf::extract_dxf_embedded_preview(*source_result.source);
        if (!saved.ok()) {
            using Status = handlers::dxf::DxfPreviewStatus;
            ResultStatus status = ResultStatus::malformed_source;
            switch (saved.status) {
            case Status::success:
                status = ResultStatus::internal_error;
                break;
            case Status::unsupported_container:
                status = ResultStatus::unsupported;
                break;
            case Status::no_preview:
                status = ResultStatus::embedded_preview_unavailable;
                break;
            case Status::malformed:
                break;
            case Status::resource_limit:
                status = ResultStatus::resource_limit;
                break;
            case Status::io_error:
                status = ResultStatus::source_unavailable;
                break;
            }
            return failure(job, status, std::move(saved.diagnostic));
        }
        extracted.status = handlers::cdr::CdrPreviewStatus::success;
        extracted.candidates.push_back(
            {.format = saved.candidate.format,
             .normalized_name = std::move(saved.candidate.normalized_name),
             .encoded_image = std::move(saved.candidate.encoded_image),
             .page_one = true});
    } else if (job.source_format_hint == SourceFormatHint::layered_archive) {
        auto saved = handlers::archive::extract_archive_preview(*source_result.source);
        if (!saved.ok()) {
            using Status = handlers::archive::ArchivePreviewStatus;
            ResultStatus status = ResultStatus::malformed_source;
            switch (saved.status) {
            case Status::success:
                status = ResultStatus::internal_error;
                break;
            case Status::unsupported_container:
                status = ResultStatus::unsupported;
                break;
            case Status::no_preview:
                status = ResultStatus::embedded_preview_unavailable;
                break;
            case Status::malformed:
                break;
            case Status::resource_limit:
                status = ResultStatus::resource_limit;
                break;
            case Status::io_error:
                status = ResultStatus::source_unavailable;
                break;
            case Status::source_changed:
                status = ResultStatus::source_changed;
                break;
            }
            return failure(job, status, std::move(saved.diagnostic));
        }
        archive_fallback = saved.is_fallback;
        extracted.status = handlers::cdr::CdrPreviewStatus::success;
        extracted.candidates.push_back({handlers::raster::RasterFormat::Png,
                                        std::move(saved.normalized_name),
                                        std::move(saved.encoded_image), true});
    } else {
        extracted = [&] {
            switch (handlers::publishing_container(std::span(signature).first(signature_size))) {
            case handlers::PublishingContainer::indesign:
                return handlers::indesign::extract_indesign_embedded_previews(
                    *source_result.source);
            case handlers::PublishingContainer::affinity:
                return handlers::affinity::extract_affinity_embedded_previews(
                    *source_result.source);
            case handlers::PublishingContainer::unknown:
                return handlers::cdr::extract_cdr_embedded_previews(*source_result.source);
            }
            return handlers::cdr::CdrPreviewResult{};
        }();
    }
    if (!extracted.ok()) {
        return failure(job, cdr_error_status(extracted.status),
                       extracted.diagnostic.empty() ? "embedded document preview extraction failed"
                                                    : extracted.diagnostic);
    }

    std::optional<DecodedCandidate> best;
    ResultStatus last_status = ResultStatus::malformed_source;
    std::string last_diagnostic = "saved document preview could not be decoded";
    for (const auto &candidate : extracted.candidates) {
        auto decoded = decode_candidate(candidate, job, last_status, last_diagnostic);
        if (decoded && (!best || better_candidate(*decoded, *best))) {
            best = std::move(decoded);
        }
    }
    if (!best && job.source_format_hint == SourceFormatHint::layered_archive && !archive_fallback &&
        last_status == ResultStatus::resource_limit) {
        // Retry the small saved thumbnail only after a valid merged PNG exceeds decode limits.
        extracted.candidates.clear();
        auto saved = handlers::archive::extract_archive_preview(
            *source_result.source, {}, handlers::archive::ArchivePreviewSelection::thumbnail_only);
        if (saved.ok()) {
            const handlers::cdr::CdrEmbeddedCandidate thumbnail{
                handlers::raster::RasterFormat::Png, std::move(saved.normalized_name),
                std::move(saved.encoded_image), true};
            best = decode_candidate(thumbnail, job, last_status, last_diagnostic);
        }
    }
    if (!best) {
        return failure(job, last_status, std::move(last_diagnostic));
    }

    const auto validation = source_result.source->validate_unchanged();
    if (!validation.unchanged) {
        return failure(job, source_error_status(validation.error.code),
                       "document changed or became unavailable while extracting its preview");
    }

    const auto encoded =
        cache::encode_qoi_rgba8(best->rgba8, best->width, best->height, best->stride);
    if (!encoded.ok()) {
        return failure(job, ResultStatus::internal_error, encoded.error);
    }
    if (encoded.bytes.empty() || encoded.bytes.size() > job.limits.maximum_output_bytes) {
        return failure(job, ResultStatus::resource_limit,
                       "encoded document thumbnail exceeds the output limit");
    }
    if (!write_all(io.output, encoded.bytes)) {
        return failure(job, ResultStatus::internal_error, "document thumbnail output write failed");
    }

    return {.job_id = job.job_id,
            .status = ResultStatus::success,
            .width = best->width,
            .height = best->height,
            .color_model = best->color_model,
            .provenance = PreviewProvenance::embedded_preview,
            .color_profile_utf8 = std::move(best->profile_name),
            .source_profile_fingerprint = std::move(best->profile_fingerprint),
            .page_index = 0,
            .page_count = 1,
            .bytes_written = encoded.bytes.size(),
            .diagnostic_utf8 = {}};
}

} // namespace vove::worker
