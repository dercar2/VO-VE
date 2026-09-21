#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <userenv.h>

#include "vove/worker/windows_runtime_access.hpp"

#include <array>
#include <memory>
#include <string_view>

namespace vove::worker {
namespace {

struct CloseHandleDeleter {
    void operator()(void *value) const noexcept {
        if (value)
            CloseHandle(value);
    }
};
struct LocalFreeDeleter {
    void operator()(void *value) const noexcept {
        if (value)
            LocalFree(value);
    }
};
struct FreeSidDeleter {
    void operator()(void *value) const noexcept {
        if (value)
            FreeSid(value);
    }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;
constexpr DWORD kReadExecute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;

enum class ExistingAccess { missing, granted, denied, invalid };

[[nodiscard]] ExistingAccess explicit_access(PACL acl, PSID sid) {
    if (!acl)
        return ExistingAccess::granted;
    DWORD mask{};
    DWORD denied{};
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void *entry{};
        if (!GetAce(acl, index, &entry))
            return ExistingAccess::invalid;
        const auto *header = static_cast<const ACE_HEADER *>(entry);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
            continue;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(entry);
            if (EqualSid(sid, const_cast<DWORD *>(&allowed->SidStart)))
                mask |= allowed->Mask;
        } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            const auto *deny = static_cast<const ACCESS_DENIED_ACE *>(entry);
            if (EqualSid(sid, const_cast<DWORD *>(&deny->SidStart)))
                denied |= deny->Mask;
        }
    }
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                            FILE_ALL_ACCESS};
    MapGenericMask(&mask, &mapping);
    MapGenericMask(&denied, &mapping);
    if ((denied & kReadExecute) != 0)
        return ExistingAccess::denied;
    return (mask & kReadExecute) == kReadExecute ? ExistingAccess::granted
                                                 : ExistingAccess::missing;
}

[[nodiscard]] DWORD grant_one(const std::filesystem::path &path, PSID sid, const bool directory) {
    const DWORD flags =
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0U);
    const auto open = [&](DWORD access) {
        const auto raw = CreateFileW(path.c_str(), access,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, flags, nullptr);
        return Handle(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
    };
    auto handle = open(READ_CONTROL | FILE_READ_ATTRIBUTES);
    if (!handle)
        return GetLastError();
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)))
        return GetLastError();
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return ERROR_INVALID_DATA;
    PACL existing{};
    PSECURITY_DESCRIPTOR raw_descriptor{};
    auto error = GetSecurityInfo(handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                 nullptr, &existing, nullptr, &raw_descriptor);
    LocalMemory descriptor(raw_descriptor);
    if (error != ERROR_SUCCESS)
        return error;
    auto access_state = explicit_access(existing, sid);
    if (access_state == ExistingAccess::granted)
        return ERROR_SUCCESS;
    if (access_state == ExistingAccess::denied)
        return ERROR_ACCESS_DENIED;
    if (access_state == ExistingAccess::invalid)
        return ERROR_INVALID_ACL;
    handle = open(READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES);
    if (!handle)
        return GetLastError();
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)))
        return GetLastError();
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return ERROR_INVALID_DATA;
    raw_descriptor = nullptr;
    error = GetSecurityInfo(handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                            nullptr, &existing, nullptr, &raw_descriptor);
    descriptor.reset(raw_descriptor);
    if (error != ERROR_SUCCESS)
        return error;
    access_state = explicit_access(existing, sid);
    if (access_state == ExistingAccess::granted)
        return ERROR_SUCCESS;
    if (access_state == ExistingAccess::denied)
        return ERROR_ACCESS_DENIED;
    if (access_state == ExistingAccess::invalid)
        return ERROR_INVALID_ACL;
    // Preserve existing grants/denials; grant no write rights and no inherited rights.
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = kReadExecute;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    access.Trustee.ptstrName = static_cast<wchar_t *>(sid);
    PACL updated{};
    error = SetEntriesInAclW(1, &access, existing, &updated);
    LocalMemory updated_owner(updated);
    if (error != ERROR_SUCCESS)
        return error;
    return SetSecurityInfo(handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                           nullptr, updated, nullptr);
}

} // namespace

RuntimeAccessResult prepare_windows_worker_runtime(const std::filesystem::path &directory) {
    const auto raw_root = CreateFileW(
        directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw_root == INVALID_HANDLE_VALUE)
        return {GetLastError(), "runtime"};
    Handle root(raw_root);
    FILE_ATTRIBUTE_TAG_INFO root_attributes{};
    if (!GetFileInformationByHandleEx(root.get(), FileAttributeTagInfo, &root_attributes,
                                     sizeof(root_attributes)))
        return {GetLastError(), "runtime"};
    if ((root_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (root_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {ERROR_INVALID_DATA, "runtime"};
    PSID raw_sid{};
    const auto derived = DeriveAppContainerSidFromAppContainerName(kWorkerProfileName, &raw_sid);
    if (FAILED(derived))
        return {static_cast<std::uint32_t>(derived), "profile"};
    std::unique_ptr<void, FreeSidDeleter> sid(raw_sid);
    constexpr std::array names{"vove-raster-worker.exe", "vove-xcf-worker.exe",
                               "vove-document-worker.exe", "vove-cdr-worker.exe",
                               "vove-svg-worker.exe", "Qt6Core.dll", "Qt6Gui.dll",
                               "libc++.dll", "libunwind.dll"};
    bool found{};
    for (const auto *name : names) {
        const auto path = directory / name;
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND)
                continue;
            return {error, name};
        }
        found = true;
        const auto error = grant_one(path, sid.get(), false);
        if (error != ERROR_SUCCESS)
            return {error, name};
    }
    const auto image_formats = directory / L"imageformats";
    const auto image_formats_attributes = GetFileAttributesW(image_formats.c_str());
    if (image_formats_attributes != INVALID_FILE_ATTRIBUTES) {
        const auto jpeg_plugin = image_formats / L"qjpeg.dll";
        const auto plugin_attributes = GetFileAttributesW(jpeg_plugin.c_str());
        if (plugin_attributes == INVALID_FILE_ATTRIBUTES)
            return {GetLastError(), "imageformats/qjpeg.dll"};
        auto error = grant_one(jpeg_plugin, sid.get(), false);
        if (error != ERROR_SUCCESS)
            return {error, "imageformats/qjpeg.dll"};
        error = grant_one(image_formats, sid.get(), true);
        if (error != ERROR_SUCCESS)
            return {error, "imageformats"};
    } else {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return {error, "imageformats"};
    }
    if (!found)
        return {ERROR_FILE_NOT_FOUND, "runtime"};
    return {grant_one(directory, sid.get(), true), "runtime"};
}

} // namespace vove::worker
