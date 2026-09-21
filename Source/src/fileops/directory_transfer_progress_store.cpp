#include "vove/fileops/directory_transfer_progress_store.hpp"

#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/current_operation_lease.hpp"
#include "vove/fileops/directory_transfer_control.hpp"
#include "vove/fileops/directory_transfer_manifest_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace vove::fileops {
namespace {

constexpr std::size_t kMaximumTornTailBytes = kDirectoryTransferProgressRecordBytes - 1U;
constexpr std::size_t kMaximumLedgerReadBytes =
    kMaximumDirectoryTransferProgressBytes + kMaximumTornTailBytes;

DirectoryTransferProgressStoreResult result(const DirectoryTransferProgressStoreStatus status,
                                            std::string detail_utf8 = {},
                                            const std::error_code error = {}) {
    return {.status = status, .error = error, .detail_utf8 = std::move(detail_utf8)};
}

bool valid_current_path(const std::filesystem::path &path) {
    return path.is_absolute() && path.lexically_normal() == path &&
           path.filename() == kCurrentOperationJournalFilename;
}

std::filesystem::path temporary_path(const std::filesystem::path &ledger_path) {
    auto path = ledger_path;
    path += ".tmp";
    return path;
}

struct NativeIdentity {
    std::uint64_t volume{};
    std::array<std::uint8_t, 16U> object{};

    friend bool operator==(const NativeIdentity &, const NativeIdentity &) = default;
};

struct FileShape {
    NativeIdentity identity;
    std::uint64_t size{};
};

#ifdef _WIN32

class NativeHandle final {
  public:
    NativeHandle() = default;
    explicit NativeHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~NativeHandle() {
        reset();
    }

    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    NativeHandle(NativeHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    NativeHandle &operator=(NativeHandle &&other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }
    void reset() noexcept {
        if (valid()) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

std::error_code native_error(const DWORD value = GetLastError()) {
    return {static_cast<int>(value), std::system_category()};
}

bool missing_error(const DWORD value) noexcept {
    return value == ERROR_FILE_NOT_FOUND || value == ERROR_PATH_NOT_FOUND;
}

std::optional<FileShape> inspect_handle(const HANDLE handle, const bool require_directory,
                                        DirectoryTransferProgressStoreResult &failure) {
    FILE_ID_INFO identity{};
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes))) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger shape could not be inspected", native_error());
        return std::nullopt;
    }
    if (standard.EndOfFile.QuadPart < 0 || standard.DeletePending ||
        standard.Directory != static_cast<BOOLEAN>(require_directory) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (!require_directory && standard.NumberOfLinks != 1U)) {
        failure = result(DirectoryTransferProgressStoreStatus::corrupt,
                         "directory-transfer ledger or parent has an unsafe filesystem shape");
        return std::nullopt;
    }
    FileShape shape;
    shape.identity.volume = identity.VolumeSerialNumber;
    std::memcpy(shape.identity.object.data(), identity.FileId.Identifier,
                shape.identity.object.size());
    shape.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    return shape;
}

NativeHandle open_parent(const std::filesystem::path &parent,
                         DirectoryTransferProgressStoreResult &failure) {
    const auto handle = CreateFileW(
        parent.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer state directory could not be pinned", native_error());
        return {};
    }
    NativeHandle pinned(handle);
    if (!inspect_handle(handle, true, failure)) {
        return {};
    }
    FILE_REMOTE_PROTOCOL_INFO remote{};
    if (GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote)) !=
        FALSE) {
        failure = result(DirectoryTransferProgressStoreStatus::unsupported_filesystem,
                         "directory-transfer ledger requires a local state directory");
        return {};
    }
    const auto remote_error = GetLastError();
    if (remote_error != ERROR_INVALID_PARAMETER && remote_error != ERROR_NOT_SUPPORTED) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer state directory locality could not be verified",
                         native_error(remote_error));
        return {};
    }
    wchar_t filesystem[32]{};
    if (!GetVolumeInformationByHandleW(handle, nullptr, 0U, nullptr, nullptr, nullptr, filesystem,
                                       static_cast<DWORD>(std::size(filesystem))) ||
        (_wcsicmp(filesystem, L"NTFS") != 0 && _wcsicmp(filesystem, L"ReFS") != 0)) {
        failure = result(DirectoryTransferProgressStoreStatus::unsupported_filesystem,
                         "directory-transfer ledger requires local NTFS or ReFS");
        return {};
    }
    return pinned;
}

NativeHandle open_path(const std::filesystem::path &path, const bool writable, const bool deletable,
                       DirectoryTransferProgressStoreResult &failure, bool &missing) {
    missing = false;
    DWORD access = FILE_READ_ATTRIBUTES | GENERIC_READ;
    if (writable) {
        access |= GENERIC_WRITE;
    }
    if (deletable) {
        access |= DELETE;
    }
    const DWORD sharing = deletable ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
                                    : FILE_SHARE_READ | FILE_SHARE_WRITE;
    const auto handle = CreateFileW(
        path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (missing_error(code)) {
            missing = true;
            return {};
        }
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger could not be opened", native_error(code));
        return {};
    }
    NativeHandle opened(handle);
    if (!inspect_handle(handle, false, failure)) {
        return {};
    }
    return opened;
}

bool verify_parent(const NativeHandle &parent, const std::filesystem::path &parent_path,
                   DirectoryTransferProgressStoreResult &failure) {
    const auto expected = inspect_handle(parent.get(), true, failure);
    if (!expected) {
        return false;
    }
    const auto observed_handle = CreateFileW(
        parent_path.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (observed_handle == INVALID_HANDLE_VALUE) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer state directory no longer has its pinned identity",
                         native_error());
        return false;
    }
    NativeHandle observed(observed_handle);
    const auto shape = inspect_handle(observed.get(), true, failure);
    if (!shape || shape->identity != expected->identity) {
        if (shape) {
            failure =
                result(DirectoryTransferProgressStoreStatus::recovery_required,
                       "directory-transfer state directory was replaced while the ledger was open");
        }
        return false;
    }
    return true;
}

struct FileVerification {
    const NativeHandle &file;
    const NativeHandle &parent;
    const std::filesystem::path &parent_path;
    const std::filesystem::path &canonical_path;
    std::uint64_t expected_size{};
};

bool verify_file(const FileVerification &check, DirectoryTransferProgressStoreResult &failure) {
    if (!verify_parent(check.parent, check.parent_path, failure)) {
        return false;
    }
    const auto expected = inspect_handle(check.file.get(), false, failure);
    if (!expected || expected->size != check.expected_size) {
        if (expected) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "directory-transfer ledger has an unexpected size");
        }
        return false;
    }
    bool missing{};
    auto observed = open_path(check.canonical_path, false, false, failure, missing);
    if (!observed.valid()) {
        if (missing) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "directory-transfer ledger disappeared from its canonical path");
        }
        return false;
    }
    const auto shape = inspect_handle(observed.get(), false, failure);
    if (!shape || shape->identity != expected->identity || shape->size != check.expected_size) {
        if (shape) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "directory-transfer ledger canonical path was replaced");
        }
        return false;
    }
    return true;
}

bool read_bytes(const NativeHandle &file, const std::uint64_t size, std::vector<std::byte> &bytes,
                DirectoryTransferProgressStoreResult &failure) {
    if (size > std::numeric_limits<std::size_t>::max()) {
        failure = result(DirectoryTransferProgressStoreStatus::payload_too_large,
                         "directory-transfer ledger cannot fit in memory");
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN)) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger seek failed", native_error());
        return false;
    }
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD received{};
        if (!ReadFile(file.get(), bytes.data() + offset, request, &received, nullptr) ||
            received == 0U) {
            failure = result(DirectoryTransferProgressStoreStatus::io_error,
                             "directory-transfer ledger read was incomplete", native_error());
            return false;
        }
        offset += received;
    }
    return true;
}

bool write_bytes(const NativeHandle &file, const std::uint64_t offset,
                 const std::span<const std::byte> bytes,
                 DirectoryTransferProgressStoreResult &failure) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer ledger append seek was ambiguous", native_error());
        return false;
    }
    std::size_t written_total{};
    while (written_total < bytes.size()) {
        DWORD written{};
        const auto request = static_cast<DWORD>(bytes.size() - written_total);
        if (!WriteFile(file.get(), bytes.data() + written_total, request, &written, nullptr) ||
            written == 0U) {
            failure =
                result(DirectoryTransferProgressStoreStatus::recovery_required,
                       "directory-transfer ledger append outcome is ambiguous", native_error());
            return false;
        }
        written_total += written;
    }
    if (!FlushFileBuffers(file.get())) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer ledger flush outcome is ambiguous", native_error());
        return false;
    }
    return true;
}

bool truncate_file(const NativeHandle &file, const std::uint64_t size,
                   DirectoryTransferProgressStoreResult &failure) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN) || !SetEndOfFile(file.get()) ||
        !FlushFileBuffers(file.get())) {
        failure =
            result(DirectoryTransferProgressStoreStatus::recovery_required,
                   "directory-transfer torn tail could not be durably removed", native_error());
        return false;
    }
    return true;
}

bool remove_safe_file(const std::filesystem::path &path,
                      DirectoryTransferProgressStoreResult &failure) {
    bool missing{};
    auto file = open_path(path, false, true, failure, missing);
    if (!file.valid()) {
        return missing;
    }
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition,
                                    sizeof(disposition))) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "stale directory-transfer ledger temporary file could not be removed",
                         native_error());
        return false;
    }
    file.reset();
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES || !missing_error(GetLastError())) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "stale directory-transfer ledger temporary path was replaced");
        return false;
    }
    return true;
}

NativeHandle publish_header(const std::filesystem::path &ledger_path,
                            const std::filesystem::path &temp_path, const NativeHandle &parent,
                            const std::filesystem::path &parent_path,
                            const std::span<const std::byte> header,
                            DirectoryTransferProgressStoreResult &failure) {
    const auto raw = CreateFileW(
        temp_path.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger header temporary file could not be created",
                         native_error());
        return {};
    }
    NativeHandle temp(raw);
    if (!write_bytes(temp, 0U, header, failure)) {
        return {};
    }
    const auto temp_shape = inspect_handle(temp.get(), false, failure);
    if (!temp_shape || temp_shape->size != header.size() ||
        !verify_parent(parent, parent_path, failure)) {
        if (temp_shape && temp_shape->size != header.size()) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "directory-transfer ledger header has the wrong durable size");
        }
        return {};
    }
    if (!MoveFileExW(temp_path.c_str(), ledger_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        failure = result(code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS
                             ? DirectoryTransferProgressStoreStatus::payload_mismatch
                             : DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer ledger header publication was not confirmed",
                         native_error(code));
        return {};
    }
    const auto observed_raw = CreateFileW(
        ledger_path.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (observed_raw == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (missing_error(code)) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "published directory-transfer ledger header disappeared");
        } else {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "published directory-transfer ledger could not be verified",
                             native_error(code));
        }
        return {};
    }
    NativeHandle observed(observed_raw);
    const auto observed_shape = inspect_handle(observed.get(), false, failure);
    if (!observed_shape || observed_shape->identity != temp_shape->identity ||
        observed_shape->size != header.size()) {
        if (observed_shape && observed_shape->identity != temp_shape->identity) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "published directory-transfer ledger identity changed");
        }
        return {};
    }
    temp.reset();
    bool missing{};
    auto canonical = open_path(ledger_path, true, false, failure, missing);
    if (!canonical.valid() || !verify_file({.file = canonical,
                                            .parent = parent,
                                            .parent_path = parent_path,
                                            .canonical_path = ledger_path,
                                            .expected_size = header.size()},
                                           failure)) {
        if (missing) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "published directory-transfer ledger disappeared before pinning");
        }
        return {};
    }
    const auto canonical_shape = inspect_handle(canonical.get(), false, failure);
    if (!canonical_shape || canonical_shape->identity != observed_shape->identity) {
        if (canonical_shape) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "published directory-transfer ledger changed before pinning");
        }
        return {};
    }
    return canonical;
}

#else

class NativeHandle final {
  public:
    NativeHandle() = default;
    explicit NativeHandle(const int descriptor) noexcept : descriptor_(descriptor) {}
    ~NativeHandle() {
        reset();
    }

    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    NativeHandle(NativeHandle &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    NativeHandle &operator=(NativeHandle &&other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return descriptor_ >= 0;
    }
    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }
    void reset() noexcept {
        if (valid()) {
            static_cast<void>(close(descriptor_));
            descriptor_ = -1;
        }
    }

  private:
    int descriptor_{-1};
};

std::error_code native_error(const int value = errno) {
    return {value, std::generic_category()};
}

NativeIdentity identity(const struct stat &information) noexcept {
    NativeIdentity value;
    value.volume = static_cast<std::uint64_t>(information.st_dev);
    const auto inode = static_cast<std::uint64_t>(information.st_ino);
    for (std::size_t index{}; index < sizeof(inode); ++index) {
        value.object[index] = static_cast<std::uint8_t>(inode >> (index * 8U));
    }
    return value;
}

std::optional<FileShape> inspect_descriptor(const int descriptor, const bool require_directory,
                                            DirectoryTransferProgressStoreResult &failure) {
    struct stat information{};
    if (fstat(descriptor, &information) != 0) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger shape could not be inspected", native_error());
        return std::nullopt;
    }
    if ((require_directory && !S_ISDIR(information.st_mode)) ||
        (!require_directory && (!S_ISREG(information.st_mode) || information.st_nlink != 1)) ||
        information.st_size < 0) {
        failure = result(DirectoryTransferProgressStoreStatus::corrupt,
                         "directory-transfer ledger or parent has an unsafe filesystem shape");
        return std::nullopt;
    }
    return FileShape{.identity = identity(information),
                     .size = static_cast<std::uint64_t>(information.st_size)};
}

bool supported_filesystem(const int descriptor) {
    struct statfs filesystem{};
    if (fstatfs(descriptor, &filesystem) != 0) {
        return false;
    }
    constexpr std::array<std::uint64_t, 4U> supported{
        0x0000ef53U, // ext2/3/4
        0x58465342U, // XFS
        0x9123683eU, // Btrfs
        0x2fc12fc1U, // ZFS
    };
    const auto type = static_cast<std::uint64_t>(filesystem.f_type);
    return std::ranges::find(supported, type) != supported.end();
}

NativeHandle open_parent(const std::filesystem::path &parent,
                         DirectoryTransferProgressStoreResult &failure) {
    const int descriptor = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer state directory could not be pinned", native_error());
        return {};
    }
    NativeHandle pinned(descriptor);
    struct stat information{};
    if (!inspect_descriptor(descriptor, true, failure)) {
        return {};
    }
    if (fstat(descriptor, &information) != 0 || information.st_uid != geteuid() ||
        (information.st_mode & (S_IWGRP | S_IWOTH)) != 0 || !supported_filesystem(descriptor)) {
        failure = result(DirectoryTransferProgressStoreStatus::unsupported_filesystem,
                         "directory-transfer ledger requires a private durable local filesystem");
        return {};
    }
    return pinned;
}

NativeHandle open_child(const NativeHandle &parent, const std::filesystem::path &name,
                        const bool writable, DirectoryTransferProgressStoreResult &failure,
                        bool &missing) {
    missing = false;
    const int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW;
    const int descriptor = openat(parent.get(), name.c_str(), flags);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            missing = true;
            return {};
        }
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger could not be opened", native_error());
        return {};
    }
    NativeHandle opened(descriptor);
    if (!inspect_descriptor(descriptor, false, failure)) {
        return {};
    }
    return opened;
}

bool verify_parent(const NativeHandle &parent, const std::filesystem::path &parent_path,
                   DirectoryTransferProgressStoreResult &failure) {
    const auto expected = inspect_descriptor(parent.get(), true, failure);
    if (!expected) {
        return false;
    }
    struct stat observed{};
    if (lstat(parent_path.c_str(), &observed) != 0 || !S_ISDIR(observed.st_mode)) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer state directory no longer has its pinned identity",
                         native_error());
        return false;
    }
    if (identity(observed) != expected->identity) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer state directory was replaced while open");
        return false;
    }
    return true;
}

struct FileVerification {
    const NativeHandle &file;
    const NativeHandle &parent;
    const std::filesystem::path &parent_path;
    const std::filesystem::path &canonical_path;
    std::uint64_t expected_size{};
};

bool verify_file(const FileVerification &check, DirectoryTransferProgressStoreResult &failure) {
    if (!verify_parent(check.parent, check.parent_path, failure)) {
        return false;
    }
    const auto expected = inspect_descriptor(check.file.get(), false, failure);
    if (!expected || expected->size != check.expected_size) {
        if (expected) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "directory-transfer ledger has an unexpected size");
        }
        return false;
    }
    struct stat observed{};
    if (fstatat(check.parent.get(), check.canonical_path.c_str(), &observed, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        !S_ISREG(observed.st_mode) || observed.st_nlink != 1 ||
        static_cast<std::uint64_t>(observed.st_size) != check.expected_size ||
        identity(observed) != expected->identity) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer ledger canonical path was replaced", native_error());
        return false;
    }
    return true;
}

bool read_bytes(const NativeHandle &file, const std::uint64_t size, std::vector<std::byte> &bytes,
                DirectoryTransferProgressStoreResult &failure) {
    if (size > std::numeric_limits<std::size_t>::max()) {
        failure = result(DirectoryTransferProgressStoreStatus::payload_too_large,
                         "directory-transfer ledger cannot fit in memory");
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto received = pread(file.get(), bytes.data() + offset, bytes.size() - offset,
                                    static_cast<off_t>(offset));
        if (received <= 0) {
            failure = result(DirectoryTransferProgressStoreStatus::io_error,
                             "directory-transfer ledger read was incomplete", native_error());
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool write_bytes(const NativeHandle &file, const std::uint64_t offset,
                 const std::span<const std::byte> bytes,
                 DirectoryTransferProgressStoreResult &failure) {
    std::size_t written_total{};
    while (written_total < bytes.size()) {
        const auto written =
            pwrite(file.get(), bytes.data() + written_total, bytes.size() - written_total,
                   static_cast<off_t>(offset + written_total));
        if (written <= 0) {
            failure =
                result(DirectoryTransferProgressStoreStatus::recovery_required,
                       "directory-transfer ledger append outcome is ambiguous", native_error());
            return false;
        }
        written_total += static_cast<std::size_t>(written);
    }
    if (fdatasync(file.get()) != 0) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer ledger flush outcome is ambiguous", native_error());
        return false;
    }
    return true;
}

bool truncate_file(const NativeHandle &file, const std::uint64_t size,
                   DirectoryTransferProgressStoreResult &failure) {
    if (ftruncate(file.get(), static_cast<off_t>(size)) != 0 || fdatasync(file.get()) != 0) {
        failure =
            result(DirectoryTransferProgressStoreStatus::recovery_required,
                   "directory-transfer torn tail could not be durably removed", native_error());
        return false;
    }
    return true;
}

bool remove_safe_file(const NativeHandle &parent, const std::filesystem::path &name,
                      DirectoryTransferProgressStoreResult &failure) {
    bool missing{};
    auto file = open_child(parent, name, false, failure, missing);
    if (!file.valid()) {
        return missing;
    }
    const auto expected = inspect_descriptor(file.get(), false, failure);
    struct stat observed{};
    if (!expected || fstatat(parent.get(), name.c_str(), &observed, AT_SYMLINK_NOFOLLOW) != 0 ||
        identity(observed) != expected->identity || !S_ISREG(observed.st_mode) ||
        observed.st_nlink != 1) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "stale directory-transfer ledger temporary path is ambiguous");
        return false;
    }
    if (unlinkat(parent.get(), name.c_str(), 0) != 0 || fsync(parent.get()) != 0) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "stale directory-transfer ledger temporary file could not be removed",
                         native_error());
        return false;
    }
    return true;
}

bool rename_no_replace(const NativeHandle &parent, const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       DirectoryTransferProgressStoreResult &failure) {
#if defined(SYS_renameat2)
    if (syscall(SYS_renameat2, parent.get(), source.c_str(), parent.get(), destination.c_str(),
                1U) == 0) {
        return true;
    }
    const auto code = errno;
    const auto unsupported = code == ENOSYS || code == EINVAL
#ifdef EOPNOTSUPP
                             || code == EOPNOTSUPP
#endif
        ;
    failure =
        result(unsupported      ? DirectoryTransferProgressStoreStatus::unsupported_filesystem
               : code == EEXIST ? DirectoryTransferProgressStoreStatus::payload_mismatch
                                : DirectoryTransferProgressStoreStatus::recovery_required,
               unsupported ? "directory-transfer ledger requires atomic no-replace rename support"
                           : "directory-transfer ledger header publication was not confirmed",
               native_error(code));
    return false;
#else
    static_cast<void>(parent);
    static_cast<void>(source);
    static_cast<void>(destination);
    failure = result(DirectoryTransferProgressStoreStatus::unsupported_filesystem,
                     "directory-transfer ledger requires atomic no-replace rename support");
    return false;
#endif
}

NativeHandle publish_header(const std::filesystem::path &ledger_name,
                            const std::filesystem::path &temp_name, const NativeHandle &parent,
                            const std::filesystem::path &parent_path,
                            const std::span<const std::byte> header,
                            DirectoryTransferProgressStoreResult &failure) {
    const int descriptor = openat(parent.get(), temp_name.c_str(),
                                  O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        failure = result(DirectoryTransferProgressStoreStatus::io_error,
                         "directory-transfer ledger header temporary file could not be created",
                         native_error());
        return {};
    }
    NativeHandle temp(descriptor);
    if (!write_bytes(temp, 0U, header, failure)) {
        return {};
    }
    const auto shape = inspect_descriptor(temp.get(), false, failure);
    if (!shape || shape->size != header.size() || !verify_parent(parent, parent_path, failure)) {
        if (shape && shape->size != header.size()) {
            failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                             "directory-transfer ledger header has the wrong durable size");
        }
        return {};
    }
    if (!rename_no_replace(parent, temp_name, ledger_name, failure)) {
        return {};
    }
    if (fsync(parent.get()) != 0) {
        failure = result(DirectoryTransferProgressStoreStatus::recovery_required,
                         "directory-transfer ledger parent flush was ambiguous", native_error());
        return {};
    }
    if (!verify_file({.file = temp,
                      .parent = parent,
                      .parent_path = parent_path,
                      .canonical_path = ledger_name,
                      .expected_size = header.size()},
                     failure)) {
        return {};
    }
    return temp;
}

#endif

DirectoryTransferProgressStoreResult
map_current_read(const CurrentOperationJournalReadResult &current) {
    if (current.status == DurableJournalStatus::not_found) {
        return result(DirectoryTransferProgressStoreStatus::not_found,
                      "no prepared directory transfer exists");
    }
    if (current.status == DurableJournalStatus::payload_too_large) {
        return result(DirectoryTransferProgressStoreStatus::payload_too_large,
                      "current-operation journal exceeds its bound", current.error);
    }
    if (!current.ok()) {
        return result(current.status == DurableJournalStatus::io_error
                          ? DirectoryTransferProgressStoreStatus::io_error
                          : DirectoryTransferProgressStoreStatus::corrupt,
                      "current-operation journal is unavailable or corrupt", current.error);
    }
    return result(DirectoryTransferProgressStoreStatus::success);
}

DirectoryTransferProgressStoreResult
map_manifest_read(const DirectoryTransferManifestStoreReadResult &manifest) {
    switch (manifest.status) {
    case DirectoryTransferManifestStoreStatus::success:
        return result(DirectoryTransferProgressStoreStatus::success);
    case DirectoryTransferManifestStoreStatus::not_found:
        return result(DirectoryTransferProgressStoreStatus::recovery_required,
                      "prepared directory transfer lost its immutable manifest");
    case DirectoryTransferManifestStoreStatus::invalid_manifest:
    case DirectoryTransferManifestStoreStatus::corrupt:
        return result(DirectoryTransferProgressStoreStatus::corrupt, manifest.detail_utf8,
                      manifest.error);
    case DirectoryTransferManifestStoreStatus::incompatible_version:
        return result(DirectoryTransferProgressStoreStatus::incompatible_version,
                      manifest.detail_utf8, manifest.error);
    case DirectoryTransferManifestStoreStatus::payload_mismatch:
        return result(DirectoryTransferProgressStoreStatus::payload_mismatch, manifest.detail_utf8,
                      manifest.error);
    case DirectoryTransferManifestStoreStatus::payload_too_large:
        return result(DirectoryTransferProgressStoreStatus::payload_too_large, manifest.detail_utf8,
                      manifest.error);
    case DirectoryTransferManifestStoreStatus::io_error:
        return result(DirectoryTransferProgressStoreStatus::io_error, manifest.detail_utf8,
                      manifest.error);
    }
    return result(DirectoryTransferProgressStoreStatus::io_error, "unknown manifest-store status");
}

bool safe_size_sum(const std::uint64_t records, std::uint64_t &size) noexcept {
    if (records >
        (std::numeric_limits<std::uint64_t>::max() - kDirectoryTransferProgressHeaderBytes) /
            kDirectoryTransferProgressRecordBytes) {
        return false;
    }
    size = kDirectoryTransferProgressHeaderBytes + records * kDirectoryTransferProgressRecordBytes;
    return true;
}

} // namespace

struct DirectoryTransferProgressSession::Impl final {
    Impl(CurrentOperationLease current, CurrentOperationLease manifest,
         CurrentOperationLease ledger, NativeHandle parent_handle, NativeHandle ledger_handle,
         DirectoryTransferProgressPlan prepared_plan,
         DirectoryTransferProgressState recovered_state, std::filesystem::path current_path_value,
         std::filesystem::path manifest_path_value, std::filesystem::path ledger_path_value)
        : current_lease(std::move(current)), manifest_lease(std::move(manifest)),
          ledger_lease(std::move(ledger)), parent(std::move(parent_handle)),
          ledger(std::move(ledger_handle)), plan(std::move(prepared_plan)), state(recovered_state),
          current_path(std::move(current_path_value)),
          manifest_path(std::move(manifest_path_value)), ledger_path(std::move(ledger_path_value)) {
    }

    void close() noexcept {
        poisoned = true;
        ledger.reset();
        parent.reset();
        ledger_lease.release();
        manifest_lease.release();
        current_lease.release();
    }

    CurrentOperationLease current_lease;
    CurrentOperationLease manifest_lease;
    CurrentOperationLease ledger_lease;
    NativeHandle parent;
    NativeHandle ledger;
    DirectoryTransferProgressPlan plan;
    DirectoryTransferProgressState state;
    std::filesystem::path current_path;
    std::filesystem::path manifest_path;
    std::filesystem::path ledger_path;
    bool poisoned{};
};

DirectoryTransferProgressSession::DirectoryTransferProgressSession(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

DirectoryTransferProgressSession::~DirectoryTransferProgressSession() {
    close();
}

DirectoryTransferProgressSession::DirectoryTransferProgressSession(
    DirectoryTransferProgressSession &&) noexcept = default;

DirectoryTransferProgressSession &
DirectoryTransferProgressSession::operator=(DirectoryTransferProgressSession &&other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

DirectoryTransferProgressOpenResult DirectoryTransferProgressSession::open_or_create(
    const std::filesystem::path &current_journal_path) {
    DirectoryTransferProgressOpenResult opened;
    if (!valid_current_path(current_journal_path)) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::invalid_argument,
                   "current-operation journal path is not canonical");
        return opened;
    }

    std::error_code lease_error;
    auto current_lease = CurrentOperationLease::try_acquire(current_journal_path, lease_error);
    if (!current_lease.owns_lock()) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::busy, "current-operation journal is busy",
                   lease_error);
        return opened;
    }
    const auto current = CurrentOperationJournalStore(current_journal_path).read();
    auto failure = map_current_read(current);
    if (!failure.ok()) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
        return opened;
    }
    if (current.encoding != CurrentOperationJournalEncoding::typed ||
        current.kind != CurrentOperationKind::directory_transfer) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::payload_mismatch,
                   "another operation owns the current journal");
        return opened;
    }
    DirectoryTransferControlRecord control;
    std::string detail;
    if (!decode_directory_transfer_control(current.payload, control, detail)) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::corrupt, std::move(detail));
        return opened;
    }

    std::filesystem::path manifest_path;
    std::filesystem::path ledger_path;
    try {
        manifest_path =
            directory_transfer_manifest_sidecar_path(current_journal_path, control.operation_id);
        ledger_path =
            directory_transfer_progress_ledger_path(current_journal_path, control.operation_id);
    } catch (const std::exception &error) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::invalid_argument, error.what());
        return opened;
    }

    auto manifest_lease = CurrentOperationLease::try_acquire(manifest_path, lease_error);
    if (!manifest_lease.owns_lock()) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::busy,
                   "directory-transfer manifest is busy", lease_error);
        return opened;
    }
    const DirectoryTransferManifestStore manifest_store(manifest_path);
    auto stored_manifest = manifest_store.read_locked(manifest_lease);
    failure = map_manifest_read(stored_manifest);
    if (!failure.ok()) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
        return opened;
    }

    auto ledger_lease = CurrentOperationLease::try_acquire(ledger_path, lease_error);
    if (!ledger_lease.owns_lock()) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::busy,
                   "directory-transfer progress ledger is busy", lease_error);
        return opened;
    }
    const auto parent_path = ledger_path.parent_path();
    auto parent = open_parent(parent_path, failure);
    if (!parent.valid()) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
        return opened;
    }

#ifdef _WIN32
    const auto ledger_name = ledger_path;
    const auto temp_name = temporary_path(ledger_path);
    bool ledger_missing{};
    auto ledger = open_path(ledger_path, true, false, failure, ledger_missing);
#else
    const auto ledger_name = ledger_path.filename();
    const auto temp_name = temporary_path(ledger_path).filename();
    bool ledger_missing{};
    auto ledger = open_child(parent, ledger_name, true, failure, ledger_missing);
#endif
    if (!ledger.valid() && !ledger_missing) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
        return opened;
    }

    std::optional<DirectoryTransferProgressPlan> prepared;
    try {
        prepared = prepare_directory_transfer_progress_plan(control, stored_manifest.manifest);
    } catch (const std::exception &error) {
        static_cast<DirectoryTransferProgressStoreResult &>(opened) =
            result(DirectoryTransferProgressStoreStatus::payload_mismatch, error.what());
        return opened;
    }
    auto plan = std::move(*prepared);
    auto recovered_state = initialize_directory_transfer_progress(plan);
    bool repaired_tail{};
    bool created{};

    if (ledger_missing) {
#ifdef _WIN32
        if (!remove_safe_file(temp_name, failure)) {
#else
        if (!remove_safe_file(parent, temp_name, failure)) {
#endif
            static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
            return opened;
        }
        const auto encoded_header = encode_directory_transfer_progress_header(plan.header());
        ledger =
            publish_header(ledger_name, temp_name, parent, parent_path, encoded_header, failure);
        if (!ledger.valid()) {
            static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
            return opened;
        }
        created = true;
    } else {
        const auto shape =
#ifdef _WIN32
            inspect_handle(ledger.get(), false, failure);
#else
            inspect_descriptor(ledger.get(), false, failure);
#endif
        if (!shape || shape->size < kDirectoryTransferProgressHeaderBytes ||
            shape->size > kMaximumLedgerReadBytes ||
            !verify_file({.file = ledger,
                          .parent = parent,
                          .parent_path = parent_path,
                          .canonical_path = ledger_name,
                          .expected_size = shape->size},
                         failure)) {
            if (shape && shape->size < kDirectoryTransferProgressHeaderBytes) {
                failure = result(DirectoryTransferProgressStoreStatus::corrupt,
                                 "directory-transfer ledger header is truncated");
            } else if (shape && shape->size > kMaximumLedgerReadBytes) {
                failure = result(DirectoryTransferProgressStoreStatus::payload_too_large,
                                 "directory-transfer ledger exceeds its global bound");
            }
            static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
            return opened;
        }
        std::vector<std::byte> bytes;
        if (!read_bytes(ledger, shape->size, bytes, failure) ||
            !verify_file({.file = ledger,
                          .parent = parent,
                          .parent_path = parent_path,
                          .canonical_path = ledger_name,
                          .expected_size = shape->size},
                         failure)) {
            static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
            return opened;
        }
        DirectoryTransferProgressHeader header;
        if (!decode_directory_transfer_progress_header(
                std::span<const std::byte>(bytes.data(), kDirectoryTransferProgressHeaderBytes),
                header, detail)) {
            static_cast<DirectoryTransferProgressStoreResult &>(opened) =
                result(DirectoryTransferProgressStoreStatus::corrupt, std::move(detail));
            return opened;
        }
        auto bound = bind_directory_transfer_progress_plan(header, control,
                                                           stored_manifest.manifest, detail);
        if (!bound) {
            static_cast<DirectoryTransferProgressStoreResult &>(opened) =
                result(DirectoryTransferProgressStoreStatus::payload_mismatch, std::move(detail));
            return opened;
        }
        plan = std::move(*bound);
        std::uint64_t exact_max{};
        if (!safe_size_sum(plan.header().maximum_record_count, exact_max) ||
            shape->size > exact_max + kMaximumTornTailBytes) {
            static_cast<DirectoryTransferProgressStoreResult &>(opened) =
                result(DirectoryTransferProgressStoreStatus::payload_too_large,
                       "directory-transfer ledger exceeds its prepared-plan bound");
            return opened;
        }
        const auto record_bytes = bytes.size() - kDirectoryTransferProgressHeaderBytes;
        const auto complete_records = record_bytes / kDirectoryTransferProgressRecordBytes;
        const auto tail_bytes = record_bytes % kDirectoryTransferProgressRecordBytes;
        std::vector<DirectoryTransferProgressRecord> records;
        records.reserve(complete_records);
        for (std::size_t index{}; index < complete_records; ++index) {
            const auto offset = kDirectoryTransferProgressHeaderBytes +
                                index * kDirectoryTransferProgressRecordBytes;
            DirectoryTransferProgressRecord record;
            if (!decode_directory_transfer_progress_record(
                    std::span<const std::byte>(bytes.data() + offset,
                                               kDirectoryTransferProgressRecordBytes),
                    record, detail)) {
                static_cast<DirectoryTransferProgressStoreResult &>(opened) =
                    result(DirectoryTransferProgressStoreStatus::corrupt, std::move(detail));
                return opened;
            }
            records.push_back(record);
        }
        auto replayed = replay_directory_transfer_progress(plan, records, detail);
        if (!replayed) {
            static_cast<DirectoryTransferProgressStoreResult &>(opened) =
                result(DirectoryTransferProgressStoreStatus::corrupt, std::move(detail));
            return opened;
        }
        recovered_state = *replayed;
        if (tail_bytes != 0U) {
            const auto repaired_size = shape->size - tail_bytes;
            if (!verify_file({.file = ledger,
                              .parent = parent,
                              .parent_path = parent_path,
                              .canonical_path = ledger_name,
                              .expected_size = shape->size},
                             failure) ||
                !truncate_file(ledger, repaired_size, failure) ||
                !verify_file({.file = ledger,
                              .parent = parent,
                              .parent_path = parent_path,
                              .canonical_path = ledger_name,
                              .expected_size = repaired_size},
                             failure)) {
                static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
                return opened;
            }
            repaired_tail = true;
        }
#ifdef _WIN32
        if (!remove_safe_file(temp_name, failure)) {
#else
        if (!remove_safe_file(parent, temp_name, failure)) {
#endif
            static_cast<DirectoryTransferProgressStoreResult &>(opened) = std::move(failure);
            return opened;
        }
    }

    auto impl = std::make_unique<Impl>(
        std::move(current_lease), std::move(manifest_lease), std::move(ledger_lease),
        std::move(parent), std::move(ledger), std::move(plan), recovered_state,
        current_journal_path, std::move(manifest_path), std::move(ledger_path));
    opened.status = DirectoryTransferProgressStoreStatus::success;
    opened.created = created;
    opened.repaired_tail = repaired_tail;
    opened.session = std::unique_ptr<DirectoryTransferProgressSession>(
        new DirectoryTransferProgressSession(std::move(impl)));
    return opened;
}

template <typename Evidence>
DirectoryTransferProgressAppendResult
DirectoryTransferProgressSession::append_evidence(const Evidence &evidence) {
    DirectoryTransferProgressAppendResult output;
    if (!impl_ || impl_->poisoned || !impl_->ledger.valid()) {
        output.status = DirectoryTransferProgressStoreStatus::recovery_required;
        output.detail_utf8 = "directory-transfer progress session is closed or poisoned";
        return output;
    }
    auto next = impl_->state;
    DirectoryTransferProgressRecord record;
    std::string detail;
    if (!advance_directory_transfer_progress(impl_->plan, evidence, next, record, detail)) {
        output.status = DirectoryTransferProgressStoreStatus::invalid_argument;
        output.detail_utf8 = std::move(detail);
        return output;
    }
    std::vector<std::byte> encoded;
    try {
        encoded = encode_directory_transfer_progress_record(record);
    } catch (const std::exception &error) {
        output.status = DirectoryTransferProgressStoreStatus::invalid_argument;
        output.detail_utf8 = error.what();
        return output;
    }
    std::uint64_t expected_size{};
    if (!safe_size_sum(impl_->state.next_sequence() - 1U, expected_size)) {
        impl_->poisoned = true;
        output.status = DirectoryTransferProgressStoreStatus::recovery_required;
        output.detail_utf8 = "directory-transfer ledger size overflowed";
        return output;
    }
    DirectoryTransferProgressStoreResult failure;
    const auto parent_path = impl_->ledger_path.parent_path();
#ifdef _WIN32
    const auto canonical_name = impl_->ledger_path;
#else
    const auto canonical_name = impl_->ledger_path.filename();
#endif
    if (!verify_file({.file = impl_->ledger,
                      .parent = impl_->parent,
                      .parent_path = parent_path,
                      .canonical_path = canonical_name,
                      .expected_size = expected_size},
                     failure) ||
        !write_bytes(impl_->ledger, expected_size, encoded, failure) ||
        !verify_file({.file = impl_->ledger,
                      .parent = impl_->parent,
                      .parent_path = parent_path,
                      .canonical_path = canonical_name,
                      .expected_size = expected_size + encoded.size()},
                     failure)) {
        impl_->poisoned = true;
        static_cast<DirectoryTransferProgressStoreResult &>(output) = std::move(failure);
        output.status = DirectoryTransferProgressStoreStatus::recovery_required;
        return output;
    }
    static_assert(std::is_nothrow_copy_assignable_v<DirectoryTransferProgressState>);
    impl_->state = next;
    output.status = DirectoryTransferProgressStoreStatus::success;
    output.record = record;
    return output;
}

DirectoryTransferProgressAppendResult
DirectoryTransferProgressSession::append(const DirectoryTransferRootStagingEvidence &evidence) {
    return append_evidence(evidence);
}

DirectoryTransferProgressAppendResult
DirectoryTransferProgressSession::append(const DirectoryTransferEntryEvidence &evidence) {
    return append_evidence(evidence);
}

DirectoryTransferProgressAppendResult
DirectoryTransferProgressSession::append(const DirectoryTransferRootPublicationEvidence &evidence) {
    return append_evidence(evidence);
}

const DirectoryTransferProgressPlan &DirectoryTransferProgressSession::plan() const {
    if (!impl_) {
        throw std::logic_error("directory-transfer progress session is closed");
    }
    return impl_->plan;
}

const DirectoryTransferProgressState &DirectoryTransferProgressSession::state() const {
    if (!impl_) {
        throw std::logic_error("directory-transfer progress session is closed");
    }
    return impl_->state;
}

const std::filesystem::path &DirectoryTransferProgressSession::ledger_path() const {
    if (!impl_) {
        throw std::logic_error("directory-transfer progress session is closed");
    }
    return impl_->ledger_path;
}

bool DirectoryTransferProgressSession::poisoned() const noexcept {
    return !impl_ || impl_->poisoned;
}

void DirectoryTransferProgressSession::close() noexcept {
    if (impl_) {
        impl_->close();
        impl_.reset();
    }
}

} // namespace vove::fileops
