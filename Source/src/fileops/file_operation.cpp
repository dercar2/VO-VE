#include "vove/fileops/file_operation.hpp"

#include "vove/core/reserved_names.hpp"

#include "delete_identity.hpp"
#include "operation_evidence.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <limits>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "windows/remote_protocol.hpp"
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace vove::fileops {
namespace {

OperationStatus delete_target_error_status(const std::uint32_t code) noexcept {
#ifdef _WIN32
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return OperationStatus::not_found;
    case ERROR_ACCESS_DENIED:
        return OperationStatus::permission_denied;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return OperationStatus::file_in_use;
    case ERROR_SEM_TIMEOUT:
        return OperationStatus::timed_out;
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_UNAVAIL:
        return OperationStatus::disconnected;
    default:
        return OperationStatus::io_error;
    }
#else
    switch (static_cast<int>(code)) {
    case ENOENT:
    case ENOTDIR:
        return OperationStatus::not_found;
    case EACCES:
    case EPERM:
        return OperationStatus::permission_denied;
    case EBUSY:
    case ETXTBSY:
        return OperationStatus::file_in_use;
    case ETIMEDOUT:
        return OperationStatus::timed_out;
    case ENETDOWN:
    case ENETUNREACH:
    case ECONNABORTED:
    case ECONNRESET:
    case EHOSTUNREACH:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
#ifdef ESTALE
    case ESTALE:
#endif
        return OperationStatus::disconnected;
    default:
        return OperationStatus::io_error;
    }
#endif
}

std::string ascii_upper(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        if (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) {
            return static_cast<char>(character - static_cast<unsigned char>('a') +
                                     static_cast<unsigned char>('A'));
        }
        return static_cast<char>(character);
    });
    return value;
}

bool reserved_windows_stem(const std::string &name) {
    const auto dot = name.find('.');
    const auto stem = ascii_upper(name.substr(0, dot));
    constexpr std::array reserved{
        std::string_view{"CON"},    std::string_view{"PRN"},    std::string_view{"AUX"},
        std::string_view{"NUL"},    std::string_view{"CONIN$"}, std::string_view{"CONOUT$"},
        std::string_view{"CLOCK$"},
    };
    if (std::ranges::find(reserved, stem) != reserved.end()) {
        return true;
    }
    if (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem[3] >= '1' && stem[3] <= '9') {
        return true;
    }
    constexpr std::array superscript_digits{
        std::string_view{"\xC2\xB9"}, std::string_view{"\xC2\xB2"}, std::string_view{"\xC2\xB3"}};
    if (stem.size() == 5 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        std::ranges::find(superscript_digits, std::string_view(stem).substr(3)) !=
            superscript_digits.end()) {
        return true;
    }
    return false;
}

bool valid_portable_filename(const std::filesystem::path &path, std::string &detail) {
    const auto filename = path.filename().u8string();
    if (filename.empty() || filename == u8"." || filename == u8"..") {
        detail = "destination filename is empty or reserved";
        return false;
    }
    if (filename.back() == u8'.' || filename.back() == u8' ') {
        detail = "destination filename cannot end with a dot or space";
        return false;
    }
    std::string narrow;
    narrow.reserve(filename.size());
    for (const auto code_unit : filename) {
        const auto value = static_cast<unsigned char>(code_unit);
        if (value < 0x20U || value == '<' || value == '>' || value == ':' || value == '"' ||
            value == '/' || value == '\\' || value == '|' || value == '?' || value == '*') {
            detail = "destination filename contains a forbidden character";
            return false;
        }
        narrow.push_back(static_cast<char>(value));
    }
    if (reserved_windows_stem(narrow)) {
        detail = "destination filename is reserved by Windows";
        return false;
    }
    return true;
}

bool same_extension(const std::filesystem::path &source, const std::filesystem::path &destination) {
    auto source_extension = source.extension().u8string();
    auto destination_extension = destination.extension().u8string();
#ifdef _WIN32
    const auto fold = [](const char8_t value) {
        return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value + 0x20) : value;
    };
    std::ranges::transform(source_extension, source_extension.begin(), fold);
    std::ranges::transform(destination_extension, destination_extension.begin(), fold);
#endif
    return source_extension == destination_extension;
}

bool same_path_spelling(const std::filesystem::path &source,
                        const std::filesystem::path &destination) {
    auto source_text = source.lexically_normal().u8string();
    auto destination_text = destination.lexically_normal().u8string();
#ifdef _WIN32
    const auto fold = [](const char8_t value) {
        return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value + 0x20) : value;
    };
    std::ranges::transform(source_text, source_text.begin(), fold);
    std::ranges::transform(destination_text, destination_text.begin(), fold);
#endif
    return source_text == destination_text;
}

bool valid_utf8(const std::u8string_view text) noexcept {
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1;
            code_point = first & 0x1FU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

bool valid_utf8(const std::string_view text) noexcept {
    return valid_utf8(
        std::u8string_view(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

} // namespace

std::string stable_object_identity(const std::string_view source_revision) {
    if (source_revision.starts_with("win-file128:")) {
        const auto volume_end = source_revision.find(':', std::string_view{"win-file128:"}.size());
        if (volume_end == std::string_view::npos) {
            return {};
        }
        const auto id_end = source_revision.find(':', volume_end + 1U);
        if (id_end == volume_end + 1U) {
            return {};
        }
        if (id_end == std::string_view::npos) {
            return std::string(source_revision);
        }
        return std::string(source_revision.substr(0, id_end));
    }
    if (source_revision.starts_with("posix2:")) {
        const auto separator_count =
            static_cast<std::size_t>(std::ranges::count(source_revision, ':'));
        if (separator_count == 3U) {
            return std::string(source_revision);
        }
        if (separator_count != 4U) {
            return {};
        }
        const auto last = source_revision.find_last_of(':');
        return last == std::string_view::npos || last == 0
                   ? std::string{}
                   : std::string(source_revision.substr(0, last));
    }
    if (source_revision.starts_with("win-file:") || source_revision.starts_with("win-smb64:") ||
        source_revision.starts_with("posix:")) {
        const auto separator_count =
            static_cast<std::size_t>(std::ranges::count(source_revision, ':'));
        if (separator_count == 2U) {
            return std::string(source_revision);
        }
        if (separator_count != 3U) {
            return {};
        }
        const auto last = source_revision.find_last_of(':');
        if (last == std::string_view::npos || last == 0) {
            return {};
        }
        return std::string(source_revision.substr(0, last));
    }
    return {};
}

namespace {

std::string revision_change(const std::string_view revision) {
    const auto last = revision.find_last_of(':');
    return last == std::string_view::npos || last + 1U == revision.size()
               ? std::string{}
               : std::string(revision.substr(last + 1U));
}

std::string legacy_object_identity(const std::string_view revision) {
    if (revision.starts_with("posix:")) {
        return stable_object_identity(revision);
    }
    if (revision.starts_with("win-file:")) {
        return stable_object_identity(revision);
    }
    if (revision.starts_with("win-smb64:")) {
        const auto stable = stable_object_identity(revision);
        return stable.empty() ? std::string{} : "win-file:" + stable.substr(10U);
    }
    if (!revision.starts_with("win-file128:")) {
        return {};
    }
    const auto prefix_size = std::string_view{"win-file128:"}.size();
    const auto volume_end = revision.find(':', prefix_size);
    if (volume_end == std::string_view::npos) {
        return {};
    }
    const auto id_begin = volume_end + 1U;
    const auto id_end = revision.find(':', id_begin);
    if (id_end == std::string_view::npos) {
        return {};
    }
    const auto volume_text = revision.substr(prefix_size, volume_end - prefix_size);
    const auto id_text = revision.substr(id_begin, id_end - id_begin);
    if (volume_text.empty() || id_text.size() != 32U) {
        return {};
    }
    try {
        const auto volume = std::stoull(std::string(volume_text));
        std::uint64_t legacy_id{};
        for (std::size_t index{}; index < 8U; ++index) {
            const auto byte = std::stoul(std::string(id_text.substr(index * 2U, 2U)), nullptr, 16);
            legacy_id |= static_cast<std::uint64_t>(byte) << (index * 8U);
        }
        if (legacy_id == 0) {
            return {};
        }
        return "win-file:" + std::to_string(static_cast<std::uint32_t>(volume & 0xffffffffULL)) +
               ':' + std::to_string(legacy_id);
    } catch (...) {
        return {};
    }
}

std::string legacy_posix_object_identity(const std::string_view revision) {
    const auto stable_identity = stable_object_identity(revision);
    if (stable_identity.empty()) {
        return {};
    }
    if (revision.starts_with("posix:")) {
        return stable_identity;
    }
    if (!revision.starts_with("posix2:")) {
        return {};
    }
    const auto birth_time_separator = stable_identity.find_last_of(':');
    if (birth_time_separator == std::string::npos) {
        return {};
    }
    return "posix:" +
           stable_identity.substr(std::string_view{"posix2:"}.size(),
                                  birth_time_separator - std::string_view{"posix2:"}.size());
}

bool legacy_posix_identity_bridge(const std::string_view left, const std::string_view right) {
    const auto mixed_versions = (left.starts_with("posix:") && right.starts_with("posix2:")) ||
                                (left.starts_with("posix2:") && right.starts_with("posix:"));
    if (!mixed_versions) {
        return false;
    }
    const auto legacy_left = legacy_posix_object_identity(left);
    const auto legacy_right = legacy_posix_object_identity(right);
    if (legacy_left.empty() || legacy_left != legacy_right) {
        return false;
    }

    const auto legacy = left.starts_with("posix:") ? left : right;
    const auto strong = left.starts_with("posix2:") ? left : right;
    if (std::ranges::count(legacy, ':') != 3 || std::ranges::count(strong, ':') != 4) {
        return false;
    }
    const auto legacy_change_separator = legacy.find_last_of(':');
    const auto strong_change_separator = strong.find_last_of(':');
    const auto strong_birth_separator = strong.rfind(':', strong_change_separator - 1U);
    if (legacy_change_separator == std::string_view::npos ||
        strong_change_separator == std::string_view::npos ||
        strong_birth_separator == std::string_view::npos) {
        return false;
    }
    std::int64_t legacy_change{};
    std::int64_t strong_birth{};
    const auto legacy_text = legacy.substr(legacy_change_separator + 1U);
    const auto strong_text = strong.substr(strong_birth_separator + 1U,
                                           strong_change_separator - strong_birth_separator - 1U);
    const auto legacy_result =
        std::from_chars(legacy_text.data(), legacy_text.data() + legacy_text.size(), legacy_change);
    const auto strong_result =
        std::from_chars(strong_text.data(), strong_text.data() + strong_text.size(), strong_birth);
    return legacy_result.ec == std::errc{} &&
           legacy_result.ptr == legacy_text.data() + legacy_text.size() &&
           strong_result.ec == std::errc{} &&
           strong_result.ptr == strong_text.data() + strong_text.size() && strong_birth >= 0 &&
           strong_birth <= legacy_change;
}

// The first two parameters are deliberately symmetric; tolerance is a distinct unit-bearing bound.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool timestamps_within_tolerance(const std::int64_t before, const std::int64_t after,
                                 const std::uint64_t tolerance) noexcept {
    if (before == after) {
        return true;
    }
    const auto magnitude = [](const std::int64_t value) noexcept {
        return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                         : static_cast<std::uint64_t>(value);
    };
    std::uint64_t distance{};
    if ((before < 0) == (after < 0)) {
        distance = before >= after ? static_cast<std::uint64_t>(before - after)
                                   : static_cast<std::uint64_t>(after - before);
    } else {
        const auto before_magnitude = magnitude(before);
        const auto after_magnitude = magnitude(after);
        distance = before_magnitude > std::numeric_limits<std::uint64_t>::max() - after_magnitude
                       ? std::numeric_limits<std::uint64_t>::max()
                       : before_magnitude + after_magnitude;
    }
    return distance <= tolerance;
}

} // namespace

// The comparison is deliberately symmetric.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool same_object_identity(const std::string_view left, const std::string_view right) {
    const auto stable_left = stable_object_identity(left);
    const auto stable_right = stable_object_identity(right);
    if (stable_left.empty() || stable_right.empty()) {
        return false;
    }
    if (left.starts_with("posix2:") || right.starts_with("posix2:")) {
        return stable_left == stable_right;
    }
    const auto left_is_typed = left.starts_with("win-file128:") || left.starts_with("win-smb64:");
    const auto right_is_typed =
        right.starts_with("win-file128:") || right.starts_with("win-smb64:");
    if (left_is_typed && right_is_typed) {
        return stable_left == stable_right;
    }
    if (stable_left == stable_right) {
        return true;
    }
    const auto legacy_left = legacy_object_identity(left);
    const auto legacy_right = legacy_object_identity(right);
    return !legacy_left.empty() && !legacy_right.empty() && legacy_left == legacy_right;
}

// The comparison is deliberately symmetric.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool same_source_revision(const std::string_view left, const std::string_view right) {
    const auto left_change = revision_change(left);
    return !left_change.empty() && left_change == revision_change(right) &&
           same_object_identity(left, right);
}

bool same_object_after_rename(const SourceSnapshot &before, const SourceSnapshot &after) {
    constexpr std::uint64_t timestamp_tolerance_ns = 2'000'000'000ULL;
    const auto exact_identity =
        same_object_identity(before.source_revision_utf8, after.source_revision_utf8);
    const auto legacy_posix_bridge =
        legacy_posix_identity_bridge(before.source_revision_utf8, after.source_revision_utf8);
    if (before.size_bytes != after.size_bytes || (!exact_identity && !legacy_posix_bridge) ||
        (legacy_posix_bridge && before.modified_unix_ns != after.modified_unix_ns)) {
        return false;
    }
    const auto before_posix = before.source_revision_utf8.starts_with("posix:") ||
                              before.source_revision_utf8.starts_with("posix2:");
    const auto after_posix = after.source_revision_utf8.starts_with("posix:") ||
                             after.source_revision_utf8.starts_with("posix2:");
    if (!before_posix || !after_posix) {
        return before.modified_unix_ns == after.modified_unix_ns;
    }
    return timestamps_within_tolerance(before.modified_unix_ns, after.modified_unix_ns,
                                       timestamp_tolerance_ns);
}

bool same_content_after_rename(const SourceSnapshot &before, const SourceSnapshot &after) {
    return before.size_bytes == after.size_bytes &&
           before.modified_unix_ns == after.modified_unix_ns &&
           same_object_after_rename(before, after);
}

bool same_private_trash_payload_after_remount(
    const SourceSnapshot &before, const SourceSnapshot &after,
    const std::string_view expected_storage_identity,
    const std::string_view observed_storage_identity) noexcept {
    constexpr std::uint64_t timestamp_tolerance_ns = 2'000'000'000ULL;
    return !expected_storage_identity.empty() &&
           expected_storage_identity.starts_with("linux-smb:") &&
           expected_storage_identity == observed_storage_identity &&
           before.size_bytes == after.size_bytes &&
           (before.source_revision_utf8.starts_with("posix:") ||
            before.source_revision_utf8.starts_with("posix2:")) &&
           (after.source_revision_utf8.starts_with("posix:") ||
            after.source_revision_utf8.starts_with("posix2:")) &&
           timestamps_within_tolerance(before.modified_unix_ns, after.modified_unix_ns,
                                       timestamp_tolerance_ns);
}

bool valid_destination_filename(const std::filesystem::path &path, std::string &detail_utf8) {
    if (path.empty() || path != path.filename()) {
        detail_utf8 = "destination filename must not contain a path";
        return false;
    }
    if (!valid_utf8(path.u8string())) {
        detail_utf8 = "destination filename must contain valid UTF-8";
        return false;
    }
    constexpr std::size_t portable_maximum_component_units = 255;
    if (path.native().size() > portable_maximum_component_units ||
        path.u8string().size() > portable_maximum_component_units) {
        detail_utf8 = "destination filename is too long";
        return false;
    }
    return valid_portable_filename(path, detail_utf8);
}

bool is_trash_container(const std::filesystem::path &path) {
    const auto filename = path.filename().u8string();
    const auto prefix = core::kTrashFilenamePrefix;
    if (!core::is_trash_filename(filename) || filename.size() <= prefix.size()) {
        return false;
    }
    return std::all_of(filename.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       filename.end(), [](const char8_t character) {
                           return (character >= u8'0' && character <= u8'9') ||
                                  (character >= u8'a' && character <= u8'f');
                       });
}

bool is_trash_item(const std::filesystem::path &path) {
    const auto filename = path.filename().u8string();
    const auto prefix = core::kTrashItemFilenamePrefix;
    if (!core::is_trash_item_filename(filename) || filename.size() <= prefix.size() ||
        !is_trash_container(path.parent_path())) {
        return false;
    }
    return std::all_of(filename.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       filename.end(), [](const char8_t character) {
                           return (character >= u8'0' && character <= u8'9') ||
                                  (character >= u8'a' && character <= u8'f');
                       });
}

bool is_owned_trash_payload_path(const std::filesystem::path &path) {
    const auto normalized = path.lexically_normal();
    if (!same_path_spelling(path, normalized)) {
        return false;
    }
    for (auto candidate = normalized; !candidate.empty();) {
        if (is_trash_item(candidate)) {
            return true;
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return false;
}

bool transfer_source_name(const std::filesystem::path &path) {
    return path.filename().u8string().starts_with(core::kTransferSourceFilenamePrefix);
}

bool transfer_destination_name(const std::filesystem::path &path) {
    return path.filename().u8string().starts_with(core::kTransferDestinationFilenamePrefix);
}

bool transfer_overwrite_backup_name(const std::filesystem::path &path) {
    return path.filename().u8string().starts_with(core::kTransferOverwriteBackupFilenamePrefix);
}

bool transfer_rename_mode(const RenameMode mode) noexcept {
    return mode == RenameMode::transfer_atomic || mode == RenameMode::transfer_stage ||
           mode == RenameMode::transfer_publish || mode == RenameMode::transfer_restore ||
           mode == RenameMode::transfer_publish_replace ||
           mode == RenameMode::transfer_atomic_replace ||
           mode == RenameMode::transfer_overwrite_stage ||
           mode == RenameMode::transfer_overwrite_restore;
}

bool transfer_delete_mode(const DeleteMode mode) noexcept {
    return mode == DeleteMode::transfer_temp_cleanup ||
           mode == DeleteMode::transfer_source_commit ||
           mode == DeleteMode::transfer_overwrite_cleanup;
}

bool valid_rename_request(const RenameRequest &request, std::string &detail_utf8) {
    const auto has_anchor_path = !request.destination_anchor_path.empty();
    const auto has_anchor_identity = !request.destination_anchor_identity_utf8.empty();
    const auto replacement = request.mode == RenameMode::transfer_publish_replace ||
                             request.mode == RenameMode::transfer_atomic_replace;
    if (replacement != !request.expected_destination.source_revision_utf8.empty() ||
        (replacement &&
         (has_anchor_path ||
          stable_object_identity(request.expected_destination.source_revision_utf8).empty() ||
          request.expected_destination.source_revision_utf8.starts_with("win-file:") ||
          same_object_identity(request.expected_source.source_revision_utf8,
                               request.expected_destination.source_revision_utf8)))) {
        detail_utf8 = "replacement requires a distinct authorized destination snapshot";
        return false;
    }
    const bool single_mode = request.mode == RenameMode::single_preserve_extension ||
                             request.mode == RenameMode::single_allow_extension_change;
    const bool directory_object = request.object_kind == OperationObjectKind::directory;
    if (!single_mode && request.mode != RenameMode::batch_internal &&
        request.mode != RenameMode::trash_internal && request.mode != RenameMode::trash_restore &&
        !transfer_rename_mode(request.mode)) {
        detail_utf8 = "rename mode is invalid";
        return false;
    }
    if (request.object_kind != OperationObjectKind::regular_file && !directory_object) {
        detail_utf8 = "rename object kind is invalid";
        return false;
    }
    if (directory_object && !single_mode && request.mode != RenameMode::batch_internal &&
        request.mode != RenameMode::trash_internal &&
        request.mode != RenameMode::trash_restore) {
        detail_utf8 = "directory rename requires same-parent rename or VO-VE Trash";
        return false;
    }
    if (request.operation_id == 0 || request.source.empty() || request.destination.empty() ||
        request.expected_source.source_revision_utf8.empty()) {
        detail_utf8 = "rename request is incomplete";
        return false;
    }
    if (!request.source.is_absolute() || !request.destination.is_absolute()) {
        detail_utf8 = "rename paths must be absolute";
        return false;
    }
    if (!valid_utf8(request.source.u8string()) || !valid_utf8(request.destination.u8string())) {
        detail_utf8 = "rename paths must contain valid UTF-8";
        return false;
    }
    if (has_anchor_path != has_anchor_identity ||
        (has_anchor_path &&
         (!transfer_rename_mode(request.mode) || !request.destination_anchor_path.is_absolute() ||
          !valid_utf8(request.destination_anchor_path.u8string()) ||
          request.destination_anchor_identity_utf8.starts_with("win-file:") ||
          stable_object_identity(request.destination_anchor_identity_utf8) !=
              request.destination_anchor_identity_utf8))) {
        detail_utf8 = "rename destination anchor is incomplete or unsafe";
        return false;
    }
    if (request.source == request.destination) {
        detail_utf8 = "source and destination paths are identical";
        return false;
    }
    const bool ordinary_mode = single_mode || request.mode == RenameMode::batch_internal;
    const bool trash_store_shape = request.mode == RenameMode::trash_internal &&
                                   !is_trash_item(request.source) &&
                                   is_trash_item(request.destination);
    const bool trash_restore_shape = request.mode == RenameMode::trash_restore &&
                                     is_trash_item(request.source) &&
                                     !is_trash_item(request.destination);
    const bool same_parent =
        same_path_spelling(request.source.parent_path(), request.destination.parent_path());
    const bool transfer_shape =
        ((request.mode == RenameMode::transfer_atomic ||
          request.mode == RenameMode::transfer_atomic_replace) &&
         !core::is_transfer_filename(request.source.filename().u8string()) &&
         !core::is_transfer_filename(request.destination.filename().u8string())) ||
        (request.mode == RenameMode::transfer_stage && same_parent &&
         !core::is_transfer_filename(request.source.filename().u8string()) &&
         transfer_source_name(request.destination)) ||
        ((request.mode == RenameMode::transfer_publish ||
          request.mode == RenameMode::transfer_publish_replace) &&
         same_parent && transfer_destination_name(request.source) &&
         !core::is_transfer_filename(request.destination.filename().u8string())) ||
        (request.mode == RenameMode::transfer_restore && same_parent &&
         transfer_source_name(request.source) &&
         !core::is_transfer_filename(request.destination.filename().u8string())) ||
        (request.mode == RenameMode::transfer_overwrite_stage && same_parent &&
         !core::is_transfer_filename(request.source.filename().u8string()) &&
         transfer_overwrite_backup_name(request.destination)) ||
        (request.mode == RenameMode::transfer_overwrite_restore && same_parent &&
         transfer_overwrite_backup_name(request.source) &&
         !core::is_transfer_filename(request.destination.filename().u8string()));
    if ((ordinary_mode && !same_parent) ||
        (!ordinary_mode && !trash_store_shape && !trash_restore_shape && !transfer_shape)) {
        detail_utf8 = ordinary_mode ? "single rename must stay inside the current directory"
                                    : "internal rename has an invalid reserved-path shape";
        return false;
    }
    if (!valid_destination_filename(request.destination.filename(), detail_utf8)) {
        return false;
    }
    if (single_mode && core::is_internal_filename(request.destination.filename().u8string())) {
        detail_utf8 = "rename destination uses a reserved VO-VE filename";
        return false;
    }
    if (!directory_object && request.mode == RenameMode::single_preserve_extension &&
        !same_extension(request.source, request.destination)) {
        detail_utf8 = "rename cannot change the file extension";
        return false;
    }
    if ((request.mode == RenameMode::trash_internal || request.mode == RenameMode::trash_restore) &&
#ifdef _WIN32
        (!request.expected_source.source_revision_utf8.starts_with("win-file128:") ||
         !detail::valid_strong_delete_revision(request.expected_source.source_revision_utf8))
#else
        (!request.expected_source.source_revision_utf8.starts_with("posix:") &&
         !request.expected_source.source_revision_utf8.starts_with("posix2:"))
#endif
    ) {
#ifdef _WIN32
        detail_utf8 = "VO-VE Trash requires a strong local NTFS identity";
#else
        detail_utf8 = "VO-VE Trash requires a typed POSIX source identity";
#endif
        return false;
    }
    const auto trash_mode =
        request.mode == RenameMode::trash_internal || request.mode == RenameMode::trash_restore;
    if (trash_mode && !request.source_parent_identity_utf8.empty() &&
        (!valid_utf8(request.source_parent_identity_utf8) ||
         request.source_parent_identity_utf8.find('\0') != std::string::npos ||
         !request.source_parent_identity_utf8.starts_with("linux-smb:"))) {
        detail_utf8 = "rename request storage identity is inconsistent with its mode";
        return false;
    }
    if (transfer_rename_mode(request.mode) &&
        (stable_object_identity(request.source_parent_identity_utf8) !=
             request.source_parent_identity_utf8 ||
         stable_object_identity(request.destination_parent_identity_utf8) !=
             request.destination_parent_identity_utf8 ||
         request.source_parent_identity_utf8.starts_with("win-file:") ||
         request.destination_parent_identity_utf8.starts_with("win-file:") ||
         request.source.filename().u8string().find(u8':') != std::u8string::npos ||
         request.destination.filename().u8string().find(u8':') != std::u8string::npos ||
         request.expected_source.source_revision_utf8.starts_with("win-file:"))) {
        detail_utf8 = "file transfer rename requires typed source and parent identities";
        return false;
    }
    if (stable_object_identity(request.expected_source.source_revision_utf8).empty()) {
        detail_utf8 = "rename request has no stable source identity";
        return false;
    }
    return true;
}

bool valid_delete_request(const DeleteRequest &request, std::string &detail_utf8) {
    if (request.mode != DeleteMode::permanent_remote && request.mode != DeleteMode::trash_purge &&
        !transfer_delete_mode(request.mode)) {
        detail_utf8 = "delete mode is invalid";
        return false;
    }
    if (request.object_kind == OperationObjectKind::directory &&
        request.mode != DeleteMode::trash_purge) {
        detail_utf8 = "directory deletion is restricted to VO-VE Trash purge";
        return false;
    }
    if (request.operation_id == 0 || request.source.empty() ||
        request.expected_source.source_revision_utf8.empty()) {
        detail_utf8 = "delete request is incomplete";
        return false;
    }
    if (!request.source.is_absolute()) {
        detail_utf8 = "delete source path must be absolute";
        return false;
    }
    if (!valid_utf8(request.source.u8string())) {
        detail_utf8 = "delete source path must contain valid UTF-8";
        return false;
    }
    if (transfer_delete_mode(request.mode)) {
        const auto temp_cleanup = request.mode == DeleteMode::transfer_temp_cleanup;
        const auto overwrite_cleanup = request.mode == DeleteMode::transfer_overwrite_cleanup;
        const auto valid_shape =
            temp_cleanup ? transfer_destination_name(request.source)
            : overwrite_cleanup
                ? transfer_overwrite_backup_name(request.source)
                : (transfer_source_name(request.source) && !request.guard_path.empty() &&
                   request.guard_path.is_absolute() &&
                   !core::is_transfer_filename(request.guard_path.filename().u8string()));
        if (!valid_shape ||
            stable_object_identity(request.source_parent_identity_utf8) !=
                request.source_parent_identity_utf8 ||
            request.source_parent_identity_utf8.starts_with("win-file:") ||
            request.source.filename().u8string().find(u8':') != std::u8string::npos ||
            request.expected_source.source_revision_utf8.starts_with("win-file:") ||
            (!temp_cleanup && !overwrite_cleanup &&
             (stable_object_identity(request.expected_guard.source_revision_utf8).empty() ||
              stable_object_identity(request.guard_parent_identity_utf8) !=
                  request.guard_parent_identity_utf8 ||
              request.guard_parent_identity_utf8.starts_with("win-file:") ||
              request.guard_path.filename().u8string().find(u8':') != std::u8string::npos ||
              request.expected_guard.source_revision_utf8.starts_with("win-file:") ||
              !valid_utf8(request.guard_path.u8string())))) {
            detail_utf8 = "file transfer delete has invalid ownership guards";
            return false;
        }
        if ((temp_cleanup || overwrite_cleanup) &&
            (!request.guard_path.empty() || !request.expected_guard.source_revision_utf8.empty() ||
             request.expected_guard.size_bytes != 0U ||
             request.expected_guard.modified_unix_ns != 0 ||
             !request.guard_parent_identity_utf8.empty())) {
            detail_utf8 = "transfer cleanup cannot carry a publication guard";
            return false;
        }
    }
#ifdef _WIN32
    if (!detail::valid_strong_delete_revision(request.expected_source.source_revision_utf8) ||
        stable_object_identity(request.expected_source.source_revision_utf8).empty()) {
        detail_utf8 = "delete request requires a strong local or typed SMB source identity";
        return false;
    }
    if (request.mode == DeleteMode::trash_purge &&
        (!is_owned_trash_payload_path(request.source) ||
         !request.expected_source.source_revision_utf8.starts_with("win-file128:"))) {
        detail_utf8 = "trash purge requires an owned item and a strong local NTFS identity";
        return false;
    }
#else
    if (stable_object_identity(request.expected_source.source_revision_utf8).empty()) {
        detail_utf8 = "delete request has no stable source identity";
        return false;
    }
    if (request.mode == DeleteMode::trash_purge &&
        (!is_owned_trash_payload_path(request.source) ||
         (!request.expected_source.source_revision_utf8.starts_with("posix:") &&
          !request.expected_source.source_revision_utf8.starts_with("posix2:")))) {
        detail_utf8 = "trash purge requires an owned item and a typed POSIX identity";
        return false;
    }
#endif
    if (request.mode == DeleteMode::trash_purge && !request.source_parent_identity_utf8.empty() &&
        (!valid_utf8(request.source_parent_identity_utf8) ||
         request.source_parent_identity_utf8.find('\0') != std::string::npos ||
         !request.source_parent_identity_utf8.starts_with("linux-smb:"))) {
        detail_utf8 = "delete request storage identity is inconsistent with its mode";
        return false;
    }
    return true;
}

bool valid_create_directory_request(const CreateDirectoryRequest &request,
                                    std::string &detail_utf8) {
    if (request.mode != CreateDirectoryMode::user_visible &&
        request.mode != CreateDirectoryMode::trash_internal &&
        request.mode != CreateDirectoryMode::trash_internal_remove_empty) {
        detail_utf8 = "create-directory mode is invalid";
        return false;
    }
    if (request.operation_id == 0 || request.destination.empty() ||
        (request.mode == CreateDirectoryMode::user_visible &&
         request.destination_parent_revision_utf8.empty())) {
        detail_utf8 = "create-directory request is incomplete";
        return false;
    }
    if (!request.destination.is_absolute() || request.destination.parent_path().empty()) {
        detail_utf8 = "create-directory destination must be absolute and have a parent";
        return false;
    }
    if (!valid_utf8(request.destination.u8string()) ||
        !valid_utf8(request.destination_parent_revision_utf8)) {
        detail_utf8 = "create-directory request text must contain valid UTF-8";
        return false;
    }
    if (!valid_destination_filename(request.destination.filename(), detail_utf8)) {
        return false;
    }
    const auto filename = request.destination.filename().u8string();
    if (!request.destination_parent_revision_utf8.empty() &&
        stable_object_identity(request.destination_parent_revision_utf8).empty()) {
        detail_utf8 = "create-directory request has an invalid name or parent identity";
        return false;
    }
    if (request.mode == CreateDirectoryMode::user_visible) {
        if (core::is_internal_filename(filename)) {
            detail_utf8 = "create-directory request uses a reserved VO-VE name";
            return false;
        }
        return true;
    }
    std::array<char, 32> operation{};
    const auto [end, error] = std::to_chars(operation.data(), operation.data() + operation.size(),
                                            request.operation_id, 16);
    if (error != std::errc{}) {
        detail_utf8 = "trash container identifier could not be encoded";
        return false;
    }
    std::u8string expected(core::kTrashFilenamePrefix);
    expected.append(reinterpret_cast<const char8_t *>(operation.data()),
                    reinterpret_cast<const char8_t *>(end));
    if (filename != expected) {
        detail_utf8 = "trash container name does not match its operation";
        return false;
    }
    return true;
}

DeleteTargetClassification inspect_delete_target(const std::filesystem::path &path) noexcept {
#ifdef _WIN32
    const auto handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return {.failure_status = delete_target_error_status(code), .platform_code = code};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const auto code = GetLastError();
        static_cast<void>(CloseHandle(handle));
        return {.failure_status = OperationStatus::unsupported, .platform_code = code};
    }
    FILE_REMOTE_PROTOCOL_INFO remote{};
    const auto remote_result =
        GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote));
    const auto code = remote_result == FALSE ? GetLastError() : ERROR_SUCCESS;
    static_cast<void>(CloseHandle(handle));
    if (remote_result != FALSE && platform::windows_detail::is_smb_remote_protocol(remote)) {
        return {.kind = DeleteTargetKind::remote};
    }
    if (remote_result != FALSE) {
        std::array<wchar_t, MAX_PATH + 1> volume_path{};
        if (GetVolumePathNameW(path.c_str(), volume_path.data(),
                               static_cast<DWORD>(volume_path.size())) != FALSE &&
            GetDriveTypeW(volume_path.data()) == DRIVE_FIXED) {
            return {.kind = DeleteTargetKind::local};
        }
        return {.failure_status = OperationStatus::unsupported};
    }
    if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED) {
        return {.kind = DeleteTargetKind::local};
    }
    return {.failure_status = delete_target_error_status(code), .platform_code = code};
#elif defined(__linux__)
    auto flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        const auto code = errno;
        return {.failure_status = delete_target_error_status(static_cast<std::uint32_t>(code)),
                .platform_code = static_cast<std::uint32_t>(code)};
    }
    struct statfs filesystem{};
    const auto inspected = ::fstatfs(descriptor, &filesystem) == 0;
    const auto code = inspected ? 0 : errno;
    static_cast<void>(::close(descriptor));
    if (!inspected) {
        return {.failure_status = delete_target_error_status(static_cast<std::uint32_t>(code)),
                .platform_code = static_cast<std::uint32_t>(code)};
    }
    constexpr auto cifs_magic = static_cast<decltype(filesystem.f_type)>(0xFF534D42UL);
    constexpr auto smb2_magic = static_cast<decltype(filesystem.f_type)>(0xFE534D42UL);
    return {.kind = filesystem.f_type == cifs_magic || filesystem.f_type == smb2_magic
                        ? DeleteTargetKind::remote
                        : DeleteTargetKind::local};
#else
    static_cast<void>(path);
    return {.failure_status = OperationStatus::unsupported};
#endif
}

DeleteTargetKind classify_delete_target(const std::filesystem::path &path) noexcept {
    return inspect_delete_target(path).kind;
}

namespace detail {

OperationResult merge_reconciliation(OperationResult attempted, OperationResult observed) {
    const auto evidence_is_already_conflicting =
        attempted.evidence == OperationEvidence::conflicting ||
        observed.evidence == OperationEvidence::conflicting;
    const auto evidence_conflicts = (attempted.evidence == OperationEvidence::committed &&
                                     observed.evidence == OperationEvidence::no_commit) ||
                                    (attempted.evidence == OperationEvidence::no_commit &&
                                     observed.evidence == OperationEvidence::committed);
    if (evidence_is_already_conflicting || evidence_conflicts) {
        attempted.status = OperationStatus::unknown_outcome;
        attempted.evidence = OperationEvidence::conflicting;
        attempted.platform_code = observed.platform_code;
        attempted.source_present = observed.source_present;
        attempted.source_matches_expected = observed.source_matches_expected;
        attempted.destination_present = observed.destination_present;
        attempted.destination_matches_source = observed.destination_matches_source;
        attempted.confirmed_snapshot = {};
        attempted.detail_utf8 =
            "operation result conflicts with read-only reconciliation; recovery is required";
        return attempted;
    }

    if (attempted.status == OperationStatus::unknown_outcome &&
        attempted.evidence == OperationEvidence::committed && attempted.destination_present &&
        attempted.destination_matches_source && observed.status == OperationStatus::success &&
        observed.evidence == OperationEvidence::committed) {
        attempted.source_present = observed.source_present;
        attempted.source_matches_expected = observed.source_matches_expected;
        attempted.destination_present = observed.destination_present;
        attempted.destination_matches_source = observed.destination_matches_source;
        attempted.confirmed_snapshot = observed.confirmed_snapshot;
        return attempted;
    }

    if (observed.status == OperationStatus::success ||
        observed.status == OperationStatus::conflict ||
        observed.status == OperationStatus::source_changed) {
        if (observed.status != OperationStatus::success && !attempted.detail_utf8.empty()) {
            observed.detail_utf8 += "; operation attempt: " + attempted.detail_utf8;
            if (observed.platform_code == 0) {
                observed.platform_code = attempted.platform_code;
            }
        }
        return observed;
    }

    attempted.source_present = observed.source_present;
    attempted.source_matches_expected = observed.source_matches_expected;
    attempted.destination_present = observed.destination_present;
    attempted.destination_matches_source = observed.destination_matches_source;
    if (attempted.evidence == OperationEvidence::none) {
        attempted.evidence = observed.evidence;
    }
    return attempted;
}

} // namespace detail

} // namespace vove::fileops
