#include "file_operation_executor.hpp"
#include "file_in_use_error.hpp"

#include "operation_evidence.hpp"
#include "file_operation_platform.hpp"
#include "vove/platform/read_only_source.hpp"

#include <bit>
#include <limits>
#include <string>
#include <system_error>

#ifdef _WIN32
#include "windows/trash_security.hpp"
#include "windows/file_identity.hpp"
#include "windows/file_time.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include "posix/file_identity.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace vove::fileops::detail {
namespace {

struct ObservedSource {
    bool present{};
    bool matches{};
    bool stable_identity_matches{};
    bool remote_smb{};
    std::string storage_identity_utf8;
    OperationStatus failure{OperationStatus::not_found};
    std::int64_t platform_code{};
    std::string detail;
    SourceSnapshot snapshot;
};

OperationStatus map_open_error(const platform::SourceOpenErrorKind kind,
                               const std::int64_t platform_code) {
    if (file_in_use_error(platform_code)) {
        return OperationStatus::file_in_use;
    }
    using Source = platform::SourceOpenErrorKind;
    switch (kind) {
    case Source::not_found:
        return OperationStatus::not_found;
    case Source::access_denied:
        return OperationStatus::permission_denied;
    case Source::authentication_failed:
        return OperationStatus::authentication_required;
    case Source::timed_out:
        return OperationStatus::timed_out;
    case Source::disconnected:
        return OperationStatus::disconnected;
    case Source::not_regular_file:
        return OperationStatus::unsupported;
    case Source::invalid_path:
    case Source::too_large:
        return OperationStatus::source_changed;
    case Source::io_error:
    case Source::none:
        return OperationStatus::io_error;
    }
    return OperationStatus::io_error;
}

bool transfer_rename(const RenameMode mode) noexcept {
    return mode == RenameMode::transfer_atomic || mode == RenameMode::transfer_stage ||
           mode == RenameMode::transfer_publish || mode == RenameMode::transfer_restore ||
           mode == RenameMode::transfer_publish_replace ||
           mode == RenameMode::transfer_atomic_replace ||
           mode == RenameMode::transfer_overwrite_stage ||
           mode == RenameMode::transfer_overwrite_restore;
}

ObservedSource observe(const std::filesystem::path &path, const SourceSnapshot &expected,
                       const bool require_single_link) {
    // Reconciliation needs native identity even after another client changed the size. Opening the
    // metadata handle does not read file contents; the expected size is checked below.
    auto opened = platform::open_read_only_source(path, std::numeric_limits<std::uint64_t>::max());
    if (!opened.source) {
        return {.storage_identity_utf8 = {},
                .failure = map_open_error(opened.error.kind, opened.error.platform_code),
                .platform_code = opened.error.platform_code,
                .detail = std::move(opened.error.detail),
                .snapshot = {}};
    }
#ifdef _WIN32
    if (require_single_link) {
        FILE_STANDARD_INFO standard{};
        if (GetFileInformationByHandleEx(std::bit_cast<HANDLE>(opened.source->native_object()),
                                         FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
            return {.present = true,
                    .storage_identity_utf8 = {},
                    .failure = OperationStatus::io_error,
                    .platform_code = static_cast<std::int64_t>(GetLastError()),
                    .detail = "file link count is unavailable",
                    .snapshot = {}};
        }
        if (standard.NumberOfLinks != 1) {
            return {.present = true,
                    .storage_identity_utf8 = {},
                    .failure = OperationStatus::unsupported,
                    .detail = "VO-VE Trash does not accept files with multiple hard links",
                    .snapshot = {}};
        }
    }
#else
    static_cast<void>(require_single_link);
#endif
    const auto &source = *opened.source;
#if defined(__linux__)
    struct statfs filesystem{};
    constexpr std::uint32_t cifs_magic = 0xFF534D42U;
    constexpr std::uint32_t smb2_magic = 0xFE534D42U;
    const auto statfs_ok = ::fstatfs(static_cast<int>(source.native_object()), &filesystem) == 0;
    const auto filesystem_magic = static_cast<std::uint32_t>(filesystem.f_type);
    const auto remote_smb =
        statfs_ok && (filesystem_magic == cifs_magic || filesystem_magic == smb2_magic);
#else
    constexpr bool remote_smb = false;
#endif
    const auto storage_identity =
        remote_smb ? platform::storage_identity(source.native_object()) : std::string{};
    const SourceSnapshot observed_snapshot{.size_bytes = source.size_bytes(),
                                           .modified_unix_ns = source.modified_unix_ns(),
                                           .source_revision_utf8 = source.source_revision_utf8()};
    const bool stable_identity_matches =
        same_object_identity(expected.source_revision_utf8, source.source_revision_utf8()) ||
        same_object_after_rename(expected, observed_snapshot);
    const bool revision_matches =
        same_source_revision(expected.source_revision_utf8, source.source_revision_utf8());
    const bool matches = source.size_bytes() == expected.size_bytes &&
                         source.modified_unix_ns() == expected.modified_unix_ns && revision_matches;
    return {.present = true,
            .matches = matches,
            .stable_identity_matches = stable_identity_matches,
            .remote_smb = remote_smb,
            .storage_identity_utf8 = storage_identity,
            .failure = matches ? OperationStatus::success : OperationStatus::source_changed,
            .detail = matches ? std::string{} : "file identity changed before operation",
            .snapshot = std::move(observed_snapshot)};
}

ObservedSource observe_directory(const std::filesystem::path &path,
                                 const SourceSnapshot &expected) {
#ifdef _WIN32
    const auto handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return {.storage_identity_utf8 = {},
                .failure = file_in_use_error(error) ? OperationStatus::file_in_use
                           : error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                               ? OperationStatus::not_found
                           : error == ERROR_ACCESS_DENIED ? OperationStatus::permission_denied
                                                          : OperationStatus::io_error,
                .platform_code = static_cast<std::int64_t>(error),
                .detail = "directory is unavailable",
                .snapshot = {}};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    const auto metadata_ok =
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) != FALSE &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE &&
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) !=
            FALSE;
    if (!metadata_ok || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const auto error = metadata_ok ? ERROR_NOT_SUPPORTED : GetLastError();
        static_cast<void>(CloseHandle(handle));
        return {.present = metadata_ok,
                .storage_identity_utf8 = {},
                .failure = metadata_ok ? OperationStatus::unsupported : OperationStatus::io_error,
                .platform_code = static_cast<std::int64_t>(error),
                .detail = "trash target is not a physical directory",
                .snapshot = {}};
    }
    const auto identity = platform::windows_detail::query_file_identity(handle, basic.ChangeTime);
    const auto revision = platform::windows_detail::preferred_revision(identity);
    static_cast<void>(CloseHandle(handle));
    if (revision.empty()) {
        return {.present = true,
                .storage_identity_utf8 = {},
                .failure = OperationStatus::io_error,
                .detail = "directory identity is unavailable",
                .snapshot = {}};
    }
    SourceSnapshot snapshot{.size_bytes = 0,
                            .modified_unix_ns =
                                platform::windows_detail::filetime_to_unix_ns(basic.LastWriteTime),
                            .source_revision_utf8 = revision};
#else
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        const auto error = errno;
        return {.present = false,
                .matches = false,
                .stable_identity_matches = false,
                .remote_smb = false,
                .storage_identity_utf8 = {},
                .failure = file_in_use_error(error) ? OperationStatus::file_in_use
                           : error == ENOENT || error == ENOTDIR ? OperationStatus::not_found
                           : error == EACCES || error == EPERM ? OperationStatus::permission_denied
                                                               : OperationStatus::io_error,
                .platform_code = error,
                .detail = "directory is unavailable",
                .snapshot = {}};
    }
    struct stat status{};
    struct statfs filesystem{};
    const auto metadata_ok = ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode);
    const auto statfs_ok = ::fstatfs(descriptor, &filesystem) == 0;
    if (!metadata_ok) {
        const auto error = errno;
        static_cast<void>(::close(descriptor));
        return {.present = true,
                .matches = false,
                .stable_identity_matches = false,
                .remote_smb = false,
                .storage_identity_utf8 = {},
                .failure = OperationStatus::unsupported,
                .platform_code = error,
                .detail = "trash target is not a physical directory",
                .snapshot = {}};
    }
    constexpr std::uint32_t cifs_magic = 0xFF534D42U;
    constexpr std::uint32_t smb2_magic = 0xFE534D42U;
    const auto filesystem_magic = static_cast<std::uint32_t>(filesystem.f_type);
    const auto remote_smb =
        statfs_ok && (filesystem_magic == cifs_magic || filesystem_magic == smb2_magic);
    const auto storage_identity =
        remote_smb ? platform::storage_identity(static_cast<platform::NativeFileObject>(descriptor))
                   : std::string{};
    SourceSnapshot snapshot{
        .size_bytes = 0,
        .modified_unix_ns = static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL +
                            status.st_mtim.tv_nsec,
        .source_revision_utf8 =
            platform::posix_identity::revision_from_descriptor(descriptor, status)};
    static_cast<void>(::close(descriptor));
#endif
    const auto stable =
        same_object_identity(expected.source_revision_utf8, snapshot.source_revision_utf8);
    return {.present = true,
            .matches = stable,
            .stable_identity_matches = stable,
#ifdef _WIN32
            .remote_smb = false,
            .storage_identity_utf8 = {},
#else
            .remote_smb = remote_smb,
            .storage_identity_utf8 = storage_identity,
#endif
            .failure = stable ? OperationStatus::success : OperationStatus::source_changed,
            .detail = stable ? std::string{} : "directory identity changed before operation",
            .snapshot = std::move(snapshot)};
}

ObservedSource observe_object(const std::filesystem::path &path, const SourceSnapshot &expected,
                              const bool require_single_link,
                              const OperationObjectKind object_kind) {
    return object_kind == OperationObjectKind::directory
               ? observe_directory(path, expected)
               : observe(path, expected, require_single_link);
}

bool expected_storage_matches(const std::string_view expected,
                              const ObservedSource &observed) noexcept {
    return expected.empty() || (observed.remote_smb && observed.storage_identity_utf8 == expected);
}

bool expected_storage_matches_parent(const std::string_view expected,
                                     const std::filesystem::path &path) noexcept {
    if (expected.empty()) {
        return true;
    }
    try {
        return platform::storage_identity(path.parent_path()) == expected;
    } catch (...) {
        return false;
    }
}

#ifdef _WIN32
bool case_only_rename(const RenameRequest &request) {
    const auto &source = request.source.native();
    const auto &destination = request.destination.native();
    return source != destination &&
           CompareStringOrdinal(source.c_str(), static_cast<int>(source.size()),
                                destination.c_str(), static_cast<int>(destination.size()),
                                TRUE) == CSTR_EQUAL;
}

bool destination_spelling_is_published(const std::filesystem::path &destination) {
    std::error_code error;
    for (std::filesystem::directory_iterator current(destination.parent_path(), error), end;
         !error && current != end; current.increment(error)) {
        if (current->path().filename().native() == destination.filename().native()) {
            return true;
        }
    }
    return false;
}
#endif

} // namespace

OperationResult reconcile_rename(const RenameRequest &request) {
    OperationResult result;
    result.operation_id = request.operation_id;
    std::string validation;
    if (!valid_rename_request(request, validation)) {
        result.status = OperationStatus::invalid_request;
        result.detail_utf8 = std::move(validation);
        return result;
    }
    const auto verify_anchor = [&request]() {
        return detail::verify_rename_destination_anchor(request);
    };
    auto anchor = verify_anchor();
    if (!anchor.ok()) {
        return anchor;
    }

    const bool trash_mode =
        is_trash_store_mode(request.mode) || is_trash_restore_mode(request.mode);
    const bool source_is_private_payload = is_trash_restore_mode(request.mode);
    const auto expected_storage =
        trash_mode ? std::string_view(request.source_parent_identity_utf8) : std::string_view{};
    const auto source =
        observe_object(request.source, request.expected_source, trash_mode, request.object_kind);
    const auto destination = observe_object(request.destination, request.expected_source,
                                            trash_mode, request.object_kind);
#ifdef _WIN32
    if (preserves_trash_permissions(request.mode)) {
        for (const auto &path : {request.source, request.destination}) {
            const auto &observed = path == request.source ? source : destination;
            if (!observed.present || !observed.stable_identity_matches) continue;
            const auto checked = verify_preserved_trash_payload(path, request.expected_source,
                request.trash_security_baseline_sddl_utf8, true);
            if (!checked.ok()) {
                result.status = OperationStatus::source_changed;
                result.detail_utf8 = checked.detail_utf8;
                return result;
            }
        }
    }
#endif
    const auto source_storage_matches = expected_storage_matches(expected_storage, source);
    const auto destination_storage_matches =
        expected_storage_matches(expected_storage, destination);
    const auto source_matches_after_remount =
        source_is_private_payload && source.remote_smb &&
        same_private_trash_payload_after_remount(request.expected_source, source.snapshot,
                                                 request.source_parent_identity_utf8,
                                                 source.storage_identity_utf8);
    const auto destination_matches_after_remount =
        trash_mode && destination.remote_smb &&
        same_private_trash_payload_after_remount(request.expected_source, destination.snapshot,
                                                 request.source_parent_identity_utf8,
                                                 destination.storage_identity_utf8);
    const auto source_matches = source.matches && source_storage_matches;
    const auto source_equivalent =
        (source.stable_identity_matches && source_storage_matches) || source_matches_after_remount;
    const auto destination_equivalent =
        (destination.stable_identity_matches && destination_storage_matches) ||
        destination_matches_after_remount;
    result.source_present = source.present;
    result.source_matches_expected = source_matches || source_matches_after_remount;
    result.destination_present = destination.present;
    result.destination_matches_source = destination_equivalent;
    const bool destination_known_not_expected =
        (destination.present && !destination_equivalent) ||
        (!destination.present && destination.failure == OperationStatus::not_found);
    if (request.mode == RenameMode::transfer_overwrite_stage && destination_equivalent) {
        anchor = verify_anchor();
        if (!anchor.ok()) {
            return anchor;
        }
        result.status = OperationStatus::unknown_outcome;
        result.evidence = OperationEvidence::committed;
        result.destination_matches_source = false;
        result.confirmed_snapshot = destination.snapshot;
        result.detail_utf8 = "overwrite evacuation committed without a surviving content proof";
        return result;
    }
#ifdef _WIN32
    const auto is_case_only = case_only_rename(request);
    if (destination.stable_identity_matches &&
        destination_spelling_is_published(request.destination) &&
        (is_case_only || !source.stable_identity_matches)) {
        anchor = verify_anchor();
        if (!anchor.ok()) {
            return anchor;
        }
        result.status = OperationStatus::success;
        result.evidence = OperationEvidence::committed;
        result.confirmed_snapshot = destination.snapshot;
        return result;
    }
#endif
    if (!source_equivalent && destination_equivalent) {
        anchor = verify_anchor();
        if (!anchor.ok()) {
            return anchor;
        }
        result.status = OperationStatus::success;
        result.evidence = OperationEvidence::committed;
        result.confirmed_snapshot = destination.snapshot;
        return result;
    }
    if (source.present && (source_matches || source_matches_after_remount) && destination.present) {
        result.status = OperationStatus::conflict;
        result.evidence =
            destination_equivalent ? OperationEvidence::none : OperationEvidence::no_commit;
        result.platform_code = destination.platform_code;
        result.detail_utf8 = "destination is occupied by another object";
        if ((request.mode == RenameMode::transfer_publish ||
             request.mode == RenameMode::transfer_atomic) &&
            !destination_equivalent) {
            result.confirmed_snapshot = destination.snapshot;
        }
        return result;
    }
    if (source.present && !source_matches && !source_matches_after_remount) {
        result.status = OperationStatus::source_changed;
        result.evidence = source_equivalent && destination_known_not_expected
                              ? OperationEvidence::no_commit
                              : OperationEvidence::none;
        result.platform_code = source.platform_code;
        result.detail_utf8 = "source identity changed during rename";
        return result;
    }
    if (source_equivalent && destination_known_not_expected) {
        result.evidence = OperationEvidence::no_commit;
    }
    result.status = OperationStatus::unknown_outcome;
    result.platform_code =
        source.platform_code != 0 ? source.platform_code : destination.platform_code;
    result.detail_utf8 = "rename outcome is uncertain; the operation was not repeated";
    return result;
}

OperationResult execute_rename(const RenameRequest &request) {
    std::string validation;
    if (!valid_rename_request(request, validation)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = std::move(validation)};
    }
    const bool trash_mode =
        is_trash_store_mode(request.mode) || is_trash_restore_mode(request.mode);
    const bool source_is_private_payload = is_trash_restore_mode(request.mode);
    const auto expected_storage =
        trash_mode ? std::string_view(request.source_parent_identity_utf8) : std::string_view{};
    const auto source =
        observe_object(request.source, request.expected_source, trash_mode, request.object_kind);
    const auto source_storage_matches = expected_storage_matches(expected_storage, source);
    const auto source_matches = source.matches && source_storage_matches;
    if (!source.present || !source_matches) {
        const auto refreshable_after_remount =
            source_is_private_payload &&
            same_private_trash_payload_after_remount(request.expected_source, source.snapshot,
                                                     request.source_parent_identity_utf8,
                                                     source.storage_identity_utf8);
        if ((transfer_rename(request.mode) || source_is_private_payload) && source.present &&
            source.remote_smb &&
            ((source_storage_matches &&
              same_object_after_rename(request.expected_source, source.snapshot)) ||
             refreshable_after_remount)) {
            auto refreshed = request;
            refreshed.expected_source = source.snapshot;
            auto result = rename_no_replace(refreshed);
            if (result.status == OperationStatus::unknown_outcome ||
                result.status == OperationStatus::timed_out ||
                result.status == OperationStatus::disconnected ||
                result.status == OperationStatus::io_error) {
                result = merge_reconciliation(std::move(result), reconcile_rename(refreshed));
            }
            return result;
        }
        const auto storage_changed = source.present && !source_storage_matches;
        return {.operation_id = request.operation_id,
                .status = storage_changed ? OperationStatus::source_changed : source.failure,
                .evidence = OperationEvidence::no_commit,
                .platform_code = source.platform_code,
                .source_present = source.present,
                .source_matches_expected = source_matches,
                .confirmed_snapshot = {},
                .detail_utf8 = storage_changed ? "source storage identity changed before rename"
                                               : (source.detail.empty() ? "source is unavailable"
                                                                        : source.detail)};
    }
    auto result = rename_no_replace(request);
    if (result.status == OperationStatus::success) {
        return result;
    }
    if (result.status == OperationStatus::unknown_outcome ||
        result.status == OperationStatus::timed_out ||
        result.status == OperationStatus::disconnected ||
        result.status == OperationStatus::io_error) {
        result = merge_reconciliation(std::move(result), reconcile_rename(request));
    }
    return result;
}

OperationResult reconcile_delete(const DeleteRequest &request) {
    OperationResult result;
    result.operation_id = request.operation_id;
    std::string validation;
    if (!valid_delete_request(request, validation)) {
        result.status = OperationStatus::invalid_request;
        result.evidence = OperationEvidence::no_commit;
        result.detail_utf8 = std::move(validation);
        return result;
    }

    const bool trash_purge = is_trash_purge_mode(request.mode);
    const auto expected_storage =
        trash_purge ? std::string_view(request.source_parent_identity_utf8) : std::string_view{};
    const auto source =
        observe_object(request.source, request.expected_source, trash_purge, request.object_kind);
#ifdef _WIN32
    if (preserves_trash_permissions(request.mode) && source.present && source.stable_identity_matches) {
        const auto checked = verify_preserved_trash_payload(request.source, request.expected_source,
            request.trash_security_baseline_sddl_utf8, true);
        if (!checked.ok()) {
            result.status = OperationStatus::source_changed;
            result.detail_utf8 = checked.detail_utf8;
            return result;
        }
    }
#endif
    const auto source_storage_matches = expected_storage_matches(expected_storage, source);
    const auto source_matches_after_remount =
        trash_purge && source.remote_smb &&
        same_private_trash_payload_after_remount(request.expected_source, source.snapshot,
                                                 request.source_parent_identity_utf8,
                                                 source.storage_identity_utf8);
    result.source_present = source.present;
    result.source_matches_expected =
        (source.matches && source_storage_matches) || source_matches_after_remount;
    if (!source.present && source.failure == OperationStatus::not_found) {
        if (!expected_storage_matches_parent(expected_storage, request.source)) {
            result.status = OperationStatus::source_changed;
            result.evidence = OperationEvidence::conflicting;
            result.detail_utf8 = "source storage identity changed while delete was pending";
            return result;
        }
        if (trash_purge || request.mode == DeleteMode::transfer_temp_cleanup ||
            request.mode == DeleteMode::transfer_overwrite_cleanup) {
            result.status = OperationStatus::success;
            result.evidence = OperationEvidence::committed;
            result.detail_utf8 = trash_purge
                                     ? "private trash payload is absent after purge intent"
                                     : "owned transfer temporary path is absent after cleanup";
            return result;
        }
        result.status = OperationStatus::unknown_outcome;
        result.evidence = OperationEvidence::none;
        result.detail_utf8 =
            "delete outcome is uncertain because absence cannot distinguish deletion from a "
            "concurrent remote rename";
        return result;
    }
    if ((source.matches && source_storage_matches) || source_matches_after_remount) {
        result.status = OperationStatus::conflict;
        result.evidence = OperationEvidence::no_commit;
        result.confirmed_snapshot = source.snapshot;
        result.detail_utf8 = "reconciliation confirmed that delete did not commit";
        return result;
    }
    if (source.stable_identity_matches && source_storage_matches) {
        result.status = OperationStatus::source_changed;
        result.evidence = OperationEvidence::no_commit;
        result.confirmed_snapshot = source.snapshot;
        result.detail_utf8 = "source changed while delete was pending";
        return result;
    }
    if (source.present) {
        result.status = OperationStatus::unknown_outcome;
        result.evidence = OperationEvidence::conflicting;
        result.confirmed_snapshot = source.snapshot;
        result.detail_utf8 =
            "delete outcome is uncertain because the source path now names another object";
        return result;
    }
    result.status = OperationStatus::unknown_outcome;
    result.evidence = OperationEvidence::none;
    result.platform_code = source.platform_code;
    result.detail_utf8 =
        source.detail.empty() ? "delete outcome could not be reconciled" : source.detail;
    return result;
}

OperationResult execute_delete(const DeleteRequest &request) {
    std::string validation;
    if (!valid_delete_request(request, validation)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = std::move(validation)};
    }
    const auto source =
        observe_object(request.source, request.expected_source,
                       is_trash_purge_mode(request.mode), request.object_kind);
    const auto trash_purge = is_trash_purge_mode(request.mode);
    const auto expected_storage =
        trash_purge ? std::string_view(request.source_parent_identity_utf8) : std::string_view{};
    const auto source_storage_matches = expected_storage_matches(expected_storage, source);
    const auto source_matches = source.matches && source_storage_matches;
    if (!source.present || !source_matches) {
        const auto refreshable_after_remount =
            is_trash_purge_mode(request.mode) &&
            same_private_trash_payload_after_remount(request.expected_source, source.snapshot,
                                                     request.source_parent_identity_utf8,
                                                     source.storage_identity_utf8);
        if ((request.mode == DeleteMode::transfer_temp_cleanup ||
             is_trash_purge_mode(request.mode)) &&
            source.present && source.remote_smb &&
            ((source_storage_matches &&
              same_object_after_rename(request.expected_source, source.snapshot)) ||
             refreshable_after_remount)) {
            auto refreshed = request;
            refreshed.expected_source = source.snapshot;
            auto result = permanent_delete_remote(refreshed);
            if (result.status == OperationStatus::unknown_outcome ||
                result.status == OperationStatus::timed_out ||
                result.status == OperationStatus::disconnected ||
                result.status == OperationStatus::io_error) {
                result = merge_reconciliation(std::move(result), reconcile_delete(refreshed));
            }
            return result;
        }
        const auto storage_changed = source.present && !source_storage_matches;
        return {.operation_id = request.operation_id,
                .status = storage_changed ? OperationStatus::source_changed : source.failure,
                .evidence = OperationEvidence::no_commit,
                .platform_code = source.platform_code,
                .source_present = source.present,
                .source_matches_expected = source_matches,
                .confirmed_snapshot = {},
                .detail_utf8 = storage_changed ? "source storage identity changed before delete"
                                               : (source.detail.empty() ? "source is unavailable"
                                                                        : source.detail)};
    }
    auto result = permanent_delete_remote(request);
    if (result.status == OperationStatus::success) {
        return result;
    }
    if (result.status == OperationStatus::unknown_outcome ||
        result.status == OperationStatus::timed_out ||
        result.status == OperationStatus::disconnected ||
        result.status == OperationStatus::io_error) {
        result = merge_reconciliation(std::move(result), reconcile_delete(request));
    }
    return result;
}

OperationResult reconcile_create_directory(const CreateDirectoryRequest &request) {
    OperationResult result{
        .operation_id = request.operation_id, .confirmed_snapshot = {}, .detail_utf8 = {}};
    std::string validation;
    if (!valid_create_directory_request(request, validation)) {
        result.status = OperationStatus::invalid_request;
        result.evidence = OperationEvidence::no_commit;
        result.detail_utf8 = std::move(validation);
        return result;
    }
    const auto parent = platform::query_directory_revision(request.destination.parent_path());
    if (!parent) {
        result.status = map_open_error(parent.error.kind, parent.error.platform_code);
        result.platform_code = parent.error.platform_code;
        result.detail_utf8 = parent.error.detail;
        return result;
    }
    if (!request.destination_parent_revision_utf8.empty() &&
        !same_object_identity(parent.revision_utf8, request.destination_parent_revision_utf8)) {
        result.status = OperationStatus::source_changed;
        result.evidence = OperationEvidence::conflicting;
        result.detail_utf8 = "destination parent identity changed";
        return result;
    }

    const auto destination = platform::query_directory_revision(request.destination);
    if (request.mode == CreateDirectoryMode::trash_internal_remove_empty) {
        if (destination ||
            destination.error.kind == platform::SourceOpenErrorKind::not_regular_file) {
            result.status = OperationStatus::conflict;
            result.evidence = OperationEvidence::no_commit;
            result.destination_present = true;
            if (destination) {
                result.confirmed_snapshot.source_revision_utf8 = destination.revision_utf8;
            }
            result.detail_utf8 = "trash container remains present";
            return result;
        }
        if (destination.error.kind == platform::SourceOpenErrorKind::not_found) {
            result.status = OperationStatus::success;
            result.evidence = OperationEvidence::committed;
            result.destination_present = false;
            result.detail_utf8.clear();
            return result;
        }
        result.status = map_open_error(destination.error.kind, destination.error.platform_code);
        result.platform_code = destination.error.platform_code;
        result.detail_utf8 = destination.error.detail;
        return result;
    }
    if (destination && request.mode == CreateDirectoryMode::trash_internal) {
        return verify_private_empty_directory_relative(request);
    }
    if (destination) {
        result.status = OperationStatus::unknown_outcome;
        result.evidence = OperationEvidence::conflicting;
        result.destination_present = true;
        result.confirmed_snapshot.source_revision_utf8 = destination.revision_utf8;
        result.detail_utf8 =
            "destination directory exists but ownership of the create result is unproven";
        return result;
    }
    if (destination.error.kind == platform::SourceOpenErrorKind::not_found) {
        result.status = OperationStatus::unknown_outcome;
        result.evidence = OperationEvidence::none;
        result.detail_utf8 = "destination directory is absent; create ownership is unproven";
        return result;
    }
    if (destination.error.kind == platform::SourceOpenErrorKind::not_regular_file) {
        result.status = OperationStatus::unknown_outcome;
        result.evidence = OperationEvidence::conflicting;
        result.destination_present = true;
        result.detail_utf8 = "destination path is occupied, but create ownership is unproven";
        return result;
    }
    result.status = map_open_error(destination.error.kind, destination.error.platform_code);
    result.platform_code = destination.error.platform_code;
    result.detail_utf8 = destination.error.detail;
    return result;
}

OperationResult execute_create_directory(const CreateDirectoryRequest &request) {
    std::string validation;
    if (!valid_create_directory_request(request, validation)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = std::move(validation)};
    }
    const auto parent = platform::query_directory_revision(request.destination.parent_path());
    if (!parent) {
        return {.operation_id = request.operation_id,
                .status = map_open_error(parent.error.kind, parent.error.platform_code),
                .evidence = OperationEvidence::no_commit,
                .platform_code = parent.error.platform_code,
                .confirmed_snapshot = {},
                .detail_utf8 = parent.error.detail};
    }
    if (!request.destination_parent_revision_utf8.empty() &&
        !same_object_identity(parent.revision_utf8, request.destination_parent_revision_utf8)) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::source_changed,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = "destination parent identity changed before creation"};
    }

    if (request.mode == CreateDirectoryMode::trash_internal_remove_empty) {
        return remove_empty_directory_relative(request);
    }

    const auto before = platform::query_directory_revision(request.destination);
    if (before || before.error.kind == platform::SourceOpenErrorKind::not_regular_file) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::conflict,
                .evidence = OperationEvidence::no_commit,
                .destination_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "destination path already exists"};
    }
    if (before.error.kind != platform::SourceOpenErrorKind::not_found) {
        return {.operation_id = request.operation_id,
                .status = map_open_error(before.error.kind, before.error.platform_code),
                .evidence = OperationEvidence::no_commit,
                .platform_code = before.error.platform_code,
                .confirmed_snapshot = {},
                .detail_utf8 = before.error.detail};
    }

    return create_directory_relative(request);
}

} // namespace vove::fileops::detail
