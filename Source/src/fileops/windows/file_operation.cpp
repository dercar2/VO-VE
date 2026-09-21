#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <winternl.h>

#include "../file_operation_platform.hpp"
#include "../file_in_use_error.hpp"
#include "windows/delete_target.hpp"
#include "windows/file_identity.hpp"
#include "windows/file_time.hpp"
#include "windows/filesystem_error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vove::fileops::detail {
namespace {

#ifdef VOVE_FILEOP_TEST_HOOKS
std::atomic<BeforeDirectoryPinHook> before_directory_pin_hook{nullptr};
std::atomic<PinnedSourcePathHook> pinned_source_path_hook{nullptr};
std::atomic<BeforeRenameCommitHook> before_rename_commit_hook{nullptr};
std::atomic<BeforeTransferRenameSyscallHook> before_transfer_rename_syscall_hook{nullptr};
std::atomic<AfterTransferRenameSyscallHook> after_transfer_rename_syscall_hook{nullptr};
std::atomic<BeforeTransferRenameRollbackHook> before_transfer_rename_rollback_hook{nullptr};
std::atomic<BeforeTransferSourceDeleteHook> before_transfer_source_delete_hook{nullptr};
std::atomic<DeleteCloseErrorHook> delete_close_error_hook{nullptr};
std::atomic<BeforeCreateDirectoryCommitHook> before_create_directory_commit_hook{nullptr};
std::atomic<AfterCreateDirectoryCommitHook> after_create_directory_commit_hook{nullptr};
#endif

class ScopedHandle final {
  public:
    explicit ScopedHandle(const HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
        }
    }

    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    ScopedHandle(ScopedHandle &&other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    ScopedHandle &operator=(ScopedHandle &&other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE) {
                static_cast<void>(CloseHandle(handle_));
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool close(DWORD &error) noexcept {
        error = ERROR_SUCCESS;
        if (handle_ == INVALID_HANDLE_VALUE) {
            return true;
        }
        const auto handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        if (CloseHandle(handle) != FALSE) {
            return true;
        }
        error = GetLastError();
        return false;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
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

  private:
    HLOCAL value_{};
};

bool current_user_owns_handle(const HANDLE handle, std::string &detail) {
    PSID owner{};
    PSECURITY_DESCRIPTOR raw_descriptor{};
    const auto owner_error = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                             &owner, nullptr, nullptr, nullptr, &raw_descriptor);
    ScopedLocal descriptor(raw_descriptor);
    if (owner_error != ERROR_SUCCESS || owner == nullptr) {
        detail = "trash payload owner is unavailable: Windows error " + std::to_string(owner_error);
        return false;
    }
    HANDLE raw_token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == FALSE) {
        detail =
            "current user token is unavailable: Windows error " + std::to_string(GetLastError());
        return false;
    }
    ScopedHandle token(raw_token);
    DWORD bytes{};
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes));
    if (bytes == 0) {
        detail =
            "current user SID size is unavailable: Windows error " + std::to_string(GetLastError());
        return false;
    }
    std::vector<std::byte> buffer(bytes);
    if (GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes) == FALSE) {
        detail = "current user SID is unavailable: Windows error " + std::to_string(GetLastError());
        return false;
    }
    const auto user = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
    if (EqualSid(owner, user) == FALSE) {
        detail = "trash payload has a foreign owner";
        return false;
    }
    return true;
}

OperationStatus status_from_error(const DWORD code) {
    if (file_in_use_error(code)) {
        return OperationStatus::file_in_use;
    }
    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
        return OperationStatus::conflict;
    }
    if (code == ERROR_WRITE_PROTECT) {
        return OperationStatus::permission_denied;
    }
    if (code == ERROR_NOT_SAME_DEVICE) {
        return OperationStatus::cross_device;
    }
    using Error = platform::detail::WindowsFilesystemErrorKind;
    switch (platform::detail::classify_windows_filesystem_error(code)) {
    case Error::not_found:
        return OperationStatus::not_found;
    case Error::permission_denied:
        return OperationStatus::permission_denied;
    case Error::authentication_required:
        return OperationStatus::authentication_required;
    case Error::timed_out:
        return OperationStatus::timed_out;
    case Error::disconnected:
        return OperationStatus::disconnected;
    case Error::io_error:
        return OperationStatus::io_error;
    }
    return OperationStatus::io_error;
}

std::optional<std::filesystem::path> final_path(const HANDLE handle) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const auto written = GetFinalPathNameByHandleW(handle, buffer.data(), required, flags);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    buffer.resize(written);
    return std::filesystem::path(std::move(buffer));
}

bool starts_with_path_prefix(const std::wstring_view text,
                             const std::wstring_view prefix) noexcept {
    const auto ascii_fold = [](const wchar_t value) noexcept {
        return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
    };
    if (text.size() < prefix.size()) {
        return false;
    }
    return std::equal(prefix.cbegin(), prefix.cend(), text.cbegin(),
                      [ascii_fold](const wchar_t left, const wchar_t right) noexcept {
                          return ascii_fold(left) == ascii_fold(right);
                      });
}

std::wstring comparable_path_text(const std::filesystem::path &path) {
    auto text = path.native();
    std::ranges::replace(text, L'/', L'\\');
    constexpr std::wstring_view extended_unc_prefix = LR"(\\?\UNC\)";
    constexpr std::wstring_view extended_prefix = LR"(\\?\)";
    if (starts_with_path_prefix(text, extended_unc_prefix)) {
        text = LR"(\\)" + text.substr(extended_unc_prefix.size());
    } else if (starts_with_path_prefix(text, extended_prefix)) {
        text.erase(0, extended_prefix.size());
    }
    while (text.size() > 1U && text.back() == L'\\') {
        text.pop_back();
    }
    return text;
}

bool same_path(const std::filesystem::path &left, const std::filesystem::path &right) {
    const auto volume_guid_path =
        [](const std::filesystem::path &path) -> std::optional<std::wstring> {
        std::array<wchar_t, 32'768> full{};
        const auto written =
            GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (written == 0 || static_cast<std::size_t>(written) >= full.size()) {
            return std::nullopt;
        }
        std::array<wchar_t, 32'768> volume_root{};
        if (GetVolumePathNameW(full.data(), volume_root.data(),
                               static_cast<DWORD>(volume_root.size())) == FALSE) {
            return std::nullopt;
        }
        std::array<wchar_t, MAX_PATH + 1U> volume_guid{};
        if (GetVolumeNameForVolumeMountPointW(volume_root.data(), volume_guid.data(),
                                              static_cast<DWORD>(volume_guid.size())) == FALSE) {
            return std::nullopt;
        }
        const std::wstring_view absolute(full.data(), written);
        const std::wstring_view root(volume_root.data());
        if (absolute.size() < root.size() ||
            CompareStringOrdinal(std::wstring(absolute).c_str(), static_cast<int>(root.size()),
                                 std::wstring(root).c_str(), static_cast<int>(root.size()),
                                 TRUE) != CSTR_EQUAL) {
            return std::nullopt;
        }
        std::wstring canonical(volume_guid.data());
        canonical.append(absolute.substr(root.size()));
        return canonical;
    };
    const auto left_guid = volume_guid_path(left);
    const auto right_guid = volume_guid_path(right);
    if (left_guid && right_guid) {
        return CompareStringOrdinal(left_guid->data(), static_cast<int>(left_guid->size()),
                                    right_guid->data(), static_cast<int>(right_guid->size()),
                                    TRUE) == CSTR_EQUAL;
    }
    const auto left_text = comparable_path_text(left);
    const auto right_text = comparable_path_text(right);
    return CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()),
                                right_text.data(), static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
}

std::filesystem::path stable_path_root(const std::filesystem::path &path) {
    auto text = path.native();
    std::ranges::replace(text, L'/', L'\\');
    constexpr std::wstring_view extended_unc_prefix = LR"(\\?\UNC\)";
    if (starts_with_path_prefix(text, extended_unc_prefix)) {
        const auto server_end = text.find(L'\\', extended_unc_prefix.size());
        if (server_end != std::wstring::npos) {
            const auto share_end = text.find(L'\\', server_end + 1U);
            return std::filesystem::path(text.substr(0, share_end));
        }
        return {};
    }

    constexpr std::wstring_view extended_prefix = LR"(\\?\)";
    if (starts_with_path_prefix(text, extended_prefix)) {
        const auto component_end = text.find(L'\\', extended_prefix.size());
        if (component_end != std::wstring::npos) {
            return std::filesystem::path(text.substr(0, component_end + 1U));
        }
        return {};
    }

    constexpr std::wstring_view device_prefix = LR"(\\.\)";
    if (starts_with_path_prefix(text, device_prefix)) {
        return {};
    }

    constexpr std::wstring_view unc_prefix = LR"(\\)";
    if (text.starts_with(unc_prefix)) {
        const auto server_end = text.find(L'\\', unc_prefix.size());
        if (server_end != std::wstring::npos) {
            const auto share_end = text.find(L'\\', server_end + 1U);
            return std::filesystem::path(text.substr(0, share_end));
        }
        return {};
    }

    return path.root_path();
}

bool ambiguous_status(const OperationStatus status) noexcept {
    return status == OperationStatus::unknown_outcome || status == OperationStatus::timed_out ||
           status == OperationStatus::disconnected || status == OperationStatus::io_error;
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

bool ordinary_transfer_file(const FILE_ATTRIBUTE_TAG_INFO &attributes,
                            const FILE_STANDARD_INFO &standard) noexcept {
    return (attributes.FileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
             FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE)) == 0 &&
           standard.NumberOfLinks == 1;
}

bool parent_identity_matches(const HANDLE handle, const std::string_view expected, DWORD &error) {
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        error = GetLastError();
        return false;
    }
    const auto identity = platform::windows_detail::query_file_identity(handle, basic.ChangeTime);
    const auto &revision = platform::windows_detail::preferred_revision(identity);
    if (revision.empty()) {
        error = identity.error;
        return false;
    }
    error = ERROR_SUCCESS;
    return same_object_identity(revision, expected);
}

bool supported_transfer_shape(const HANDLE handle, DWORD &error) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE) {
        error = GetLastError();
        return false;
    }
    if (!ordinary_transfer_file(attributes, standard)) {
        error = ERROR_NOT_SUPPORTED;
        return false;
    }
    return platform::windows_detail::has_only_supported_transfer_data_streams(handle, error);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
NTSTATUS rename_handle_no_replace_at(const HANDLE source, const HANDLE destination_directory,
                                     const std::filesystem::path &filename,
                                     const bool replace = false) {
    const auto &destination = filename.native();
    const auto name_bytes = destination.size() * sizeof(wchar_t);
    const auto buffer_bytes =
        offsetof(FILE_RENAME_INFORMATION, FileName) + name_bytes + sizeof(wchar_t);
    std::vector<std::byte> buffer(buffer_bytes);
    auto *rename = reinterpret_cast<FILE_RENAME_INFORMATION *>(buffer.data());
    rename->ReplaceIfExists = replace ? TRUE : FALSE;
    rename->RootDirectory = destination_directory;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, destination.data(), name_bytes);
    rename->FileName[destination.size()] = L'\0';
    IO_STATUS_BLOCK io_status{};
    return NtSetInformationFile(source, &io_status, rename, static_cast<ULONG>(buffer.size()),
                                FileRenameInformation);
}

template <typename Request>
OperationResult windows_failure(const Request &request, const DWORD code,
                                const OperationEvidence evidence = OperationEvidence::none) {
    return {.operation_id = request.operation_id,
            .status = status_from_error(code),
            .evidence = evidence,
            .platform_code = static_cast<std::int64_t>(code),
            .confirmed_snapshot = {},
            .detail_utf8 = "Windows error " + std::to_string(code)};
}

} // namespace

#ifdef VOVE_FILEOP_TEST_HOOKS
std::filesystem::path stable_path_root_for_test(const std::filesystem::path &path) {
    return stable_path_root(path);
}

bool lexical_same_path_for_test(const std::filesystem::path &left,
                                const std::filesystem::path &right) {
    const auto left_text = comparable_path_text(left);
    const auto right_text = comparable_path_text(right);
    return CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()),
                                right_text.data(), static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
}
#endif

#ifdef VOVE_FILEOP_TEST_HOOKS
void set_before_directory_pin_hook(const BeforeDirectoryPinHook hook) noexcept {
    before_directory_pin_hook.store(hook, std::memory_order_release);
}

void set_pinned_source_path_hook(const PinnedSourcePathHook hook) noexcept {
    pinned_source_path_hook.store(hook, std::memory_order_release);
}

void set_before_rename_commit_hook(const BeforeRenameCommitHook hook) noexcept {
    before_rename_commit_hook.store(hook, std::memory_order_release);
}

void set_before_transfer_rename_syscall_hook(const BeforeTransferRenameSyscallHook hook) noexcept {
    before_transfer_rename_syscall_hook.store(hook, std::memory_order_release);
}

void set_after_transfer_rename_syscall_hook(const AfterTransferRenameSyscallHook hook) noexcept {
    after_transfer_rename_syscall_hook.store(hook, std::memory_order_release);
}

void set_before_transfer_rename_rollback_hook(
    const BeforeTransferRenameRollbackHook hook) noexcept {
    before_transfer_rename_rollback_hook.store(hook, std::memory_order_release);
}

void set_before_transfer_source_delete_hook(const BeforeTransferSourceDeleteHook hook) noexcept {
    before_transfer_source_delete_hook.store(hook, std::memory_order_release);
}

void set_delete_close_error_hook(const DeleteCloseErrorHook hook) noexcept {
    delete_close_error_hook.store(hook, std::memory_order_release);
}

void set_before_create_directory_commit_hook(const BeforeCreateDirectoryCommitHook hook) noexcept {
    before_create_directory_commit_hook.store(hook, std::memory_order_release);
}

void set_after_create_directory_commit_hook(const AfterCreateDirectoryCommitHook hook) noexcept {
    after_create_directory_commit_hook.store(hook, std::memory_order_release);
}
#endif

OperationResult verify_rename_destination_anchor(const RenameRequest &request) {
    if (request.destination_anchor_path.empty()) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::success,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = {}};
    }

    std::vector<ScopedHandle> pinned;
    std::vector<std::filesystem::path> pinned_paths;
    std::optional<std::size_t> anchor_index;
    auto directory = request.destination.parent_path();
    const auto root = stable_path_root(directory);
    if (directory.empty() || root.empty() || same_path(directory, root)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = "rename destination has no pinnable anchor chain"};
    }
    while (!same_path(directory, root)) {
        ScopedHandle handle(
            CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                         sizeof(attributes)) == FALSE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination anchor chain is not physical"};
        }
        const auto opened_path = final_path(handle.get());
        if (!opened_path || !same_path(*opened_path, directory)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination anchor path changed while open"};
        }
        if (same_path(*opened_path, request.destination_anchor_path)) {
            DWORD identity_error{};
            if (!parent_identity_matches(handle.get(), request.destination_anchor_identity_utf8,
                                         identity_error)) {
                return identity_error == ERROR_SUCCESS
                           ? OperationResult{.operation_id = request.operation_id,
                                             .status = OperationStatus::source_changed,
                                             .evidence = OperationEvidence::no_commit,
                                             .confirmed_snapshot = {},
                                             .detail_utf8 =
                                                 "rename destination anchor identity changed"}
                           : windows_failure(request, identity_error, OperationEvidence::no_commit);
            }
            anchor_index = pinned.size();
        }
        pinned_paths.push_back(*opened_path);
        pinned.push_back(std::move(handle));
        const auto parent = directory.parent_path();
        if (parent.empty() || same_path(parent, directory)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination anchor chain has no stable root"};
        }
        directory = parent;
    }
    if (!anchor_index) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = "rename destination anchor is not an ancestor"};
    }
    for (std::size_t index{}; index < pinned.size(); ++index) {
        const auto current = final_path(pinned[index].get());
        if (!current || !same_path(*current, pinned_paths[index])) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination anchor chain changed"};
        }
    }
    DWORD identity_error{};
    if (!parent_identity_matches(pinned[*anchor_index].get(),
                                 request.destination_anchor_identity_utf8, identity_error)) {
        return identity_error == ERROR_SUCCESS
                   ? OperationResult{.operation_id = request.operation_id,
                                     .status = OperationStatus::source_changed,
                                     .evidence = OperationEvidence::no_commit,
                                     .confirmed_snapshot = {},
                                     .detail_utf8 = "rename destination anchor identity changed"}
                   : windows_failure(request, identity_error, OperationEvidence::no_commit);
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::no_commit,
            .confirmed_snapshot = {},
            .detail_utf8 = {}};
}

OperationResult rename_no_replace(const RenameRequest &request) {
    const auto failed_before_commit = [&request](const DWORD code, const std::string_view stage) {
        auto result = windows_failure(request, code, OperationEvidence::no_commit);
        result.detail_utf8 += " while ";
        result.detail_utf8 += stage;
        return result;
    };
    const auto object_is_directory = request.object_kind == OperationObjectKind::directory;
    const DWORD source_access = object_is_directory
                                    ? FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE | SYNCHRONIZE
                                    : GENERIC_READ | DELETE;
    const DWORD source_flags =
        FILE_FLAG_OPEN_REPARSE_POINT |
        (object_is_directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL);
    ScopedHandle source(CreateFileW(request.source.c_str(), source_access, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, source_flags, nullptr));
    if (source.get() == INVALID_HANDLE_VALUE) {
        return failed_before_commit(GetLastError(), "opening rename source with delete access");
    }
    LARGE_INTEGER size{};
    FILE_BASIC_INFO basic{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    if ((!object_is_directory &&
         (GetFileSizeEx(source.get(), &size) == FALSE || size.QuadPart < 0)) ||
        GetFileInformationByHandleEx(source.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
        GetFileInformationByHandleEx(source.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        GetFileInformationByHandleEx(source.get(), FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE) {
        return failed_before_commit(GetLastError(), "reading rename source metadata");
    }
    const auto observed_directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        observed_directory != object_is_directory) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "renaming reparse points is not supported"};
    }
    if (!object_is_directory && transfer_rename_mode(request.mode) &&
        !ordinary_transfer_file(attributes, standard)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "file transfer accepts only ordinary single-link files"};
    }
    const auto source_path = final_path(source.get());
    if (!source_path) {
        return failed_before_commit(GetLastError(), "resolving opened rename source path");
    }
    if (!same_path(*source_path, request.source)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "opened source path differs from the requested path"};
    }
    const auto trash_mode =
        request.mode == RenameMode::trash_internal || request.mode == RenameMode::trash_restore;
    if (trash_mode) {
        std::string owner_detail;
        if (!current_user_owns_handle(source.get(), owner_detail)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::permission_denied,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = std::move(owner_detail)};
        }
        if (!object_is_directory && standard.NumberOfLinks != 1) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "VO-VE Trash does not accept files with multiple hard links"};
        }
        FILE_REMOTE_PROTOCOL_INFO remote{};
        const auto remote_query = GetFileInformationByHandleEx(source.get(), FileRemoteProtocolInfo,
                                                               &remote, sizeof(remote)) != FALSE;
        const auto remote_error = remote_query ? ERROR_SUCCESS : GetLastError();
        if (remote_query && platform::windows_detail::is_smb_remote_protocol(remote)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "VO-VE Trash is restricted to local filesystems"};
        }
        if (!remote_query && remote_error != ERROR_SUCCESS &&
            remote_error != ERROR_INVALID_PARAMETER && remote_error != ERROR_NOT_SUPPORTED) {
            return windows_failure(request, remote_error, OperationEvidence::no_commit);
        }
        std::array<wchar_t, 32> filesystem{};
        if (GetVolumeInformationByHandleW(source.get(), nullptr, 0, nullptr, nullptr, nullptr,
                                          filesystem.data(),
                                          static_cast<DWORD>(filesystem.size())) == FALSE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        std::array<wchar_t, MAX_PATH + 1> volume_path{};
        if (GetVolumePathNameW(source_path->c_str(), volume_path.data(),
                               static_cast<DWORD>(volume_path.size())) == FALSE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        if (_wcsicmp(filesystem.data(), L"NTFS") != 0 ||
            GetDriveTypeW(volume_path.data()) != DRIVE_FIXED) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "VO-VE Trash requires a fixed local NTFS volume"};
        }
        if (request.mode == RenameMode::trash_internal &&
            (attributes.FileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "hidden and system files are outside the VO-VE Trash pilot"};
        }
    }
    const auto identity =
        platform::windows_detail::query_file_identity(source.get(), basic.ChangeTime);
    const auto &revision = platform::windows_detail::matching_revision(
        identity, request.expected_source.source_revision_utf8);
    if (revision.empty()) {
        return failed_before_commit(identity.error, "reading rename source identity");
    }
    const auto size_matches =
        (object_is_directory ? 0U : static_cast<std::uint64_t>(size.QuadPart)) ==
        request.expected_source.size_bytes;
    const auto modified_matches =
        platform::windows_detail::filetime_to_unix_ns(basic.LastWriteTime) ==
        request.expected_source.modified_unix_ns;
    const auto revision_matches =
        same_source_revision(revision, request.expected_source.source_revision_utf8);
    if (!size_matches || !modified_matches || !revision_matches) {
        std::string detail = "source identity changed before handle rename:";
        if (!size_matches) {
            detail += " size";
        }
        if (!modified_matches) {
            detail += " modified time";
        }
        if (!revision_matches) {
            detail += " revision";
        }
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = std::move(detail)};
    }

#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = before_directory_pin_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook(*source_path);
    }
#endif

    std::vector<ScopedHandle> pinned_directories;
    auto directory = source_path->parent_path();
    const auto root = stable_path_root(directory);
    while (!same_path(directory, root)) {
        ScopedHandle pinned(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (pinned.get() == INVALID_HANDLE_VALUE) {
            auto failure = windows_failure(request, GetLastError(), OperationEvidence::no_commit);
            failure.detail_utf8 +=
                " while pinning directory " + std::to_string(pinned_directories.size());
            return failure;
        }
        pinned_directories.emplace_back(std::move(pinned));

        const auto parent = directory.parent_path();
        if (parent.empty() || same_path(parent, directory)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "source path has no stable filesystem root"};
        }
        directory = parent;
    }

    auto pinned_source_path = final_path(source.get());
    if (!pinned_source_path) {
        return failed_before_commit(GetLastError(), "rechecking pinned rename source path");
    }
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = pinned_source_path_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook(*pinned_source_path);
    }
#endif
    if (!same_path(*source_path, *pinned_source_path)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "source path moved before handle rename"};
    }
    auto expected_directory = pinned_source_path->parent_path();
    for (const auto &pinned : pinned_directories) {
        const auto pinned_path = final_path(pinned.get());
        if (!pinned_path) {
            return failed_before_commit(GetLastError(), "resolving pinned rename source parent");
        }
        if (!same_path(expected_directory, *pinned_path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "source directory chain changed before handle rename"};
        }
        expected_directory = expected_directory.parent_path();
    }
    if (transfer_rename_mode(request.mode)) {
        DWORD anchor_error{};
        if (pinned_directories.empty() ||
            !parent_identity_matches(pinned_directories.front().get(),
                                     request.source_parent_identity_utf8, anchor_error)) {
            return anchor_error == ERROR_SUCCESS
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::source_changed,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 = "source parent identity changed"}
                       : windows_failure(request, anchor_error, OperationEvidence::no_commit);
        }
    }

    std::vector<ScopedHandle> pinned_destination_directories;
    std::optional<std::size_t> destination_anchor_index;
    auto destination_directory_path = request.destination.parent_path();
    const auto destination_root = stable_path_root(destination_directory_path);
    while (!same_path(destination_directory_path, destination_root)) {
        ScopedHandle pinned(CreateFileW(
            destination_directory_path.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (pinned.get() == INVALID_HANDLE_VALUE) {
            auto failure = failed_before_commit(GetLastError(), "pinning rename destination parent");
            failure.detail_utf8 += " " + std::to_string(pinned_destination_directories.size());
            return failure;
        }
        FILE_ATTRIBUTE_TAG_INFO destination_attributes{};
        if (GetFileInformationByHandleEx(pinned.get(), FileAttributeTagInfo,
                                         &destination_attributes,
                                         sizeof(destination_attributes)) == FALSE) {
            return failed_before_commit(GetLastError(), "reading pinned rename destination attributes");
        }
        if ((destination_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (destination_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination chain is not physical"};
        }
        const auto pinned_path = final_path(pinned.get());
        if (!pinned_path || !same_path(*pinned_path, destination_directory_path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination path changed while open"};
        }
        if (!request.destination_anchor_path.empty() &&
            same_path(*pinned_path, request.destination_anchor_path)) {
            DWORD anchor_error{};
            if (!parent_identity_matches(pinned.get(), request.destination_anchor_identity_utf8,
                                         anchor_error)) {
                return anchor_error == ERROR_SUCCESS
                           ? OperationResult{.operation_id = request.operation_id,
                                             .status = OperationStatus::source_changed,
                                             .evidence = OperationEvidence::no_commit,
                                             .source_present = true,
                                             .confirmed_snapshot = {},
                                             .detail_utf8 =
                                                 "rename destination anchor identity changed"}
                           : windows_failure(request, anchor_error, OperationEvidence::no_commit);
            }
            destination_anchor_index = pinned_destination_directories.size();
        }
        pinned_destination_directories.emplace_back(std::move(pinned));
        const auto parent = destination_directory_path.parent_path();
        if (parent.empty() || same_path(parent, destination_directory_path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "destination path has no stable filesystem root"};
        }
        destination_directory_path = parent;
    }
    if (pinned_destination_directories.empty()) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "rename destination cannot be the filesystem root"};
    }
    if (!request.destination_anchor_path.empty() && !destination_anchor_index) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "rename destination anchor is not an ancestor"};
    }
    auto expected_destination_directory = request.destination.parent_path();
    for (const auto &pinned : pinned_destination_directories) {
        const auto pinned_path = final_path(pinned.get());
        if (!pinned_path || !same_path(expected_destination_directory, *pinned_path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination chain changed before handle rename"};
        }
        expected_destination_directory = expected_destination_directory.parent_path();
    }
    if (transfer_rename_mode(request.mode)) {
        DWORD anchor_error{};
        if (!parent_identity_matches(pinned_destination_directories.front().get(),
                                     request.destination_parent_identity_utf8, anchor_error)) {
            return anchor_error == ERROR_SUCCESS
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::source_changed,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 = "destination parent identity changed"}
                       : windows_failure(request, anchor_error, OperationEvidence::no_commit);
        }
    }
    const auto &destination_directory = pinned_destination_directories.front();
    BY_HANDLE_FILE_INFORMATION source_volume{};
    BY_HANDLE_FILE_INFORMATION destination_volume{};
    if (GetFileInformationByHandle(source.get(), &source_volume) == FALSE ||
        GetFileInformationByHandle(destination_directory.get(), &destination_volume) == FALSE) {
        return failed_before_commit(GetLastError(), "comparing rename source and destination volumes");
    }
    if (source_volume.dwVolumeSerialNumber != destination_volume.dwVolumeSerialNumber) {
        const auto atomic = request.mode == RenameMode::transfer_atomic ||
                            request.mode == RenameMode::transfer_atomic_replace;
        return {.operation_id = request.operation_id,
                .status = atomic ? OperationStatus::cross_device : OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .platform_code = atomic ? static_cast<std::int64_t>(ERROR_NOT_SAME_DEVICE) : 0,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "rename destination is on another volume"};
    }

#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = before_rename_commit_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook();
    }
#endif
    expected_destination_directory = request.destination.parent_path();
    for (const auto &pinned : pinned_destination_directories) {
        const auto pinned_path = final_path(pinned.get());
        if (!pinned_path || !same_path(expected_destination_directory, *pinned_path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "rename destination chain changed immediately before commit"};
        }
        expected_destination_directory = expected_destination_directory.parent_path();
    }
    if (transfer_rename_mode(request.mode)) {
        DWORD source_anchor_error{};
        DWORD destination_anchor_error{};
        const auto source_parent_matches =
            !pinned_directories.empty() &&
            parent_identity_matches(pinned_directories.front().get(),
                                    request.source_parent_identity_utf8, source_anchor_error);
        const auto destination_parent_matches = parent_identity_matches(
            pinned_destination_directories.front().get(), request.destination_parent_identity_utf8,
            destination_anchor_error);
        if (!source_parent_matches || !destination_parent_matches) {
            const auto code = source_anchor_error != ERROR_SUCCESS ? source_anchor_error
                                                                   : destination_anchor_error;
            return code == ERROR_SUCCESS
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::source_changed,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 =
                                             "file transfer parent identity changed before commit"}
                       : windows_failure(request, code, OperationEvidence::no_commit);
        }
        if (destination_anchor_index) {
            DWORD root_anchor_error{};
            if (!parent_identity_matches(
                    pinned_destination_directories[*destination_anchor_index].get(),
                    request.destination_anchor_identity_utf8, root_anchor_error)) {
                return root_anchor_error == ERROR_SUCCESS
                           ? OperationResult{.operation_id = request.operation_id,
                                             .status = OperationStatus::source_changed,
                                             .evidence = OperationEvidence::no_commit,
                                             .source_present = true,
                                             .confirmed_snapshot = {},
                                             .detail_utf8 = "file transfer destination anchor "
                                                            "changed before commit"}
                           : windows_failure(request, root_anchor_error,
                                             OperationEvidence::no_commit);
            }
        }
        DWORD stream_error{};
        if (!platform::windows_detail::has_only_supported_transfer_data_streams(source.get(),
                                                                                stream_error)) {
            return stream_error == ERROR_NOT_SUPPORTED
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::unsupported,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 =
                                             "file transfer does not support named data streams"}
                       : windows_failure(request, stream_error, OperationEvidence::no_commit);
        }
    }
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (transfer_rename_mode(request.mode)) {
        if (const auto hook = before_transfer_rename_syscall_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            hook();
        }
    }
#endif
    const auto replacement = request.mode == RenameMode::transfer_publish_replace ||
                             request.mode == RenameMode::transfer_atomic_replace;
    ScopedHandle old_destination(INVALID_HANDLE_VALUE);
    if (replacement) {
        // Deny writers while checking the approved revision of the replacement target.
        old_destination = ScopedHandle(CreateFileW(
            request.destination.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (old_destination.get() == INVALID_HANDLE_VALUE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        DWORD shape_error{};
        if (!supported_transfer_shape(old_destination.get(), shape_error) ||
            !supported_transfer_shape(source.get(), shape_error)) {
            return windows_failure(request, shape_error, OperationEvidence::no_commit);
        }
        LARGE_INTEGER old_size{};
        FILE_BASIC_INFO old_basic{};
        if (GetFileSizeEx(old_destination.get(), &old_size) == FALSE ||
            GetFileInformationByHandleEx(old_destination.get(), FileBasicInfo, &old_basic,
                                         sizeof(old_basic)) == FALSE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        const auto old_identity = platform::windows_detail::query_file_identity(
            old_destination.get(), old_basic.ChangeTime);
        const auto &old_revision = platform::windows_detail::matching_revision(
            old_identity, request.expected_destination.source_revision_utf8);
        const auto old_path = final_path(old_destination.get());
        if (!old_path || !same_path(*old_path, request.destination) || old_size.QuadPart < 0 ||
            static_cast<std::uint64_t>(old_size.QuadPart) !=
                request.expected_destination.size_bytes ||
            platform::windows_detail::filetime_to_unix_ns(old_basic.LastWriteTime) !=
                request.expected_destination.modified_unix_ns ||
            !same_source_revision(old_revision,
                                  request.expected_destination.source_revision_utf8)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "authorized overwrite destination changed before publication"};
        }
        // FileRenameInformation cannot replace an open target. Closing is not deletion;
        // the old destination remains in place until the atomic rename succeeds.
        DWORD close_error{};
        if (!old_destination.close(close_error)) {
            return windows_failure(request, close_error, OperationEvidence::no_commit);
        }
    }
    const auto rename_status = rename_handle_no_replace_at(
        source.get(), destination_directory.get(), request.destination.filename(), replacement);
    if (rename_status >= 0) {
        const auto committed_path = final_path(source.get());
        if (!committed_path || !same_path(*committed_path, request.destination)) {
            auto failure =
                windows_failure(request, committed_path ? ERROR_INVALID_NAME : GetLastError());
            failure.status = OperationStatus::unknown_outcome;
            failure.evidence = OperationEvidence::committed;
            failure.destination_present = true;
            failure.destination_matches_source = true;
            failure.detail_utf8 = "rename committed outside the requested destination path";
            return failure;
        }
        if (transfer_rename_mode(request.mode)) {
            DWORD shape_error{};
            if (!supported_transfer_shape(source.get(), shape_error)) {
                if (replacement) {
                    return {.operation_id = request.operation_id,
                            .status = OperationStatus::unknown_outcome,
                            .evidence = OperationEvidence::committed,
                            .destination_present = true,
                            .destination_matches_source = true,
                            .confirmed_snapshot = {},
                            .detail_utf8 =
                                "replacement committed but file shape could not be confirmed"};
                }
#ifdef VOVE_FILEOP_TEST_HOOKS
                if (const auto hook =
                        before_transfer_rename_rollback_hook.load(std::memory_order_acquire);
                    hook != nullptr) {
                    hook();
                }
#endif
                const auto rollback_status = rename_handle_no_replace_at(
                    source.get(), pinned_directories.front().get(), request.source.filename());
                const auto rolled_back_path = final_path(source.get());
                if (rollback_status >= 0 && rolled_back_path &&
                    same_path(*rolled_back_path, request.source)) {
                    if (shape_error == ERROR_NOT_SUPPORTED) {
                        return {.operation_id = request.operation_id,
                                .status = OperationStatus::unsupported,
                                .evidence = OperationEvidence::no_commit,
                                .source_present = true,
                                .confirmed_snapshot = {},
                                .detail_utf8 =
                                    "unsupported file shape appeared during transfer rename"};
                    }
                    return windows_failure(request, shape_error, OperationEvidence::no_commit);
                }
                auto failure = windows_failure(
                    request, static_cast<DWORD>(RtlNtStatusToDosError(rollback_status)));
                failure.status = OperationStatus::unknown_outcome;
                failure.evidence = OperationEvidence::committed;
                failure.destination_present = true;
                failure.destination_matches_source = true;
                failure.detail_utf8 =
                    "transfer rename committed with an unsupported file shape and rollback failed";
                return failure;
            }
        }
        LARGE_INTEGER confirmed_size{};
        FILE_BASIC_INFO confirmed_basic{};
        if ((!object_is_directory && (GetFileSizeEx(source.get(), &confirmed_size) == FALSE ||
                                      confirmed_size.QuadPart < 0)) ||
            GetFileInformationByHandleEx(source.get(), FileBasicInfo, &confirmed_basic,
                                         sizeof(confirmed_basic)) == FALSE) {
            auto failure = windows_failure(request, GetLastError());
            failure.status = OperationStatus::unknown_outcome;
            failure.evidence = OperationEvidence::committed;
            failure.destination_present = true;
            failure.destination_matches_source = true;
            failure.detail_utf8 = "rename committed but its new snapshot could not be read";
            return failure;
        }
        const auto confirmed_identity =
            platform::windows_detail::query_file_identity(source.get(), confirmed_basic.ChangeTime);
        const auto &confirmed_revision = platform::windows_detail::matching_revision(
            confirmed_identity, request.expected_source.source_revision_utf8);
        if (confirmed_revision.empty()) {
            auto failure = windows_failure(request, confirmed_identity.error);
            failure.status = OperationStatus::unknown_outcome;
            failure.evidence = OperationEvidence::committed;
            failure.destination_present = true;
            failure.destination_matches_source = true;
            failure.detail_utf8 = "rename committed but its new identity could not be read";
            return failure;
        }
        return {
            .operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .source_present = false,
            .destination_present = true,
            .destination_matches_source = true,
            .confirmed_snapshot =
                {.size_bytes =
                     object_is_directory ? 0U : static_cast<std::uint64_t>(confirmed_size.QuadPart),
                 .modified_unix_ns =
                     platform::windows_detail::filetime_to_unix_ns(confirmed_basic.LastWriteTime),
                 .source_revision_utf8 = confirmed_revision},
            .detail_utf8 = {}};
    }
    const auto code = static_cast<DWORD>(RtlNtStatusToDosError(rename_status));
    auto failure = windows_failure(request, code);
    failure.detail_utf8 += object_is_directory ? " while committing directory rename"
                                              : " while committing file rename";
    failure.detail_utf8 += " (NtSetInformationFile status " +
                           std::to_string(static_cast<std::int64_t>(rename_status)) + ")";
    if (failure.status == OperationStatus::cross_device &&
        request.mode != RenameMode::transfer_atomic &&
        request.mode != RenameMode::transfer_atomic_replace) {
        failure.status = OperationStatus::unsupported;
    }
    if (!ambiguous_status(failure.status)) {
        failure.evidence = OperationEvidence::no_commit;
    }
    return failure;
}

OperationResult permanent_delete_remote(const DeleteRequest &request) {
    const auto object_is_directory = request.object_kind == OperationObjectKind::directory;
    const auto source_access = FILE_READ_ATTRIBUTES | DELETE |
                               (request.mode == DeleteMode::trash_purge ? READ_CONTROL : 0U);
    const auto source_flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
    ScopedHandle source(
        CreateFileW(request.source.c_str(), source_access, 0, nullptr, OPEN_EXISTING,
                    source_flags, nullptr));
    if (source.get() == INVALID_HANDLE_VALUE) {
        auto failure = windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        failure.detail_utf8 = "delete source could not be opened: " + failure.detail_utf8;
        return failure;
    }
    LARGE_INTEGER size{};
    FILE_BASIC_INFO basic{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    FILE_REMOTE_PROTOCOL_INFO remote{};
    if ((!object_is_directory &&
         (GetFileSizeEx(source.get(), &size) == FALSE || size.QuadPart < 0)) ||
        GetFileInformationByHandleEx(source.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
        GetFileInformationByHandleEx(source.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        GetFileInformationByHandleEx(source.get(), FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE) {
        auto failure = windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        failure.detail_utf8 = "delete source metadata could not be read: " + failure.detail_utf8;
        return failure;
    }
    const auto observed_directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        observed_directory != object_is_directory ||
        (!object_is_directory &&
         !platform::windows_detail::is_regular_delete_target(attributes.FileAttributes))) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "delete target does not match its verified object kind"};
    }
    if (transfer_delete_mode(request.mode) && !ordinary_transfer_file(attributes, standard)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "file transfer deletes only ordinary single-link files"};
    }
    if (request.mode == DeleteMode::trash_purge) {
        std::string owner_detail;
        if (!current_user_owns_handle(source.get(), owner_detail)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::permission_denied,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = std::move(owner_detail)};
        }
    }
    if (!object_is_directory && request.mode == DeleteMode::trash_purge &&
        standard.NumberOfLinks != 1) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "VO-VE Trash purge rejects files with multiple hard links"};
    }
    const auto source_path = final_path(source.get());
    if (!source_path) {
        auto failure = windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        failure.detail_utf8 = "delete source path could not be resolved: " + failure.detail_utf8;
        return failure;
    }
    if (!same_path(*source_path, request.source)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "opened source path differs from the requested path"};
    }
    if (request.mode == DeleteMode::permanent_remote) {
        const auto remote_query = GetFileInformationByHandleEx(
                                      source.get(), FileRemoteProtocolInfo, &remote,
                                      sizeof(remote)) != FALSE;
        const auto code = remote_query ? ERROR_SUCCESS : GetLastError();
        if (!remote_query || !platform::windows_detail::is_smb_remote_protocol(remote)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .platform_code = static_cast<std::int64_t>(code),
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 =
                        "permanent delete is restricted to verified SMB filesystems"};
        }
    } else if (request.mode == DeleteMode::trash_purge) {
        const auto remote_query = GetFileInformationByHandleEx(source.get(), FileRemoteProtocolInfo,
                                                               &remote, sizeof(remote)) != FALSE;
        if (remote_query && platform::windows_detail::is_smb_remote_protocol(remote)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "trash purge is restricted to local filesystems"};
        }
        std::array<wchar_t, MAX_PATH + 1> volume_path{};
        if (GetVolumePathNameW(source_path->c_str(), volume_path.data(),
                               static_cast<DWORD>(volume_path.size())) == FALSE) {
            auto failure = windows_failure(request, GetLastError(), OperationEvidence::no_commit);
            failure.detail_utf8 =
                "trash source volume could not be resolved: " + failure.detail_utf8;
            return failure;
        }
        if (GetDriveTypeW(volume_path.data()) != DRIVE_FIXED) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "trash purge is restricted to a local fixed filesystem"};
        }
    }
    const auto identity =
        platform::windows_detail::query_file_identity(source.get(), basic.ChangeTime);
    const auto &revision = platform::windows_detail::matching_revision(
        identity, request.expected_source.source_revision_utf8);
    if (revision.empty()) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .platform_code = static_cast<std::int64_t>(identity.error),
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "filesystem did not provide a stable object identity"};
    }
    const auto exact_snapshot =
        (object_is_directory ? 0U : static_cast<std::uint64_t>(size.QuadPart)) ==
            request.expected_source.size_bytes &&
        platform::windows_detail::filetime_to_unix_ns(basic.LastWriteTime) ==
            request.expected_source.modified_unix_ns &&
        same_source_revision(revision, request.expected_source.source_revision_utf8);
    const auto stable_directory =
        object_is_directory && request.expected_source.size_bytes == 0 &&
        same_object_identity(revision, request.expected_source.source_revision_utf8);
    if (!exact_snapshot && !stable_directory) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "source identity changed before handle delete"};
    }

#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = before_directory_pin_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook(*source_path);
    }
#endif

    std::vector<ScopedHandle> pinned_directories;
    auto directory = source_path->parent_path();
    const auto root = stable_path_root(directory);
    while (!same_path(directory, root)) {
        ScopedHandle pinned(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (pinned.get() == INVALID_HANDLE_VALUE) {
            auto failure = windows_failure(request, GetLastError(), OperationEvidence::no_commit);
            failure.detail_utf8 +=
                " while pinning directory " + std::to_string(pinned_directories.size());
            return failure;
        }
        pinned_directories.emplace_back(std::move(pinned));
        const auto parent = directory.parent_path();
        if (parent.empty() || same_path(parent, directory)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "source path has no stable filesystem root"};
        }
        directory = parent;
    }

    const auto pinned_source_path = final_path(source.get());
    if (!pinned_source_path || !same_path(*source_path, *pinned_source_path)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "source path moved before handle delete"};
    }
    auto expected_directory = pinned_source_path->parent_path();
    for (const auto &pinned : pinned_directories) {
        const auto pinned_path = final_path(pinned.get());
        if (!pinned_path || !same_path(expected_directory, *pinned_path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "source directory chain changed before handle delete"};
        }
        expected_directory = expected_directory.parent_path();
    }

    if (transfer_delete_mode(request.mode)) {
        DWORD anchor_error{};
        if (pinned_directories.empty() ||
            !parent_identity_matches(pinned_directories.front().get(),
                                     request.source_parent_identity_utf8, anchor_error)) {
            return anchor_error == ERROR_SUCCESS
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::source_changed,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 = "delete source parent identity changed"}
                       : windows_failure(request, anchor_error, OperationEvidence::no_commit);
        }
    }

    ScopedHandle publication_guard(INVALID_HANDLE_VALUE);
    ScopedHandle publication_parent(INVALID_HANDLE_VALUE);
    if (request.mode == DeleteMode::transfer_source_commit) {
        publication_guard = ScopedHandle(CreateFileW(
            request.guard_path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (publication_guard.get() == INVALID_HANDLE_VALUE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        LARGE_INTEGER guard_size{};
        FILE_BASIC_INFO guard_basic{};
        FILE_ATTRIBUTE_TAG_INFO guard_attributes{};
        FILE_STANDARD_INFO guard_standard{};
        if (GetFileSizeEx(publication_guard.get(), &guard_size) == FALSE ||
            guard_size.QuadPart < 0 ||
            GetFileInformationByHandleEx(publication_guard.get(), FileBasicInfo, &guard_basic,
                                         sizeof(guard_basic)) == FALSE ||
            GetFileInformationByHandleEx(publication_guard.get(), FileAttributeTagInfo,
                                         &guard_attributes, sizeof(guard_attributes)) == FALSE ||
            GetFileInformationByHandleEx(publication_guard.get(), FileStandardInfo, &guard_standard,
                                         sizeof(guard_standard)) == FALSE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        const auto guard_path = final_path(publication_guard.get());
        const auto guard_identity = platform::windows_detail::query_file_identity(
            publication_guard.get(), guard_basic.ChangeTime);
        const auto &guard_revision = platform::windows_detail::matching_revision(
            guard_identity, request.expected_guard.source_revision_utf8);
        DWORD guard_stream_error{};
        const auto guard_streams_are_supported =
            platform::windows_detail::has_only_supported_transfer_data_streams(
                publication_guard.get(), guard_stream_error);
        if (!ordinary_transfer_file(guard_attributes, guard_standard) ||
            !guard_streams_are_supported || !guard_path ||
            !same_path(*guard_path, request.guard_path) || guard_revision.empty() ||
            static_cast<std::uint64_t>(guard_size.QuadPart) != request.expected_guard.size_bytes ||
            platform::windows_detail::filetime_to_unix_ns(guard_basic.LastWriteTime) !=
                request.expected_guard.modified_unix_ns ||
            !same_source_revision(guard_revision, request.expected_guard.source_revision_utf8)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .destination_present = true,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "published destination guard changed before source delete"};
        }
        publication_parent = ScopedHandle(
            CreateFileW(request.guard_path.parent_path().c_str(),
                        FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (publication_parent.get() == INVALID_HANDLE_VALUE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        FILE_ATTRIBUTE_TAG_INFO parent_attributes{};
        const auto parent_path = final_path(publication_parent.get());
        DWORD parent_error{};
        if (GetFileInformationByHandleEx(publication_parent.get(), FileAttributeTagInfo,
                                         &parent_attributes, sizeof(parent_attributes)) == FALSE ||
            (parent_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (parent_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !parent_path || !same_path(*parent_path, request.guard_path.parent_path()) ||
            !parent_identity_matches(publication_parent.get(), request.guard_parent_identity_utf8,
                                     parent_error)) {
            return parent_error == ERROR_SUCCESS
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::source_changed,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .destination_present = true,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 =
                                             "published destination parent identity changed"}
                       : windows_failure(request, parent_error, OperationEvidence::no_commit);
        }
    }

    if (transfer_delete_mode(request.mode)) {
        DWORD source_stream_error{};
        if (!platform::windows_detail::has_only_supported_transfer_data_streams(
                source.get(), source_stream_error)) {
            return source_stream_error == ERROR_NOT_SUPPORTED
                       ? OperationResult{.operation_id = request.operation_id,
                                         .status = OperationStatus::unsupported,
                                         .evidence = OperationEvidence::no_commit,
                                         .source_present = true,
                                         .destination_present =
                                             request.mode == DeleteMode::transfer_source_commit,
                                         .confirmed_snapshot = {},
                                         .detail_utf8 =
                                             "file transfer does not support named data streams"}
                       : windows_failure(request, source_stream_error,
                                         OperationEvidence::no_commit);
        }
    }

    auto disposition_flags = static_cast<DWORD>(FILE_DISPOSITION_FLAG_DELETE);
    if (request.mode == DeleteMode::trash_purge) {
        disposition_flags |= static_cast<DWORD>(FILE_DISPOSITION_FLAG_POSIX_SEMANTICS);
    }
    FILE_DISPOSITION_INFO_EX extended_disposition{.Flags = disposition_flags};
    auto extended_disposition_used =
        SetFileInformationByHandle(source.get(), FileDispositionInfoEx, &extended_disposition,
                                   sizeof(extended_disposition)) != FALSE;
    auto disposition_set = extended_disposition_used;
    auto disposition_error = disposition_set ? ERROR_SUCCESS : GetLastError();
    if (!disposition_set && (disposition_error == ERROR_INVALID_PARAMETER ||
                             disposition_error == ERROR_NOT_SUPPORTED)) {
        FILE_DISPOSITION_INFO legacy_disposition{TRUE};
        disposition_set =
            SetFileInformationByHandle(source.get(), FileDispositionInfo, &legacy_disposition,
                                       sizeof(legacy_disposition)) != FALSE;
        disposition_error = disposition_set ? ERROR_SUCCESS : GetLastError();
    }
    if (disposition_set) {
        const auto cancel_delete = [&](OperationResult result) {
            FILE_DISPOSITION_INFO_EX extended_keep{.Flags = 0U};
            FILE_DISPOSITION_INFO legacy_keep{FALSE};
            const auto cancelled =
                extended_disposition_used
                    ? SetFileInformationByHandle(source.get(), FileDispositionInfoEx,
                                                 &extended_keep, sizeof(extended_keep)) != FALSE
                    : SetFileInformationByHandle(source.get(), FileDispositionInfo, &legacy_keep,
                                                 sizeof(legacy_keep)) != FALSE;
            if (cancelled) {
                return result;
            }
            return OperationResult{
                .operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::none,
                .platform_code = static_cast<std::int64_t>(GetLastError()),
                .source_present = false,
                .source_matches_expected = false,
                .destination_present = request.mode == DeleteMode::transfer_source_commit,
                .confirmed_snapshot = {},
                .detail_utf8 =
                    "delete barrier found changed evidence but could not cancel disposition"};
        };

#ifdef VOVE_FILEOP_TEST_HOOKS
        if (request.mode == DeleteMode::transfer_source_commit) {
            if (const auto hook =
                    before_transfer_source_delete_hook.load(std::memory_order_acquire);
                hook != nullptr) {
                hook();
            }
        }
#endif

        if (transfer_delete_mode(request.mode)) {
            LARGE_INTEGER barrier_size{};
            FILE_BASIC_INFO barrier_basic{};
            FILE_ATTRIBUTE_TAG_INFO barrier_attributes{};
            FILE_STANDARD_INFO barrier_standard{};
            const auto barrier_path = final_path(source.get());
            if (GetFileSizeEx(source.get(), &barrier_size) == FALSE || barrier_size.QuadPart < 0 ||
                GetFileInformationByHandleEx(source.get(), FileBasicInfo, &barrier_basic,
                                             sizeof(barrier_basic)) == FALSE ||
                GetFileInformationByHandleEx(source.get(), FileAttributeTagInfo,
                                             &barrier_attributes,
                                             sizeof(barrier_attributes)) == FALSE ||
                GetFileInformationByHandleEx(source.get(), FileStandardInfo, &barrier_standard,
                                             sizeof(barrier_standard)) == FALSE) {
                return cancel_delete(
                    windows_failure(request, GetLastError(), OperationEvidence::no_commit));
            }
            const auto barrier_identity = platform::windows_detail::query_file_identity(
                source.get(), barrier_basic.ChangeTime);
            const auto &barrier_revision = platform::windows_detail::matching_revision(
                barrier_identity, request.expected_source.source_revision_utf8);
            if (!barrier_path || !same_path(*barrier_path, request.source) ||
                barrier_revision.empty() ||
                static_cast<std::uint64_t>(barrier_size.QuadPart) !=
                    request.expected_source.size_bytes ||
                platform::windows_detail::filetime_to_unix_ns(barrier_basic.LastWriteTime) !=
                    request.expected_source.modified_unix_ns ||
                !same_source_revision(barrier_revision,
                                      request.expected_source.source_revision_utf8)) {
                return cancel_delete(
                    {.operation_id = request.operation_id,
                     .status = OperationStatus::source_changed,
                     .evidence = OperationEvidence::no_commit,
                     .source_present = true,
                     .destination_present = request.mode == DeleteMode::transfer_source_commit,
                     .confirmed_snapshot = {},
                     .detail_utf8 =
                         "transfer source changed before the delete barrier was committed; path=" +
                         std::to_string(barrier_path.has_value()) +
                         "; links=" + std::to_string(barrier_standard.NumberOfLinks) +
                         "; revision=" + barrier_revision});
            }
        }

        if (request.mode == DeleteMode::transfer_source_commit) {
            LARGE_INTEGER guard_size{};
            FILE_BASIC_INFO guard_basic{};
            FILE_ATTRIBUTE_TAG_INFO guard_attributes{};
            FILE_STANDARD_INFO guard_standard{};
            const auto guard_path = final_path(publication_guard.get());
            DWORD guard_stream_error{};
            const auto guard_streams_are_supported =
                platform::windows_detail::has_only_supported_transfer_data_streams(
                    publication_guard.get(), guard_stream_error);
            if (GetFileSizeEx(publication_guard.get(), &guard_size) == FALSE ||
                guard_size.QuadPart < 0 ||
                GetFileInformationByHandleEx(publication_guard.get(), FileBasicInfo, &guard_basic,
                                             sizeof(guard_basic)) == FALSE ||
                GetFileInformationByHandleEx(publication_guard.get(), FileAttributeTagInfo,
                                             &guard_attributes,
                                             sizeof(guard_attributes)) == FALSE ||
                GetFileInformationByHandleEx(publication_guard.get(), FileStandardInfo,
                                             &guard_standard, sizeof(guard_standard)) == FALSE) {
                return cancel_delete(
                    windows_failure(request, GetLastError(), OperationEvidence::no_commit));
            }
            const auto guard_identity = platform::windows_detail::query_file_identity(
                publication_guard.get(), guard_basic.ChangeTime);
            const auto &guard_revision = platform::windows_detail::matching_revision(
                guard_identity, request.expected_guard.source_revision_utf8);
            DWORD parent_error{};
            const auto parent_path = final_path(publication_parent.get());
            if (!ordinary_transfer_file(guard_attributes, guard_standard) ||
                !guard_streams_are_supported || !guard_path ||
                !same_path(*guard_path, request.guard_path) || guard_revision.empty() ||
                static_cast<std::uint64_t>(guard_size.QuadPart) !=
                    request.expected_guard.size_bytes ||
                platform::windows_detail::filetime_to_unix_ns(guard_basic.LastWriteTime) !=
                    request.expected_guard.modified_unix_ns ||
                !same_source_revision(guard_revision,
                                      request.expected_guard.source_revision_utf8) ||
                !parent_path || !same_path(*parent_path, request.guard_path.parent_path()) ||
                !parent_identity_matches(publication_parent.get(),
                                         request.guard_parent_identity_utf8, parent_error)) {
                return cancel_delete(
                    {.operation_id = request.operation_id,
                     .status = OperationStatus::source_changed,
                     .evidence = OperationEvidence::no_commit,
                     .source_present = true,
                     .destination_present = true,
                     .confirmed_snapshot = {},
                     .detail_utf8 =
                         "published destination changed after the source delete barrier"});
            }
        }

        DWORD close_error{};
        auto closed = source.close(close_error);
#ifdef VOVE_FILEOP_TEST_HOOKS
        if (const auto hook = delete_close_error_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            if (const auto injected = hook(); injected != ERROR_SUCCESS) {
                closed = false;
                close_error = injected;
            }
        }
#endif
        if (!closed) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unknown_outcome,
                    .evidence = OperationEvidence::none,
                    .platform_code = static_cast<std::int64_t>(close_error),
                    .source_present = false,
                    .source_matches_expected = false,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "delete disposition was set but handle close was not confirmed"};
        }
        const auto remaining = GetFileAttributesW(request.source.c_str());
        if (remaining == INVALID_FILE_ATTRIBUTES) {
            const auto probe_error = GetLastError();
            if (probe_error == ERROR_FILE_NOT_FOUND || probe_error == ERROR_PATH_NOT_FOUND) {
                return {.operation_id = request.operation_id,
                        .status = OperationStatus::success,
                        .evidence = OperationEvidence::committed,
                        .source_present = false,
                        .source_matches_expected = false,
                        .confirmed_snapshot = {},
                        .detail_utf8 = {}};
            }
            return windows_failure(request, probe_error, OperationEvidence::none);
        }
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::conflicting,
                .source_present = true,
                .source_matches_expected = false,
                .confirmed_snapshot = {},
                .detail_utf8 = "delete handle closed but the source path is occupied"};
    }
    auto failure = windows_failure(request, disposition_error);
    failure.detail_utf8 = "handle delete disposition could not be set: " + failure.detail_utf8;
    if (!ambiguous_status(failure.status)) {
        failure.evidence = OperationEvidence::no_commit;
    }
    return failure;
}

OperationResult create_directory_relative(const CreateDirectoryRequest &request) {
    struct PinnedDirectory {
        std::filesystem::path path;
        ScopedHandle handle;
    };
    std::vector<PinnedDirectory> pinned;
    auto directory = request.destination.parent_path();
    const auto root = stable_path_root(directory);
    for (;;) {
        ScopedHandle handle(
            CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                         sizeof(attributes)) == FALSE) {
            return windows_failure(request, GetLastError(), OperationEvidence::no_commit);
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "create-directory parent chain contains a reparse point"};
        }
        pinned.push_back({.path = directory, .handle = std::move(handle)});
        if (same_path(directory, root)) {
            break;
        }
        const auto next = directory.parent_path();
        if (next.empty() || same_path(next, directory)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "create-directory path has no stable filesystem root"};
        }
        directory = next;
    }

#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = before_create_directory_commit_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook();
    }
#endif

    for (const auto &anchor : pinned) {
        const auto observed = final_path(anchor.handle.get());
        if (!observed || !same_path(*observed, anchor.path)) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::no_commit,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "create-directory ancestor moved before publication"};
        }
    }
    DWORD identity_error{};
    if (!parent_identity_matches(pinned.front().handle.get(),
                                 request.destination_parent_revision_utf8, identity_error)) {
        return identity_error == ERROR_SUCCESS
                   ? OperationResult{.operation_id = request.operation_id,
                                     .status = OperationStatus::source_changed,
                                     .evidence = OperationEvidence::no_commit,
                                     .confirmed_snapshot = {},
                                     .detail_utf8 =
                                         "destination parent identity changed before publication"}
                   : [&] {
                         auto failure =
                             windows_failure(request, identity_error, OperationEvidence::no_commit);
                         failure.detail_utf8 += " while confirming the pinned create parent";
                         return failure;
                     }();
    }

    const auto leaf = request.destination.filename().native();
    const auto name_bytes = leaf.size() * sizeof(wchar_t);
    if (name_bytes > std::numeric_limits<USHORT>::max()) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = "create-directory leaf is too long for Windows"};
    }
    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(name_bytes);
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(leaf.data());
    OBJECT_ATTRIBUTES object_attributes{};
    InitializeObjectAttributes(&object_attributes, &name, OBJ_CASE_INSENSITIVE,
                               pinned.front().handle.get(), nullptr);
    IO_STATUS_BLOCK io_status{};
    HANDLE raw_created{};
    const auto create_status = NtCreateFile(
        &raw_created, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object_attributes,
        &io_status, nullptr, FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_READ, FILE_CREATE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
            FILE_OPEN_FOR_BACKUP_INTENT,
        nullptr, 0);
    if (create_status < 0) {
        const auto code = static_cast<DWORD>(RtlNtStatusToDosError(create_status));
        auto result = windows_failure(request, code);
        result.detail_utf8 += " while creating a directory relative to its pinned parent";
        if (!ambiguous_status(result.status)) {
            result.evidence = OperationEvidence::no_commit;
        }
        return result;
    }
    ScopedHandle created(raw_created);
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = after_create_directory_commit_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook();
    }
#endif
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_BASIC_INFO basic{};
    const auto created_path = final_path(created.get());
    if (!created_path) {
        const auto code = GetLastError();
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::none,
                .platform_code = static_cast<std::int64_t>(code),
                .destination_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 =
                    "directory creation committed but its final path could not be confirmed"};
    }
    DWORD confirmation_error{};
    if (confirmation_error == ERROR_SUCCESS &&
        GetFileInformationByHandleEx(created.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE) {
        confirmation_error = GetLastError();
    }
    if (confirmation_error == ERROR_SUCCESS &&
        GetFileInformationByHandleEx(created.get(), FileBasicInfo, &basic, sizeof(basic)) ==
            FALSE) {
        confirmation_error = GetLastError();
    }
    if (confirmation_error == ERROR_SUCCESS &&
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
         (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
         !same_path(*created_path, request.destination))) {
        confirmation_error = ERROR_INVALID_DATA;
    }
    if (confirmation_error != ERROR_SUCCESS) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::none,
                .platform_code = static_cast<std::int64_t>(confirmation_error),
                .destination_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 =
                    "directory creation committed but the created object could not be confirmed"};
    }
    const auto identity =
        platform::windows_detail::query_file_identity(created.get(), basic.ChangeTime);
    const auto &revision = platform::windows_detail::preferred_revision(identity);
    if (revision.empty()) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::none,
                .platform_code = static_cast<std::int64_t>(identity.error),
                .destination_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 =
                    "directory creation committed but its physical identity is unavailable"};
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .destination_present = true,
            .destination_matches_source = true,
            .confirmed_snapshot = {.source_revision_utf8 = revision},
            .detail_utf8 = {}};
}

OperationResult verify_private_empty_directory_relative(const CreateDirectoryRequest &request) {
    return {.operation_id = request.operation_id,
            .status = OperationStatus::unsupported,
            .evidence = OperationEvidence::no_commit,
            .confirmed_snapshot = {},
            .detail_utf8 = "POSIX private trash directory verification is unavailable"};
}

OperationResult remove_empty_directory_relative(const CreateDirectoryRequest &request) {
    return {.operation_id = request.operation_id,
            .status = OperationStatus::unsupported,
            .evidence = OperationEvidence::no_commit,
            .confirmed_snapshot = {},
            .detail_utf8 = "private trash container cleanup uses the Windows vault path"};
}

} // namespace vove::fileops::detail
