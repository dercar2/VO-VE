#include "vove/preview/worker_thumbnail_renderer.hpp"
#include "vove/handlers/publishing_signature.hpp"

#include "vove/cache/cache_key.hpp"
#include "vove/cache/qoi_codec.hpp"
#include "vove/color/color_transform.h"
#include "vove/handlers/raster/native_source.hpp"
#include "vove/handlers/raster/signature_probe.hpp"
#include "vove/handlers/raster/psd_decoder.hpp"
#include "vove/handlers/xcf/kimageformats_xcf_decoder.hpp"
#include "vove/platform/read_only_source.hpp"
#include "vove/worker/supervisor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vove::preview {
namespace {

void record_test_audit_event(const std::string_view event) noexcept {
    const auto *path = std::getenv("VOVE_TEST_PREVIEW_AUDIT_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    if (auto *output = std::fopen(path, "ab")) {
        static_cast<void>(std::fwrite(event.data(), 1, event.size(), output));
        static_cast<void>(std::fwrite("\n", 1, 1, output));
        static_cast<void>(std::fclose(output));
    }
}

inline constexpr std::uint64_t kMaximumDecodedSourceBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumPdfSourceBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumContainerSourceBytes = worker::kMaximumInputBytes;
inline constexpr std::uint64_t kMaximumSvgSourceBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumHpglSourceBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumWorkerOutputBytes = cache::kMaxArtifactBytes;
inline constexpr std::uint64_t kMaximumGhostscriptOutputBytes =
    static_cast<std::uint64_t>(cache::kMaxArtifactDimension) * cache::kMaxArtifactDimension * 3U +
    4096U;
inline constexpr std::uint64_t kWorkerMemoryBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kSelectedPdfMemoryBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kGhostscriptMemoryBytes = 768ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kExtendedMemoryBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kExtendedTimeoutMs = 120'000;
static_assert(kExtendedMemoryBytes <= worker::kMaximumWorkerMemoryBytes);
static_assert(kExtendedTimeoutMs <= worker::kMaximumWallTimeoutMs);
inline constexpr std::size_t kFingerprintChunkBytes = std::size_t{4} * 1024U;
inline constexpr std::size_t kMaximumFontInventoryEntries = 100'000;
inline constexpr std::string_view kFingerprintMarker = "|sample:";
inline constexpr std::array kPdfSignature{std::byte{0x25}, std::byte{0x50}, std::byte{0x44},
                                          std::byte{0x46}, std::byte{0x2d}};
inline constexpr std::array kPostScriptSignature{std::byte{0x25}, std::byte{0x21}, std::byte{0x50},
                                                 std::byte{0x53}};
inline constexpr std::array kBinaryEpsSignature{std::byte{0xc5}, std::byte{0xd0}, std::byte{0xd3},
                                                std::byte{0xc6}};
inline constexpr std::array kZipSignature{std::byte{0x50}, std::byte{0x4b}, std::byte{0x03},
                                          std::byte{0x04}};
inline constexpr std::array kRiffSignature{std::byte{0x52}, std::byte{0x49}, std::byte{0x46},
                                           std::byte{0x46}};
inline constexpr std::array kCdrFormSignature{std::byte{0x43}, std::byte{0x44}, std::byte{0x52}};
inline constexpr std::array kPsdSignature{std::byte{0x38}, std::byte{0x42}, std::byte{0x50},
                                          std::byte{0x53}};
inline constexpr std::array kXcfSignature{std::byte{'g'}, std::byte{'i'}, std::byte{'m'},
                                          std::byte{'p'}, std::byte{' '}, std::byte{'x'},
                                          std::byte{'c'}, std::byte{'f'}, std::byte{' '}};

enum class HandlerKind : std::uint8_t {
    raster,
    psd,
    raw,
    pdf,
    postscript,
    cdr,
    indesign,
    idml,
    affinity,
    archive,
    svg,
    hpgl,
    xcf,
    dxf
};

struct RendererState {
    WorkerThumbnailRendererOptions options;
    std::shared_ptr<std::FILE> fallback_cmyk_profile;
    std::string fallback_cmyk_fingerprint;
    std::string fallback_cmyk_diagnostic;
};

struct RevisionParts {
    std::string base;
    std::optional<std::string> fingerprint;
};

struct PpmImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && width != 0 && height != 0 && !rgba8.empty();
    }
};

[[nodiscard]] bool ppm_space(const std::byte value) noexcept {
    switch (std::to_integer<unsigned char>(value)) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::optional<std::string_view> ppm_token(const std::span<const std::byte> bytes,
                                                        std::size_t &position) noexcept {
    for (;;) {
        while (position < bytes.size() && ppm_space(bytes[position])) {
            ++position;
        }
        if (position >= bytes.size() || bytes[position] != std::byte{'#'}) {
            break;
        }
        while (position < bytes.size() && bytes[position] != std::byte{'\n'} &&
               bytes[position] != std::byte{'\r'}) {
            ++position;
        }
    }
    const auto beginning = position;
    while (position < bytes.size() && !ppm_space(bytes[position]) &&
           bytes[position] != std::byte{'#'}) {
        ++position;
    }
    if (position == beginning || position - beginning > 16U) {
        return std::nullopt;
    }
    return std::string_view{reinterpret_cast<const char *>(bytes.data() + beginning),
                            position - beginning};
}

[[nodiscard]] std::optional<std::uint32_t>
ppm_number(const std::optional<std::string_view> token) noexcept {
    if (!token || token->empty()) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto parsed = std::from_chars(token->data(), token->data() + token->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token->data() + token->size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] PpmImage parse_ghostscript_ppm(const std::span<const std::byte> bytes,
                                             const std::uint32_t maximum_edge) {
    std::size_t position{};
    const auto magic = ppm_token(bytes, position);
    const auto width = ppm_number(ppm_token(bytes, position));
    const auto height = ppm_number(ppm_token(bytes, position));
    const auto maximum = ppm_number(ppm_token(bytes, position));
    if (!magic || *magic != "P6" || !width || !height || !maximum || *width == 0 || *height == 0 ||
        *width > maximum_edge || *height > maximum_edge || *maximum != 255) {
        return {.width = 0, .height = 0, .rgba8 = {}, .error = "Ghostscript PPM header is invalid"};
    }
    if (position >= bytes.size() || !ppm_space(bytes[position])) {
        return {.width = 0,
                .height = 0,
                .rgba8 = {},
                .error = "Ghostscript PPM raster delimiter is missing"};
    }
    if (bytes[position] == std::byte{'\r'} && position + 1U < bytes.size() &&
        bytes[position + 1U] == std::byte{'\n'}) {
        position += 2U;
    } else {
        ++position;
    }
    const auto pixels = static_cast<std::uint64_t>(*width) * static_cast<std::uint64_t>(*height);
    const auto rgb_bytes = pixels * 3U;
    if (rgb_bytes > bytes.size() - position ||
        pixels > std::numeric_limits<std::size_t>::max() / 4U) {
        return {
            .width = 0, .height = 0, .rgba8 = {}, .error = "Ghostscript PPM raster is truncated"};
    }
    const auto raster_end = position + static_cast<std::size_t>(rgb_bytes);
    if (std::any_of(bytes.begin() + static_cast<std::ptrdiff_t>(raster_end), bytes.end(),
                    [](const std::byte value) { return !ppm_space(value); })) {
        return {.width = 0,
                .height = 0,
                .rgba8 = {},
                .error = "Ghostscript PPM output contains trailing data"};
    }
    PpmImage image{.width = *width, .height = *height, .rgba8 = {}, .error = {}};
    image.rgba8.resize(static_cast<std::size_t>(pixels) * 4U);
    for (std::size_t source = position, destination = 0; source < raster_end;
         source += 3U, destination += 4U) {
        image.rgba8[destination] = bytes[source];
        image.rgba8[destination + 1U] = bytes[source + 1U];
        image.rgba8[destination + 2U] = bytes[source + 2U];
        image.rgba8[destination + 3U] = std::byte{0xff};
    }
    return image;
}

[[nodiscard]] RevisionParts split_revision(const std::optional<std::string> &revision) {
    if (!revision) {
        return {};
    }
    const auto marker = revision->find(kFingerprintMarker);
    if (marker == std::string::npos) {
        return {.base = *revision, .fingerprint = std::nullopt};
    }
    return {.base = revision->substr(0, marker),
            .fingerprint = revision->substr(marker + kFingerprintMarker.size())};
}

[[nodiscard]] bool needs_bounded_fingerprint(const std::filesystem::path &path,
                                             const platform::ReadOnlySource &source,
                                             const std::string_view revision) noexcept {
#ifdef _WIN32
    static_cast<void>(source);
    const auto &native = path.native();
    if (native.starts_with(L"\\\\") || !revision.starts_with("win-file:")) {
        return true;
    }
    std::array<wchar_t, MAX_PATH> volume_root{};
    if (GetVolumePathNameW(path.c_str(), volume_root.data(),
                           static_cast<DWORD>(volume_root.size())) == FALSE) {
        return true;
    }
    std::array<wchar_t, MAX_PATH> filesystem{};
    if (GetVolumeInformationW(volume_root.data(), nullptr, 0, nullptr, nullptr, nullptr,
                              filesystem.data(), static_cast<DWORD>(filesystem.size())) == FALSE) {
        return true;
    }
    const std::wstring_view name(filesystem.data());
    return name != L"NTFS" && name != L"ReFS";
#else
    static_cast<void>(path);
    static_cast<void>(revision);
#ifdef __linux__
    struct statfs filesystem{};
    if (::fstatfs(static_cast<int>(source.native_object()), &filesystem) != 0) {
        return true;
    }
    constexpr std::array<long, 5> strong_filesystems{
        0xEF53L,     // ext2/3/4
        0x58465342L, // XFS
        0x9123683EL, // Btrfs
        0x01021994L, // tmpfs
        0x794c7630L, // overlayfs
    };
    return std::find(strong_filesystems.cbegin(), strong_filesystems.cend(), filesystem.f_type) ==
           strong_filesystems.cend();
#else
    static_cast<void>(source);
    return true;
#endif
#endif
}

[[nodiscard]] bool read_at(const platform::ReadOnlySource &source, const std::uint64_t offset,
                           const std::span<std::byte> output) noexcept {
#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(source.native_object());
    LARGE_INTEGER original{};
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(handle, zero, &original, FILE_CURRENT) == FALSE) {
        return false;
    }
    LARGE_INTEGER requested{};
    requested.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(handle, requested, nullptr, FILE_BEGIN) == FALSE) {
        return false;
    }
    std::size_t completed{};
    while (completed < output.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(output.size() - completed, static_cast<std::size_t>(MAXDWORD)));
        DWORD received{};
        if (ReadFile(handle, output.data() + completed, chunk, &received, nullptr) == FALSE ||
            received == 0) {
            static_cast<void>(SetFilePointerEx(handle, original, nullptr, FILE_BEGIN));
            return false;
        }
        completed += received;
    }
    return SetFilePointerEx(handle, original, nullptr, FILE_BEGIN) != FALSE;
#else
    const auto descriptor = static_cast<int>(source.native_object());
    std::size_t completed{};
    while (completed < output.size()) {
        const auto received =
            ::pread(descriptor, output.data() + completed, output.size() - completed,
                    static_cast<off_t>(offset + completed));
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (received == 0) {
            return false;
        }
        completed += static_cast<std::size_t>(received);
    }
    return true;
#endif
}

[[nodiscard]] std::optional<std::string>
bounded_fingerprint(const platform::ReadOnlySource &source) {
    std::vector<std::byte> sampled;
    sampled.reserve(sizeof(std::uint64_t) + 3U * kFingerprintChunkBytes);
    const auto size = source.size_bytes();
    for (std::size_t shift = 0; shift < sizeof(size); ++shift) {
        sampled.push_back(static_cast<std::byte>((size >> (shift * 8U)) & 0xffU));
    }
    const std::array offsets{std::uint64_t{0}, size / 2U,
                             size > kFingerprintChunkBytes ? size - kFingerprintChunkBytes : 0U};
    std::uint64_t previous = std::numeric_limits<std::uint64_t>::max();
    for (const auto offset : offsets) {
        if (offset == previous || offset >= size) {
            continue;
        }
        previous = offset;
        const auto bytes = static_cast<std::size_t>(
            std::min<std::uint64_t>(kFingerprintChunkBytes, size - offset));
        const auto begin = sampled.size();
        sampled.resize(begin + bytes);
        if (!read_at(source, offset, std::span(sampled).subspan(begin, bytes))) {
            return std::nullopt;
        }
    }
    return cache::hash_bytes(sampled).hex();
}

[[nodiscard]] std::u8string lower_extension(const std::filesystem::path &path) {
    auto extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const char8_t value) {
        return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value + (u8'a' - u8'A'))
                                                : value;
    });
    return extension;
}

[[nodiscard]] bool raw_extension(const std::filesystem::path &path) {
    const auto extension = lower_extension(path);
    constexpr std::array<std::u8string_view, 11> extensions{u8".arw", u8".cr2", u8".cr3", u8".dng",
                                                            u8".nef", u8".nrw", u8".orf", u8".pef",
                                                            u8".raf", u8".rw2", u8".srw"};
    return std::ranges::find(extensions, std::u8string_view{extension}) != extensions.end();
}

[[nodiscard]] bool svg_extension(const std::filesystem::path &path) {
    const auto extension = lower_extension(path);
    return extension == u8".svg" || extension == u8".svgz";
}

[[nodiscard]] bool font_extension(const std::filesystem::path &path) {
    const auto extension = lower_extension(path);
    constexpr std::array<std::u8string_view, 5> extensions{u8".otf", u8".ttc", u8".ttf", u8".woff",
                                                           u8".woff2"};
    return std::ranges::find(extensions, std::u8string_view{extension}) != extensions.end();
}

void append_environment_path(std::vector<std::filesystem::path> &directories, const char *variable,
                             const std::filesystem::path &suffix = {}) {
    const auto *value = std::getenv(variable);
    if (value != nullptr && *value != '\0') {
        directories.emplace_back(std::filesystem::path{value} / suffix);
    }
}

[[nodiscard]] std::string system_font_environment_fingerprint() {
    std::vector<std::filesystem::path> directories;
#ifdef _WIN32
    append_environment_path(directories, "WINDIR", "Fonts");
    append_environment_path(directories, "LOCALAPPDATA", "Microsoft/Windows/Fonts");
    append_environment_path(directories, "APPDATA", "Microsoft/Windows/Fonts");
#else
    directories.emplace_back("/usr/share/fonts");
    directories.emplace_back("/usr/local/share/fonts");
    append_environment_path(directories, "HOME", ".local/share/fonts");
    append_environment_path(directories, "HOME", ".fonts");
#endif

    std::vector<std::string> records;
    records.reserve(1024);
    for (const auto &directory : directories) {
        std::error_code error;
        std::filesystem::recursive_directory_iterator current(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && current != end && records.size() < kMaximumFontInventoryEntries) {
            const auto &entry = *current;
            if (entry.is_regular_file(error) && !error && font_extension(entry.path())) {
                const auto bytes = entry.file_size(error);
                if (!error) {
                    const auto modified = entry.last_write_time(error);
                    if (!error) {
                        const auto utf8 = entry.path().lexically_normal().generic_u8string();
                        std::string record(reinterpret_cast<const char *>(utf8.data()),
                                           utf8.size());
                        record.push_back('|');
                        record += std::to_string(bytes);
                        record.push_back('|');
                        record += std::to_string(
                            static_cast<long long>(modified.time_since_epoch().count()));
                        records.push_back(std::move(record));
                    }
                }
            }
            error.clear();
            current.increment(error);
        }
    }
    std::sort(records.begin(), records.end());
    std::string inventory;
    for (const auto &record : records) {
        inventory.append(record);
        inventory.push_back('\0');
    }
    for (const auto *variable : {"FONTCONFIG_FILE", "FONTCONFIG_PATH"}) {
        if (const auto *value = std::getenv(variable); value != nullptr) {
            inventory.append(variable);
            inventory.push_back('=');
            inventory.append(value);
            inventory.push_back('\0');
        }
    }
    return cache::hash_bytes(std::as_bytes(std::span(inventory))).hex();
}

[[nodiscard]] HandlerKind detect_handler(const platform::ReadOnlySource &source,
                                         const std::filesystem::path &path,
                                         handlers::raster::RasterFormat *raster_format = nullptr) {
    if (raster_format != nullptr) {
        *raster_format = handlers::raster::RasterFormat::Unknown;
    }
    std::array<std::byte, 16> signature{};
    const auto signature_bytes =
        static_cast<std::size_t>(std::min<std::uint64_t>(source.size_bytes(), signature.size()));
    if (signature_bytes == 0 || !read_at(source, 0, std::span(signature).first(signature_bytes))) {
        return HandlerKind::raster;
    }
    switch (handlers::publishing_container(std::span(signature).first(signature_bytes))) {
    case handlers::PublishingContainer::indesign:
        return HandlerKind::indesign;
    case handlers::PublishingContainer::affinity:
        return HandlerKind::affinity;
    case handlers::PublishingContainer::unknown:
        break;
    }
    if (signature_bytes >= kPdfSignature.size() &&
        std::equal(kPdfSignature.cbegin(), kPdfSignature.cend(), signature.cbegin())) {
        return HandlerKind::pdf;
    }
    if (signature_bytes >= kPsdSignature.size() &&
        std::equal(kPsdSignature.cbegin(), kPsdSignature.cend(), signature.cbegin())) {
        if (raster_format != nullptr) {
            *raster_format = handlers::raster::RasterFormat::Psd;
        }
        return HandlerKind::psd;
    }
    if (signature_bytes >= kXcfSignature.size() &&
        std::equal(kXcfSignature.cbegin(), kXcfSignature.cend(), signature.cbegin())) {
        return HandlerKind::xcf;
    }
    if (signature_bytes >= kPostScriptSignature.size() &&
        std::equal(kPostScriptSignature.cbegin(), kPostScriptSignature.cend(),
                   signature.cbegin())) {
        return HandlerKind::postscript;
    }
    if (signature_bytes >= kBinaryEpsSignature.size() &&
        std::equal(kBinaryEpsSignature.cbegin(), kBinaryEpsSignature.cend(), signature.cbegin())) {
        return HandlerKind::postscript;
    }
    if (signature_bytes >= kZipSignature.size() &&
        std::equal(kZipSignature.cbegin(), kZipSignature.cend(), signature.cbegin())) {
        const auto extension = lower_extension(path);
        if (extension == u8".idml") {
            return HandlerKind::idml;
        }
        return extension == u8".kra" || extension == u8".ora" ? HandlerKind::archive
                                                              : HandlerKind::cdr;
    }
    if (signature_bytes >= 12 &&
        std::equal(kRiffSignature.cbegin(), kRiffSignature.cend(), signature.cbegin()) &&
        std::equal(kCdrFormSignature.cbegin(), kCdrFormSignature.cend(), signature.cbegin() + 8)) {
        return HandlerKind::cdr;
    }
    if (svg_extension(path)) {
        return HandlerKind::svg;
    }
    const auto extension = lower_extension(path);
    if (extension == u8".dxf") {
        return HandlerKind::dxf;
    }
    if (extension == u8".plt" || extension == u8".hpgl") {
        return HandlerKind::hpgl;
    }
    if (raw_extension(path)) {
        return HandlerKind::raw;
    }
    if (raster_format != nullptr) {
        *raster_format = handlers::raster::probe_raster_signature(
                             std::span<const std::byte>(signature).first(signature_bytes))
                             .format;
    }
    return HandlerKind::raster;
}

[[nodiscard]] bool
uses_fallback_profile(const HandlerKind handler,
                      const handlers::raster::RasterFormat raster_format) noexcept {
    return handler == HandlerKind::psd || (handler == HandlerKind::raster &&
                                           (raster_format == handlers::raster::RasterFormat::Jpeg ||
                                            raster_format == handlers::raster::RasterFormat::Tiff));
}

[[nodiscard]] std::string_view handler_id(const HandlerKind handler) noexcept {
    switch (handler) {
    case HandlerKind::raster:
        return kRasterThumbnailHandlerId;
    case HandlerKind::psd:
        return kPsdCompositeHandlerId;
    case HandlerKind::raw:
        return kRawEmbeddedPreviewHandlerId;
    case HandlerKind::pdf:
        return kMuPdfThumbnailHandlerId;
    case HandlerKind::postscript:
        return kPostScriptPreviewHandlerId;
    case HandlerKind::cdr:
        return kCdrEmbeddedPreviewHandlerId;
    case HandlerKind::indesign:
        return kInDesignPreviewHandlerId;
    case HandlerKind::idml:
        return kIdmlPreviewHandlerId;
    case HandlerKind::affinity:
        return kAffinityPreviewHandlerId;
    case HandlerKind::archive:
        return kArchivePreviewHandlerId;
    case HandlerKind::svg:
        return kSvgThumbnailHandlerId;
    case HandlerKind::hpgl:
        return kHpglThumbnailHandlerId;
    case HandlerKind::xcf:
        return kXcfThumbnailHandlerId;
    case HandlerKind::dxf:
        return kDxfEmbeddedPreviewHandlerId;
    }
    return kRasterThumbnailHandlerId;
}

[[nodiscard]] std::uint32_t handler_version(const HandlerKind handler) noexcept {
    switch (handler) {
    case HandlerKind::raster:
        return kRasterThumbnailHandlerVersion;
    case HandlerKind::psd:
        return kPsdCompositeHandlerVersion;
    case HandlerKind::raw:
        return kRawEmbeddedPreviewHandlerVersion;
    case HandlerKind::pdf:
        return kMuPdfThumbnailHandlerVersion;
    case HandlerKind::postscript:
        return kPostScriptPreviewHandlerVersion;
    case HandlerKind::cdr:
        return kCdrEmbeddedPreviewHandlerVersion;
    case HandlerKind::indesign:
        return kInDesignPreviewHandlerVersion;
    case HandlerKind::idml:
        return kIdmlPreviewHandlerVersion;
    case HandlerKind::affinity:
        return kAffinityPreviewHandlerVersion;
    case HandlerKind::archive:
        return kArchivePreviewHandlerVersion;
    case HandlerKind::svg:
        return kSvgThumbnailHandlerVersion;
    case HandlerKind::hpgl:
        return kHpglThumbnailHandlerVersion;
    case HandlerKind::xcf:
        return kXcfThumbnailHandlerVersion;
    case HandlerKind::dxf:
        return kDxfEmbeddedPreviewHandlerVersion;
    }
    return kRasterThumbnailHandlerVersion;
}

struct FileCloser {
    void operator()(std::FILE *file) const noexcept {
        if (file != nullptr) {
            static_cast<void>(std::fclose(file));
        }
    }
};

using UniqueFile = std::unique_ptr<std::FILE, FileCloser>;

#ifndef _WIN32
struct FileDescriptorCloser {
    void operator()(const int *descriptor) const noexcept {
        if (descriptor != nullptr && *descriptor >= 0) {
            static_cast<void>(::close(*descriptor));
        }
        delete descriptor;
    }
};

using UniqueFileDescriptor = std::unique_ptr<int, FileDescriptorCloser>;

struct PosixTemporaryOutput {
    UniqueFile reader;
    UniqueFileDescriptor writer;
};

[[nodiscard]] PosixTemporaryOutput make_posix_temporary_output() noexcept {
    char path[] = "/tmp/vove-thumbnail-XXXXXX";
    const auto reader_descriptor = ::mkstemp(path);
    if (reader_descriptor < 0) {
        return {};
    }
    static_cast<void>(::fcntl(reader_descriptor, F_SETFD, FD_CLOEXEC));

    const auto writer_descriptor = ::open(path, O_WRONLY | O_CLOEXEC);
    static_cast<void>(::unlink(path));
    if (writer_descriptor < 0) {
        static_cast<void>(::close(reader_descriptor));
        return {};
    }

    UniqueFile reader{::fdopen(reader_descriptor, "rb")};
    if (!reader) {
        static_cast<void>(::close(reader_descriptor));
        static_cast<void>(::close(writer_descriptor));
        return {};
    }
    return {.reader = std::move(reader),
            .writer = UniqueFileDescriptor{new int{writer_descriptor}}};
}
#endif

[[nodiscard]] RenderedThumbnail failure(const ThumbnailPipelineStatus status,
                                        std::string diagnostic) {
    return {.status = status,
            .metadata = {},
            .qoi_payload = {},
            .source_profile_name = {},
            .source_profile_fingerprint = {},
            .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string &utf8) {
#ifdef _WIN32
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const auto bytes = static_cast<int>(utf8.size());
    const auto characters =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, nullptr, 0);
    if (characters <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(characters), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, wide.data(),
                            characters) != characters) {
        return {};
    }
    return std::filesystem::path{wide};
#else
    return std::filesystem::path{utf8};
#endif
}

[[nodiscard]] bool path_may_use_fallback_profile(const std::filesystem::path &path) {
    auto extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const char8_t value) {
        return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value + (u8'a' - u8'A'))
                                                : value;
    });
    return extension == u8".psd" || extension == u8".psb" || extension == u8".jpg" ||
           extension == u8".jpeg" || extension == u8".tif" || extension == u8".tiff";
}

[[nodiscard]] std::string color_policy_fingerprint(const bool uses_fallback,
                                                   const std::string_view fingerprint,
                                                   const std::string_view svg_fonts = {}) {
    auto result = uses_fallback && !fingerprint.empty() ? "sRGB-v1|cmyk:" + std::string(fingerprint)
                                                        : std::string{"sRGB-v1|cmyk:none"};
    if (!svg_fonts.empty()) {
        result += "|svg-fonts:";
        result += svg_fonts;
    }
    return result;
}

[[nodiscard]] bool plausible_cmyk_icc(const std::span<const std::byte> bytes) noexcept {
    constexpr std::size_t header_bytes = 128;
    if (bytes.size() < header_bytes) {
        return false;
    }
    const auto declared_size = (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
                               (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
                               (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
                               std::to_integer<std::uint32_t>(bytes[3]);
    constexpr std::array cmyk{std::byte{'C'}, std::byte{'M'}, std::byte{'Y'}, std::byte{'K'}};
    constexpr std::array signature{std::byte{'a'}, std::byte{'c'}, std::byte{'s'}, std::byte{'p'}};
    return declared_size >= header_bytes && declared_size <= bytes.size() &&
           std::equal(cmyk.cbegin(), cmyk.cend(), bytes.begin() + 16) &&
           std::equal(signature.cbegin(), signature.cend(), bytes.begin() + 36);
}

#ifdef _WIN32
[[nodiscard]] worker::NativeObject output_object(std::FILE *file) noexcept {
    const auto descriptor = _fileno(file);
    if (descriptor < 0) {
        return worker::kInvalidNativeObject;
    }
    const auto handle = _get_osfhandle(descriptor);
    return handle == -1 ? worker::kInvalidNativeObject : static_cast<worker::NativeObject>(handle);
}
#endif

[[nodiscard]] worker::NativeObject file_object(std::FILE *file) noexcept {
#ifdef _WIN32
    return output_object(file);
#else
    return file == nullptr ? worker::kInvalidNativeObject
                           : static_cast<worker::NativeObject>(::fileno(file));
#endif
}

[[nodiscard]] std::shared_ptr<std::FILE>
make_profile_snapshot(const std::span<const std::byte> bytes) noexcept {
#ifdef _WIN32
    auto *file = std::tmpfile();
    if (file == nullptr) {
        return {};
    }
    std::shared_ptr<std::FILE> snapshot(file, [](std::FILE *value) {
        if (value != nullptr) {
            static_cast<void>(std::fclose(value));
        }
    });
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() ||
        std::fflush(file) != 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        return {};
    }
    return snapshot;
#else
    char path[] = "/tmp/vove-cmyk-profile-XXXXXX";
    const auto writer = ::mkstemp(path);
    if (writer < 0) {
        return {};
    }
    auto reader = -1;
    const auto cleanup = [&]() noexcept {
        static_cast<void>(::unlink(path));
        static_cast<void>(::close(writer));
        if (reader >= 0) {
            static_cast<void>(::close(reader));
        }
    };
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto written = ::write(writer, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            cleanup();
            return {};
        }
        offset += static_cast<std::size_t>(written);
    }
    reader = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat written_status{};
    struct stat read_status{};
    const auto same_file = reader >= 0 && ::fstat(writer, &written_status) == 0 &&
                           ::fstat(reader, &read_status) == 0 &&
                           written_status.st_dev == read_status.st_dev &&
                           written_status.st_ino == read_status.st_ino &&
                           written_status.st_size == read_status.st_size;
    const auto unlinked = ::unlink(path) == 0;
    static_cast<void>(::close(writer));
    if (!same_file || !unlinked) {
        if (reader >= 0) {
            static_cast<void>(::close(reader));
        }
        return {};
    }
    auto *file = ::fdopen(reader, "rb");
    if (file == nullptr) {
        static_cast<void>(::close(reader));
        return {};
    }
    return std::shared_ptr<std::FILE>(file, [](std::FILE *value) {
        if (value != nullptr) {
            static_cast<void>(std::fclose(value));
        }
    });
#endif
}

[[nodiscard]] ThumbnailPipelineStatus
map_source_error(const platform::SourceOpenErrorKind error) noexcept {
    switch (error) {
    case platform::SourceOpenErrorKind::too_large:
        return ThumbnailPipelineStatus::resource_limit;
    case platform::SourceOpenErrorKind::none:
        return ThumbnailPipelineStatus::internal_error;
    case platform::SourceOpenErrorKind::invalid_path:
    case platform::SourceOpenErrorKind::not_regular_file:
        return ThumbnailPipelineStatus::malformed_source;
    case platform::SourceOpenErrorKind::not_found:
        return ThumbnailPipelineStatus::source_not_found;
    case platform::SourceOpenErrorKind::access_denied:
        return ThumbnailPipelineStatus::permission_denied;
    case platform::SourceOpenErrorKind::authentication_failed:
        return ThumbnailPipelineStatus::authentication_failed;
    case platform::SourceOpenErrorKind::timed_out:
        return ThumbnailPipelineStatus::timed_out;
    case platform::SourceOpenErrorKind::disconnected:
        return ThumbnailPipelineStatus::disconnected;
    case platform::SourceOpenErrorKind::io_error:
        return ThumbnailPipelineStatus::source_unavailable;
    }
    return ThumbnailPipelineStatus::internal_error;
}

[[nodiscard]] ThumbnailPipelineStatus
map_worker_status(const worker::ResultStatus status) noexcept {
    switch (status) {
    case worker::ResultStatus::success:
        return ThumbnailPipelineStatus::success;
    case worker::ResultStatus::unsupported:
        return ThumbnailPipelineStatus::unsupported;
    case worker::ResultStatus::malformed_source:
        return ThumbnailPipelineStatus::malformed_source;
    case worker::ResultStatus::timed_out:
        return ThumbnailPipelineStatus::processing_timed_out;
    case worker::ResultStatus::cancelled:
        return ThumbnailPipelineStatus::cancelled;
    case worker::ResultStatus::resource_limit:
        return ThumbnailPipelineStatus::resource_limit;
    case worker::ResultStatus::memory_limit:
        return ThumbnailPipelineStatus::memory_limit;
    case worker::ResultStatus::source_unavailable:
        return ThumbnailPipelineStatus::source_unavailable;
    case worker::ResultStatus::source_changed:
        return ThumbnailPipelineStatus::source_changed;
    case worker::ResultStatus::disconnected:
        return ThumbnailPipelineStatus::disconnected;
    case worker::ResultStatus::color_profile_required:
        return ThumbnailPipelineStatus::color_profile_required;
    case worker::ResultStatus::password_required:
        return ThumbnailPipelineStatus::password_required;
    case worker::ResultStatus::reserved_source_authentication_failure:
        return ThumbnailPipelineStatus::internal_error;
    case worker::ResultStatus::document_password_incorrect:
        return ThumbnailPipelineStatus::document_password_incorrect;
    case worker::ResultStatus::ghostscript_required:
        return ThumbnailPipelineStatus::ghostscript_required;
    case worker::ResultStatus::embedded_preview_unavailable:
        return ThumbnailPipelineStatus::embedded_preview_unavailable;
    case worker::ResultStatus::internal_error:
        return ThumbnailPipelineStatus::internal_error;
    }
    return ThumbnailPipelineStatus::internal_error;
}

[[nodiscard]] cache::ColorModel map_color_model(const worker::ColorModel model) noexcept {
    switch (model) {
    case worker::ColorModel::unknown:
        return cache::ColorModel::unknown;
    case worker::ColorModel::grayscale:
        return cache::ColorModel::gray;
    case worker::ColorModel::rgb:
        return cache::ColorModel::rgb;
    case worker::ColorModel::cmyk:
        return cache::ColorModel::cmyk;
    case worker::ColorModel::lab:
        return cache::ColorModel::lab;
    case worker::ColorModel::mixed:
        return cache::ColorModel::mixed;
    }
    return cache::ColorModel::unknown;
}

[[nodiscard]] ResolvedThumbnailSource
resolve_source_identity(const ThumbnailPipelineRequest &request,
                        const bool force_bounded_fingerprint,
                        const std::string_view fallback_cmyk_fingerprint,
                        const std::string_view svg_font_environment_fingerprint) {
    record_test_audit_event("source-open");
    const auto path = path_from_utf8(request.source.source_identity_utf8);
    if (path.empty()) {
        return {.status = ThumbnailPipelineStatus::malformed_source,
                .source = {},
                .diagnostic = "source path is invalid UTF-8"};
    }
    auto opened = platform::open_read_only_source(path, kMaximumContainerSourceBytes);
    if (!opened.source || !opened.source->valid()) {
        return {.status = map_source_error(opened.error.kind),
                .source = {},
                .diagnostic = std::move(opened.error.detail)};
    }
    const auto revision = split_revision(request.source.stable_file_id);
    if (opened.source->size_bytes() != request.source.size_bytes ||
        opened.source->modified_unix_ns() != request.source.modified_unix_ns ||
        (!revision.base.empty() && opened.source->source_revision_utf8() != revision.base)) {
        return {.status = ThumbnailPipelineStatus::source_changed,
                .source = {},
                .diagnostic = "source identity changed before cache lookup"};
    }

    auto resolved = request.source;
    if (!opened.source->source_revision_utf8().empty()) {
        resolved.stable_file_id = opened.source->source_revision_utf8();
    }
    if (force_bounded_fingerprint ||
        needs_bounded_fingerprint(path, *opened.source, opened.source->source_revision_utf8())) {
        const auto fingerprint = bounded_fingerprint(*opened.source);
        if (!fingerprint) {
            return {.status = ThumbnailPipelineStatus::source_unavailable,
                    .source = {},
                    .diagnostic = "bounded source fingerprint could not be read"};
        }
        resolved.stable_file_id =
            opened.source->source_revision_utf8() + std::string{kFingerprintMarker} + *fingerprint;
    }
    handlers::raster::RasterFormat raster_format{};
    const auto handler = detect_handler(*opened.source, path, &raster_format);
    const auto handler_uses_fallback = uses_fallback_profile(handler, raster_format);
    return {
        .status = ThumbnailPipelineStatus::success,
            .source = std::move(resolved),
            .handler_id = std::string(handler_id(handler)),
            .handler_version = handler_version(handler),
            .color_policy_fingerprint = color_policy_fingerprint(
                handler_uses_fallback, fallback_cmyk_fingerprint,
            handler == HandlerKind::svg ? svg_font_environment_fingerprint : std::string_view{}),
            .diagnostic = {}};
}

[[nodiscard]] RenderedThumbnail render_postscript_with_ghostscript(
    const RendererState &state, const ThumbnailPipelineRequest &request,
    const platform::ReadOnlySource &source, const RevisionParts &revision) {
    if (state.options.ghostscript_executable.empty()) {
        return failure(ThumbnailPipelineStatus::ghostscript_required,
                       "Ghostscript executable is not configured");
    }
    if (request.page_index != 0) {
        return failure(ThumbnailPipelineStatus::unsupported,
                       "PostScript preview currently supports only the first page");
    }
    if (source.size_bytes() > kMaximumDecodedSourceBytes) {
        return failure(ThumbnailPipelineStatus::resource_limit,
                       "PostScript source exceeds the bounded renderer input limit");
    }

#ifdef _WIN32
    UniqueFile output{std::tmpfile()};
    if (!output) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "temporary Ghostscript output is unavailable");
    }
    const auto native_output = output_object(output.get());
    if (native_output == worker::kInvalidNativeObject) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "temporary Ghostscript output handle is invalid");
    }
#else
    auto temporary_output = make_posix_temporary_output();
    if (!temporary_output.reader || !temporary_output.writer) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "temporary Ghostscript output is unavailable");
    }
    auto &output = temporary_output.reader;
    const auto native_output = static_cast<worker::NativeObject>(*temporary_output.writer);
#endif

    const auto edge =
        std::clamp<std::uint32_t>(request.canonical_edge, 1U, cache::kMaxArtifactDimension);
    const auto wall_timeout =
        request.extended_limits
            ? kExtendedTimeoutMs
            : std::clamp<std::int64_t>(state.options.timeout.count(), 1, kExtendedTimeoutMs);
    const std::vector<std::string> arguments{"-q",
                                             "-dSAFER",
                                             "-dBATCH",
                                             "-dNOPAUSE",
                                             "-dFirstPage=1",
                                             "-dLastPage=1",
                                             "-sDEVICE=ppmraw",
                                             "-dTextAlphaBits=4",
                                             "-dGraphicsAlphaBits=4",
                                             "-dEPSCrop",
                                             "-sstdout=%stderr",
                                             "-g" + std::to_string(edge) + "x" +
                                                 std::to_string(edge),
                                             "-dFIXEDMEDIA",
                                             "-dFitPage",
                                             "-sOutputFile=%stdout",
                                             "-"};
    const worker::ExternalRendererRequest renderer_request{
        .executable = state.options.ghostscript_executable,
        .arguments_utf8 = arguments,
        .source_object = static_cast<worker::NativeObject>(source.native_object()),
        .output_object = native_output,
        .limits = {.maximum_input_bytes = kMaximumDecodedSourceBytes,
                   .maximum_output_bytes = kMaximumGhostscriptOutputBytes,
                   .memory_limit_bytes =
                       request.extended_limits ? kExtendedMemoryBytes : kGhostscriptMemoryBytes,
                   .wall_timeout_ms = static_cast<std::uint32_t>(wall_timeout),
                   .canonical_edge = edge},
        .timeout = std::chrono::milliseconds{wall_timeout},
        .require_minimum_sandbox = true};
    if (std::getenv("VOVE_TEST_PREVIEW_AUDIT_LOG") != nullptr) {
        const auto &limits = renderer_request.limits;
        record_test_audit_event("ghostscript-budget:" + std::to_string(limits.maximum_input_bytes) +
                                ":" + std::to_string(limits.memory_limit_bytes) + ":" +
                                std::to_string(limits.wall_timeout_ms));
    }
    const auto rendered = worker::run_external_renderer_once(renderer_request);
    if (!rendered.ok()) {
        if (rendered.error == worker::SupervisorError::timed_out) {
            return failure(ThumbnailPipelineStatus::processing_timed_out, rendered.detail);
        }
        if (rendered.error == worker::SupervisorError::resource_limit) {
            return failure(ThumbnailPipelineStatus::resource_limit, rendered.detail);
        }
        if (rendered.error == worker::SupervisorError::worker_crashed) {
            return failure(ThumbnailPipelineStatus::malformed_source,
                           "Ghostscript could not render this document");
        }
        if (rendered.error == worker::SupervisorError::transport_error &&
            rendered.detail.find("output boundary violated") != std::string::npos) {
            return failure(ThumbnailPipelineStatus::malformed_source,
                           "Ghostscript produced no valid document image");
        }
        if (rendered.error == worker::SupervisorError::invalid_request ||
            rendered.error == worker::SupervisorError::launch_failed ||
            rendered.error == worker::SupervisorError::sandbox_unavailable) {
            return failure(ThumbnailPipelineStatus::ghostscript_required,
                           rendered.detail.empty() ? "Ghostscript is unavailable"
                                                   : rendered.detail);
        }
        return failure(ThumbnailPipelineStatus::internal_error, rendered.detail);
    }

    const auto identity_status = source.identity_status();
    if (identity_status == platform::SourceIdentityStatus::unavailable) {
        return failure(ThumbnailPipelineStatus::source_unavailable,
                       "source identity became unavailable during Ghostscript rendering");
    }
    if (identity_status == platform::SourceIdentityStatus::changed) {
        return failure(ThumbnailPipelineStatus::source_changed,
                       "source identity changed during Ghostscript rendering");
    }
    if (revision.fingerprint) {
        const auto actual = bounded_fingerprint(source);
        if (!actual) {
            return failure(ThumbnailPipelineStatus::source_unavailable,
                           "source fingerprint became unavailable during Ghostscript rendering");
        }
        if (*actual != *revision.fingerprint) {
            return failure(ThumbnailPipelineStatus::source_changed,
                           "source fingerprint changed during Ghostscript rendering");
        }
    }
    if (rendered.bytes_written == 0 || rendered.bytes_written > kMaximumGhostscriptOutputBytes ||
        std::fseek(output.get(), 0, SEEK_SET) != 0) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "Ghostscript output metadata is invalid");
    }
    std::vector<std::byte> ppm(static_cast<std::size_t>(rendered.bytes_written));
    if (std::fread(ppm.data(), 1, ppm.size(), output.get()) != ppm.size()) {
        return failure(ThumbnailPipelineStatus::internal_error, "Ghostscript output is truncated");
    }
    auto image = parse_ghostscript_ppm(ppm, edge);
    if (!image.ok()) {
        return failure(ThumbnailPipelineStatus::internal_error, std::move(image.error));
    }
    auto encoded = cache::encode_qoi_rgba8(image.rgba8, image.width, image.height,
                                           static_cast<std::size_t>(image.width) * 4U);
    if (!encoded.ok() || encoded.bytes.empty() ||
        encoded.bytes.size() > kMaximumWorkerOutputBytes) {
        return failure(ThumbnailPipelineStatus::resource_limit,
                       encoded.error.empty() ? "PostScript thumbnail exceeds the output limit"
                                             : std::move(encoded.error));
    }
    return {.status = ThumbnailPipelineStatus::success,
            .metadata = {.encoding = cache::ArtifactEncoding::qoi_rgba8,
                         .width = image.width,
                         .height = image.height,
                         .payload_bytes = 0,
                         .payload_crc32 = 0,
                         .page_count = 1,
                         .source_color_model = cache::ColorModel::unknown,
                         .provenance = cache::PreviewProvenance::primary_render},
            .qoi_payload = std::move(encoded.bytes),
            .source_profile_name = {},
            .source_profile_fingerprint = {},
            .diagnostic = {}};
}

[[nodiscard]] RenderedThumbnail render_with_worker(const RendererState &state,
                                                   const ThumbnailPipelineRequest &request) {
    const auto &options = state.options;
    record_test_audit_event("worker-launch");
    const auto path = path_from_utf8(request.source.source_identity_utf8);
    if (path.empty()) {
        return failure(ThumbnailPipelineStatus::malformed_source, "source path is invalid UTF-8");
    }
    auto opened = platform::open_read_only_source(path, kMaximumContainerSourceBytes);
    if (!opened.source || !opened.source->valid()) {
        return failure(map_source_error(opened.error.kind), opened.error.detail);
    }
    if (opened.source->size_bytes() != request.source.size_bytes ||
        opened.source->modified_unix_ns() != request.source.modified_unix_ns) {
        return failure(ThumbnailPipelineStatus::source_changed,
                       "source identity changed before decoding");
    }
    const auto revision = split_revision(request.source.stable_file_id);
    if (!revision.base.empty() && opened.source->source_revision_utf8() != revision.base) {
        return failure(ThumbnailPipelineStatus::source_changed,
                       "source revision changed before decoding");
    }
    if (revision.fingerprint) {
        const auto actual = bounded_fingerprint(*opened.source);
        if (!actual) {
            return failure(ThumbnailPipelineStatus::source_unavailable,
                           "source fingerprint became unavailable before decoding");
        }
        if (*actual != *revision.fingerprint) {
            return failure(ThumbnailPipelineStatus::source_changed,
                           "source fingerprint changed before decoding");
        }
    }

    handlers::raster::RasterFormat raster_format{};
    const auto handler = detect_handler(*opened.source, path, &raster_format);
    if (request.handler_id != handler_id(handler) ||
        request.handler_version != handler_version(handler)) {
        return failure(ThumbnailPipelineStatus::source_changed,
                       "source handler identity changed before decoding");
    }
    std::optional<RenderedThumbnail> postscript_primary_failure;
    if (handler == HandlerKind::postscript && !options.ghostscript_executable.empty()) {
        auto primary = render_postscript_with_ghostscript(state, request, *opened.source, revision);
        if (primary.status == ThumbnailPipelineStatus::success ||
            (primary.status != ThumbnailPipelineStatus::malformed_source &&
             primary.status != ThumbnailPipelineStatus::ghostscript_required)) {
            return primary;
        }
        postscript_primary_failure = std::move(primary);
    }
    const auto document = handler == HandlerKind::pdf || handler == HandlerKind::postscript;
    const auto selected_pdf = handler == HandlerKind::pdf && request.selected_view;
    if (handler == HandlerKind::pdf && opened.source->size_bytes() > kMaximumPdfSourceBytes) {
        return failure(ThumbnailPipelineStatus::pdf_input_too_large,
                       "PDF source exceeds the 2 GiB input limit");
    }
    const auto embedded_document =
        handler == HandlerKind::cdr || handler == HandlerKind::indesign ||
        handler == HandlerKind::idml || handler == HandlerKind::affinity ||
        handler == HandlerKind::archive || handler == HandlerKind::dxf;
    const auto hpgl = handler == HandlerKind::hpgl;
    const auto svg = handler == HandlerKind::svg || hpgl;
    const auto xcf = handler == HandlerKind::xcf;
    const auto handler_uses_fallback = uses_fallback_profile(handler, raster_format);
    const auto use_fallback_profile =
        handler_uses_fallback && static_cast<bool>(state.fallback_cmyk_profile);
    const auto maximum_source_bytes =
        handler == HandlerKind::pdf   ? kMaximumPdfSourceBytes
        : handler == HandlerKind::psd ? handlers::raster::kMaximumPsbSourceBytes
        : xcf                         ? handlers::xcf::kMaximumXcfSourceBytes
        : hpgl                        ? kMaximumHpglSourceBytes
        : svg                         ? kMaximumSvgSourceBytes
              : (embedded_document || handler == HandlerKind::raw ? kMaximumContainerSourceBytes
                                                                  : kMaximumDecodedSourceBytes);
    const auto &worker_executable =
        xcf   ? options.xcf_worker_executable
        : svg ? options.svg_worker_executable
            : (embedded_document
                   ? options.cdr_worker_executable
                   : (document ? options.document_worker_executable : options.worker_executable));
    const auto &expected_build_id =
        xcf   ? options.xcf_expected_build_id
        : svg ? options.svg_expected_build_id
            : (embedded_document
                   ? options.cdr_expected_build_id
                   : (document ? options.document_expected_build_id : options.expected_build_id));
    const auto preview_worker = xcf                 ? PreviewWorker::xcf
                                : svg               ? PreviewWorker::svg
                                : embedded_document ? PreviewWorker::cdr
                                : document          ? PreviewWorker::document
                                                    : PreviewWorker::raster;
    if (worker_executable.empty() || expected_build_id.empty()) {
        if (postscript_primary_failure) {
            return std::move(*postscript_primary_failure);
        }
        auto result = failure(ThumbnailPipelineStatus::worker_start_failed,
                              "preview worker is not configured");
        result.startup_diagnostic = {WorkerStartupStage::configuration, preview_worker};
        return result;
    }

#ifdef _WIN32
    UniqueFile output{std::tmpfile()};
    if (!output) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "temporary thumbnail output is unavailable");
    }
    const auto native_output = output_object(output.get());
    if (native_output == worker::kInvalidNativeObject) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "temporary thumbnail output handle is invalid");
    }
#else
    auto temporary_output = make_posix_temporary_output();
    if (!temporary_output.reader || !temporary_output.writer) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "temporary thumbnail output is unavailable");
    }
    auto &output = temporary_output.reader;
    const auto native_output = static_cast<worker::NativeObject>(*temporary_output.writer);
#endif

    const auto wall_timeout =
        request.extended_limits
            ? kExtendedTimeoutMs
            : std::clamp<std::int64_t>(
                  (selected_pdf ? options.selected_document_timeout : options.timeout).count(), 1,
                  kExtendedTimeoutMs);
    const worker::SupervisorRequest supervisor_request{
        .worker_executable = worker_executable,
        .source_object = static_cast<worker::NativeObject>(opened.source->native_object()),
        .output_object = native_output,
        .profile_object = use_fallback_profile ? file_object(state.fallback_cmyk_profile.get())
                                               : worker::kInvalidNativeObject,
        .job = {.job_id = 1,
                .generation = 1,
                .source_token = 1,
                .output_token = 2,
                .limits = {.maximum_input_bytes = maximum_source_bytes,
                           .maximum_output_bytes = kMaximumWorkerOutputBytes,
                           .memory_limit_bytes = request.extended_limits ? kExtendedMemoryBytes
                                                 : selected_pdf          ? kSelectedPdfMemoryBytes
                                                                         : kWorkerMemoryBytes,
                           .wall_timeout_ms = static_cast<std::uint32_t>(wall_timeout),
                           .canonical_edge = request.canonical_edge},
                .page_index = request.page_index,
                .source_format_hint =
                    handler == HandlerKind::raw       ? worker::SourceFormatHint::camera_raw
                    : handler == HandlerKind::archive ? worker::SourceFormatHint::layered_archive
                    : hpgl                            ? worker::SourceFormatHint::hpgl
                    : handler == HandlerKind::dxf     ? worker::SourceFormatHint::dxf
                    : handler == HandlerKind::idml    ? worker::SourceFormatHint::idml
                    : handler == HandlerKind::xcf     ? worker::SourceFormatHint::xcf
                                                      : worker::SourceFormatHint::auto_detect,
                .password_utf8 = request.password_utf8},
        .expected_build_id = expected_build_id,
        .timeout = std::chrono::milliseconds{wall_timeout},
        .require_minimum_sandbox = true};
    if (std::getenv("VOVE_TEST_PREVIEW_AUDIT_LOG") != nullptr) {
        const auto &limits = supervisor_request.job.limits;
        record_test_audit_event("worker-budget:" + std::to_string(limits.maximum_input_bytes) +
                                ":" + std::to_string(limits.memory_limit_bytes) + ":" +
                                std::to_string(limits.wall_timeout_ms));
    }
    const auto supervised = worker::run_worker_once(supervisor_request);
    if (supervised.error != worker::SupervisorError::none) {
        const auto startup = classify_worker_startup_failure(supervised, preview_worker);
        const auto status = startup.present() ? ThumbnailPipelineStatus::worker_start_failed
                            : supervised.error == worker::SupervisorError::timed_out
                                ? ThumbnailPipelineStatus::processing_timed_out
                                : ThumbnailPipelineStatus::internal_error;
        if (postscript_primary_failure && !startup.present() &&
            status != ThumbnailPipelineStatus::processing_timed_out) {
            return std::move(*postscript_primary_failure);
        }
        auto result = failure(status, supervised.detail + " (exit " +
                                          std::to_string(supervised.exit_code) + ")");
        result.startup_diagnostic = startup;
        return result;
    }
    if (supervised.worker_result.status != worker::ResultStatus::success) {
        if (supervised.worker_result.status == worker::ResultStatus::color_profile_required &&
            handler_uses_fallback && !state.fallback_cmyk_diagnostic.empty()) {
            return failure(ThumbnailPipelineStatus::color_profile_required,
                           state.fallback_cmyk_diagnostic);
        }
        if (postscript_primary_failure) {
            return std::move(*postscript_primary_failure);
        }
        return failure(map_worker_status(supervised.worker_result.status),
                       supervised.worker_result.diagnostic_utf8);
    }
    if (supervised.worker_result.page_index != request.page_index) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "worker returned a different document page");
    }
    const auto identity_status = opened.source->identity_status();
    if (identity_status == platform::SourceIdentityStatus::unavailable) {
        return failure(ThumbnailPipelineStatus::source_unavailable,
                       "source identity became unavailable during decoding");
    }
    if (identity_status == platform::SourceIdentityStatus::changed) {
        return failure(ThumbnailPipelineStatus::source_changed,
                       "source identity changed during decoding");
    }
    if (revision.fingerprint) {
        const auto actual = bounded_fingerprint(*opened.source);
        if (!actual) {
            return failure(ThumbnailPipelineStatus::source_unavailable,
                           "source fingerprint became unavailable during decoding");
        }
        if (*actual != *revision.fingerprint) {
            return failure(ThumbnailPipelineStatus::source_changed,
                           "source fingerprint changed during decoding");
        }
    }
    if (supervised.worker_result.bytes_written == 0 ||
        supervised.worker_result.bytes_written > kMaximumWorkerOutputBytes ||
        std::fseek(output.get(), 0, SEEK_SET) != 0) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "worker thumbnail output metadata is invalid");
    }

    std::vector<std::byte> payload(
        static_cast<std::size_t>(supervised.worker_result.bytes_written));
    const auto read = std::fread(payload.data(), 1, payload.size(), output.get());
    if (read != payload.size()) {
        return failure(ThumbnailPipelineStatus::internal_error,
                       "worker thumbnail output is truncated");
    }

    return {
        .status = ThumbnailPipelineStatus::success,
        .metadata = {.encoding = cache::ArtifactEncoding::qoi_rgba8,
                     .width = supervised.worker_result.width,
                     .height = supervised.worker_result.height,
                     .payload_bytes = 0,
                     .payload_crc32 = 0,
                     .page_count = supervised.worker_result.page_count,
                     .source_color_model = map_color_model(supervised.worker_result.color_model),
                     .provenance = supervised.worker_result.provenance ==
                                           worker::PreviewProvenance::embedded_preview
                                       ? cache::PreviewProvenance::embedded_preview
                                       : cache::PreviewProvenance::primary_render},
        .qoi_payload = std::move(payload),
        .source_profile_name = supervised.worker_result.color_profile_utf8,
        .source_profile_fingerprint = supervised.worker_result.source_profile_fingerprint,
        .diagnostic = {}};
}

} // namespace

WorkerStartupDiagnostic
classify_worker_startup_failure(const worker::SupervisorResult &result,
                                const PreviewWorker preview_worker) noexcept {
    WorkerStartupStage stage{};
    switch (result.error) {
    case worker::SupervisorError::launch_failed:
        stage = WorkerStartupStage::launch;
        break;
    case worker::SupervisorError::handshake_failed:
        stage = WorkerStartupStage::handshake;
        break;
    case worker::SupervisorError::incompatible_worker:
        stage = WorkerStartupStage::compatibility;
        break;
    case worker::SupervisorError::sandbox_unavailable:
        stage = WorkerStartupStage::containment;
        break;
    case worker::SupervisorError::transport_error:
    case worker::SupervisorError::timed_out:
    case worker::SupervisorError::worker_crashed:
        if (result.handshake_completed)
            return {};
        stage = WorkerStartupStage::handshake;
        break;
    case worker::SupervisorError::none:
    case worker::SupervisorError::invalid_request:
    case worker::SupervisorError::resource_limit:
        return {};
    }
    const WorkerStartupDiagnostic diagnostic{stage, preview_worker, result.system_error,
                                             static_cast<std::uint32_t>(result.exit_code)};
    return diagnostic.valid() ? diagnostic : WorkerStartupDiagnostic{};
}

WorkerThumbnailBackend make_worker_thumbnail_renderer(WorkerThumbnailRendererOptions options) {
    auto state = std::make_shared<RendererState>();
    state->options = std::move(options);
    if (!state->options.svg_worker_executable.empty() &&
        !state->options.svg_expected_build_id.empty() &&
        state->options.svg_font_environment_fingerprint.empty()) {
        state->options.svg_font_environment_fingerprint = system_font_environment_fingerprint();
    }
    if (!state->options.fallback_cmyk_profile.empty()) {
        auto opened = platform::open_read_only_source(state->options.fallback_cmyk_profile,
                                                      color::kMaximumIccProfileBytes);
        if (!opened.source || !opened.source->valid() || opened.source->size_bytes() == 0) {
            state->fallback_cmyk_diagnostic =
                "Configured fallback CMYK profile is unavailable or too large";
        } else {
#ifdef _WIN32
            const auto native_profile = reinterpret_cast<handlers::raster::NativeSourceHandle>(
                opened.source->native_object());
#else
            const auto native_profile =
                static_cast<handlers::raster::NativeSourceHandle>(opened.source->native_object());
#endif
            auto profile = handlers::raster::make_native_source(native_profile,
                                                                color::kMaximumIccProfileBytes);
            if (!profile.ok() || !profile.source.has_value()) {
                state->fallback_cmyk_diagnostic =
                    "Configured fallback CMYK profile could not be read";
            } else {
                std::vector<std::byte> bytes(static_cast<std::size_t>(profile.source->size()));
                const auto read = profile.source->read_at(0, bytes);
                if (!read.ok() || read.bytes_read != bytes.size() ||
                    !profile.source->validate_unchanged().unchanged) {
                    state->fallback_cmyk_diagnostic =
                        "Configured fallback CMYK profile changed while loading";
                } else if (!plausible_cmyk_icc(bytes)) {
                    state->fallback_cmyk_diagnostic =
                        "Configured fallback CMYK profile is invalid or not CMYK";
                } else {
                    state->fallback_cmyk_profile = make_profile_snapshot(bytes);
                    if (!state->fallback_cmyk_profile) {
                        state->fallback_cmyk_diagnostic =
                            "Configured fallback CMYK profile could not be prepared";
                    } else {
                        state->fallback_cmyk_fingerprint = cache::hash_bytes(bytes).hex();
                    }
                }
            }
        }
    }
    return {.resolve_source =
                [state](const ThumbnailPipelineRequest &request) {
                    return resolve_source_identity(request,
                                                   state->options.force_bounded_fingerprint,
                                                   state->fallback_cmyk_fingerprint,
                                                   state->options.svg_font_environment_fingerprint);
                },
            .render =
                [state](const ThumbnailPipelineRequest &request) {
                    return render_with_worker(*state, request);
                },
            .resolve_color_policy =
                [state](const ThumbnailPipelineRequest &request) {
                    const auto path = path_from_utf8(request.source.source_identity_utf8);
                    return color_policy_fingerprint(
                        !path.empty() && path_may_use_fallback_profile(path),
                        state->fallback_cmyk_fingerprint,
                        !path.empty() && svg_extension(path)
                            ? state->options.svg_font_environment_fingerprint
                            : std::string_view{});
                }};
}

} // namespace vove::preview
