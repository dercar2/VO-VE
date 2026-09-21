#include "xcf_executor.hpp"

#include "vove/cache/qoi_codec.hpp"
#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/xcf/kimageformats_xcf_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
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

[[nodiscard]] ResultStatus
decode_error_status(const handlers::xcf::XcfDecodeErrorCode code) noexcept {
    using Error = handlers::xcf::XcfDecodeErrorCode;
    switch (code) {
    case Error::source_io_error:
        return ResultStatus::source_unavailable;
    case Error::source_disconnected:
        return ResultStatus::disconnected;
    case Error::source_limit_exceeded:
    case Error::dimension_limit_exceeded:
    case Error::output_limit_exceeded:
    case Error::decoder_resource_limit_exceeded:
        return ResultStatus::resource_limit;
    case Error::malformed_input:
    case Error::decode_failed:
        return ResultStatus::malformed_source;
    case Error::compression_not_supported:
    case Error::unsupported_version:
        return ResultStatus::unsupported;
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
            std::min<std::size_t>(bytes.size() - offset, static_cast<std::size_t>(1U << 20U)));
        DWORD written{};
        if (WriteFile(output, bytes.data() + offset, chunk, &written, nullptr) == FALSE ||
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

WorkerResult execute_xcf_job_native(const NativeJobIo io, const WorkerJob &job) {
    if (job.source_format_hint != SourceFormatHint::xcf || job.page_index != 0) {
        return failure(job, ResultStatus::unsupported, "XCF exposes one composite preview");
    }
    const auto source_limit = std::min<std::uint64_t>(
        job.limits.maximum_input_bytes, handlers::xcf::kMaximumXcfSourceBytes);
    auto source = handlers::raster::make_native_source(io.source, source_limit);
    if (!source.ok() || !source.source.has_value()) {
        return failure(job, source_error_status(source.error.code), "XCF source could not be opened");
    }
    const handlers::xcf::XcfDecodeLimits limits{
        .maximum_source_bytes = source_limit,
        .maximum_source_pixels = handlers::xcf::kMaximumXcfSourcePixels,
        .maximum_output_edge =
            std::min(job.limits.canonical_edge, handlers::xcf::kMaximumXcfOutputEdge),
        .maximum_output_bytes = std::min<std::uint64_t>(
            job.limits.maximum_output_bytes, handlers::xcf::kMaximumXcfOutputBytes),
    };
    auto decoded = handlers::xcf::decode_xcf(*source.source, limits);
    if (!decoded.ok()) {
        return failure(job, decode_error_status(decoded.error.code),
                       decoded.error.detail.empty() ? "embedded XCF decode failed"
                                                    : std::move(decoded.error.detail));
    }
    const auto validation = source.source->validate_unchanged();
    if (!validation.unchanged) {
        return failure(job, source_error_status(validation.error.code),
                       "XCF source changed or became unavailable while decoding");
    }
    const auto encoded = cache::encode_qoi_rgba8(
        decoded.image.rgba8, decoded.image.width, decoded.image.height,
        static_cast<std::size_t>(decoded.image.width) * 4U);
    if (!encoded.ok()) {
        return failure(job, ResultStatus::internal_error, encoded.error);
    }
    if (encoded.bytes.empty() || encoded.bytes.size() > job.limits.maximum_output_bytes) {
        return failure(job, ResultStatus::resource_limit,
                       "encoded XCF thumbnail exceeds the output limit");
    }
    if (!write_all(io.output, encoded.bytes)) {
        return failure(job, ResultStatus::internal_error, "XCF thumbnail output write failed");
    }
    auto profile = std::move(decoded.image.source_color_profile);
    if (profile.empty()) {
        profile = "sRGB (assumed)";
    }
    if (profile.size() > kMaximumProfileNameBytes) {
        profile.resize(kMaximumProfileNameBytes);
    }
    return {.job_id = job.job_id,
            .status = ResultStatus::success,
            .width = decoded.image.width,
            .height = decoded.image.height,
            .color_model = ColorModel::rgb,
            .provenance = PreviewProvenance::primary_render,
            .color_profile_utf8 = std::move(profile),
            .source_profile_fingerprint =
                std::move(decoded.image.source_profile_fingerprint),
            .page_index = 0,
            .page_count = 1,
            .bytes_written = encoded.bytes.size(),
            .diagnostic_utf8 = {}};
}

} // namespace vove::worker
