#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "file_time.hpp"
#include "file_identity.hpp"
#include "filesystem_error.hpp"
#include "vove/platform/read_only_source.hpp"

#include <bit>
#include <cstdint>
#include <string>
#include <utility>

namespace vove::platform {
namespace {

[[nodiscard]] SourceOpenErrorKind windows_error_kind(const DWORD code) noexcept {
    switch (detail::classify_windows_filesystem_error(code)) {
    case detail::WindowsFilesystemErrorKind::not_found:
        return SourceOpenErrorKind::not_found;
    case detail::WindowsFilesystemErrorKind::permission_denied:
        return SourceOpenErrorKind::access_denied;
    case detail::WindowsFilesystemErrorKind::authentication_required:
        return SourceOpenErrorKind::authentication_failed;
    case detail::WindowsFilesystemErrorKind::timed_out:
        return SourceOpenErrorKind::timed_out;
    case detail::WindowsFilesystemErrorKind::disconnected:
        return SourceOpenErrorKind::disconnected;
    case detail::WindowsFilesystemErrorKind::io_error:
        return SourceOpenErrorKind::io_error;
    }
    return SourceOpenErrorKind::io_error;
}

[[nodiscard]] SourceOpenError windows_error(const DWORD code) {
    return {.kind = windows_error_kind(code),
            .platform_code = static_cast<std::int64_t>(code),
            .detail = "Windows error " + std::to_string(code)};
}

[[nodiscard]] HANDLE as_handle(const NativeFileObject object) noexcept {
    return std::bit_cast<HANDLE>(object);
}

struct SourceRevisionQuery {
    std::string value;
    DWORD error{};

    [[nodiscard]] bool ok() const noexcept {
        return !value.empty();
    }
};

[[nodiscard]] SourceRevisionQuery source_revision(const HANDLE handle, const FILE_BASIC_INFO &basic,
                                                  const std::string_view expected = {}) {
    const auto identity = windows_detail::query_file_identity(handle, basic.ChangeTime);
    if (!expected.empty() && !expected.starts_with("win-birth:")) {
        if (const auto &matching = windows_detail::matching_revision(identity, expected);
            !matching.empty()) {
            return {.value = matching, .error = ERROR_SUCCESS};
        }
    } else if (expected.empty()) {
        if (const auto preferred = windows_detail::preferred_revision(identity);
            !preferred.empty()) {
            return {.value = preferred, .error = ERROR_SUCCESS};
        }
    }
    if (!windows_detail::identity_fallback_is_usable(identity.error)) {
        return {.value = {}, .error = identity.error};
    }
    if (basic.CreationTime.QuadPart <= 0) {
        return {.value = {},
                .error = identity.error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : identity.error};
    }
    const auto birth =
        "win-birth:" + std::to_string(static_cast<std::uint64_t>(basic.CreationTime.QuadPart));
    if (expected.empty() || expected.starts_with("win-birth:")) {
        return {.value = birth, .error = ERROR_SUCCESS};
    }
    return {.value = {}, .error = ERROR_NOT_SUPPORTED};
}

} // namespace

ReadOnlySource::ReadOnlySource(Snapshot snapshot)
    : object_(snapshot.object), size_bytes_(snapshot.size_bytes),
      modified_unix_ns_(snapshot.modified_unix_ns),
      source_revision_utf8_(std::move(snapshot.source_revision_utf8)) {}

ReadOnlySource::~ReadOnlySource() {
    close();
}

ReadOnlySource::ReadOnlySource(ReadOnlySource &&other) noexcept
    : object_(std::exchange(other.object_, kInvalidNativeFileObject)),
      size_bytes_(std::exchange(other.size_bytes_, 0)),
      modified_unix_ns_(std::exchange(other.modified_unix_ns_, 0)),
      source_revision_utf8_(std::move(other.source_revision_utf8_)) {}

ReadOnlySource &ReadOnlySource::operator=(ReadOnlySource &&other) noexcept {
    if (this != &other) {
        close();
        object_ = std::exchange(other.object_, kInvalidNativeFileObject);
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        modified_unix_ns_ = std::exchange(other.modified_unix_ns_, 0);
        source_revision_utf8_ = std::move(other.source_revision_utf8_);
    }
    return *this;
}

bool ReadOnlySource::valid() const noexcept {
    return object_ != kInvalidNativeFileObject;
}

NativeFileObject ReadOnlySource::native_object() const noexcept {
    return object_;
}

std::uint64_t ReadOnlySource::size_bytes() const noexcept {
    return size_bytes_;
}

std::int64_t ReadOnlySource::modified_unix_ns() const noexcept {
    return modified_unix_ns_;
}

const std::string &ReadOnlySource::source_revision_utf8() const noexcept {
    return source_revision_utf8_;
}

SourceIdentityResult ReadOnlySource::identity_result() const noexcept {
    if (!valid()) {
        return {.status = SourceIdentityStatus::unavailable,
                .error_kind = SourceOpenErrorKind::invalid_path,
                .platform_code = ERROR_INVALID_HANDLE};
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(as_handle(object_), FileBasicInfo, &basic, sizeof(basic)) ==
        FALSE) {
        const auto code = GetLastError();
        return {.status = SourceIdentityStatus::unavailable,
                .error_kind = windows_error_kind(code),
                .platform_code = static_cast<std::int64_t>(code)};
    }
    try {
        const auto revision = source_revision(as_handle(object_), basic, source_revision_utf8_);
        if (!revision.ok()) {
            return {.status = SourceIdentityStatus::unavailable,
                    .error_kind = windows_error_kind(revision.error),
                    .platform_code = static_cast<std::int64_t>(revision.error)};
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(as_handle(object_), &size) == FALSE || size.QuadPart < 0) {
            const auto code = GetLastError();
            return {.status = SourceIdentityStatus::unavailable,
                    .error_kind = windows_error_kind(code),
                    .platform_code = static_cast<std::int64_t>(code)};
        }
        const auto unchanged =
            static_cast<std::uint64_t>(size.QuadPart) == size_bytes_ &&
            windows_detail::filetime_to_unix_ns(basic.LastWriteTime) == modified_unix_ns_ &&
            revision.value == source_revision_utf8_;
        return {.status =
                    unchanged ? SourceIdentityStatus::unchanged : SourceIdentityStatus::changed};
    } catch (...) {
        return {.status = SourceIdentityStatus::changed};
    }
}

SourceIdentityStatus ReadOnlySource::identity_status() const noexcept {
    return identity_result().status;
}

bool ReadOnlySource::identity_unchanged() const noexcept {
    return identity_status() == SourceIdentityStatus::unchanged;
}

void ReadOnlySource::close() noexcept {
    if (valid()) {
        static_cast<void>(CloseHandle(as_handle(object_)));
        object_ = kInvalidNativeFileObject;
        size_bytes_ = 0;
        modified_unix_ns_ = 0;
        source_revision_utf8_.clear();
    }
}

SourceOpenResult open_read_only_source(const std::filesystem::path &path,
                                       const std::uint64_t maximum_bytes) {
    SourceOpenResult result;
    if (path.empty() || maximum_bytes == 0) {
        result.error = {.kind = SourceOpenErrorKind::invalid_path,
                        .platform_code = 0,
                        .detail = "source path or size limit is empty"};
        return result;
    }
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.error = {.kind = SourceOpenErrorKind::not_regular_file,
                        .platform_code = 0,
                        .detail = "source is not a regular file"};
        return result;
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = windows_error(GetLastError());
        return result;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        static_cast<void>(CloseHandle(handle));
        result.error = {.kind = SourceOpenErrorKind::not_regular_file,
                        .platform_code = 0,
                        .detail = "source is not a disk file"};
        return result;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE) {
        const auto error = GetLastError();
        static_cast<void>(CloseHandle(handle));
        result.error = windows_error(error);
        return result;
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        static_cast<void>(CloseHandle(handle));
        result.error = {.kind = SourceOpenErrorKind::not_regular_file,
                        .platform_code = 0,
                        .detail = "source is a reparse point"};
        return result;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0) {
        const auto error = GetLastError();
        static_cast<void>(CloseHandle(handle));
        result.error = windows_error(error);
        return result;
    }
    const auto unsigned_size = static_cast<std::uint64_t>(size.QuadPart);
    if (unsigned_size > maximum_bytes) {
        static_cast<void>(CloseHandle(handle));
        result.error = {.kind = SourceOpenErrorKind::too_large,
                        .platform_code = 0,
                        .detail = "source exceeds the configured byte limit"};
        return result;
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        const auto error = GetLastError();
        static_cast<void>(CloseHandle(handle));
        result.error = windows_error(error);
        return result;
    }
    const auto modified = windows_detail::filetime_to_unix_ns(basic.LastWriteTime);
    static_cast<void>(SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0));
    const auto revision = source_revision(handle, basic);
    if (!revision.ok()) {
        static_cast<void>(CloseHandle(handle));
        result.error = windows_error(revision.error);
        return result;
    }
    result.source = ReadOnlySource(
        ReadOnlySource::Snapshot{.object = reinterpret_cast<NativeFileObject>(handle),
                                 .size_bytes = unsigned_size,
                                 .modified_unix_ns = modified,
                                 .source_revision_utf8 = revision.value});
    return result;
}

DirectoryRevisionResult query_directory_revision(const std::filesystem::path &path) {
    DirectoryRevisionResult result;
    if (path.empty()) {
        result.error = {.kind = SourceOpenErrorKind::invalid_path,
                        .platform_code = 0,
                        .detail = "directory path is empty"};
        return result;
    }
    const auto handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = windows_error(GetLastError());
        return result;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE) {
        const auto error = GetLastError();
        static_cast<void>(CloseHandle(handle));
        result.error = windows_error(error);
        return result;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        static_cast<void>(CloseHandle(handle));
        result.error = {.kind = SourceOpenErrorKind::not_regular_file,
                        .platform_code = 0,
                        .detail = "path is not a direct directory"};
        return result;
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        const auto error = GetLastError();
        static_cast<void>(CloseHandle(handle));
        result.error = windows_error(error);
        return result;
    }
    const auto identity = windows_detail::query_file_identity(handle, basic.ChangeTime);
    result.revision_utf8 = windows_detail::preferred_revision(identity);
    static_cast<void>(CloseHandle(handle));
    if (result.revision_utf8.empty()) {
        result.error =
            windows_error(identity.error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : identity.error);
    }
    return result;
}

std::string storage_identity(const NativeFileObject object) noexcept {
    static_cast<void>(object);
    return {};
}

std::string storage_identity(const std::filesystem::path &path) noexcept {
    static_cast<void>(path);
    return {};
}

} // namespace vove::platform
