#include "svg_executor.hpp"

#include "vove/cache/qoi_codec.hpp"
#include "vove/handlers/hpgl/preview.hpp"
#include "vove/handlers/raster/native_source.hpp"
#include "vove_resvg_bridge.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
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

inline constexpr std::uint64_t kMaximumSvgSourceBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumHpglSourceBytes = 32ULL * 1024ULL * 1024ULL;

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

[[nodiscard]] std::pair<ResultStatus, std::string> bridge_error(const std::int32_t status) {
    switch (status) {
    case VOVE_RESVG_INVALID_ARGUMENT:
        return {ResultStatus::internal_error, "SVG renderer received invalid bounds"};
    case VOVE_RESVG_SOURCE_LIMIT:
    case VOVE_RESVG_ELEMENT_LIMIT:
    case VOVE_RESVG_OUTPUT_TOO_SMALL:
        return {ResultStatus::resource_limit, "SVG exceeds the bounded preview limits"};
    case VOVE_RESVG_NOT_UTF8:
        return {ResultStatus::malformed_source, "SVG text is not valid UTF-8"};
    case VOVE_RESVG_MALFORMED_GZIP:
        return {ResultStatus::malformed_source, "SVGZ stream is malformed"};
    case VOVE_RESVG_INVALID_SIZE:
        return {ResultStatus::malformed_source, "SVG has no valid canvas size"};
    case VOVE_RESVG_PARSING_FAILED:
        return {ResultStatus::malformed_source, "SVG markup could not be parsed"};
    case VOVE_RESVG_INTERNAL_ERROR:
    default:
        return {ResultStatus::internal_error, "SVG renderer failed"};
    }
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

} // namespace

WorkerResult execute_svg_job_native(const NativeJobIo io, const WorkerJob &job) {
    const bool hpgl = job.source_format_hint == SourceFormatHint::hpgl;
    if (!hpgl && job.source_format_hint != SourceFormatHint::auto_detect) {
        return failure(job, ResultStatus::unsupported, "Unsupported vector source hint");
    }
    if (job.page_index != 0) {
        return failure(job, ResultStatus::unsupported,
                       hpgl ? "HPGL preview supports the first page" : "SVG contains one canvas");
    }
    const auto maximum_source = std::min(job.limits.maximum_input_bytes,
                                         hpgl ? kMaximumHpglSourceBytes : kMaximumSvgSourceBytes);
    auto source_result = handlers::raster::make_native_source(io.source, maximum_source);
    if (!source_result.ok() || !source_result.source.has_value()) {
        return failure(job, source_error_status(source_result.error.code),
                       "Vector source could not be opened");
    }
    auto &source = *source_result.source;
    if (source.size() == 0) {
        return failure(job, ResultStatus::malformed_source, "Vector source is empty");
    }
    std::vector<std::byte> source_bytes(static_cast<std::size_t>(source.size()));
    const auto read = source.read_at(0, source_bytes);
    if (!read.ok() || read.bytes_read != source_bytes.size()) {
        return failure(job, source_error_status(read.error.code), "Vector source could not be read");
    }

    const auto edge = std::min(job.limits.canonical_edge, kMaximumThumbnailEdge);
    if (edge == 0) {
        return failure(job, ResultStatus::resource_limit, "Vector thumbnail edge must be positive");
    }
    std::string converted_svg;
    std::span<const std::byte> render_source = source_bytes;
    if (hpgl) {
        auto converted = handlers::hpgl::render_svg(source_bytes);
        using Status = handlers::hpgl::Status;
        if (converted.status != Status::success) {
            const auto status = converted.status == Status::unsupported ? ResultStatus::unsupported
                                : converted.status == Status::resource_limit ? ResultStatus::resource_limit
                                                                             : ResultStatus::malformed_source;
            return failure(job, status, std::move(converted.diagnostic));
        }
        converted_svg = std::move(converted.svg);
        render_source = std::as_bytes(std::span(converted_svg));
    }
    const auto raw_capacity = static_cast<std::size_t>(edge) * edge * 4U;
    std::vector<std::byte> rgba8(raw_capacity);
    const auto rendered = vove_resvg_render(
        reinterpret_cast<const std::uint8_t *>(render_source.data()), render_source.size(), edge,
        reinterpret_cast<std::uint8_t *>(rgba8.data()), rgba8.size());
    if (rendered.status != VOVE_RESVG_OK) {
        auto [status, diagnostic] = bridge_error(rendered.status);
        return failure(job, status, std::move(diagnostic));
    }
    const auto expected_bytes = static_cast<std::uint64_t>(rendered.width) * rendered.height * 4U;
    if (rendered.width == 0 || rendered.height == 0 || rendered.width > edge ||
        rendered.height > edge || rendered.bytes_written != expected_bytes ||
        rendered.bytes_written > rgba8.size()) {
        return failure(job, ResultStatus::internal_error,
                       "SVG renderer returned invalid output metadata");
    }
    rgba8.resize(rendered.bytes_written);

    const auto validation = source.validate_unchanged();
    if (!validation.unchanged) {
        return failure(job, source_error_status(validation.error.code),
                       "Vector source changed or became unavailable while rendering");
    }
    const auto encoded = cache::encode_qoi_rgba8(rgba8, rendered.width, rendered.height,
                                                 static_cast<std::size_t>(rendered.width) * 4U);
    if (!encoded.ok()) {
        return failure(job, ResultStatus::internal_error, encoded.error);
    }
    if (encoded.bytes.empty() || encoded.bytes.size() > job.limits.maximum_output_bytes) {
        return failure(job, ResultStatus::resource_limit,
                       "encoded SVG thumbnail exceeds the output limit");
    }
    if (!write_all(io.output, encoded.bytes)) {
        return failure(job, ResultStatus::internal_error, "SVG thumbnail output write failed");
    }

    return {.job_id = job.job_id,
            .status = ResultStatus::success,
            .width = rendered.width,
            .height = rendered.height,
            .color_model = ColorModel::rgb,
            .provenance = PreviewProvenance::primary_render,
            .color_profile_utf8 = "sRGB",
            .source_profile_fingerprint = {},
            .page_index = 0,
            .page_count = 1,
            .bytes_written = encoded.bytes.size(),
            .diagnostic_utf8 = {}};
}

} // namespace vove::worker
