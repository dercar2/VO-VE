#include "vove/fileops/durable_journal.hpp"

#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS) && !defined(_WIN32)
#include "durable_journal_test_hooks.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace vove::fileops {
namespace {

#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS) && !defined(_WIN32)
std::atomic<detail::MatchedRemoveBeforeClaimHook> matched_remove_before_claim_hook{};
std::atomic<detail::MatchedRemoveCrashHook> matched_remove_crash_hook{};
#endif

constexpr std::array<std::byte, 8> kEnvelopeMagic{std::byte{0x56}, std::byte{0x4f}, std::byte{0x56},
                                                  std::byte{0x45}, std::byte{0x4a}, std::byte{0x52},
                                                  std::byte{0x4e}, std::byte{0x4c}};
constexpr std::size_t kEnvelopeHeaderBytes = 20U;
static_assert(kAbsoluteMaximumDurableJournalPayloadBytes <=
              std::numeric_limits<std::size_t>::max() - kEnvelopeHeaderBytes);

std::size_t maximum_envelope_bytes(const std::size_t maximum_payload_bytes) noexcept {
    return kEnvelopeHeaderBytes + maximum_payload_bytes;
}

enum class FileReadStatus : std::uint8_t {
    success,
    missing,
    too_large,
    short_read,
    io_error,
};

struct FileReadResult {
    FileReadStatus status{FileReadStatus::io_error};
    std::vector<std::byte> bytes;
    std::error_code error;
};

enum class InspectionStatus : std::uint8_t {
    valid,
    missing,
    corrupt,
    io_error,
};

struct InspectionResult {
    InspectionStatus status{InspectionStatus::io_error};
    std::vector<std::byte> payload;
    std::error_code error;
};

struct RemovePathResult {
    bool removed{};
    std::error_code error;
};

enum class MatchedRemoveStatus : std::uint8_t {
    removed,
    missing,
    mismatch,
    io_error,
};

struct MatchedRemoveResult {
    MatchedRemoveStatus status{MatchedRemoveStatus::io_error};
    std::error_code error;
};

std::uint32_t read_u32_le(const std::span<const std::byte> bytes,
                          const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u32_le(const std::span<std::byte> bytes, const std::size_t offset,
                  const std::uint32_t value) noexcept {
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
    bytes[offset + 1U] = std::byte{static_cast<std::uint8_t>(value >> 8U)};
    bytes[offset + 2U] = std::byte{static_cast<std::uint8_t>(value >> 16U)};
    bytes[offset + 3U] = std::byte{static_cast<std::uint8_t>(value >> 24U)};
}

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::vector<std::byte> make_envelope(const std::span<const std::byte> payload) {
    std::vector<std::byte> envelope(kEnvelopeHeaderBytes + payload.size());
    std::copy(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), envelope.begin());
    const auto bytes = std::span<std::byte>(envelope);
    write_u32_le(bytes, 8U, kDurableJournalSchemaVersion);
    write_u32_le(bytes, 12U, static_cast<std::uint32_t>(payload.size()));
    write_u32_le(bytes, 16U, crc32(payload));
    std::copy(payload.begin(), payload.end(), envelope.begin() + kEnvelopeHeaderBytes);
    return envelope;
}

#ifdef _WIN32

std::error_code windows_error(const DWORD value = GetLastError()) {
    return {static_cast<int>(value), std::system_category()};
}

bool is_missing_error(const DWORD value) noexcept {
    return value == ERROR_FILE_NOT_FOUND || value == ERROR_PATH_NOT_FOUND;
}

FileReadResult read_file_bytes(const std::filesystem::path &path, const std::size_t maximum_bytes) {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (is_missing_error(error)) {
            return {.status = FileReadStatus::missing, .bytes = {}, .error = {}};
        }
        return {.status = FileReadStatus::io_error, .bytes = {}, .error = windows_error(error)};
    }

    LARGE_INTEGER file_size{};
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileType(file) != FILE_TYPE_DISK ||
        GetFileInformationByHandle(file, &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.nNumberOfLinks != 1 || !GetFileSizeEx(file, &file_size)) {
        const auto error = windows_error();
        CloseHandle(file);
        return {.status = FileReadStatus::io_error, .bytes = {}, .error = error};
    }
    if (file_size.QuadPart < 0 || static_cast<std::uint64_t>(file_size.QuadPart) > maximum_bytes) {
        CloseHandle(file);
        return {.status = FileReadStatus::too_large, .bytes = {}, .error = {}};
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD received = 0;
        if (!ReadFile(file, bytes.data() + offset, requested, &received, nullptr)) {
            const auto error = windows_error();
            CloseHandle(file);
            return {.status = FileReadStatus::io_error, .bytes = {}, .error = error};
        }
        if (received == 0U) {
            CloseHandle(file);
            return {.status = FileReadStatus::short_read, .bytes = {}, .error = {}};
        }
        offset += received;
    }
    CloseHandle(file);
    return {.status = FileReadStatus::success, .bytes = std::move(bytes), .error = {}};
}

std::error_code write_file_durably(const std::filesystem::path &path,
                                   const std::span<const std::byte> bytes) {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return windows_error();
    }

    if (GetFileType(file) != FILE_TYPE_DISK) {
        CloseHandle(file);
        return std::make_error_code(std::errc::operation_not_supported);
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file, &information) == FALSE) {
        const auto error = windows_error();
        CloseHandle(file);
        return error;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.nNumberOfLinks != 1) {
        CloseHandle(file);
        return std::make_error_code(std::errc::operation_not_supported);
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, requested, &written, nullptr)) {
            const auto error = windows_error();
            CloseHandle(file);
            return error;
        }
        if (written == 0U) {
            CloseHandle(file);
            return std::make_error_code(std::errc::io_error);
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const auto error = windows_error();
        CloseHandle(file);
        return error;
    }
    if (!CloseHandle(file)) {
        return windows_error();
    }
    return {};
}

std::error_code replace_path_durably(const std::filesystem::path &source,
                                     const std::filesystem::path &destination) {
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return windows_error();
    }
    return {};
}

std::error_code sync_parent_directory(const std::filesystem::path &) {
    return {};
}

MatchedRemoveResult remove_path_if_bytes_match(const std::filesystem::path &path,
                                               const std::span<const std::byte> expected,
                                               const std::size_t maximum_bytes) {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return is_missing_error(error)
                   ? MatchedRemoveResult{.status = MatchedRemoveStatus::missing, .error = {}}
                   : MatchedRemoveResult{.status = MatchedRemoveStatus::io_error,
                                         .error = windows_error(error)};
    }

    LARGE_INTEGER file_size{};
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileType(file) != FILE_TYPE_DISK ||
        GetFileInformationByHandle(file, &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.nNumberOfLinks != 1 || !GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart < 0 || static_cast<std::uint64_t>(file_size.QuadPart) > maximum_bytes) {
        const auto error = GetLastError();
        CloseHandle(file);
        return {.status = MatchedRemoveStatus::io_error,
                .error = error == ERROR_SUCCESS
                             ? std::make_error_code(std::errc::operation_not_supported)
                             : windows_error(error)};
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size.QuadPart));
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD received{};
        if (!ReadFile(file, bytes.data() + offset, requested, &received, nullptr) ||
            received == 0) {
            const auto error = GetLastError();
            CloseHandle(file);
            return {.status = MatchedRemoveStatus::io_error,
                    .error = error == ERROR_SUCCESS ? std::make_error_code(std::errc::io_error)
                                                    : windows_error(error)};
        }
        offset += received;
    }
    if (bytes.size() != expected.size() ||
        !std::equal(bytes.begin(), bytes.end(), expected.begin())) {
        CloseHandle(file);
        return {.status = MatchedRemoveStatus::mismatch, .error = {}};
    }

    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    if (SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) ==
        FALSE) {
        const auto error = windows_error();
        CloseHandle(file);
        return {.status = MatchedRemoveStatus::io_error, .error = error};
    }
    if (CloseHandle(file) == FALSE) {
        return {.status = MatchedRemoveStatus::io_error, .error = windows_error()};
    }
    const auto replacement = GetFileAttributesW(path.c_str());
    if (replacement != INVALID_FILE_ATTRIBUTES || !is_missing_error(GetLastError())) {
        return {.status = MatchedRemoveStatus::mismatch, .error = {}};
    }
    return {.status = MatchedRemoveStatus::removed, .error = {}};
}

RemovePathResult remove_path(const std::filesystem::path &path) {
    if (DeleteFileW(path.c_str())) {
        return {.removed = true, .error = {}};
    }
    const auto error = GetLastError();
    if (is_missing_error(error)) {
        return {.removed = false, .error = {}};
    }
    return {.removed = false, .error = windows_error(error)};
}

#else

std::error_code posix_error(const int value = errno) {
    return {value, std::generic_category()};
}

FileReadResult read_file_bytes(const std::filesystem::path &path, const std::size_t maximum_bytes) {
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int file = open(path.c_str(), flags);
    if (file < 0) {
        if (errno == ENOENT) {
            return {.status = FileReadStatus::missing, .bytes = {}, .error = {}};
        }
        return {.status = FileReadStatus::io_error, .bytes = {}, .error = posix_error()};
    }

    struct stat metadata{};
    if (fstat(file, &metadata) != 0) {
        const auto error = posix_error();
        close(file);
        return {.status = FileReadStatus::io_error, .bytes = {}, .error = error};
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > maximum_bytes) {
        close(file);
        return {.status = FileReadStatus::too_large, .bytes = {}, .error = {}};
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(metadata.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received = ::read(file, bytes.data() + offset, bytes.size() - offset);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto error = posix_error();
            close(file);
            return {.status = FileReadStatus::io_error, .bytes = {}, .error = error};
        }
        if (received == 0) {
            close(file);
            return {.status = FileReadStatus::short_read, .bytes = {}, .error = {}};
        }
        offset += static_cast<std::size_t>(received);
    }
    close(file);
    return {.status = FileReadStatus::success, .bytes = std::move(bytes), .error = {}};
}

std::error_code write_file_durably(const std::filesystem::path &path,
                                   const std::span<const std::byte> bytes) {
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int file = open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (file < 0) {
        return posix_error();
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto error = posix_error();
            close(file);
            return error;
        }
        if (written == 0) {
            close(file);
            return std::make_error_code(std::errc::io_error);
        }
        offset += static_cast<std::size_t>(written);
    }
    while (fsync(file) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const auto error = posix_error();
        close(file);
        return error;
    }
    if (close(file) != 0) {
        return posix_error();
    }
    return {};
}

std::error_code replace_path_durably(const std::filesystem::path &source,
                                     const std::filesystem::path &destination) {
    if (rename(source.c_str(), destination.c_str()) != 0) {
        return posix_error();
    }
    return {};
}

std::error_code sync_parent_directory(const std::filesystem::path &path) {
    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int directory = open(parent.c_str(), flags);
    if (directory < 0) {
        return posix_error();
    }
    while (fsync(directory) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const auto error = posix_error();
        close(directory);
        return error;
    }
    if (close(directory) != 0) {
        return posix_error();
    }
    return {};
}

RemovePathResult remove_path(const std::filesystem::path &path) {
    if (unlink(path.c_str()) == 0) {
        return {.removed = true, .error = {}};
    }
    if (errno == ENOENT) {
        return {.removed = false, .error = {}};
    }
    return {.removed = false, .error = posix_error()};
}

std::filesystem::path removal_staging_path(const std::filesystem::path &path) {
    return std::filesystem::path(path).concat(".vove-removing");
}

std::filesystem::path removal_claim_path(const std::filesystem::path &path) {
    return removal_staging_path(path) / "candidate";
}

RemovePathResult remove_empty_removal_staging(const std::filesystem::path &path) {
    if (rmdir(removal_staging_path(path).c_str()) == 0) {
        return {.removed = true, .error = {}};
    }
    if (errno == ENOENT) {
        return {.removed = false, .error = {}};
    }
    return {.removed = false, .error = posix_error()};
}

MatchedRemoveResult remove_path_if_bytes_match(const std::filesystem::path &path,
                                               const std::span<const std::byte> expected,
                                               const std::size_t maximum_bytes) {
#if defined(__linux__) && defined(SYS_renameat2)
    const auto staging_path = removal_staging_path(path);
    const auto claimed_path = removal_claim_path(path);
    bool already_claimed{};
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int file = open(path.c_str(), flags);
    if (file < 0 && errno == ENOENT) {
        file = open(claimed_path.c_str(), flags);
        already_claimed = file >= 0;
    }
    if (file < 0) {
        return errno == ENOENT
                   ? MatchedRemoveResult{.status = MatchedRemoveStatus::missing, .error = {}}
                   : MatchedRemoveResult{.status = MatchedRemoveStatus::io_error,
                                         .error = posix_error()};
    }
    struct stat opened{};
    if (fstat(file, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
        opened.st_size < 0 || static_cast<std::uint64_t>(opened.st_size) > maximum_bytes) {
        const auto error =
            errno == 0 ? std::make_error_code(std::errc::operation_not_supported) : posix_error();
        close(file);
        return {.status = MatchedRemoveStatus::io_error, .error = error};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(opened.st_size));
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto received = ::read(file, bytes.data() + offset, bytes.size() - offset);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            const auto error =
                received < 0 ? posix_error() : std::make_error_code(std::errc::io_error);
            close(file);
            return {.status = MatchedRemoveStatus::io_error, .error = error};
        }
        offset += static_cast<std::size_t>(received);
    }
    if (bytes.size() != expected.size() ||
        !std::equal(bytes.begin(), bytes.end(), expected.begin())) {
        close(file);
        return {.status = MatchedRemoveStatus::mismatch, .error = {}};
    }

    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    const auto filename = path.filename().native();
    if (filename.empty() || filename == "." || filename == "..") {
        close(file);
        return {.status = MatchedRemoveStatus::io_error,
                .error = std::make_error_code(std::errc::invalid_argument)};
    }

    int directory_flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY;
#ifdef O_NOFOLLOW
    directory_flags |= O_NOFOLLOW;
#endif
    const int parent_fd = open(parent.c_str(), directory_flags);
    if (parent_fd < 0) {
        const auto error = posix_error();
        close(file);
        return {.status = MatchedRemoveStatus::io_error, .error = error};
    }

    const auto staging_name = staging_path.filename().native();
    if (!already_claimed && mkdirat(parent_fd, staging_name.c_str(), S_IRWXU) != 0) {
        if (errno != EEXIST || unlinkat(parent_fd, staging_name.c_str(), AT_REMOVEDIR) != 0 ||
            mkdirat(parent_fd, staging_name.c_str(), S_IRWXU) != 0) {
            const auto error = posix_error();
            close(parent_fd);
            close(file);
            return {.status = MatchedRemoveStatus::io_error, .error = error};
        }
    }
    const int staging_fd = openat(parent_fd, staging_name.c_str(), directory_flags);
    if (staging_fd < 0) {
        const auto error = posix_error();
        if (!already_claimed) {
            static_cast<void>(unlinkat(parent_fd, staging_name.c_str(), AT_REMOVEDIR));
        }
        close(parent_fd);
        close(file);
        return {.status = MatchedRemoveStatus::io_error, .error = error};
    }

    struct stat staging_metadata{};
    if (fstat(staging_fd, &staging_metadata) != 0 || !S_ISDIR(staging_metadata.st_mode) ||
        staging_metadata.st_uid != geteuid() || (staging_metadata.st_mode & 0077) != 0) {
        const auto error =
            errno == 0 ? std::make_error_code(std::errc::permission_denied) : posix_error();
        close(staging_fd);
        close(parent_fd);
        close(file);
        return {.status = MatchedRemoveStatus::io_error, .error = error};
    }

    const auto sync_fd = [](const int descriptor) -> std::error_code {
        while (fsync(descriptor) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return posix_error();
        }
        return {};
    };

    if (!already_claimed) {
        if (const auto error = sync_fd(parent_fd); error) {
            close(staging_fd);
            close(parent_fd);
            close(file);
            return {.status = MatchedRemoveStatus::io_error, .error = error};
        }
#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS)
        if (const auto hook = matched_remove_crash_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            hook(path, detail::MatchedRemoveCrashPoint::staging_durable);
        }
#endif

#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS)
        if (const auto hook = matched_remove_before_claim_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            hook(path);
        }
#endif

        constexpr auto candidate_name = "candidate";
        if (syscall(SYS_renameat2, parent_fd, filename.c_str(), staging_fd, candidate_name,
                    RENAME_NOREPLACE) != 0) {
            const auto rename_error = errno;
            close(staging_fd);
            static_cast<void>(unlinkat(parent_fd, staging_name.c_str(), AT_REMOVEDIR));
            close(parent_fd);
            close(file);
            return rename_error == ENOENT
                       ? MatchedRemoveResult{.status = MatchedRemoveStatus::missing, .error = {}}
                       : MatchedRemoveResult{
                             .status = MatchedRemoveStatus::io_error,
                             .error = std::error_code(rename_error, std::generic_category())};
        }
    }

    constexpr auto candidate_name = "candidate";
    struct stat claimed{};
    const auto claim_stat_ok =
        fstatat(staging_fd, candidate_name, &claimed, AT_SYMLINK_NOFOLLOW) == 0;
    const auto claim_stat_error = claim_stat_ok ? 0 : errno;
    const auto claim_matches = claim_stat_ok && S_ISREG(claimed.st_mode) && claimed.st_nlink == 1 &&
                               claimed.st_dev == opened.st_dev && claimed.st_ino == opened.st_ino;
    if (!claim_matches) {
        const auto restored =
            !already_claimed && syscall(SYS_renameat2, staging_fd, candidate_name, parent_fd,
                                        filename.c_str(), RENAME_NOREPLACE) == 0;
        const auto restore_error = restored ? 0 : errno;
        std::error_code cleanup_error;
        if (restored) {
            cleanup_error = sync_fd(staging_fd);
            if (const auto error = sync_fd(parent_fd); !cleanup_error && error) {
                cleanup_error = error;
            }
        }
        if (close(staging_fd) != 0 && !cleanup_error) {
            cleanup_error = posix_error();
        }
        if (restored && unlinkat(parent_fd, staging_name.c_str(), AT_REMOVEDIR) != 0 &&
            !cleanup_error) {
            cleanup_error = posix_error();
        }
        if (restored) {
            if (const auto error = sync_fd(parent_fd); !cleanup_error && error) {
                cleanup_error = error;
            }
        }
        if (close(parent_fd) != 0 && !cleanup_error) {
            cleanup_error = posix_error();
        }
        if (close(file) != 0 && !cleanup_error) {
            cleanup_error = posix_error();
        }
        if (!restored && !already_claimed) {
            return {.status = MatchedRemoveStatus::io_error,
                    .error = std::error_code(restore_error, std::generic_category())};
        }
        if (cleanup_error) {
            return {.status = MatchedRemoveStatus::io_error, .error = cleanup_error};
        }
        if (!claim_stat_ok) {
            return {.status = MatchedRemoveStatus::io_error,
                    .error = std::error_code(claim_stat_error, std::generic_category())};
        }
        return {.status = MatchedRemoveStatus::mismatch, .error = {}};
    }

    if (!already_claimed) {
        if (const auto error = sync_fd(staging_fd); error) {
            close(staging_fd);
            close(parent_fd);
            close(file);
            return {.status = MatchedRemoveStatus::io_error, .error = error};
        }
        if (const auto error = sync_fd(parent_fd); error) {
            close(staging_fd);
            close(parent_fd);
            close(file);
            return {.status = MatchedRemoveStatus::io_error, .error = error};
        }
#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS)
        if (const auto hook = matched_remove_crash_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            hook(path, detail::MatchedRemoveCrashPoint::claim_durable);
        }
#endif
    }

    if (unlinkat(staging_fd, candidate_name, 0) != 0) {
        const auto error = posix_error();
        static_cast<void>(syscall(SYS_renameat2, staging_fd, candidate_name, parent_fd,
                                  filename.c_str(), RENAME_NOREPLACE));
        close(staging_fd);
        static_cast<void>(unlinkat(parent_fd, staging_name.c_str(), AT_REMOVEDIR));
        close(parent_fd);
        close(file);
        return {.status = MatchedRemoveStatus::io_error, .error = error};
    }
    auto cleanup_error = sync_fd(staging_fd);
#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS)
    if (!cleanup_error) {
        if (const auto hook = matched_remove_crash_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            hook(path, detail::MatchedRemoveCrashPoint::unlink_durable);
        }
    }
#endif
    if (close(staging_fd) != 0 && !cleanup_error) {
        cleanup_error = posix_error();
    }
    if (unlinkat(parent_fd, staging_name.c_str(), AT_REMOVEDIR) != 0 && !cleanup_error) {
        cleanup_error = posix_error();
    }
    if (fsync(parent_fd) != 0 && !cleanup_error) {
        cleanup_error = posix_error();
    }
    if (close(parent_fd) != 0 && !cleanup_error) {
        cleanup_error = posix_error();
    }
    if (close(file) != 0 && !cleanup_error) {
        cleanup_error = posix_error();
    }
    if (cleanup_error) {
        return {.status = MatchedRemoveStatus::io_error, .error = cleanup_error};
    }
    return {.status = MatchedRemoveStatus::removed, .error = {}};
#else
    static_cast<void>(path);
    static_cast<void>(expected);
    static_cast<void>(maximum_bytes);
    return {.status = MatchedRemoveStatus::io_error,
            .error = std::make_error_code(std::errc::operation_not_supported)};
#endif
}

#endif

InspectionResult inspect(const std::filesystem::path &path,
                         const std::size_t maximum_payload_bytes) {
    auto file = read_file_bytes(path, maximum_envelope_bytes(maximum_payload_bytes));
    switch (file.status) {
    case FileReadStatus::missing:
        return {.status = InspectionStatus::missing, .payload = {}, .error = {}};
    case FileReadStatus::too_large:
    case FileReadStatus::short_read:
        return {.status = InspectionStatus::corrupt, .payload = {}, .error = {}};
    case FileReadStatus::io_error:
        return {.status = InspectionStatus::io_error, .payload = {}, .error = file.error};
    case FileReadStatus::success:
        break;
    }

    const auto bytes = std::span<const std::byte>(file.bytes);
    if (bytes.size() < kEnvelopeHeaderBytes ||
        !std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), bytes.begin())) {
        return {.status = InspectionStatus::corrupt, .payload = {}, .error = {}};
    }
    if (read_u32_le(bytes, 8U) != kDurableJournalSchemaVersion) {
        return {.status = InspectionStatus::corrupt, .payload = {}, .error = {}};
    }
    const auto payload_size = static_cast<std::size_t>(read_u32_le(bytes, 12U));
    if (payload_size > maximum_payload_bytes ||
        bytes.size() != kEnvelopeHeaderBytes + payload_size) {
        return {.status = InspectionStatus::corrupt, .payload = {}, .error = {}};
    }
    const auto payload = bytes.subspan(kEnvelopeHeaderBytes, payload_size);
    if (crc32(payload) != read_u32_le(bytes, 16U)) {
        return {.status = InspectionStatus::corrupt, .payload = {}, .error = {}};
    }
    return {.status = InspectionStatus::valid,
            .payload = std::vector<std::byte>(payload.begin(), payload.end()),
            .error = {}};
}

DurableJournalReadResult read_generation(const std::filesystem::path &path,
                                         const DurableJournalReadSource source,
                                         const std::size_t maximum_payload_bytes) {
    auto inspected = inspect(path, maximum_payload_bytes);
#ifndef _WIN32
    if (inspected.status == InspectionStatus::missing) {
        inspected = inspect(removal_claim_path(path), maximum_payload_bytes);
    }
#endif
    DurableJournalReadResult result;
    switch (inspected.status) {
    case InspectionStatus::valid:
        result.status = DurableJournalStatus::success;
        result.source = source;
        result.payload = std::move(inspected.payload);
        break;
    case InspectionStatus::missing:
        result.status = DurableJournalStatus::not_found;
        break;
    case InspectionStatus::corrupt:
        result.status = DurableJournalStatus::corrupt;
        break;
    case InspectionStatus::io_error:
        result.status = DurableJournalStatus::io_error;
        result.error = inspected.error;
        break;
    }
    return result;
}

DurableJournalResult io_failure(const std::error_code &error) {
    return {.status = DurableJournalStatus::io_error, .error = error};
}

void discard_temporary(const std::filesystem::path &path) {
    static_cast<void>(remove_path(path));
}

} // namespace

DurableJournalStore::DurableJournalStore(std::filesystem::path primary_path,
                                         const std::size_t maximum_payload_bytes)
    : primary_path_(std::move(primary_path)), previous_path_(primary_path_),
      temporary_path_(primary_path_), maximum_payload_bytes_(maximum_payload_bytes) {
    if (maximum_payload_bytes_ > kAbsoluteMaximumDurableJournalPayloadBytes) {
        throw std::invalid_argument("durable journal payload limit exceeds 64 MiB");
    }
    previous_path_ += ".previous";
    temporary_path_ += ".temporary";
}

DurableJournalResult DurableJournalStore::write(const std::span<const std::byte> payload) const {
    if (payload.size() > maximum_payload_bytes_) {
        return {.status = DurableJournalStatus::payload_too_large, .error = {}};
    }

    const auto current = inspect(primary_path_, maximum_payload_bytes_);
    if (current.status == InspectionStatus::io_error) {
        return io_failure(current.error);
    }
#ifndef _WIN32
    for (const auto *path : {&primary_path_, &previous_path_}) {
        if (const auto claim = inspect(removal_claim_path(*path), maximum_payload_bytes_);
            claim.status != InspectionStatus::missing) {
            return io_failure(claim.status == InspectionStatus::io_error
                                  ? claim.error
                                  : std::make_error_code(std::errc::device_or_resource_busy));
        }
    }
#endif

    const auto envelope = make_envelope(payload);
    discard_temporary(temporary_path_);
    if (const auto error = write_file_durably(temporary_path_, envelope); error) {
        discard_temporary(temporary_path_);
        return io_failure(error);
    }

    if (current.status == InspectionStatus::valid) {
        if (const auto error = replace_path_durably(primary_path_, previous_path_); error) {
            discard_temporary(temporary_path_);
            return io_failure(error);
        }
        if (const auto error = sync_parent_directory(primary_path_); error) {
            discard_temporary(temporary_path_);
            return io_failure(error);
        }
    }

    if (const auto error = replace_path_durably(temporary_path_, primary_path_); error) {
        discard_temporary(temporary_path_);
        return io_failure(error);
    }
    if (const auto error = sync_parent_directory(primary_path_); error) {
        return io_failure(error);
    }
    return {.status = DurableJournalStatus::success, .error = {}};
}

DurableJournalReadResult DurableJournalStore::read() const {
    const auto primary = inspect(primary_path_, maximum_payload_bytes_);
    if (primary.status == InspectionStatus::valid) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::success;
        result.source = DurableJournalReadSource::primary;
        result.payload = primary.payload;
        return result;
    }
#ifndef _WIN32
    const auto primary_claim =
        primary.status == InspectionStatus::missing
            ? inspect(removal_claim_path(primary_path_), maximum_payload_bytes_)
            : InspectionResult{.status = InspectionStatus::missing, .payload = {}, .error = {}};
    if (primary_claim.status == InspectionStatus::valid) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::success;
        result.source = DurableJournalReadSource::primary;
        result.payload = primary_claim.payload;
        return result;
    }
#endif

    const auto previous = inspect(previous_path_, maximum_payload_bytes_);
    if (previous.status == InspectionStatus::valid) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::success;
        result.source = DurableJournalReadSource::previous;
        result.payload = previous.payload;
        return result;
    }
#ifndef _WIN32
    const auto previous_claim =
        previous.status == InspectionStatus::missing
            ? inspect(removal_claim_path(previous_path_), maximum_payload_bytes_)
            : InspectionResult{.status = InspectionStatus::missing, .payload = {}, .error = {}};
    if (previous_claim.status == InspectionStatus::valid) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::success;
        result.source = DurableJournalReadSource::previous;
        result.payload = previous_claim.payload;
        return result;
    }
#endif
    if (primary.status == InspectionStatus::io_error) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::io_error;
        result.error = primary.error;
        return result;
    }
    if (previous.status == InspectionStatus::io_error) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::io_error;
        result.error = previous.error;
        return result;
    }
#ifndef _WIN32
    if (primary_claim.status == InspectionStatus::io_error ||
        previous_claim.status == InspectionStatus::io_error) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::io_error;
        result.error = primary_claim.status == InspectionStatus::io_error ? primary_claim.error
                                                                          : previous_claim.error;
        return result;
    }
#endif
    if (primary.status == InspectionStatus::corrupt || previous.status == InspectionStatus::corrupt
#ifndef _WIN32
        || primary_claim.status == InspectionStatus::corrupt ||
        previous_claim.status == InspectionStatus::corrupt
#endif
    ) {
        DurableJournalReadResult result;
        result.status = DurableJournalStatus::corrupt;
        return result;
    }
    DurableJournalReadResult result;
    result.status = DurableJournalStatus::not_found;
    return result;
}

DurableJournalReadResult DurableJournalStore::read_primary_generation() const {
    return read_generation(primary_path_, DurableJournalReadSource::primary,
                           maximum_payload_bytes_);
}

DurableJournalReadResult DurableJournalStore::read_previous_generation() const {
    return read_generation(previous_path_, DurableJournalReadSource::previous,
                           maximum_payload_bytes_);
}

DurableJournalResult DurableJournalStore::remove() const {
    std::error_code first_error;
    bool removed_any = false;
    for (const auto *path : {&primary_path_, &previous_path_, &temporary_path_}) {
        const auto result = remove_path(*path);
        removed_any = removed_any || result.removed;
        if (!first_error && result.error) {
            first_error = result.error;
        }
    }
#ifndef _WIN32
    for (const auto *path : {&primary_path_, &previous_path_}) {
        const auto claim = remove_path(removal_claim_path(*path));
        removed_any = removed_any || claim.removed;
        if (!first_error && claim.error) {
            first_error = claim.error;
        }
        const auto staging = remove_empty_removal_staging(*path);
        removed_any = removed_any || staging.removed;
        if (!first_error && staging.error) {
            first_error = staging.error;
        }
    }
#endif
    if (first_error) {
        return io_failure(first_error);
    }
    if (removed_any) {
        if (const auto error = sync_parent_directory(primary_path_); error) {
            return io_failure(error);
        }
    }
    return {.status = DurableJournalStatus::success, .error = {}};
}

DurableJournalResult DurableJournalStore::remove_if_payload_matches(
    const std::span<const std::byte> expected_payload) const {
    if (expected_payload.size() > maximum_payload_bytes_) {
        return {.status = DurableJournalStatus::payload_too_large, .error = {}};
    }
    const auto expected = make_envelope(expected_payload);
    bool removed_any{};
    for (const auto *path : {&primary_path_, &previous_path_}) {
        const auto result = remove_path_if_bytes_match(
            *path, expected, maximum_envelope_bytes(maximum_payload_bytes_));
        if (result.status == MatchedRemoveStatus::mismatch) {
            return {.status = DurableJournalStatus::payload_mismatch, .error = {}};
        }
        if (result.status == MatchedRemoveStatus::io_error) {
            return io_failure(result.error);
        }
        removed_any = removed_any || result.status == MatchedRemoveStatus::removed;
#ifndef _WIN32
        const auto staging = remove_empty_removal_staging(*path);
        removed_any = removed_any || staging.removed;
        if (staging.error) {
            return io_failure(staging.error);
        }
#endif
    }
    if (std::filesystem::exists(temporary_path_)) {
        return {.status = DurableJournalStatus::payload_mismatch, .error = {}};
    }
    if (removed_any) {
        if (const auto error = sync_parent_directory(primary_path_); error) {
            return io_failure(error);
        }
    }
    return {.status = DurableJournalStatus::success, .error = {}};
}

const std::filesystem::path &DurableJournalStore::primary_path() const noexcept {
    return primary_path_;
}

const std::filesystem::path &DurableJournalStore::previous_path() const noexcept {
    return previous_path_;
}

const std::filesystem::path &DurableJournalStore::temporary_path() const noexcept {
    return temporary_path_;
}

std::size_t DurableJournalStore::maximum_payload_bytes() const noexcept {
    return maximum_payload_bytes_;
}

} // namespace vove::fileops

#if defined(VOVE_DURABLE_JOURNAL_TEST_HOOKS) && !defined(_WIN32)
namespace vove::fileops::detail {

void set_matched_remove_before_claim_hook(const MatchedRemoveBeforeClaimHook hook) noexcept {
    matched_remove_before_claim_hook.store(hook, std::memory_order_release);
}

void set_matched_remove_crash_hook(const MatchedRemoveCrashHook hook) noexcept {
    matched_remove_crash_hook.store(hook, std::memory_order_release);
}

} // namespace vove::fileops::detail
#endif
