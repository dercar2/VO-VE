#include "document_executor.hpp"

#include "mupdf_adapter.h"
#include "vove/cache/qoi_codec.hpp"
#include "vove/preview/rgba_scaler.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vove::worker {
namespace {

inline constexpr std::size_t kMaximumPostScriptPreviewScanBytes = std::size_t{8} * 1024U * 1024U;
inline constexpr std::uint64_t kMaximumPostScriptPreviewPixels = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::array kBinaryEpsSignature{std::byte{0xc5}, std::byte{0xd0}, std::byte{0xd3},
                                                std::byte{0xc6}};

enum class EmbeddedPreviewStatus : std::uint8_t {
    not_postscript,
    success,
    no_preview,
    malformed,
    io_error
};

struct EmbeddedPreview {
    EmbeddedPreviewStatus status{EmbeddedPreviewStatus::not_postscript};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::string diagnostic;
};

[[nodiscard]] EmbeddedPreview embedded_preview_failure(const EmbeddedPreviewStatus status,
                                                       std::string diagnostic) {
    return {.status = status,
            .width = 0,
            .height = 0,
            .rgba8 = {},
            .diagnostic = std::move(diagnostic)};
}

class SourceFile final {
  public:
    explicit SourceFile(const NativeIoHandle source) {
#ifdef _WIN32
        HANDLE duplicate{};
        if (source == nullptr ||
            DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(source), GetCurrentProcess(),
                            &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
            return;
        }
        const auto descriptor =
            _open_osfhandle(reinterpret_cast<std::intptr_t>(duplicate), _O_RDONLY | _O_BINARY);
        if (descriptor == -1) {
            CloseHandle(duplicate);
            return;
        }
        file_ = _fdopen(descriptor, "rb");
        if (file_ == nullptr) {
            _close(descriptor);
        }
#else
        if (source < 0) {
            return;
        }
        const auto descriptor = ::dup(source);
        if (descriptor < 0) {
            return;
        }
        file_ = ::fdopen(descriptor, "rb");
        if (file_ == nullptr) {
            ::close(descriptor);
        }
#endif
    }

    ~SourceFile() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    SourceFile(const SourceFile &) = delete;
    SourceFile &operator=(const SourceFile &) = delete;

    [[nodiscard]] FILE *get() const noexcept {
        return file_;
    }

  private:
    FILE *file_{};
};

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

[[nodiscard]] int hex_digit(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] bool whitespace(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] std::size_t find_dsc_marker(const std::string_view bytes,
                                          const std::string_view marker) noexcept {
    auto position = bytes.find(marker);
    while (position != std::string_view::npos) {
        if (position == 0 || bytes[position - 1U] == '\n' || bytes[position - 1U] == '\r') {
            return position;
        }
        position = bytes.find(marker, position + marker.size());
    }
    return std::string_view::npos;
}

[[nodiscard]] bool parse_numbers(std::string_view text, std::span<std::uint32_t> values) {
    for (auto &value : values) {
        while (!text.empty() && whitespace(text.front())) {
            text.remove_prefix(1);
        }
        if (text.empty()) {
            return false;
        }
        const auto *begin = text.data();
        const auto *end = begin + text.size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == begin) {
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(parsed.ptr - begin));
    }
    return true;
}

[[nodiscard]] std::optional<std::string>
read_postscript_prefix(FILE *source, const std::uint64_t source_bytes, bool &io_error) {
    io_error = false;
    if (std::fseek(source, 0, SEEK_SET) != 0) {
        io_error = true;
        return std::nullopt;
    }
    const auto scan_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
        source_bytes, static_cast<std::uint64_t>(kMaximumPostScriptPreviewScanBytes)));
    std::string bytes(scan_bytes, '\0');
    const auto received = std::fread(bytes.data(), 1, bytes.size(), source);
    if (std::ferror(source) != 0) {
        io_error = true;
        return std::nullopt;
    }
    bytes.resize(received);
    if (std::fseek(source, 0, SEEK_SET) != 0) {
        io_error = true;
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] bool has_postscript_signature(FILE *source, bool &io_error) noexcept {
    io_error = false;
    if (std::fseek(source, 0, SEEK_SET) != 0) {
        io_error = true;
        return false;
    }
    std::array<std::byte, 10> signature{};
    const auto received = std::fread(signature.data(), 1, signature.size(), source);
    if (std::ferror(source) != 0 || std::fseek(source, 0, SEEK_SET) != 0) {
        io_error = true;
        return false;
    }
    constexpr std::array text_signature{std::byte{0x25}, std::byte{0x21}, std::byte{0x50},
                                        std::byte{0x53}};
    return (received >= text_signature.size() &&
            std::equal(text_signature.cbegin(), text_signature.cend(), signature.cbegin())) ||
           (received >= kBinaryEpsSignature.size() &&
            std::equal(kBinaryEpsSignature.cbegin(), kBinaryEpsSignature.cend(),
                       signature.cbegin()));
}

[[nodiscard]] std::optional<std::uint64_t> source_size(FILE *source) noexcept {
#ifdef _WIN32
    if (_fseeki64(source, 0, SEEK_END) != 0) {
        return std::nullopt;
    }
    const auto size = _ftelli64(source);
    if (size < 0 || _fseeki64(source, 0, SEEK_SET) != 0) {
        return std::nullopt;
    }
#else
    if (fseeko(source, 0, SEEK_END) != 0) {
        return std::nullopt;
    }
    const auto size = ftello(source);
    if (size < 0 || fseeko(source, 0, SEEK_SET) != 0) {
        return std::nullopt;
    }
#endif
    return static_cast<std::uint64_t>(size);
}

struct EmbeddedPreviewRequest {
    std::uint64_t source_bytes{};
    std::uint32_t canonical_edge{};
};

[[nodiscard]] EmbeddedPreview extract_postscript_preview(FILE *source,
                                                         const EmbeddedPreviewRequest request) {
    bool io_error{};
    const auto source_prefix = read_postscript_prefix(source, request.source_bytes, io_error);
    if (!source_prefix) {
        return embedded_preview_failure(
            io_error ? EmbeddedPreviewStatus::io_error : EmbeddedPreviewStatus::not_postscript,
            io_error ? "PostScript preview could not be read" : std::string{});
    }
    const std::string_view bytes(*source_prefix);
    const auto binary_eps = bytes.size() >= kBinaryEpsSignature.size() &&
                            std::equal(kBinaryEpsSignature.cbegin(), kBinaryEpsSignature.cend(),
                                       reinterpret_cast<const std::byte *>(bytes.data()));
    if (binary_eps) {
        return embedded_preview_failure(EmbeddedPreviewStatus::no_preview, {});
    }
    if (!bytes.starts_with("%!PS")) {
        return {};
    }

    constexpr std::string_view epsi_marker = "%%BeginPreview:";
    const auto marker_position = find_dsc_marker(bytes, epsi_marker);
    if (marker_position == std::string_view::npos) {
        return embedded_preview_failure(EmbeddedPreviewStatus::no_preview, {});
    }
    const auto header_begin = marker_position + epsi_marker.size();
    const auto header_end = bytes.find_first_of("\r\n", header_begin);
    if (header_end == std::string_view::npos) {
        return embedded_preview_failure(EmbeddedPreviewStatus::malformed,
                                        "embedded PostScript preview header is truncated");
    }
    std::array<std::uint32_t, 4> header{};
    if (!parse_numbers(bytes.substr(header_begin, header_end - header_begin), header)) {
        return embedded_preview_failure(EmbeddedPreviewStatus::malformed,
                                        "embedded PostScript preview header is invalid");
    }
    const auto width = header[0];
    const auto height = header[1];
    const auto depth = header[2];
    if (width == 0 || height == 0 || width > 4'096 || height > 4'096 ||
        (depth != 1 && depth != 2 && depth != 4 && depth != 8) ||
        static_cast<std::uint64_t>(width) * height > kMaximumPostScriptPreviewPixels ||
        header[3] == 0 || header[3] > 1'000'000) {
        return embedded_preview_failure(EmbeddedPreviewStatus::malformed,
                                        "embedded PostScript preview dimensions are invalid");
    }

    const auto row_bytes = (static_cast<std::size_t>(width) * depth + 7U) / 8U;
    const auto expected_bytes = row_bytes * height;
    std::vector<std::uint8_t> packed;
    packed.reserve(expected_bytes);
    auto high_nibble = -1;
    bool payload_overflow{};
    auto cursor = bytes.find_first_not_of("\r\n", header_end);
    bool ended{};
    while (cursor != std::string_view::npos && cursor < bytes.size()) {
        const auto line_end = bytes.find_first_of("\r\n", cursor);
        const auto line = bytes.substr(
            cursor, line_end == std::string_view::npos ? bytes.size() - cursor : line_end - cursor);
        if (line.starts_with("%%EndPreview") || line.starts_with("%%EndData")) {
            ended = true;
            break;
        }
        if (!line.empty() && line.front() == '%' && !line.starts_with("%%")) {
            for (const auto character : line.substr(1)) {
                if (whitespace(character)) {
                    continue;
                }
                const auto digit = hex_digit(character);
                if (digit < 0) {
                    return embedded_preview_failure(
                        EmbeddedPreviewStatus::malformed,
                        "embedded PostScript preview contains invalid hex");
                }
                if (high_nibble < 0) {
                    high_nibble = digit;
                } else {
                    if (packed.size() < expected_bytes) {
                        packed.push_back(static_cast<std::uint8_t>((high_nibble << 4) | digit));
                    } else {
                        payload_overflow = true;
                    }
                    high_nibble = -1;
                }
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor = bytes.find_first_not_of("\r\n", line_end);
    }
    if (!ended || high_nibble >= 0 || payload_overflow || packed.size() != expected_bytes) {
        return embedded_preview_failure(EmbeddedPreviewStatus::malformed,
                                        "embedded PostScript preview payload is truncated");
    }

    std::vector<std::byte> rgba(static_cast<std::size_t>(width) * height * 4U);
    const auto maximum_sample = (1U << depth) - 1U;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto bit_offset = static_cast<std::size_t>(x) * depth;
            const auto packed_byte =
                packed[static_cast<std::size_t>(y) * row_bytes + bit_offset / 8U];
            const auto shift = 8U - depth - static_cast<std::uint32_t>(bit_offset % 8U);
            const auto sample = (packed_byte >> shift) & maximum_sample;
            const auto gray = static_cast<std::uint8_t>(
                255U - (static_cast<std::uint32_t>(sample) * 255U / maximum_sample));
            const auto pixel = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[pixel + 0U] = static_cast<std::byte>(gray);
            rgba[pixel + 1U] = static_cast<std::byte>(gray);
            rgba[pixel + 2U] = static_cast<std::byte>(gray);
            rgba[pixel + 3U] = std::byte{0xff};
        }
    }
    auto scaled = preview::scale_rgba8_to_edge(rgba, width, height, request.canonical_edge);
    if (!scaled.ok()) {
        return embedded_preview_failure(EmbeddedPreviewStatus::malformed,
                                        scaled.error.empty()
                                            ? "embedded PostScript preview is invalid"
                                            : std::move(scaled.error));
    }
    return {.status = EmbeddedPreviewStatus::success,
            .width = scaled.width,
            .height = scaled.height,
            .rgba8 = std::move(scaled.rgba8),
            .diagnostic = {}};
}

[[nodiscard]] ResultStatus map_status(const vove_mupdf_status status) noexcept {
    switch (status) {
    case VOVE_MUPDF_SUCCESS:
        return ResultStatus::success;
    case VOVE_MUPDF_UNSUPPORTED:
        return ResultStatus::unsupported;
    case VOVE_MUPDF_MALFORMED:
        return ResultStatus::malformed_source;
    case VOVE_MUPDF_RESOURCE_LIMIT:
        return ResultStatus::resource_limit;
    case VOVE_MUPDF_MEMORY_LIMIT:
        return ResultStatus::memory_limit;
    case VOVE_MUPDF_PASSWORD_REQUIRED:
        return ResultStatus::password_required;
    case VOVE_MUPDF_AUTHENTICATION_FAILED:
        return ResultStatus::document_password_incorrect;
    case VOVE_MUPDF_INTERNAL_ERROR:
        return ResultStatus::internal_error;
    }
    return ResultStatus::internal_error;
}

[[nodiscard]] ColorModel map_color_model(const vove_mupdf_color_model model) noexcept {
    switch (model) {
    case VOVE_MUPDF_COLOR_UNKNOWN:
        return ColorModel::unknown;
    case VOVE_MUPDF_COLOR_GRAY:
        return ColorModel::grayscale;
    case VOVE_MUPDF_COLOR_RGB:
        return ColorModel::rgb;
    case VOVE_MUPDF_COLOR_CMYK:
        return ColorModel::cmyk;
    case VOVE_MUPDF_COLOR_LAB:
        return ColorModel::lab;
    case VOVE_MUPDF_COLOR_MIXED:
        return ColorModel::mixed;
    }
    return ColorModel::unknown;
}

[[nodiscard]] bool write_all(const NativeIoHandle output,
                             const std::span<const std::byte> bytes) noexcept {
    std::size_t offset{};
    while (offset < bytes.size()) {
#ifdef _WIN32
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
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

WorkerResult execute_document_job_native(const NativeJobIo io, const WorkerJob &job) {
    SourceFile source(io.source);
    if (source.get() == nullptr) {
        return failure(job, ResultStatus::source_unavailable,
                       "read-only document handle is unavailable");
    }

    const auto bytes = source_size(source.get());
    if (!bytes) {
        return failure(job, ResultStatus::source_unavailable, "document size could not be read");
    }
    if (*bytes > job.limits.maximum_input_bytes) {
        return failure(job, ResultStatus::resource_limit, "document exceeds the input limit");
    }

    bool signature_error{};
    const auto postscript_source = has_postscript_signature(source.get(), signature_error);
    if (signature_error) {
        return failure(job, ResultStatus::source_unavailable,
                       "document signature could not be read");
    }
    if (postscript_source) {
        const auto postscript = extract_postscript_preview(
            source.get(), {.source_bytes = *bytes, .canonical_edge = job.limits.canonical_edge});
        if (postscript.status == EmbeddedPreviewStatus::io_error) {
            return failure(job, ResultStatus::source_unavailable, postscript.diagnostic);
        }
        if (postscript.status == EmbeddedPreviewStatus::malformed) {
            return failure(job, ResultStatus::malformed_source, postscript.diagnostic);
        }
        if (postscript.status == EmbeddedPreviewStatus::no_preview) {
            return failure(job, ResultStatus::ghostscript_required, "Ghostscript");
        }
        if (postscript.status != EmbeddedPreviewStatus::success) {
            return failure(job, ResultStatus::internal_error,
                           "embedded preview parser returned an unknown state");
        }
        const auto encoded =
            cache::encode_qoi_rgba8(postscript.rgba8, postscript.width, postscript.height,
                                    static_cast<std::size_t>(postscript.width) * 4U);
        if (!encoded.ok() || encoded.bytes.empty() ||
            encoded.bytes.size() > job.limits.maximum_output_bytes) {
            return failure(job, ResultStatus::resource_limit,
                           encoded.error.empty() ? "embedded preview exceeds the output limit"
                                                 : encoded.error);
        }
        if (!write_all(io.output, encoded.bytes)) {
            return failure(job, ResultStatus::internal_error,
                           "embedded preview output write failed");
        }
        return {.job_id = job.job_id,
                .status = ResultStatus::success,
                .width = postscript.width,
                .height = postscript.height,
                .color_model = ColorModel::grayscale,
                .provenance = PreviewProvenance::embedded_preview,
                .color_profile_utf8 = {},
                .source_profile_fingerprint = {},
                .page_index = 0,
                .page_count = 1,
                .bytes_written = encoded.bytes.size(),
                .diagnostic_utf8 = {}};
    }

    vove_mupdf_result rendered;
    vove_mupdf_result_init(&rendered);
    const vove_mupdf_request request{.maximum_input_bytes = job.limits.maximum_input_bytes,
                                     .memory_limit_bytes = job.limits.memory_limit_bytes,
                                     .page_index = job.page_index,
                                     .canonical_edge = job.limits.canonical_edge};
    vove_mupdf_render_pdf(source.get(), &request, job.password_utf8.c_str(), &rendered);
    const auto rendered_status = map_status(rendered.status);
    if (rendered_status != ResultStatus::success) {
        const auto diagnostic = std::string(rendered.diagnostic);
        vove_mupdf_result_drop(&rendered);
        return failure(job, rendered_status,
                       diagnostic.empty() ? "document render failed" : diagnostic);
    }

    const auto rgba = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(rendered.rgba8), rendered.rgba8_bytes);
    const auto encoded = cache::encode_qoi_rgba8(rgba, rendered.width, rendered.height,
                                                 static_cast<std::size_t>(rendered.width) * 4U);
    if (!encoded.ok()) {
        const auto diagnostic = encoded.error;
        vove_mupdf_result_drop(&rendered);
        return failure(job, ResultStatus::internal_error, diagnostic);
    }
    if (encoded.bytes.empty() || encoded.bytes.size() > job.limits.maximum_output_bytes) {
        vove_mupdf_result_drop(&rendered);
        return failure(job, ResultStatus::resource_limit,
                       "encoded document thumbnail exceeds the output limit");
    }
    if (!write_all(io.output, encoded.bytes)) {
        vove_mupdf_result_drop(&rendered);
        return failure(job, ResultStatus::internal_error, "document thumbnail output write failed");
    }

    auto profile = std::string(rendered.color_profile);
    if (profile.size() > kMaximumProfileNameBytes) {
        profile.resize(kMaximumProfileNameBytes);
    }
    const auto result = WorkerResult{.job_id = job.job_id,
                                     .status = ResultStatus::success,
                                     .width = rendered.width,
                                     .height = rendered.height,
                                     .color_model = map_color_model(rendered.color_model),
                                     .color_profile_utf8 = std::move(profile),
                                     .source_profile_fingerprint = {},
                                     .page_index = rendered.page_index,
                                     .page_count = rendered.page_count,
                                     .bytes_written = encoded.bytes.size(),
                                     .diagnostic_utf8 = {}};
    vove_mupdf_result_drop(&rendered);
    return result;
}

} // namespace vove::worker
