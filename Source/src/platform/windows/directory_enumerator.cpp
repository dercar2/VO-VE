#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>

#include "../directory_enumerator.hpp"
#include "../directory_recursive.hpp"
#include "file_identity.hpp"
#include "file_time.hpp"
#include "filesystem_error.hpp"

#include "vove/core/reserved_names.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace vove::platform::detail {

namespace {

std::string wide_to_utf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const auto required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const auto converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                               static_cast<int>(text.size()), result.data(),
                                               required, nullptr, nullptr);
    return converted == required ? result : std::string{};
}

catalog::CatalogErrorKind classify_error(const DWORD code) {
    switch (classify_windows_filesystem_error(code)) {
    case WindowsFilesystemErrorKind::not_found:
        return catalog::CatalogErrorKind::not_found;
    case WindowsFilesystemErrorKind::permission_denied:
        return catalog::CatalogErrorKind::permission_denied;
    case WindowsFilesystemErrorKind::authentication_required:
        return catalog::CatalogErrorKind::authentication_required;
    case WindowsFilesystemErrorKind::timed_out:
        return catalog::CatalogErrorKind::timed_out;
    case WindowsFilesystemErrorKind::disconnected:
        return catalog::CatalogErrorKind::network_disconnected;
    case WindowsFilesystemErrorKind::io_error:
        return catalog::CatalogErrorKind::io_error;
    }
    return catalog::CatalogErrorKind::io_error;
}

catalog::CatalogError make_error(const std::filesystem::path &path, const DWORD code) {
    std::array<wchar_t, 512> buffer{};
    const auto length =
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::wstring message(buffer.data(), static_cast<std::size_t>(length));
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    auto detail = wide_to_utf8(message);
    if (detail.empty()) {
        detail = "Windows filesystem error";
    }
    return {.kind = classify_error(code),
            .message_utf8 = path_utf8(path) + ": " + detail,
            .platform_code = static_cast<std::int64_t>(code)};
}

std::string birth_revision(const FILETIME &creation_time) {
    const auto ticks = (static_cast<std::uint64_t>(creation_time.dwHighDateTime) << 32U) |
                       static_cast<std::uint64_t>(creation_time.dwLowDateTime);
    return ticks == 0 ? std::string{} : "win-birth:" + std::to_string(ticks);
}

std::string birth_revision(const LARGE_INTEGER &creation_time) {
    return creation_time.QuadPart <= 0
               ? std::string{}
               : "win-birth:" + std::to_string(static_cast<std::uint64_t>(creation_time.QuadPart));
}

std::string file_revision(const std::uint64_t volume_serial, const FILE_ID_128 &file_id,
                          const std::int64_t change_time) {
    return change_time <= 0 ? std::string{}
                            : "win-file128:" + std::to_string(volume_serial) + ':' +
                                  windows_detail::hex_file_id(file_id) + ':' +
                                  std::to_string(static_cast<std::uint64_t>(change_time));
}

std::string source_revision_for_path(const std::filesystem::path &path, const bool is_directory,
                                     const std::string &fallback) {
    const auto handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    is_directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fallback;
    }
    FILE_BASIC_INFO basic{};
    std::string result;
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE) {
        result = windows_detail::preferred_revision(
            windows_detail::query_file_identity(handle, basic.ChangeTime));
    }
    static_cast<void>(CloseHandle(handle));
    return result.empty() ? fallback : result;
}

struct FileIdEnumeration {
    bool supported{true};
    EnumerationResult result;
};

FileIdEnumeration enumerate_file_ids(const catalog::CatalogRequest &request,
                                     const EntryCallback &on_entry) {
    const auto directory = CreateFileW(request.path.c_str(), FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return {.result = {.error = make_error(request.path, GetLastError())}};
    }

    FILE_ID_INFO directory_information{};
    if (GetFileInformationByHandleEx(directory, FileIdInfo, &directory_information,
                                     sizeof(directory_information)) == FALSE) {
        static_cast<void>(CloseHandle(directory));
        return {.supported = false, .result = {}};
    }
    const auto volume_serial = static_cast<std::uint64_t>(directory_information.VolumeSerialNumber);

    std::array<std::byte, std::size_t{64} * 1024U> buffer{};
    EnumerationResult result;
    std::size_t count{};
    bool first_call{true};
    for (;;) {
        if (GetFileInformationByHandleEx(directory, FileIdExtdDirectoryInfo, buffer.data(),
                                         static_cast<DWORD>(buffer.size())) == FALSE) {
            const auto error = GetLastError();
            static_cast<void>(CloseHandle(directory));
            if (first_call && (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
                               error == ERROR_INVALID_FUNCTION)) {
                return {.supported = false, .result = {}};
            }
            if (error != ERROR_NO_MORE_FILES) {
                result.error = make_error(request.path, error);
            }
            return {.result = std::move(result)};
        }
        first_call = false;

        auto *record = reinterpret_cast<FILE_ID_EXTD_DIR_INFO *>(buffer.data());
        for (;;) {
            const auto name = std::wstring_view(
                record->FileName,
                static_cast<std::size_t>(record->FileNameLength / sizeof(wchar_t)));
            if (name != L"." && name != L".." && !core::is_internal_filename(name)) {
                if (request.maximum_entries != 0 && count >= request.maximum_entries) {
                    result.truncated = true;
                    static_cast<void>(CloseHandle(directory));
                    return {.result = std::move(result)};
                }
                const bool is_directory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                core::DirectoryEntry entry;
                entry.kind = is_directory ? core::EntryKind::directory : core::EntryKind::file;
                entry.state = core::EntryState::metadata_ready;
                entry.name_utf8 = wide_to_utf8(name);
                const auto path = request.path / std::wstring(name);
                entry.path_utf8 = path_utf8(path);
                entry.id = stable_entry_id(entry.path_utf8);
                if (!is_directory && record->EndOfFile.QuadPart >= 0) {
                    entry.size_bytes = static_cast<std::uint64_t>(record->EndOfFile.QuadPart);
                }
                entry.modified_unix_ns = windows_detail::filetime_to_unix_ns(record->LastWriteTime);
                entry.source_revision_utf8 =
                    file_revision(volume_serial, record->FileId, record->ChangeTime.QuadPart);
                if (entry.source_revision_utf8.empty()) {
                    entry.source_revision_utf8 = source_revision_for_path(
                        path, is_directory, birth_revision(record->CreationTime));
                }
                if (!on_entry(std::move(entry))) {
                    result.error.kind = catalog::CatalogErrorKind::cancelled;
                    static_cast<void>(CloseHandle(directory));
                    return {.result = std::move(result)};
                }
                ++count;
            }
            if (record->NextEntryOffset == 0) {
                break;
            }
            record = reinterpret_cast<FILE_ID_EXTD_DIR_INFO *>(
                reinterpret_cast<std::byte *>(record) + record->NextEntryOffset);
        }
    }
}

EnumerationResult enumerate_find_first(const catalog::CatalogRequest &request,
                                       const EntryCallback &on_entry) {
    const auto pattern = request.path / L"*";
    WIN32_FIND_DATAW data{};
    auto handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
        handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                  nullptr, 0);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            const auto attributes = GetFileAttributesW(request.path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return {};
            }
        }
        return {.error = make_error(request.path, error)};
    }

    EnumerationResult result;
    std::size_t count = 0;
    bool has_entry = true;
    while (has_entry) {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L".." && !core::is_internal_filename(name)) {
            if (request.maximum_entries != 0 && count >= request.maximum_entries) {
                result.truncated = true;
                break;
            }

            core::DirectoryEntry entry;
            const bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.kind = is_directory ? core::EntryKind::directory : core::EntryKind::file;
            entry.state = core::EntryState::metadata_ready;
            entry.name_utf8 = wide_to_utf8(name);
            const auto path = request.path / data.cFileName;
            entry.path_utf8 = path_utf8(path);
            entry.source_revision_utf8 =
                source_revision_for_path(path, is_directory, birth_revision(data.ftCreationTime));
            entry.id = stable_entry_id(entry.path_utf8);
            entry.size_bytes = is_directory
                                   ? 0
                                   : (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) |
                                         static_cast<std::uint64_t>(data.nFileSizeLow);
            entry.modified_unix_ns = windows_detail::filetime_to_unix_ns(data.ftLastWriteTime);
            if (!on_entry(std::move(entry))) {
                result.error.kind = catalog::CatalogErrorKind::cancelled;
                break;
            }
            ++count;
        }
        has_entry = FindNextFileW(handle, &data) != FALSE;
    }

    const auto final_error = GetLastError();
    FindClose(handle);
    if (!result.error && !result.truncated && !has_entry && final_error != ERROR_NO_MORE_FILES) {
        result.error = make_error(request.path, final_error);
    }
    return result;
}

struct RecursiveHandleCloser {
    void operator()(void *handle) const noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle));
        }
    }
};
using RecursiveHandle = std::unique_ptr<void, RecursiveHandleCloser>;

RecursiveHandle open_relative(const HANDLE parent, const std::wstring &leaf,
                              const bool directory, DWORD &error) {
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto create = std::bit_cast<decltype(&NtCreateFile)>(
        GetProcAddress(module, "NtCreateFile"));
    const auto to_error = std::bit_cast<decltype(&RtlNtStatusToDosError)>(
        GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!create || !to_error || leaf.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t)) {
        error = ERROR_NOT_SUPPORTED;
        return {};
    }
    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(leaf.data());
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status{};
    HANDLE handle{};
    const auto result = create(
        &handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE | (directory ? FILE_LIST_DIRECTORY : 0U),
        &attributes, &status, nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                       FILE_OPEN_FOR_BACKUP_INTENT | (directory ? FILE_DIRECTORY_FILE : 0U),
        nullptr, 0);
    if (result < 0) {
        error = to_error(result);
        return {};
    }
    return RecursiveHandle(handle);
}

bool inspect_recursive_handle(const HANDLE handle, BY_HANDLE_FILE_INFORMATION &information,
                              DWORD &error) {
    if (!GetFileInformationByHandle(handle, &information)) {
        error = GetLastError();
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = ERROR_CANT_ACCESS_FILE;
        return false;
    }
    return true;
}

RecursiveHandle open_recursive_root(RecursiveTraversal &scan, std::filesystem::path &path,
                                    DWORD &error) {
    std::error_code path_error;
    path = std::filesystem::absolute(path, path_error).lexically_normal();
    if (path_error || !path.is_absolute()) {
        error = ERROR_INVALID_NAME;
        return {};
    }
    auto base = path.root_path();
    const auto relative = path.relative_path();
    auto component = relative.begin();
    // A UNC server is not an openable directory; the explicitly selected share is the anchor.
    if (path.root_name().native().size() > 2) {
        if (component == relative.end() || path.native().starts_with(L"\\\\?\\") ||
            path.native().starts_with(L"\\\\.\\")) {
            error = ERROR_INVALID_NAME;
            return {};
        }
        base /= *component++;
    }
    auto raw = CreateFileW(base.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return {};
    }
    RecursiveHandle current(raw);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!inspect_recursive_handle(current.get(), information, error)) {
        return {};
    }
    for (; component != relative.end(); ++component) {
        if (!scan.tick()) {
            error = ERROR_TIMEOUT;
            return {};
        }
        if (component->empty() || *component == L".") {
            continue;
        }
        auto next = open_relative(current.get(), component->native(), true, error);
        if (!next || !inspect_recursive_handle(next.get(), information, error)) {
            return {};
        }
        current = std::move(next);
    }
    return current;
}

void walk_recursive(RecursiveTraversal &scan, const HANDLE directory,
                    const std::filesystem::path &path, const DWORD volume,
                    const std::uint32_t depth) {
    // One fixed buffer per depth, no pending-directory queue or whole-tree metadata cache.
    auto storage = std::make_unique<std::array<std::byte, 64U * 1024U>>();
    auto &buffer = *storage;
    while (scan.tick()) {
        buffer.fill(std::byte{});
        if (!GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer.data(),
                                          static_cast<DWORD>(buffer.size()))) {
            const auto code = GetLastError();
            if (code != ERROR_NO_MORE_FILES) {
                scan.skip(make_error(path, code), depth == 0);
            }
            return;
        }
        std::size_t offset{};
        for (;;) {
            if (!scan.tick()) {
                return;
            }
            constexpr auto header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if (offset > buffer.size() - header) {
                scan.skip(make_error(path, ERROR_INVALID_DATA), depth == 0);
                return;
            }
            const auto *record = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(buffer.data() + offset);
            if (record->FileNameLength % sizeof(wchar_t) != 0 ||
                record->FileNameLength > buffer.size() - offset - header) {
                scan.skip(make_error(path, ERROR_INVALID_DATA), depth == 0);
                return;
            }
            const std::wstring name(record->FileName, record->FileNameLength / sizeof(wchar_t));
            const auto child_path = path / name;
            if (!recursive_name_excluded(name) &&
                (record->FileAttributes & FILE_ATTRIBUTE_HIDDEN) == 0) {
                const auto directory_entry = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if ((record->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    scan.skip();
                } else if (!directory_entry || scan.enter(depth + 1)) {
                    DWORD error{};
                    auto child = open_relative(directory, name, directory_entry, error);
                    BY_HANDLE_FILE_INFORMATION information{};
                    if (!child || !inspect_recursive_handle(child.get(), information, error)) {
                        scan.skip(make_error(child_path, error));
                    } else if ((information.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
                        // The object's hidden flag may have changed after enumeration.
                    } else if (information.dwVolumeSerialNumber != volume ||
                               ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
                                   directory_entry) {
                        scan.skip();
                    } else if (directory_entry) {
                        walk_recursive(scan, child.get(), child_path, volume, depth + 1);
                    } else {
                        FILE_BASIC_INFO basic{};
                        if (!GetFileInformationByHandleEx(child.get(), FileBasicInfo, &basic, sizeof(basic))) {
                            scan.skip(make_error(child_path, GetLastError()));
                        } else {
                            core::DirectoryEntry entry;
                            entry.state = core::EntryState::metadata_ready;
                            entry.name_utf8 = wide_to_utf8(name);
                            entry.path_utf8 = path_utf8(child_path);
                            entry.id = stable_entry_id(entry.path_utf8);
                            entry.size_bytes = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                                               information.nFileSizeLow;
                            entry.modified_unix_ns = windows_detail::filetime_to_unix_ns(basic.LastWriteTime);
                            entry.source_revision_utf8 = windows_detail::preferred_revision(
                                windows_detail::query_file_identity(child.get(), basic.ChangeTime));
                            if (!scan.emit(std::move(entry))) {
                                return;
                            }
                        }
                    }
                }
            }
            if (record->NextEntryOffset == 0) {
                break;
            }
            if (record->NextEntryOffset < header + record->FileNameLength ||
                record->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) != 0 ||
                record->NextEntryOffset > buffer.size() - offset) {
                scan.skip(make_error(path, ERROR_INVALID_DATA), depth == 0);
                return;
            }
            offset += record->NextEntryOffset;
        }
    }
}

} // namespace

EnumerationResult enumerate_directory(const catalog::CatalogRequest &request,
                                       const EntryCallback &on_entry,
                                       const ProgressCallback &on_progress) {
    if (request.recursive) {
        if (!catalog::valid_recursive_request(request)) {
            return {.error = {.kind = catalog::CatalogErrorKind::io_error,
                              .message_utf8 = "Invalid recursive catalog bounds"},
                    .truncated = true, .root_failed = true};
        }
        RecursiveTraversal scan(request, on_entry, on_progress);
        if (!scan.enter(0)) {
            return std::move(scan.result);
        }
        auto path = request.path;
        DWORD error{};
        auto root = open_recursive_root(scan, path, error);
        BY_HANDLE_FILE_INFORMATION information{};
        if (!root || !inspect_recursive_handle(root.get(), information, error)) {
            if (!scan.result.error) {
                scan.skip(make_error(path, error), true);
            }
        } else {
            walk_recursive(scan, root.get(), path, information.dwVolumeSerialNumber, 0);
        }
        return std::move(scan.result);
    }
    auto file_ids = enumerate_file_ids(request, on_entry);
    if (file_ids.supported) {
        return std::move(file_ids.result);
    }
    return enumerate_find_first(request, on_entry);
}

EntryQueryResult query_entry(const std::filesystem::path &path) {
    const auto name = path.filename().native();
    if (name.empty() || name == L"." || name == L".." || core::is_internal_filename(name)) {
        return {.error = {.kind = catalog::CatalogErrorKind::not_found,
                          .message_utf8 = path_utf8(path) + ": object is not available"}};
    }

    const RecursiveHandle handle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        return {.error = make_error(path, GetLastError())};
    }
    if (GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return {.error = make_error(path, ERROR_NOT_SUPPORTED)};
    }
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
        return {.error = make_error(path, GetLastError())};
    }
    if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return {.error = make_error(path, ERROR_CANT_ACCESS_FILE)};
    }
    const auto is_directory = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_directory != (standard.Directory != FALSE) || standard.EndOfFile.QuadPart < 0 ||
        standard.DeletePending) {
        return {.error = make_error(path, ERROR_FILE_INVALID)};
    }
    const auto identity = windows_detail::query_file_identity(handle.get(), basic.ChangeTime);
    auto revision = windows_detail::preferred_revision(identity);
    if (revision.empty()) {
        return {.error = make_error(path, identity.error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED
                                                                         : identity.error)};
    }

    // Exact probes authorize operations: directory-record caches are not a handle snapshot.
    core::DirectoryEntry entry;
    entry.kind = is_directory ? core::EntryKind::directory : core::EntryKind::file;
    entry.state = core::EntryState::metadata_ready;
    entry.name_utf8 = wide_to_utf8(name);
    entry.path_utf8 = path_utf8(path);
    entry.source_revision_utf8 = std::move(revision);
    entry.id = stable_entry_id(entry.path_utf8);
    entry.size_bytes = is_directory ? 0 : static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    entry.modified_unix_ns = windows_detail::filetime_to_unix_ns(basic.LastWriteTime);
    return {.entry = std::move(entry)};
}

} // namespace vove::platform::detail
