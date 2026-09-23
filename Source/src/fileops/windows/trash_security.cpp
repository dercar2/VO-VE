#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "trash_security.hpp"

#include "windows/file_identity.hpp"
#include "windows/file_time.hpp"

#include <cstddef>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
#include <atomic>
#endif

namespace vove::fileops::detail {
namespace {

#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
std::atomic<TrashDirectoryCreateHook> directory_create_hook{};
std::atomic<LegacyTrashOwnerObservationHook> legacy_owner_observation_hook{};
#endif

class ScopedHandle final {
  public:
    explicit ScopedHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~ScopedHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(value_));
        }
    }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;
    ScopedHandle(ScopedHandle &&other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    ScopedHandle &operator=(ScopedHandle &&other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) {
                static_cast<void>(CloseHandle(value_));
            }
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }

  private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class ScopedLocal final {
  public:
    explicit ScopedLocal(HLOCAL value = nullptr) noexcept : value_(value) {}
    ~ScopedLocal() {
        if (value_ != nullptr) {
            static_cast<void>(LocalFree(value_));
        }
    }
    ScopedLocal(const ScopedLocal &) = delete;
    ScopedLocal &operator=(const ScopedLocal &) = delete;
    [[nodiscard]] HLOCAL get() const noexcept {
        return value_;
    }

  private:
    HLOCAL value_{};
};

OperationStatus status_from_error(const DWORD error) noexcept {
    if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
        return OperationStatus::permission_denied;
    }
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return OperationStatus::not_found;
    }
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        return OperationStatus::file_in_use;
    }
    return OperationStatus::io_error;
}

TrashSecurityResult security_failure(const OperationStatus status, std::string detail) {
    return {.status = status,
            .snapshot = {},
            .original_sddl_utf8 = {},
            .detail_utf8 = std::move(detail)};
}

TrashSecurityResult security_success(SourceSnapshot snapshot, std::string original_sddl = {}) {
    return {.status = OperationStatus::success,
            .snapshot = std::move(snapshot),
            .original_sddl_utf8 = std::move(original_sddl),
            .detail_utf8 = {}};
}

std::string windows_error_detail(const char *prefix, const DWORD error) {
    return std::string(prefix) + ": Windows error " + std::to_string(error);
}

std::optional<std::string> utf8_from_wide(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required, nullptr,
                            nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> wide_from_utf8(const std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) != required) {
        return std::nullopt;
    }
    return result;
}

struct CurrentUserSid {
    std::vector<std::byte> token_user;
    PSID sid{};
    std::wstring text;
};

std::optional<CurrentUserSid> current_user_sid(std::string &detail) {
    HANDLE raw_token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == FALSE) {
        detail = windows_error_detail("current user token is unavailable", GetLastError());
        return std::nullopt;
    }
    ScopedHandle token(raw_token);
    DWORD bytes{};
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes));
    if (bytes == 0) {
        detail = windows_error_detail("current user SID size is unavailable", GetLastError());
        return std::nullopt;
    }
    CurrentUserSid result;
    result.token_user.resize(bytes);
    if (GetTokenInformation(token.get(), TokenUser, result.token_user.data(), bytes, &bytes) ==
        FALSE) {
        detail = windows_error_detail("current user SID is unavailable", GetLastError());
        return std::nullopt;
    }
    result.sid = reinterpret_cast<TOKEN_USER *>(result.token_user.data())->User.Sid;
    LPWSTR text{};
    if (ConvertSidToStringSidW(result.sid, &text) == FALSE) {
        detail = windows_error_detail("current user SID cannot be encoded", GetLastError());
        return std::nullopt;
    }
    ScopedLocal text_owner(text);
    result.text = text;
    return result;
}

std::optional<ScopedHandle> open_verified_file(const std::filesystem::path &path,
                                               const SourceSnapshot &expected, const DWORD access,
                                               std::string &detail,
                                               const bool allow_revision_advance = false,
                                               const bool directory = false) {
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL);
    ScopedHandle handle(CreateFileW(path.c_str(), access,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, flags, nullptr));
    if (!handle.valid()) {
        detail = windows_error_detail("trash security target could not be opened", GetLastError());
        return std::nullopt;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const auto expected_directory = directory ? FILE_ATTRIBUTE_DIRECTORY : 0U;
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != expected_directory) {
        detail = directory ? "trash security target is not a physical directory"
                           : "trash security target is not a physical regular file";
        return std::nullopt;
    }
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
        GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE ||
        standard.EndOfFile.QuadPart < 0) {
        detail = windows_error_detail("trash security identity is unavailable", GetLastError());
        return std::nullopt;
    }
    const auto identity =
        platform::windows_detail::query_file_identity(handle.get(), basic.ChangeTime);
    const auto &actual =
        platform::windows_detail::matching_revision(identity, expected.source_revision_utf8);
    const auto same_revision = actual == expected.source_revision_utf8;
    const auto same_identity = allow_revision_advance && !actual.empty() &&
                               same_object_identity(actual, expected.source_revision_utf8);
    const auto size_bytes =
        directory ? 0U : static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    const auto same_modified_time = platform::windows_detail::filetime_to_unix_ns(
                                        basic.LastWriteTime) == expected.modified_unix_ns;
    if ((!same_revision && !same_identity) || size_bytes != expected.size_bytes ||
        (!same_modified_time && !same_identity)) {
        detail = "trash security target changed after it was verified";
        return std::nullopt;
    }
    return handle;
}

std::optional<SourceSnapshot> snapshot_from_handle(const HANDLE handle, std::string &detail,
                                                   const bool directory = false) {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE) {
        detail = windows_error_detail("trash security snapshot is unavailable", GetLastError());
        return std::nullopt;
    }
    const auto identity = platform::windows_detail::query_file_identity(handle, basic.ChangeTime);
    const auto revision = platform::windows_detail::preferred_revision(identity);
    if (revision.empty() || standard.EndOfFile.QuadPart < 0) {
        detail = "trash security snapshot lacks a strong identity";
        return std::nullopt;
    }
    return SourceSnapshot{
        .size_bytes = directory ? 0U : static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
        .modified_unix_ns = platform::windows_detail::filetime_to_unix_ns(basic.LastWriteTime),
        .source_revision_utf8 = revision};
}

bool same_acl(const PACL actual, const PACL expected) {
    if (actual == nullptr || expected == nullptr || actual->AclRevision != expected->AclRevision ||
        actual->AceCount != expected->AceCount) {
        return false;
    }
    for (DWORD index{}; index < actual->AceCount; ++index) {
        void *raw_actual{};
        void *raw_expected{};
        if (GetAce(actual, index, &raw_actual) == FALSE ||
            GetAce(expected, index, &raw_expected) == FALSE) {
            return false;
        }
        const auto *actual_header = static_cast<const ACE_HEADER *>(raw_actual);
        const auto *expected_header = static_cast<const ACE_HEADER *>(raw_expected);
        if (actual_header->AceSize < sizeof(ACE_HEADER) ||
            actual_header->AceSize != expected_header->AceSize ||
            std::memcmp(raw_actual, raw_expected, actual_header->AceSize) != 0) {
            return false;
        }
    }
    return true;
}

bool verify_exact_owned_dacl(const HANDLE handle, const std::wstring &descriptor_text,
                             const std::wstring &sid_text, std::string &detail,
                             const bool legacy_administrators_owner = false) {
    PSECURITY_DESCRIPTOR raw_expected{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptor_text.c_str(), SDDL_REVISION_1, &raw_expected, nullptr) == FALSE) {
        detail =
            windows_error_detail("expected trash security descriptor is invalid", GetLastError());
        return false;
    }
    ScopedLocal expected(raw_expected);
    BOOL expected_present{};
    BOOL expected_defaulted{};
    PACL expected_dacl{};
    SECURITY_DESCRIPTOR_CONTROL expected_control{};
    DWORD expected_revision{};
    if (GetSecurityDescriptorDacl(raw_expected, &expected_present, &expected_dacl,
                                  &expected_defaulted) == FALSE ||
        expected_present == FALSE || expected_dacl == nullptr ||
        IsValidAcl(expected_dacl) == FALSE ||
        GetSecurityDescriptorControl(raw_expected, &expected_control, &expected_revision) ==
            FALSE) {
        detail = "expected trash DACL is unavailable";
        return false;
    }

    PSID owner{};
    PACL actual_dacl{};
    PSECURITY_DESCRIPTOR raw_actual{};
    const auto error = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                       &owner, nullptr, &actual_dacl, nullptr, &raw_actual);
    if (error != ERROR_SUCCESS) {
        detail = windows_error_detail("trash security could not be verified", error);
        return false;
    }
    ScopedLocal actual(raw_actual);
#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
    if (legacy_administrators_owner) {
        if (const auto hook = legacy_owner_observation_hook.load(std::memory_order_acquire)) {
            hook(&owner);
        }
    }
#endif
    const auto user = current_user_sid(detail);
    const auto owner_matches = owner != nullptr && user &&
        (legacy_administrators_owner ? IsWellKnownSid(owner, WinBuiltinAdministratorsSid)
                                     : EqualSid(owner, user->sid)) != FALSE;
    if (!user || !owner_matches ||
        CompareStringOrdinal(user->text.c_str(), -1, sid_text.c_str(), -1, TRUE) != CSTR_EQUAL) {
        detail = user ? "trash object has a foreign owner" : std::move(detail);
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    const auto expected_protected = (expected_control & SE_DACL_PROTECTED) != 0;
    if (actual_dacl == nullptr || IsValidAcl(actual_dacl) == FALSE ||
        GetSecurityDescriptorControl(raw_actual, &control, &revision) == FALSE ||
        ((control & SE_DACL_PROTECTED) != 0) != expected_protected ||
        !same_acl(actual_dacl, expected_dacl)) {
        LPWSTR actual_text{};
        LPWSTR expected_text{};
        static_cast<void>(ConvertSecurityDescriptorToStringSecurityDescriptorW(
            raw_actual, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &actual_text, nullptr));
        static_cast<void>(ConvertSecurityDescriptorToStringSecurityDescriptorW(
            raw_expected, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &expected_text, nullptr));
        ScopedLocal actual_text_owner(actual_text);
        ScopedLocal expected_text_owner(expected_text);
        const auto actual_utf8 =
            actual_text != nullptr ? utf8_from_wide(actual_text) : std::nullopt;
        const auto expected_utf8 =
            expected_text != nullptr ? utf8_from_wide(expected_text) : std::nullopt;
        detail = "trash object DACL differs from its exact saved policy";
        if (actual_utf8 && expected_utf8) {
            detail += ": actual=" + *actual_utf8 + " expected=" + *expected_utf8;
        }
        return false;
    }
    return true;
}

TrashSecurityResult apply_dacl(const std::filesystem::path &path, const SourceSnapshot &expected,
                               const std::wstring &sddl, const bool directory,
                               const bool allow_revision_advance) {
    std::string detail;
    auto handle = open_verified_file(path, expected, READ_CONTROL | WRITE_DAC, detail,
                                     allow_revision_advance, directory);
    if (!handle) {
        return security_failure(OperationStatus::conflict, std::move(detail));
    }
    PSECURITY_DESCRIPTOR raw_descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                             &raw_descriptor, nullptr) == FALSE) {
        const auto error = GetLastError();
        return security_failure(
            status_from_error(error),
            windows_error_detail("trash security descriptor is invalid", error));
    }
    ScopedLocal descriptor(raw_descriptor);
    BOOL present{};
    BOOL defaulted{};
    PACL dacl{};
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    if (GetSecurityDescriptorDacl(raw_descriptor, &present, &dacl, &defaulted) == FALSE ||
        present == FALSE || dacl == nullptr ||
        GetSecurityDescriptorControl(raw_descriptor, &control, &revision) == FALSE) {
        const auto error = GetLastError();
        return security_failure(status_from_error(error),
                                windows_error_detail("trash DACL is unavailable", error));
    }
    SECURITY_INFORMATION information = DACL_SECURITY_INFORMATION;
    information |= static_cast<SECURITY_INFORMATION>((control & SE_DACL_PROTECTED) != 0
                                                         ? PROTECTED_DACL_SECURITY_INFORMATION
                                                         : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const auto error = SetSecurityInfo(handle->get(), SE_FILE_OBJECT, information, nullptr, nullptr,
                                       dacl, nullptr);
    if (error != ERROR_SUCCESS) {
        return security_failure(status_from_error(error),
                                windows_error_detail("trash DACL could not be applied", error));
    }
    auto snapshot = snapshot_from_handle(handle->get(), detail, directory);
    if (!snapshot) {
        return security_failure(OperationStatus::unknown_outcome, std::move(detail));
    }
    return security_success(std::move(*snapshot));
}

} // namespace

TrashDirectoryLease::TrashDirectoryLease(void *handle) noexcept : handle_(handle) {}

TrashDirectoryLease::~TrashDirectoryLease() {
    reset();
}

TrashDirectoryLease::TrashDirectoryLease(TrashDirectoryLease &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

TrashDirectoryLease &TrashDirectoryLease::operator=(TrashDirectoryLease &&other) noexcept {
    if (this != &other) {
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

bool TrashDirectoryLease::valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

void TrashDirectoryLease::reset() noexcept {
    if (valid()) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
    }
    handle_ = nullptr;
}

TrashSecurityResult capture_trash_security(const std::filesystem::path &path,
                                           const SourceSnapshot &expected, const bool directory,
                                           const bool allow_foreign_file_owner) {
    std::string detail;
    auto handle = open_verified_file(path, expected, READ_CONTROL, detail, directory, directory);
    if (!handle) {
        return security_failure(OperationStatus::conflict, std::move(detail));
    }
    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR raw_descriptor{};
    const auto error = GetSecurityInfo(handle->get(), SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                           DACL_SECURITY_INFORMATION,
                                       &owner, nullptr, &dacl, nullptr, &raw_descriptor);
    if (error != ERROR_SUCCESS) {
        return security_failure(status_from_error(error),
                                windows_error_detail("source security could not be read", error));
    }
    ScopedLocal descriptor(raw_descriptor);
    const auto user = current_user_sid(detail);
    const auto foreign_owner = user && owner != nullptr && EqualSid(owner, user->sid) == FALSE;
    if (!user || owner == nullptr || (foreign_owner && (!allow_foreign_file_owner || directory))) {
        return security_failure(OperationStatus::permission_denied,
                                user ? "VO-VE Trash accepts only files owned by the current user"
                                     : std::move(detail));
    }
    BOOL dacl_present{};
    BOOL dacl_defaulted{};
    PACL descriptor_dacl{};
    if (GetSecurityDescriptorDacl(raw_descriptor, &dacl_present, &descriptor_dacl,
                                  &dacl_defaulted) == FALSE ||
        dacl_present == FALSE || descriptor_dacl == nullptr ||
        IsValidAcl(descriptor_dacl) == FALSE) {
        return security_failure(OperationStatus::invalid_request,
                                "VO-VE Trash does not accept a source with a NULL DACL");
    }
    LPWSTR raw_sddl{};
    const SECURITY_INFORMATION information = foreign_owner
        ? OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
        : DACL_SECURITY_INFORMATION;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(raw_descriptor, SDDL_REVISION_1,
                                                             information, &raw_sddl,
                                                             nullptr) == FALSE) {
        const auto conversion_error = GetLastError();
        return security_failure(
            status_from_error(conversion_error),
            windows_error_detail("source DACL could not be encoded", conversion_error));
    }
    ScopedLocal sddl(raw_sddl);
    const auto encoded = utf8_from_wide(raw_sddl);
    if (!encoded || encoded->empty()) {
        return security_failure(OperationStatus::io_error,
                                "source DACL could not be encoded as UTF-8");
    }
    if (foreign_owner && !validate_preserved_trash_security(*encoded, detail)) {
        return security_failure(OperationStatus::unsupported, std::move(detail));
    }
    if (foreign_owner) {
        FILE_STANDARD_INFO standard{};
        if (!GetFileInformationByHandleEx(handle->get(), FileStandardInfo, &standard, sizeof(standard)) ||
            standard.NumberOfLinks != 1 || standard.DeletePending) {
            return security_failure(OperationStatus::unsupported,
                                    "preserved Trash accepts only ordinary single-link files");
        }
    }
    auto snapshot = snapshot_from_handle(handle->get(), detail, directory);
    if (!snapshot) return security_failure(OperationStatus::unknown_outcome, std::move(detail));
    auto result = security_success(std::move(*snapshot), *encoded);
    result.payload_policy = foreign_owner ? TrashPayloadPolicy::preserve_permissions
                                         : TrashPayloadPolicy::strict;
    return result;
}

bool validate_preserved_trash_security(const std::string &sddl_utf8, std::string &detail) {
    if (sddl_utf8.empty() || sddl_utf8.size() > kMaximumTrashSecurityBaselineBytes ||
        sddl_utf8.find('\0') != std::string::npos) {
        detail = "preserved Trash security baseline is empty or exceeds its limit";
        return false;
    }
    const auto text = wide_from_utf8(sddl_utf8);
    PSECURITY_DESCRIPTOR raw{};
    if (!text || !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                     text->c_str(), SDDL_REVISION_1, &raw, nullptr)) {
        detail = "preserved Trash security baseline is invalid";
        return false;
    }
    ScopedLocal descriptor(raw);
    PSID owner{}, group{};
    PACL dacl{};
    BOOL defaulted{}, present{};
    if (!GetSecurityDescriptorOwner(raw, &owner, &defaulted) || !owner || !IsValidSid(owner) ||
        !GetSecurityDescriptorGroup(raw, &group, &defaulted) || !group || !IsValidSid(group) ||
        !GetSecurityDescriptorDacl(raw, &present, &dacl, &defaulted) || !present ||
        !dacl || !IsValidAcl(dacl)) {
        detail = "preserved Trash baseline requires owner, group and non-NULL DACL";
        return false;
    }
    return true;
}

bool verify_preserved_trash_security_handle(void *handle, const std::string &sddl_utf8,
                                           std::string &detail) {
    if (!validate_preserved_trash_security(sddl_utf8, detail)) return false;
    const auto text = wide_from_utf8(sddl_utf8);
    PSECURITY_DESCRIPTOR expected{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            text->c_str(), SDDL_REVISION_1, &expected, nullptr)) return false;
    ScopedLocal expected_scope(expected);
    PSECURITY_DESCRIPTOR actual{};
    const auto error = GetSecurityInfo(handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &actual);
    if (error != ERROR_SUCCESS) {
        detail = windows_error_detail("preserved Trash security could not be read", error);
        return false;
    }
    ScopedLocal actual_scope(actual);
    PSID expected_owner{}, actual_owner{}, expected_group{}, actual_group{};
    PACL expected_dacl{}, actual_dacl{};
    BOOL defaulted{}, present{};
    SECURITY_DESCRIPTOR_CONTROL expected_control{}, actual_control{};
    DWORD revision{};
    const auto read = [&](PSECURITY_DESCRIPTOR sd, PSID &owner, PSID &group, PACL &dacl,
                          SECURITY_DESCRIPTOR_CONTROL &control) {
        return GetSecurityDescriptorOwner(sd, &owner, &defaulted) && owner && IsValidSid(owner) &&
               GetSecurityDescriptorGroup(sd, &group, &defaulted) && group && IsValidSid(group) &&
               GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present && dacl &&
               IsValidAcl(dacl) && GetSecurityDescriptorControl(sd, &control, &revision);
    };
    constexpr auto mask = SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED | SE_DACL_AUTO_INHERIT_REQ;
    if (!read(expected, expected_owner, expected_group, expected_dacl, expected_control) ||
        !read(actual, actual_owner, actual_group, actual_dacl, actual_control) ||
        !EqualSid(expected_owner, actual_owner) || !EqualSid(expected_group, actual_group) ||
        (expected_control & mask) != (actual_control & mask) ||
        !same_acl(actual_dacl, expected_dacl)) {
        detail = "Trash payload owner or DACL differs from its preserved baseline";
        return false;
    }
    return true;
}

TrashSecurityResult verify_preserved_trash_payload(const std::filesystem::path &path,
    const SourceSnapshot &expected, const std::string &sddl_utf8, const bool allow_revision_advance) {
    std::string detail;
    auto handle = open_verified_file(path, expected, READ_CONTROL, detail, allow_revision_advance);
    if (!handle) return security_failure(OperationStatus::conflict, std::move(detail));
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle->get(), FileStandardInfo, &standard, sizeof(standard)) ||
        standard.NumberOfLinks != 1 || standard.DeletePending ||
        !verify_preserved_trash_security_handle(handle->get(), sddl_utf8, detail)) {
        return security_failure(OperationStatus::permission_denied,
            detail.empty() ? "preserved Trash payload is not an ordinary single-link file" : std::move(detail));
    }
    auto snapshot = snapshot_from_handle(handle->get(), detail);
    if (snapshot && (snapshot->size_bytes != expected.size_bytes ||
                     snapshot->modified_unix_ns != expected.modified_unix_ns)) {
        return security_failure(OperationStatus::source_changed,
                                "preserved Trash payload content metadata changed");
    }
    return snapshot ? security_success(std::move(*snapshot))
                    : security_failure(OperationStatus::unknown_outcome, std::move(detail));
}

TrashSecurityResult harden_trash_payload(const std::filesystem::path &path,
                                         const SourceSnapshot &expected, const bool directory,
                                         const bool allow_revision_advance) {
    std::string detail;
    const auto user = current_user_sid(detail);
    if (!user) {
        return security_failure(OperationStatus::io_error, std::move(detail));
    }
    return apply_dacl(path, expected, L"D:P(A;;FA;;;SY)(A;;FA;;;" + user->text + L")", directory,
                      allow_revision_advance);
}

TrashSecurityResult restore_trash_security(const std::filesystem::path &path,
                                           const SourceSnapshot &expected,
                                           const std::string &original_sddl_utf8,
                                           const bool directory,
                                           const bool allow_revision_advance) {
    const auto sddl = wide_from_utf8(original_sddl_utf8);
    if (!sddl) {
        return security_failure(OperationStatus::invalid_request,
                                "stored source DACL is not valid UTF-8");
    }
    return apply_dacl(path, expected, *sddl, directory, allow_revision_advance);
}

TrashSecurityResult verify_hardened_trash_payload(const std::filesystem::path &path,
                                                  const SourceSnapshot &expected,
                                                  const bool allow_revision_advance,
                                                  const bool directory) {
    std::string detail;
    auto handle =
        open_verified_file(path, expected, READ_CONTROL, detail, allow_revision_advance, directory);
    if (!handle) {
        return security_failure(OperationStatus::conflict, std::move(detail));
    }
    const auto user = current_user_sid(detail);
    if (!user ||
        !verify_exact_owned_dacl(handle->get(), L"D:P(A;;FA;;;SY)(A;;FA;;;" + user->text + L")",
                                 user->text, detail)) {
        return security_failure(OperationStatus::permission_denied, std::move(detail));
    }
    auto snapshot = snapshot_from_handle(handle->get(), detail, directory);
    return snapshot ? security_success(std::move(*snapshot))
                    : security_failure(OperationStatus::unknown_outcome, std::move(detail));
}

TrashSecurityResult verify_restored_trash_payload(const std::filesystem::path &path,
                                                  const SourceSnapshot &expected,
                                                  const std::string &original_sddl_utf8,
                                                  const bool allow_revision_advance,
                                                  const bool directory) {
    const auto sddl = wide_from_utf8(original_sddl_utf8);
    if (!sddl) {
        return security_failure(OperationStatus::invalid_request,
                                "stored source DACL is not valid UTF-8");
    }
    std::string detail;
    auto handle =
        open_verified_file(path, expected, READ_CONTROL, detail, allow_revision_advance, directory);
    if (!handle) {
        return security_failure(OperationStatus::conflict, std::move(detail));
    }
    const auto user = current_user_sid(detail);
    if (!user || !verify_exact_owned_dacl(handle->get(), *sddl, user->text, detail)) {
        return security_failure(OperationStatus::permission_denied, std::move(detail));
    }
    auto snapshot = snapshot_from_handle(handle->get(), detail, directory);
    return snapshot ? security_success(std::move(*snapshot))
                    : security_failure(OperationStatus::unknown_outcome, std::move(detail));
}

bool validate_restorable_trash_dacl(const std::string &sddl_utf8, std::string &detail_utf8) {
    const auto sddl = wide_from_utf8(sddl_utf8);
    if (!sddl) {
        detail_utf8 = "stored source DACL is not valid UTF-8";
        return false;
    }
    PSECURITY_DESCRIPTOR raw_descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl->c_str(), SDDL_REVISION_1,
                                                             &raw_descriptor, nullptr) == FALSE) {
        detail_utf8 = windows_error_detail("stored source DACL is invalid", GetLastError());
        return false;
    }
    ScopedLocal descriptor(raw_descriptor);
    BOOL present{};
    BOOL defaulted{};
    PACL dacl{};
    if (GetSecurityDescriptorDacl(raw_descriptor, &present, &dacl, &defaulted) == FALSE ||
        present == FALSE || dacl == nullptr || IsValidAcl(dacl) == FALSE) {
        detail_utf8 = "stored source DACL is not restorable";
        return false;
    }
    return true;
}

bool current_user_sid_text(std::wstring &sid, std::string &detail_utf8) {
    const auto user = current_user_sid(detail_utf8);
    if (!user) {
        return false;
    }
    sid = user->text;
    return true;
}

#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
void set_trash_directory_create_hook(const TrashDirectoryCreateHook hook) noexcept {
    directory_create_hook.store(hook, std::memory_order_release);
}
void set_legacy_trash_owner_observation_hook(const LegacyTrashOwnerObservationHook hook) noexcept {
    legacy_owner_observation_hook.store(hook, std::memory_order_release);
}
#endif

bool secure_owned_trash_vault(const std::filesystem::path &vault, const std::wstring &sid_text,
                              std::string &detail_utf8) {
    // TokenOwner can be a group; new private directories must belong to TokenUser explicitly.
    const auto descriptor_text = L"O:" + sid_text + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" +
                                 sid_text + L")";
    PSECURITY_DESCRIPTOR raw_descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptor_text.c_str(), SDDL_REVISION_1, &raw_descriptor, nullptr) == FALSE) {
        detail_utf8 =
            windows_error_detail("trash vault security descriptor is invalid", GetLastError());
        return false;
    }
    ScopedLocal descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                   .lpSecurityDescriptor = raw_descriptor,
                                   .bInheritHandle = FALSE};
#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
    if (const auto hook = directory_create_hook.load(std::memory_order_acquire)) {
        hook(vault, attributes.lpSecurityDescriptor);
    }
#endif
    const auto created = CreateDirectoryW(vault.c_str(), &attributes);
    const auto create_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    if (created == FALSE && create_error != ERROR_ALREADY_EXISTS) {
        detail_utf8 =
            windows_error_detail("per-user trash vault could not be created", create_error);
        return false;
    }
    // The descriptor is applied at creation. Existing or concurrently created directories are read-only.
    return pin_owned_trash_directory(vault, sid_text, detail_utf8).valid();
}

bool verify_owned_trash_vault(const std::filesystem::path &vault, const std::wstring &sid_text,
                              std::string &detail_utf8) {
    return pin_owned_trash_directory(vault, sid_text, detail_utf8).valid();
}

bool repair_empty_legacy_trash_vault(const std::filesystem::path &vault,
                                     const std::filesystem::path &canonical_vault,
                                     const std::wstring &sid_text, const std::stop_token &stop,
                                     std::string &detail_utf8) {
    if (stop.stop_requested()) {
        detail_utf8 = "legacy Trash owner repair cancelled";
        return false;
    }
    ScopedHandle handle(CreateFileW(vault.c_str(), READ_CONTROL | WRITE_OWNER |
                                      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!handle.valid()) {
        detail_utf8 = windows_error_detail("legacy Trash vault could not be opened", GetLastError());
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        detail_utf8 = "legacy Trash vault is not a physical directory";
        return false;
    }
    std::array<wchar_t, 32'768> resolved{};
    const auto length = GetFinalPathNameByHandleW(handle.get(), resolved.data(),
                                                 static_cast<DWORD>(resolved.size()), VOLUME_NAME_GUID);
    const auto expected = canonical_vault.lexically_normal().make_preferred();
    if (length == 0 || length >= resolved.size() ||
        _wcsicmp(resolved.data(), expected.c_str()) != 0) {
        detail_utf8 = "legacy Trash vault resolved outside its expected physical path";
        return false;
    }
    std::array<wchar_t, 32'768> volume{};
    std::array<wchar_t, 32> filesystem{};
    if (GetVolumePathNameW(resolved.data(), volume.data(), static_cast<DWORD>(volume.size())) == FALSE ||
        GetDriveTypeW(volume.data()) != DRIVE_FIXED ||
        GetVolumeInformationByHandleW(handle.get(), nullptr, 0, nullptr, nullptr, nullptr,
                                        filesystem.data(), static_cast<DWORD>(filesystem.size())) == FALSE ||
        _wcsicmp(filesystem.data(), L"NTFS") != 0) {
        detail_utf8 = "legacy Trash owner repair requires a local fixed NTFS volume";
        return false;
    }
    const auto policy = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + sid_text + L")";
    if (!verify_exact_owned_dacl(handle.get(), policy, sid_text, detail_utf8, true)) {
        return false;
    }
    alignas(FILE_STREAM_INFO) std::array<std::byte, 1024> streams{};
    SetLastError(ERROR_SUCCESS);
    const auto stream_result = GetFileInformationByHandleEx(
        handle.get(), FileStreamInfo, streams.data(), static_cast<DWORD>(streams.size()));
    const auto stream_error = GetLastError();
    if (stream_result != FALSE || stream_error != ERROR_HANDLE_EOF) {
        detail_utf8 = windows_error_detail(
            "legacy Trash vault has data streams or their absence could not be verified",
            stream_result != FALSE ? ERROR_NOT_SUPPORTED : stream_error);
        return false;
    }
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 4096> entries{};
    bool empty{};
    for (unsigned page = 0; page < 4; ++page) {
        if (GetFileInformationByHandleEx(handle.get(), page == 0 ? FileIdBothDirectoryRestartInfo
                                                                : FileIdBothDirectoryInfo,
                                          entries.data(), static_cast<DWORD>(entries.size())) == FALSE) {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                empty = true;
                break;
            }
            detail_utf8 = windows_error_detail("legacy Trash vault could not be enumerated", error);
            return false;
        }
        std::size_t offset{};
        for (;;) {
            if (offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileName) > entries.size()) {
                detail_utf8 = "legacy Trash vault returned invalid directory entries";
                return false;
            }
            const auto *entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(entries.data() + offset);
            const auto bytes = static_cast<std::size_t>(entry->FileNameLength);
            if (bytes % sizeof(wchar_t) != 0 ||
                bytes > entries.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
                detail_utf8 = "legacy Trash vault returned an invalid entry name";
                return false;
            }
            const std::wstring_view name(entry->FileName, bytes / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                detail_utf8 = "legacy Trash vault is not empty; owner was not changed";
                return false;
            }
            if (entry->NextEntryOffset == 0) break;
            if (entry->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
                entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) != 0 ||
                entry->NextEntryOffset > entries.size() - offset) {
                detail_utf8 = "legacy Trash vault returned an invalid entry offset";
                return false;
            }
            offset += entry->NextEntryOffset;
        }
    }
    if (!empty || stop.stop_requested()) {
        detail_utf8 = "legacy Trash owner repair cancelled or emptiness could not be verified";
        return false;
    }
    const auto user = current_user_sid(detail_utf8);
    if (!user) return false;
    // Only the vault owner changes. Concurrent child/stream creation cannot cause data deletion.
    const auto changed = SetSecurityInfo(handle.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                         user->sid, nullptr, nullptr, nullptr);
    if (changed != ERROR_SUCCESS) {
        detail_utf8 = windows_error_detail("legacy Trash vault owner could not be corrected", changed);
        return false;
    }
    if (!verify_exact_owned_dacl(handle.get(), policy, sid_text, detail_utf8)) {
        detail_utf8 = "legacy Trash owner updated, but verification failed: " + detail_utf8;
        return false;
    }
    detail_utf8.clear();
    return true;
}

TrashDirectoryLease pin_owned_trash_directory(const std::filesystem::path &directory,
                                              const std::wstring &sid_text,
                                              std::string &detail_utf8, bool *missing) {
    if (missing != nullptr) {
        *missing = false;
    }
    ScopedHandle handle(
        CreateFileW(directory.c_str(), READ_CONTROL | FILE_LIST_DIRECTORY | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        const auto error = GetLastError();
        if (missing != nullptr &&
            (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            *missing = true;
        }
        detail_utf8 = windows_error_detail("owned trash directory could not be pinned", error);
        return {};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        detail_utf8 = "owned trash directory is not a physical directory";
        return {};
    }
    if (!verify_exact_owned_dacl(handle.get(),
                                 L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + sid_text + L")", sid_text,
                                 detail_utf8)) {
        return {};
    }
    return TrashDirectoryLease(handle.release());
}

bool remove_empty_trash_container_handle_bound(const std::filesystem::path &container,
                                               const bool tolerate_not_empty,
                                               std::string &detail_utf8) {
    ScopedHandle handle(
        CreateFileW(container.c_str(), DELETE | FILE_LIST_DIRECTORY | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        detail_utf8 = windows_error_detail("trash container could not be opened", error);
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        detail_utf8 = "trash container retirement rejected a non-physical directory";
        return false;
    }
    FILE_DISPOSITION_INFO_EX disposition{.Flags = static_cast<DWORD>(FILE_DISPOSITION_FLAG_DELETE)};
    if (SetFileInformationByHandle(handle.get(), FileDispositionInfoEx, &disposition,
                                   sizeof(disposition)) == FALSE) {
        const auto error = GetLastError();
        if (tolerate_not_empty && error == ERROR_DIR_NOT_EMPTY) {
            return true;
        }
        detail_utf8 = windows_error_detail("trash container could not be retired", error);
        return false;
    }
    return true;
}

} // namespace vove::fileops::detail
