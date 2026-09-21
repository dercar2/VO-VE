#include "vove/fileops/trash_coordinator.hpp"

#include "vove/fileops/trash_catalog.hpp"

#include "vove/core/reserved_names.hpp"
#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/current_operation_lease.hpp"
#include "vove/fileops/durable_journal.hpp"
#include "vove/platform/read_only_source.hpp"

#include "operation_evidence.hpp"
#include "trash_coordinator_test_hooks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <exception>
#include <mutex>
#include <limits>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#ifdef _WIN32
#include "windows/trash_security.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace vove::fileops {

#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
namespace detail {
namespace {

std::atomic<TrashCleanupCrashHook> trash_cleanup_crash_hook{};
std::atomic<TrashManifestRepublishHook> trash_manifest_republish_hook{};
} // namespace

void set_trash_cleanup_crash_hook(const TrashCleanupCrashHook hook) noexcept {
    trash_cleanup_crash_hook.store(hook, std::memory_order_release);
}

void set_trash_manifest_republish_hook(const TrashManifestRepublishHook hook) noexcept {
    trash_manifest_republish_hook.store(hook, std::memory_order_release);
}
} // namespace detail
#endif

detail::TrashFreeSpaceState
detail::classify_trash_free_space(const std::uintmax_t available,
                                  const std::error_code &error) noexcept {
    if (error) {
        return TrashFreeSpaceState::unavailable;
    }
    return available < kTrashCriticalFreeSpaceBytes ? TrashFreeSpaceState::insufficient
                                                    : TrashFreeSpaceState::available;
}

namespace {

std::string diagnostic_path_utf8(const std::filesystem::path &path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

std::optional<TrashResult> verify_transaction_journal_budget(const TrashTransaction &transaction,
                                                             const std::size_t total_items) {
    static_assert(kMaximumCurrentOperationPayloadBytes > kMaximumOperationPayloadBytes);
    try {
        const auto encoded = encode_trash_transaction(transaction);
        if (encoded.size() <=
            kMaximumCurrentOperationPayloadBytes - kMaximumOperationPayloadBytes) {
            return std::nullopt;
        }
    } catch (const std::length_error &) {
    }
    return TrashResult{.status = TrashRunStatus::unsupported,
                       .operation_status = OperationStatus::unsupported,
                       .completed = 0,
                       .total = total_items,
                       .manifest_path = {},
                       .stored_paths = {},
                       .detail_utf8 = "trash metadata exceeds the bounded journal budget"};
}

std::uint64_t next_trash_operation_id() noexcept {
    static std::atomic<std::uint64_t> sequence{1};
    auto operation_id =
        static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^
        sequence.fetch_add(1, std::memory_order_relaxed);
    if (operation_id == 0) {
        operation_id = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    }
    return operation_id;
}

std::optional<TrashResult> capture_trash_directories(std::vector<TrashSource> &sources,
                                                     const std::uint64_t operation_id) {
    std::size_t total_entries{};
    for (std::size_t index{}; index < sources.size(); ++index) {
        auto &source = sources[index];
        if (source.kind != TrashItemKind::directory) {
            source.payload_bytes = source.snapshot.size_bytes;
            source.directory_entries.clear();
            continue;
        }
        const auto probe_name = std::string("vove-trash-capture-") + std::to_string(operation_id) +
                                '-' + std::to_string(index);
        const auto planned = plan_basic_directory_move(
            {.source = source.path,
             .destination = source.path.parent_path() / std::filesystem::path(probe_name),
             .operation_id = operation_id ^ (static_cast<std::uint64_t>(index + 1U) << 32U)});
        if (!planned.ok()) {
            const auto status = planned.status == BasicDirectoryTransferStatus::unsupported
                                    ? TrashRunStatus::unsupported
                                    : TrashRunStatus::invalid_request;
            return TrashResult{
                .status = status,
                .operation_status = planned.status == BasicDirectoryTransferStatus::not_found
                                        ? OperationStatus::not_found
                                    : planned.status == BasicDirectoryTransferStatus::source_changed
                                        ? OperationStatus::source_changed
                                    : planned.status == BasicDirectoryTransferStatus::unsupported
                                        ? OperationStatus::unsupported
                                    : planned.status == BasicDirectoryTransferStatus::file_in_use
                                        ? OperationStatus::file_in_use
                                        : OperationStatus::io_error,
                .completed = 0,
                .total = sources.size(),
                .manifest_path = {},
                .stored_paths = {},
                .detail_utf8 = planned.detail_utf8.empty() ? "trash directory could not be captured"
                                                           : planned.detail_utf8};
        }
        if (!same_source_revision(source.snapshot.source_revision_utf8,
                                  planned.manifest.source_revision_utf8)) {
            return TrashResult{.status = TrashRunStatus::invalid_request,
                               .operation_status = OperationStatus::source_changed,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 = "trash directory changed while it was captured"};
        }
        if (planned.manifest.entries.size() > kMaximumTrashDirectoryEntries - total_entries) {
            return TrashResult{.status = TrashRunStatus::unsupported,
                               .operation_status = OperationStatus::unsupported,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 = "trash directory exceeds the 10000-entry bound"};
        }
        total_entries += planned.manifest.entries.size();
        source.payload_bytes = planned.manifest.total_bytes;
        source.directory_entries = planned.manifest.entries;
        std::ranges::sort(source.directory_entries, [](const auto &left, const auto &right) {
            if (left.depth != right.depth) {
                return left.depth > right.depth;
            }
            return left.relative_path.generic_u8string() > right.relative_path.generic_u8string();
        });
    }
    return std::nullopt;
}

SourceSnapshot directory_entry_snapshot(const BasicDirectoryTransferEntry &entry) {
    return {.size_bytes = entry.size_bytes,
            .modified_unix_ns = entry.modified_unix_ns,
            .source_revision_utf8 = entry.source_revision_utf8};
}

bool directory_entry_is_directory(const BasicDirectoryTransferEntry &entry) noexcept {
    return entry.kind == BasicDirectoryTransferEntryKind::directory;
}

#ifdef _WIN32
void update_directory_entry_snapshot(BasicDirectoryTransferEntry &entry,
                                     const SourceSnapshot &snapshot) {
    entry.size_bytes = snapshot.size_bytes;
    entry.modified_unix_ns = snapshot.modified_unix_ns;
    entry.source_revision_utf8 = snapshot.source_revision_utf8;
}

std::optional<detail::TrashSecurityResult> capture_trash_source_security(TrashSource &source) {
    const auto directory = source.kind == TrashItemKind::directory;
    auto root =
        vove::fileops::detail::capture_trash_security(source.path, source.snapshot, directory);
    if (!root.ok()) {
        root.detail_utf8 += ": " + diagnostic_path_utf8(source.path);
        return root;
    }
    source.snapshot = root.snapshot;
    source.original_security_descriptor_sddl_utf8 = std::move(root.original_sddl_utf8);
    if (!directory) {
        return std::nullopt;
    }
    source.directory_security_descriptors_sddl_utf8.clear();
    source.directory_security_descriptors_sddl_utf8.reserve(source.directory_entries.size());
    for (auto &entry : source.directory_entries) {
        auto captured = vove::fileops::detail::capture_trash_security(
            source.path / entry.relative_path, directory_entry_snapshot(entry),
            directory_entry_is_directory(entry));
        if (!captured.ok()) {
            captured.detail_utf8 += ": " + diagnostic_path_utf8(source.path / entry.relative_path);
            return captured;
        }
        update_directory_entry_snapshot(entry, captured.snapshot);
        source.directory_security_descriptors_sddl_utf8.push_back(
            std::move(captured.original_sddl_utf8));
    }
    return std::nullopt;
}
#endif

void invoke_cleanup_crash_hook(const detail::TrashCleanupCrashPoint point) {
#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
    if (const auto hook = detail::trash_cleanup_crash_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook(point);
    }
#else
    static_cast<void>(point);
#endif
}

void invoke_manifest_republish_hook(const std::filesystem::path &path) {
#if defined(VOVE_TRASH_COORDINATOR_TEST_HOOKS)
    if (const auto hook = detail::trash_manifest_republish_hook.load(std::memory_order_acquire);
        hook != nullptr) {
        hook(path);
    }
#else
    static_cast<void>(path);
#endif
}

enum class StepOutcome : std::uint8_t {
    committed,
    no_commit,
    recovery_required,
    journal_error,
    stopped,
};

struct StepResult {
    StepOutcome outcome{StepOutcome::recovery_required};
    OperationStatus status{OperationStatus::io_error};
    std::string detail;
};

struct PendingOperation {
    std::mutex mutex;
    std::condition_variable completed;
    std::optional<OperationResult> result;
};

std::size_t count_location(const TrashTransaction &transaction, const TrashItemLocation location) {
    std::size_t count{};
    for (const auto &item : transaction.items) {
        count += item.location == location ? 1U : 0U;
    }
    return count;
}

std::size_t progress_completed(const TrashTransaction &transaction) {
    if (transaction.phase == TrashPhase::restoring) {
        return count_location(transaction, TrashItemLocation::source);
    }
    if (transaction.phase == TrashPhase::purging) {
        return count_location(transaction, TrashItemLocation::deleted);
    }
    return count_location(transaction, TrashItemLocation::stored);
}

std::uint64_t step_operation_id(const TrashTransaction &transaction, const TrashStep step,
                                const std::size_t index) noexcept {
    constexpr std::uint64_t mix = 0x9e3779b97f4a7c15ULL;
    constexpr std::uint64_t cursor_mix = 0xd6e8feb86659fd93ULL;
    const auto cursor = step == TrashStep::purge && index < transaction.items.size()
                            ? transaction.items[index].directory_purge_cursor
                            : 0U;
    const auto base =
        transaction.operation_id ^ (mix * (index + 1U)) ^ (static_cast<std::uint64_t>(step) << 56U);
    const auto value =
        step == TrashStep::purge && transaction.items[index].kind == TrashItemKind::directory
            ? base ^ (cursor_mix * (static_cast<std::uint64_t>(cursor) + 1U))
            : base;
    return value == 0 ? transaction.operation_id : value;
}

std::filesystem::path manifest_path(const std::filesystem::path &directory,
                                    const std::uint64_t operation_id) {
    std::array<char, 32> operation{};
    const auto [end, error] =
        std::to_chars(operation.data(), operation.data() + operation.size(), operation_id, 16);
    if (error != std::errc{}) {
        throw std::runtime_error("trash manifest filename could not be generated");
    }
    return directory / (std::string(operation.data(), end) + ".vtrash");
}

std::string journal_error_detail(const DurableJournalResult &result, const std::string_view label) {
    if (result.status == DurableJournalStatus::payload_too_large) {
        return std::string(label) + " exceeds its size limit";
    }
    if (result.status == DurableJournalStatus::payload_mismatch) {
        return std::string(label) + " changed after it was verified";
    }
    if (result.error) {
        return std::string(label) + " I/O failed: " + result.error.message();
    }
    return std::string(label) + " could not be updated";
}

CurrentOperationKind operation_kind(const TrashTransaction &transaction) {
    switch (transaction.phase) {
    case TrashPhase::prepared:
    case TrashPhase::moving:
    case TrashPhase::rollback:
    case TrashPhase::rollback_container_remove_intent:
    case TrashPhase::manifest_intent:
    case TrashPhase::published:
        return CurrentOperationKind::trash_move;
    case TrashPhase::restore_prepared:
    case TrashPhase::restoring:
    case TrashPhase::restore_rollback:
    case TrashPhase::manifest_remove_intent:
    case TrashPhase::restore_container_remove_intent:
    case TrashPhase::restored:
        return CurrentOperationKind::trash_restore;
    case TrashPhase::purge_prepared:
    case TrashPhase::purging:
    case TrashPhase::purge_manifest_remove_intent:
    case TrashPhase::purge_container_remove_intent:
    case TrashPhase::purged:
        return CurrentOperationKind::trash_purge;
    }
    return CurrentOperationKind::trash_move;
}

bool same_manifest_identity(const TrashTransaction &manifest, const TrashTransaction &transaction,
                            const bool allow_restore_rebase) {
    if (manifest.operation_id != transaction.operation_id ||
        manifest.created_unix_ns != transaction.created_unix_ns ||
        manifest.items.size() != transaction.items.size()) {
        return false;
    }
    for (std::size_t index{}; index < manifest.items.size(); ++index) {
        const auto &left = manifest.items[index];
        const auto &right = transaction.items[index];
        auto same_payload_identity =
            same_object_identity(left.current_snapshot.source_revision_utf8,
                                 right.current_snapshot.source_revision_utf8);
#ifndef _WIN32
        same_payload_identity =
            same_payload_identity || same_private_trash_payload_after_remount(
                                         left.current_snapshot, right.current_snapshot,
                                         left.storage_identity_utf8, right.storage_identity_utf8);
#endif
        if (left.original != right.original ||
            (!allow_restore_rebase && left.restore_path != right.restore_path) ||
            left.stored != right.stored ||
            left.original_security_descriptor_sddl_utf8 !=
                right.original_security_descriptor_sddl_utf8 ||
            left.storage_identity_utf8 != right.storage_identity_utf8 || !same_payload_identity) {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
bool ensure_trash_container(const TrashTransaction &transaction, std::string &detail) {
    const auto container = transaction.items.front().stored.parent_path();
    std::wstring sid;
    if (!vove::fileops::detail::current_user_sid_text(sid, detail) ||
        !vove::fileops::detail::secure_owned_trash_vault(container, sid, detail)) {
        return false;
    }
    auto attributes = GetFileAttributesW(container.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        detail = "trash container is not a physical directory";
        return false;
    }
    attributes |=
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (SetFileAttributesW(container.c_str(), attributes) == FALSE) {
        detail = "trash container attributes could not be secured: Windows error " +
                 std::to_string(GetLastError());
        return false;
    }
    return true;
}
#endif

#ifdef _WIN32
bool remove_empty_trash_container(const TrashTransaction &transaction, std::string &detail,
                                  const bool tolerate_foreign_entries = false) {
    const auto container = transaction.items.front().stored.parent_path();
    return vove::fileops::detail::remove_empty_trash_container_handle_bound(
        container, tolerate_foreign_entries, detail);
}
#endif

#ifdef _WIN32
struct VolumeIdentity {
    std::filesystem::path root;
    std::wstring guid;
    DWORD serial{};
};

struct TrashVaultSelection {
    TrashRunStatus status{TrashRunStatus::storage_unavailable};
    OperationStatus operation_status{OperationStatus::io_error};
    std::filesystem::path path;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept {
        return status == TrashRunStatus::success;
    }
};

std::optional<VolumeIdentity> volume_identity(const std::filesystem::path &path);

std::optional<std::filesystem::path> canonical_volume_path(const std::filesystem::path &path,
                                                           std::string &detail) {
    const auto volume = volume_identity(path);
    if (!volume) {
        detail = "trash volume identity is unavailable";
        return std::nullopt;
    }
    std::array<wchar_t, 32'768> full{};
    const auto written =
        GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (written == 0 || static_cast<std::size_t>(written) >= full.size()) {
        detail = "trash source path could not be normalized: Windows error " +
                 std::to_string(GetLastError());
        return std::nullopt;
    }
    const std::wstring_view absolute(full.data(), written);
    const auto &mount = volume->root.native();
    if (absolute.size() <= mount.size() ||
        CompareStringOrdinal(absolute.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                             static_cast<int>(mount.size()), mount.data(),
                             static_cast<int>(mount.size()), TRUE) != CSTR_EQUAL) {
        detail = "trash source is outside its reported volume mount";
        return std::nullopt;
    }
    const auto relative = absolute.substr(mount.size());
    if (relative.empty() || relative.front() == L'\\' || relative.front() == L'/') {
        detail = "trash source has no safe volume-relative path";
        return std::nullopt;
    }
    return std::filesystem::path(volume->guid) / std::filesystem::path(relative);
}

std::optional<std::filesystem::path> user_volume_vault_path(const std::filesystem::path &source,
                                                            std::string &detail) {
    const auto source_volume = volume_identity(source);
    std::wstring sid;
    if (!source_volume || !vove::fileops::detail::current_user_sid_text(sid, detail)) {
        if (detail.empty()) {
            detail = "trash volume identity is unavailable";
        }
        return std::nullopt;
    }
    return std::filesystem::path(source_volume->guid) /
           std::filesystem::path(std::wstring(core::kTrashVaultDirectoryNameWide) + L"-user-" +
                                 sid);
}

std::optional<VolumeIdentity> volume_identity(const std::filesystem::path &path) {
    std::array<wchar_t, 32'768> root{};
    if (GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size())) == FALSE) {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH + 1U> guid{};
    if (GetVolumeNameForVolumeMountPointW(root.data(), guid.data(),
                                          static_cast<DWORD>(guid.size())) == FALSE) {
        return std::nullopt;
    }
    DWORD serial{};
    if (GetVolumeInformationW(root.data(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) ==
        FALSE) {
        return std::nullopt;
    }
    return VolumeIdentity{.root = std::filesystem::path(root.data()),
                          .guid = std::wstring(guid.data()),
                          .serial = serial};
}

bool same_volume(const VolumeIdentity &left, const VolumeIdentity &right) noexcept {
    return left.serial == right.serial && _wcsicmp(left.guid.c_str(), right.guid.c_str()) == 0;
}

bool verify_free_space(const std::filesystem::path &path, TrashVaultSelection &selection) {
    std::error_code error;
    const auto available = std::filesystem::space(path, error);
    const auto state = detail::classify_trash_free_space(available.available, error);
    if (state == detail::TrashFreeSpaceState::unavailable) {
        selection.status = TrashRunStatus::storage_unavailable;
        selection.operation_status = OperationStatus::io_error;
        selection.detail = "trash free space could not be verified: " + error.message();
        return false;
    }
    if (state == detail::TrashFreeSpaceState::insufficient) {
        selection.status = TrashRunStatus::insufficient_space;
        selection.operation_status = OperationStatus::io_error;
        selection.detail = "VO-VE Trash is disabled because this volume has less than 64 MiB free";
        return false;
    }
    return true;
}

TrashVaultSelection select_trash_vault_root(const std::filesystem::path &source,
                                            const std::filesystem::path &manifest_directory) {
    TrashVaultSelection selection;
    const auto source_volume = volume_identity(source);
    const auto state_volume = volume_identity(manifest_directory);
    if (!source_volume || !state_volume) {
        selection.detail = "trash volume identity is unavailable";
        return selection;
    }
    const auto shares_state_volume = same_volume(*source_volume, *state_volume);
    std::string detail;
    auto vault = shares_state_volume
                     ? manifest_directory / "payload"
                     : user_volume_vault_path(source, detail).value_or(std::filesystem::path{});
    if (vault.empty()) {
        selection.detail = std::move(detail);
        return selection;
    }
    std::wstring sid;
    if (!vove::fileops::detail::current_user_sid_text(sid, detail)) {
        selection.detail = std::move(detail);
        return selection;
    }
    bool missing{};
    const auto pinned = vove::fileops::detail::pin_owned_trash_directory(vault, sid, detail, &missing);
    if (!pinned.valid() && (!missing ||
        !vove::fileops::detail::secure_owned_trash_vault(vault, sid, detail))) {
        selection.detail = std::move(detail);
        return selection;
    }
    std::error_code error;
    const auto attributes = GetFileAttributesW(vault.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        selection.detail = "trash vault is not a physical directory";
        return selection;
    }
    if (!shares_state_volume &&
        SetFileAttributesW(vault.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN |
                                              FILE_ATTRIBUTE_SYSTEM |
                                              FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) == FALSE) {
        selection.detail =
            "trash vault could not be hidden: Windows error " + std::to_string(GetLastError());
        return selection;
    }
    if (!shares_state_volume) {
        const auto user_attributes = GetFileAttributesW(vault.c_str());
        if (user_attributes == INVALID_FILE_ATTRIBUTES ||
            (user_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (user_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            SetFileAttributesW(vault.c_str(), user_attributes | FILE_ATTRIBUTE_HIDDEN |
                                                  FILE_ATTRIBUTE_SYSTEM |
                                                  FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) == FALSE) {
            selection.detail = "per-user trash vault is not a secured physical directory";
            return selection;
        }
    }
    if (!verify_free_space(manifest_directory, selection) ||
        (!shares_state_volume && !verify_free_space(vault, selection))) {
        return selection;
    }
    selection.status = TrashRunStatus::success;
    selection.operation_status = OperationStatus::success;
    selection.path = std::move(vault);
    selection.detail.clear();
    return selection;
}

bool preflight_trash_security(const TrashTransaction &transaction, const bool restoring,
                              OperationStatus &status, std::string &detail) {
    static_cast<void>(restoring);
    if (transaction.items.empty()) {
        status = OperationStatus::invalid_request;
        detail = "trash transaction has no items";
        return false;
    }
    std::wstring sid;
    if (!vove::fileops::detail::current_user_sid_text(sid, detail)) {
        status = OperationStatus::io_error;
        return false;
    }
    const auto vault = transaction.items.front().stored.parent_path().parent_path();
    if (!vove::fileops::detail::verify_owned_trash_vault(vault, sid, detail)) {
        status = OperationStatus::permission_denied;
        return false;
    }
    for (std::size_t index{}; index < transaction.items.size(); ++index) {
        const auto &item = transaction.items[index];
        const auto directory = item.kind == TrashItemKind::directory;
        if (item.stored.parent_path().parent_path() != vault) {
            status = OperationStatus::invalid_request;
            detail = "trash transaction crosses owned vaults";
            return false;
        }
        if (!vove::fileops::detail::validate_restorable_trash_dacl(
                item.original_security_descriptor_sddl_utf8, detail)) {
            status = OperationStatus::invalid_request;
            return false;
        }
        const auto active =
            transaction.active_step != TrashStep::none && transaction.active_index == index;
        if (active && transaction.active_step == TrashStep::store) {
            const auto source = vove::fileops::detail::verify_hardened_trash_payload(
                item.restore_path, item.current_snapshot, true, directory);
            const auto stored = vove::fileops::detail::verify_hardened_trash_payload(
                item.stored, item.current_snapshot, true, directory);
            if (source.ok() == stored.ok()) {
                status = OperationStatus::conflict;
                detail = "active trash store is not anchored to exactly one hardened payload";
                return false;
            }
            continue;
        }
        if (active && transaction.active_step == TrashStep::restore) {
            const auto stored = vove::fileops::detail::verify_hardened_trash_payload(
                item.stored, item.current_snapshot, true, directory);
            const auto restored = vove::fileops::detail::verify_restored_trash_payload(
                item.restore_path, item.current_snapshot,
                item.original_security_descriptor_sddl_utf8, true, directory);
            const auto restored_hardened = vove::fileops::detail::verify_hardened_trash_payload(
                item.restore_path, item.current_snapshot, true, directory);
            const auto restored_matches = restored.ok() || restored_hardened.ok();
            if (stored.ok() == restored_matches) {
                status = OperationStatus::conflict;
                detail = "active trash restore is not anchored to exactly one owned payload";
                return false;
            }
            continue;
        }
        if (active && transaction.active_step == TrashStep::purge) {
            const auto stored = vove::fileops::detail::verify_hardened_trash_payload(
                item.stored, item.current_snapshot, true, directory);
            if (!stored.ok()) {
                const auto attributes = GetFileAttributesW(item.stored.c_str());
                const auto error =
                    attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                if (attributes != INVALID_FILE_ATTRIBUTES ||
                    (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
                    status = stored.status;
                    detail = stored.detail_utf8;
                    return false;
                }
            }
            continue;
        }
        if (item.location == TrashItemLocation::source) {
            const auto verified =
                item.security_state == TrashSecurityState::hardened
                    ? vove::fileops::detail::verify_hardened_trash_payload(
                          item.restore_path, item.current_snapshot, false, directory)
                    : vove::fileops::detail::verify_restored_trash_payload(
                          item.restore_path, item.current_snapshot,
                          item.original_security_descriptor_sddl_utf8, false, directory);
            if (!verified.ok()) {
                status = verified.status;
                detail = verified.detail_utf8;
                return false;
            }
            continue;
        }
        if (item.location != TrashItemLocation::stored) {
            continue;
        }
        const auto verified = vove::fileops::detail::verify_hardened_trash_payload(
            item.stored, item.current_snapshot, item.directory_purge_cursor != 0, directory);
        if (!verified.ok()) {
            status = verified.status;
            detail = verified.detail_utf8;
            return false;
        }
    }
    status = OperationStatus::success;
    return true;
}

struct TrashNamespaceLease {
    vove::fileops::detail::TrashDirectoryLease vault;
    vove::fileops::detail::TrashDirectoryLease container;
    bool container_missing{};

    void release_container() noexcept {
        container.reset();
    }
};

std::optional<TrashNamespaceLease> pin_trash_namespace(const TrashTransaction &transaction,
                                                       std::string &detail,
                                                       const bool allow_missing_container = false) {
    if (transaction.items.empty()) {
        detail = "trash transaction has no namespace to pin";
        return std::nullopt;
    }
    std::wstring sid;
    if (!vove::fileops::detail::current_user_sid_text(sid, detail)) {
        return std::nullopt;
    }
    const auto container_path = transaction.items.front().stored.parent_path();
    const auto vault_path = container_path.parent_path();
    auto vault = vove::fileops::detail::pin_owned_trash_directory(vault_path, sid, detail);
    if (!vault.valid()) {
        return std::nullopt;
    }
    bool container_missing{};
    auto container = vove::fileops::detail::pin_owned_trash_directory(container_path, sid, detail,
                                                                      &container_missing);
    if (!container.valid() && (!allow_missing_container || !container_missing)) {
        return std::nullopt;
    }
    if (container_missing) {
        detail.clear();
    }
    return TrashNamespaceLease{.vault = std::move(vault),
                               .container = std::move(container),
                               .container_missing = container_missing};
}

#endif

#ifndef _WIN32
struct TrashNamespaceLease {
    bool container_missing{};
    void release_container() noexcept {}
};
#endif

} // namespace

std::filesystem::path
trash_recovery_root_for_source(const std::filesystem::path &source,
                               const std::filesystem::path &manifest_directory,
                               std::string &detail_utf8) {
#ifdef _WIN32
    const auto source_volume = volume_identity(source);
    const auto state_volume = volume_identity(manifest_directory);
    if (!source_volume || !state_volume) {
        detail_utf8 = "trash volume identity is unavailable";
        return {};
    }
    if (same_volume(*source_volume, *state_volume)) {
        return manifest_directory / "payload";
    }
    const auto vault = user_volume_vault_path(source, detail_utf8);
    return vault.value_or(std::filesystem::path{});
#else
    static_cast<void>(source);
    static_cast<void>(manifest_directory);
    detail_utf8 = "VO-VE Trash recovery roots are not enabled on this platform";
    return {};
#endif
}

TrashRecoveryRootDiscovery
trash_known_recovery_roots(const std::filesystem::path &manifest_directory) {
    TrashRecoveryRootDiscovery discovery{.roots = {}, .discovery_failures = 0};
#ifdef _WIN32
    discovery.roots.push_back(manifest_directory / "payload");
    std::wstring sid;
    std::string detail;
    if (!vove::fileops::detail::current_user_sid_text(sid, detail)) {
        ++discovery.discovery_failures;
        return discovery;
    }
    std::array<wchar_t, MAX_PATH + 1U> volume{};
    const auto search = FindFirstVolumeW(volume.data(), static_cast<DWORD>(volume.size()));
    if (search == INVALID_HANDLE_VALUE) {
        ++discovery.discovery_failures;
        return discovery;
    }
    DWORD final_error = ERROR_SUCCESS;
    for (;;) {
        const auto drive_type = GetDriveTypeW(volume.data());
        if (drive_type == DRIVE_FIXED) {
            auto root = std::filesystem::path(volume.data()) /
                        std::filesystem::path(std::wstring(core::kTrashVaultDirectoryNameWide) +
                                              L"-user-" + sid);
            if (std::ranges::find(discovery.roots, root) == discovery.roots.end()) {
                discovery.roots.push_back(std::move(root));
            }
        } else if (drive_type == DRIVE_UNKNOWN) {
            ++discovery.discovery_failures;
        }
        if (FindNextVolumeW(search, volume.data(), static_cast<DWORD>(volume.size())) != FALSE) {
            continue;
        }
        final_error = GetLastError();
        break;
    }
    if (FindVolumeClose(search) == FALSE) {
        ++discovery.discovery_failures;
    }
    if (final_error != ERROR_NO_MORE_FILES) {
        ++discovery.discovery_failures;
    }
#else
    static_cast<void>(manifest_directory);
#endif
    return discovery;
}

namespace {

bool is_inside_trash_namespace(const std::filesystem::path &path) {
    return std::ranges::any_of(
        path, [](const auto &component) { return core::is_trash_filename(component.u8string()); });
}

bool validate_restore_destination(const TrashTransaction &transaction,
                                  const std::filesystem::path &destination_directory,
                                  std::string &detail) {
    if (transaction.items.empty() || destination_directory.empty() ||
        !destination_directory.is_absolute()) {
        detail = "trash restore destination is invalid";
        return false;
    }
    if (is_inside_trash_namespace(destination_directory)) {
        detail = "trash restore destination is inside VO-VE Trash storage";
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(destination_directory, error) || error) {
        detail = "trash restore destination is not an available directory";
        return false;
    }
    const auto container = transaction.items.front().stored.parent_path();
#ifdef _WIN32
    const auto stored_volume = volume_identity(container);
    const auto destination_volume = volume_identity(destination_directory);
    if (!stored_volume || !destination_volume ||
        !same_volume(*stored_volume, *destination_volume)) {
        detail = "trash restore destination must be on the same volume";
        return false;
    }
#else
    struct stat stored_status{};
    struct stat destination_status{};
    if (::stat(container.c_str(), &stored_status) != 0 ||
        ::stat(destination_directory.c_str(), &destination_status) != 0 ||
        !S_ISDIR(destination_status.st_mode) || stored_status.st_dev != destination_status.st_dev) {
        detail = "trash restore destination must be on the same filesystem";
        return false;
    }
#endif
    return true;
}

bool rebase_restore_paths(TrashTransaction &transaction,
                          const std::filesystem::path &destination_directory, std::string &detail) {
    if (!validate_restore_destination(transaction, destination_directory, detail)) {
        return false;
    }

    for (auto &item : transaction.items) {
        auto destination = destination_directory / item.original.filename();
#ifdef _WIN32
        auto canonical = canonical_volume_path(destination, detail);
        if (!canonical) {
            return false;
        }
        destination = std::move(*canonical);
#else
        destination = destination.lexically_normal();
#endif
        item.restore_path = std::move(destination);
    }
    return valid_trash_transaction(transaction, detail);
}

} // namespace

struct TrashCoordinator::State : std::enable_shared_from_this<State> {
    explicit State(TrashCoordinatorOptions value)
        : options(std::move(value)), journal(options.journal_path),
          file_operations(options.file_operations), maximum_bytes(options.maximum_bytes) {
        file_operations.retain_accepted_completions_during_stop();
        std::error_code error;
        if (!options.journal_path.parent_path().empty()) {
            std::filesystem::create_directories(options.journal_path.parent_path(), error);
        }
        if (!error && !options.manifest_directory.empty()) {
            std::filesystem::create_directories(options.manifest_directory, error);
        }
        if (error) {
            initialization_error = "trash state directory could not be created: " + error.message();
        }
        static_cast<void>(refresh_recovery_pending());
    }

    TrashCoordinatorOptions options;
    CurrentOperationJournalStore journal;
    FileOperationService file_operations;
    std::string initialization_error;
    std::atomic_bool running{false};
    std::atomic_bool stopped{false};
    std::atomic_bool recovery_pending{false};
    std::atomic<std::uintmax_t> maximum_bytes;
    std::mutex worker_mutex;
    std::condition_variable worker_completed;
    std::stop_source worker_stop;
    std::thread::id worker_id;

    [[nodiscard]] bool refresh_recovery_pending() noexcept {
        const auto pending = journal.read().status != DurableJournalStatus::not_found;
        recovery_pending.store(pending, std::memory_order_release);
        return pending;
    }

    [[nodiscard]] std::optional<TrashResult> verify_quota(const std::vector<TrashSource> &sources,
                                                          const std::stop_token &stop) const {
        const auto limit = maximum_bytes.load(std::memory_order_acquire);
        if (limit == 0) {
            return std::nullopt;
        }
        const auto storage_root = trash_storage_root_for(sources.front().path).lexically_normal();
        std::uintmax_t incoming{};
        for (const auto &source : sources) {
            if (trash_storage_root_for(source.path).lexically_normal() != storage_root) {
                return TrashResult{.status = TrashRunStatus::invalid_request,
                                   .operation_status = OperationStatus::invalid_request,
                                   .completed = 0,
                                   .total = sources.size(),
                                   .manifest_path = {},
                                   .stored_paths = {},
                                   .detail_utf8 =
                                       "one trash operation must use one storage device"};
            }
            const auto source_bytes = source.kind == TrashItemKind::directory
                                          ? source.payload_bytes
                                          : source.snapshot.size_bytes;
            if (source_bytes > std::numeric_limits<std::uintmax_t>::max() - incoming) {
                return TrashResult{.status = TrashRunStatus::insufficient_space,
                                   .operation_status = OperationStatus::io_error,
                                   .completed = 0,
                                   .total = sources.size(),
                                   .manifest_path = {},
                                   .stored_paths = {},
                                   .detail_utf8 = "trash request size overflow"};
            }
            incoming += source_bytes;
        }
        // Published operations are accounted from their central manifests. On Windows, add only
        // this source volume's redundant vault so rescue-only metadata is counted without probing
        // every local volume during each delete preflight.
        std::vector<std::filesystem::path> quota_recovery_roots;
#ifdef _WIN32
        std::string recovery_detail;
        auto recovery_root = trash_recovery_root_for_source(
            sources.front().path, options.manifest_directory, recovery_detail);
        if (recovery_root.empty()) {
            return TrashResult{.status = TrashRunStatus::storage_unavailable,
                               .operation_status = OperationStatus::io_error,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 = std::move(recovery_detail)};
        }
        std::error_code recovery_error;
        const auto recovery_exists = std::filesystem::exists(recovery_root, recovery_error);
        if (recovery_error) {
            return TrashResult{.status = TrashRunStatus::storage_unavailable,
                               .operation_status = OperationStatus::io_error,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 = "trash recovery storage could not be verified: " +
                                              recovery_error.message()};
        }
        if (recovery_exists) {
            quota_recovery_roots.push_back(std::move(recovery_root));
        }
#endif
        auto catalog = read_trash_catalog(
            options.manifest_directory, quota_recovery_roots, stop,
            TrashStorageFilter{.root = storage_root,
                               .storage_identity_utf8 = sources.front().storage_identity_utf8,
                               .source_revision_utf8 =
                                   sources.front().snapshot.source_revision_utf8});
        if (!catalog.ok() || catalog.corrupt_manifests != 0 ||
            catalog.unreadable_recovery_roots != 0 || catalog.totals_saturated) {
            return TrashResult{.status = TrashRunStatus::storage_unavailable,
                               .operation_status = OperationStatus::io_error,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 = "trash storage usage could not be verified: catalog=" +
                                   std::to_string(static_cast<int>(catalog.status)) +
                                   ", error=" + std::to_string(catalog.error.value()) +
                                   ", corrupt=" + std::to_string(catalog.corrupt_manifests) +
                                   ", unreadable=" + std::to_string(catalog.unreadable_recovery_roots)};
        }
        auto usage = std::ranges::find_if(catalog.storage_usage, [&](const auto &candidate) {
            return candidate.root.lexically_normal() == storage_root;
        });
        TrashStorageUsage empty{.root = storage_root};
        if (usage == catalog.storage_usage.end()) {
            std::error_code error;
            const auto space = std::filesystem::space(storage_root, error);
            if (!error) {
                empty.capacity_bytes = space.capacity;
                empty.available_bytes = space.available;
                empty.space_known = true;
            }
        }
        const auto &checked = usage == catalog.storage_usage.end() ? empty : *usage;
        const auto state = classify_trash_quota(checked, limit, incoming);
        if (state == TrashQuotaState::unknown) {
            return TrashResult{.status = TrashRunStatus::storage_unavailable,
                               .operation_status = OperationStatus::io_error,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 = "trash quota could not be verified"};
        }
        if (state == TrashQuotaState::exceeded) {
            return TrashResult{.status = TrashRunStatus::insufficient_space,
                               .operation_status = OperationStatus::io_error,
                               .completed = 0,
                               .total = sources.size(),
                               .manifest_path = {},
                               .stored_paths = {},
                               .detail_utf8 =
                                   "trash quota or free-space reserve would be exceeded"};
        }
        return std::nullopt;
    }

#ifdef _WIN32
    [[nodiscard]] std::optional<TrashResult> prepare_vault_owner(
        const std::vector<TrashSource> &sources, const std::stop_token &stop) const {
        std::string reason;
        const auto vault = trash_recovery_root_for_source(
            sources.front().path, options.manifest_directory, reason);
        std::wstring sid;
        if (!vault.empty() && detail::current_user_sid_text(sid, reason)) {
            bool missing{};
            const auto pinned = detail::pin_owned_trash_directory(vault, sid, reason, &missing);
            if (pinned.valid() || missing) return std::nullopt;
            std::string repair_reason;
            const auto canonical = canonical_volume_path(vault, repair_reason);
            if (canonical && detail::repair_empty_legacy_trash_vault(
                                 vault, *canonical, sid, stop, repair_reason)) {
                return std::nullopt;
            }
            reason += "; legacy owner check: " + repair_reason;
        }
        return TrashResult{.status = TrashRunStatus::storage_unavailable,
                           .operation_status = OperationStatus::io_error,
                           .completed = 0,
                           .total = sources.size(),
                           .manifest_path = {},
                           .stored_paths = {},
                           .detail_utf8 = std::move(reason)};
    }
#endif

    [[nodiscard]] bool owns_recovery() noexcept {
        const auto loaded = journal.read();
        const auto owns = loaded.ok() &&
                          loaded.encoding == CurrentOperationJournalEncoding::typed &&
                          (loaded.kind == CurrentOperationKind::trash_move ||
                           loaded.kind == CurrentOperationKind::trash_restore ||
                           loaded.kind == CurrentOperationKind::trash_purge);
        recovery_pending.store(loaded.status != DurableJournalStatus::not_found,
                               std::memory_order_release);
        return owns;
    }

    [[nodiscard]] bool persist(const TrashTransaction &transaction, std::string &detail) {
        try {
            const auto payload = encode_trash_transaction(transaction);
            const auto stored = journal.write(operation_kind(transaction), payload);
            if (!stored.ok()) {
                detail = journal_error_detail(stored, "trash journal");
                recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                                       std::memory_order_release);
                return false;
            }
            recovery_pending.store(true, std::memory_order_release);
            return true;
        } catch (const std::exception &error) {
            detail = error.what();
            recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                                   std::memory_order_release);
            return false;
        }
    }

    [[nodiscard]] bool remove_journal(std::string &detail) {
        const auto removed = journal.remove();
        if (!removed.ok()) {
            detail = journal_error_detail(removed, "trash journal");
            recovery_pending.store(true, std::memory_order_release);
            return false;
        }
        recovery_pending.store(false, std::memory_order_release);
        return true;
    }

    [[nodiscard]] OperationResult wait_for_rename(const RenameRequest &request,
                                                  const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
            }
            pending->completed.notify_all();
        };
        const auto accepted = reconcile ? file_operations.submit_reconciliation(request, completion)
                                        : file_operations.submit_rename(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected a trash rename step"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->result.has_value(); });
        if (!pending->result) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "trash rename completed without a result"};
        }
        return std::move(*pending->result);
    }

    [[nodiscard]] OperationResult wait_for_delete(const DeleteRequest &request,
                                                  const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
            }
            pending->completed.notify_all();
        };
        const auto accepted =
            reconcile ? file_operations.submit_delete_reconciliation(request, completion)
                      : file_operations.submit_delete(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected a trash purge step"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->result.has_value(); });
        if (!pending->result) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "trash purge completed without a result"};
        }
        return std::move(*pending->result);
    }

    [[nodiscard]] OperationResult wait_for_create_directory(const CreateDirectoryRequest &request,
                                                            const bool reconcile) {
        auto pending = std::make_shared<PendingOperation>();
        const auto completion = [pending](OperationResult result) {
            {
                std::scoped_lock lock(pending->mutex);
                pending->result = std::move(result);
            }
            pending->completed.notify_all();
        };
        const auto accepted =
            reconcile ? file_operations.submit_create_directory_reconciliation(request, completion)
                      : file_operations.submit_create_directory(request, completion);
        if (!accepted) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "file operation service rejected a trash container step"};
        }
        std::unique_lock lock(pending->mutex);
        pending->completed.wait(lock, [&pending] { return pending->result.has_value(); });
        if (!pending->result) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::io_error,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "trash container step completed without a result"};
        }
        return std::move(*pending->result);
    }

    [[nodiscard]] OperationResult ensure_container(const TrashTransaction &transaction) {
#ifdef _WIN32
        std::string detail;
        const auto ready = ensure_trash_container(transaction, detail);
        return {.operation_id = transaction.operation_id,
                .status = ready ? OperationStatus::success : OperationStatus::io_error,
                .evidence = ready ? OperationEvidence::committed : OperationEvidence::none,
                .confirmed_snapshot = {},
                .detail_utf8 = std::move(detail)};
#else
        const auto container = transaction.items.front().stored.parent_path();
        const CreateDirectoryRequest request{
            .operation_id = transaction.operation_id,
            .action = CreateDirectoryAction::execute,
            .mode = CreateDirectoryMode::trash_internal,
            .destination = container,
            .destination_parent_revision_utf8 = {},
        };
        auto result = wait_for_create_directory(request, false);
        if (result.status == OperationStatus::conflict) {
            result = wait_for_create_directory(request, true);
        } else if (result.status == OperationStatus::unknown_outcome ||
                   result.status == OperationStatus::timed_out ||
                   result.status == OperationStatus::disconnected ||
                   result.status == OperationStatus::io_error) {
            result = vove::fileops::detail::merge_reconciliation(
                std::move(result), wait_for_create_directory(request, true));
        }
        if (!result.ok() || result.evidence != OperationEvidence::committed) {
            if (result.detail_utf8.empty()) {
                result.detail_utf8 = "trash container could not be created safely";
            }
        }
        return result;
#endif
    }

#ifndef _WIN32
    [[nodiscard]] bool abort_untouched_preparation(const TrashTransaction &transaction,
                                                  const std::stop_token &stop,
                                                  std::string &detail) {
        // Prepared is persisted before any store intent. Still require current no-move
        // evidence: neither stale metadata nor an unavailable mount proves absence.
        if (transaction.phase != TrashPhase::prepared ||
            transaction.active_step != TrashStep::none ||
            transaction.active_evidence != OperationEvidence::none ||
            !std::ranges::all_of(transaction.items, [](const auto &item) {
                return item.location == TrashItemLocation::source &&
                       item.current == item.restore_path &&
                       item.security_state == TrashSecurityState::original;
            })) {
            detail = "trash preparation has possible move evidence";
            return false;
        }
        const DurableJournalStore durable(options.journal_path);
        const auto saved = durable.read_primary_generation();
        const auto typed = journal.read();
        const auto confirmed = durable.read_primary_generation();
        if (!saved.ok() || !confirmed.ok() || saved.payload != confirmed.payload ||
            !typed.ok() || typed.source != DurableJournalReadSource::primary ||
            typed.encoding != CurrentOperationJournalEncoding::typed ||
            typed.kind != CurrentOperationKind::trash_move ||
            typed.payload != encode_trash_transaction(transaction)) {
            detail = "trash preparation journal changed before verification";
            return false;
        }
        const auto previous = durable.read_previous_generation();
        if (previous.status != DurableJournalStatus::not_found &&
            (!previous.ok() || previous.payload != saved.payload)) {
            detail = "trash preparation has a different previous journal generation";
            return false;
        }
        const auto absent = [](const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            return status.type() == std::filesystem::file_type::not_found &&
                   (!error || error == std::errc::no_such_file_or_directory);
        };
        const DurableJournalStore manifest(
            manifest_path(options.manifest_directory, transaction.operation_id));
        const auto no_removal_metadata = [&absent](const DurableJournalStore &store) {
            return absent(std::filesystem::path(store.primary_path()).concat(".vove-removing")) &&
                   absent(std::filesystem::path(store.previous_path()).concat(".vove-removing"));
        };
        const auto metadata_absent = [&] {
            // The durable reader also recognizes .vove-removing/candidate. An empty
            // removal directory is still an unfinished cleanup, not proof of absence.
            return absent(durable.temporary_path()) && no_removal_metadata(durable) &&
                   no_removal_metadata(manifest) && absent(manifest.temporary_path()) &&
                   manifest.read_primary_generation().status == DurableJournalStatus::not_found &&
                   manifest.read_previous_generation().status == DurableJournalStatus::not_found &&
                   absent(manifest.primary_path()) && absent(manifest.previous_path());
        };
        if (!metadata_absent()) {
            detail = "trash preparation metadata is present or cannot be verified absent";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (std::size_t index{}; index < transaction.items.size(); ++index) {
            if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                detail = "trash preparation verification stopped before completion";
                return false;
            }
            const auto observed = wait_for_rename(
                rename_request(transaction, index, TrashStep::store), true);
            if (observed.evidence != OperationEvidence::no_commit ||
                !observed.source_present || !observed.source_matches_expected ||
                observed.destination_present || observed.destination_matches_source) {
                detail = "trash preparation source or destination could not be verified: " +
                         observed.detail_utf8;
                return false;
            }
            const auto &item = transaction.items[index];
            if (item.kind != TrashItemKind::directory) {
                continue;
            }
            const auto exact_snapshot = [&](const std::filesystem::path &path,
                                            const SourceSnapshot &expected,
                                            const OperationObjectKind kind,
                                            const bool modified_time_recorded = true) {
                if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                // Reconciliation in the reverse direction returns the observed
                // destination snapshot, including directory ctime/mtime. This is
                // read-only: no restore intent or execute request is issued.
                auto probe = rename_request(transaction, index, TrashStep::restore);
                probe.action = RenameAction::reconcile_only;
                probe.source = item.stored;
                probe.destination = path;
                probe.expected_source = expected;
                probe.object_kind = kind;
                const auto snapshot = wait_for_rename(probe, true);
                return snapshot.ok() && snapshot.evidence == OperationEvidence::committed &&
                       !snapshot.source_present && snapshot.destination_present &&
                       snapshot.destination_matches_source &&
                       snapshot.confirmed_snapshot.size_bytes == expected.size_bytes &&
                       (!modified_time_recorded ||
                        snapshot.confirmed_snapshot.modified_unix_ns == expected.modified_unix_ns) &&
                       same_source_revision(snapshot.confirmed_snapshot.source_revision_utf8,
                                            expected.source_revision_utf8);
            };
            if (!exact_snapshot(item.current, item.current_snapshot, OperationObjectKind::directory)) {
                detail = "trash preparation directory snapshot changed or is unavailable";
                return false;
            }
            for (const auto &entry : item.directory_entries) {
                const auto directory = directory_entry_is_directory(entry);
                // Directory capture stores the full revision (including ctime),
                // but does not populate descendant-directory mtime. Zero here is
                // not a recorded timestamp; root and file mtime remain mandatory.
                if (!exact_snapshot(item.current / entry.relative_path, directory_entry_snapshot(entry),
                                    directory ? OperationObjectKind::directory : OperationObjectKind::regular_file,
                                    !directory || entry.modified_unix_ns != 0)) {
                    detail = "trash preparation directory descendant changed or is unavailable";
                    return false;
                }
            }
            if (!exact_snapshot(item.current, item.current_snapshot, OperationObjectKind::directory)) {
                detail = "trash preparation directory changed during verification";
                return false;
            }
        }
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            detail = "trash preparation verification stopped before completion";
            return false;
        }
        const auto container = wait_for_create_directory(
            {.operation_id = transaction.operation_id,
             .action = CreateDirectoryAction::reconcile_only,
             .mode = CreateDirectoryMode::trash_internal_remove_empty,
             .destination = transaction.items.front().stored.parent_path(),
             .destination_parent_revision_utf8 = {}},
            true);
        if (!container.ok() || container.evidence != OperationEvidence::committed ||
            container.destination_present) {
            detail = "trash preparation container is not confirmed absent: " +
                     container.detail_utf8;
            return false;
        }
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline ||
            !metadata_absent()) {
            detail = "trash preparation verification expired or metadata appeared";
            return false;
        }
        const auto removed = durable.remove_if_payload_matches(saved.payload);
        recovery_pending.store(journal.read().status != DurableJournalStatus::not_found,
                               std::memory_order_release);
        if (!removed.ok() || recovery_pending.load(std::memory_order_acquire)) {
            detail = journal_error_detail(removed, "trash preparation journal");
            return false;
        }
        return true;
    }
#endif

    [[nodiscard]] bool retire_container(const TrashTransaction &transaction, std::string &detail,
                                        const bool tolerate_foreign_entries = false) {
#ifdef _WIN32
        return remove_empty_trash_container(transaction, detail, tolerate_foreign_entries);
#else
        const auto container = transaction.items.front().stored.parent_path();
        const CreateDirectoryRequest request{
            .operation_id = transaction.operation_id,
            .action = CreateDirectoryAction::execute,
            .mode = CreateDirectoryMode::trash_internal_remove_empty,
            .destination = container,
            .destination_parent_revision_utf8 = {},
        };
        auto result = wait_for_create_directory(request, false);
        if (result.status == OperationStatus::not_found) {
            result = wait_for_create_directory(request, true);
        } else if (result.status == OperationStatus::unknown_outcome ||
                   result.status == OperationStatus::timed_out ||
                   result.status == OperationStatus::disconnected ||
                   result.status == OperationStatus::io_error) {
            result = vove::fileops::detail::merge_reconciliation(
                std::move(result), wait_for_create_directory(request, true));
        }
        if (result.ok() && result.evidence == OperationEvidence::committed) {
            return true;
        }
        if (tolerate_foreign_entries && result.status == OperationStatus::conflict &&
            result.evidence == OperationEvidence::no_commit) {
            return true;
        }
        detail = result.detail_utf8.empty() ? "trash container could not be retired safely"
                                            : std::move(result.detail_utf8);
        return false;
#endif
    }

    [[nodiscard]] RenameRequest rename_request(const TrashTransaction &transaction,
                                               const std::size_t index,
                                               const TrashStep step) const {
        const auto &item = transaction.items[index];
        return {.operation_id = step_operation_id(transaction, step, index),
                .action = RenameAction::execute,
                .mode = step == TrashStep::store ? RenameMode::trash_internal
                                                 : RenameMode::trash_restore,
                .object_kind = item.kind == TrashItemKind::directory
                                   ? OperationObjectKind::directory
                                   : OperationObjectKind::regular_file,
                .source = item.current,
                .destination = step == TrashStep::store ? item.stored : item.restore_path,
                .expected_source = item.current_snapshot,
                .source_parent_identity_utf8 = item.storage_identity_utf8,
                .destination_parent_identity_utf8 = {},
                .destination_anchor_path = {},
                .destination_anchor_identity_utf8 = {}};
    }

    [[nodiscard]] DeleteRequest purge_request(const TrashTransaction &transaction,
                                              const std::size_t index) const {
        const auto &item = transaction.items[index];
        auto source = item.current;
        auto expected = item.current_snapshot;
        auto object_kind = item.kind == TrashItemKind::directory
                               ? OperationObjectKind::directory
                               : OperationObjectKind::regular_file;
        if (item.kind == TrashItemKind::directory &&
            item.directory_purge_cursor < item.directory_entries.size()) {
            const auto &entry = item.directory_entries[item.directory_purge_cursor];
            source /= entry.relative_path;
            expected = directory_entry_snapshot(entry);
            object_kind = directory_entry_is_directory(entry) ? OperationObjectKind::directory
                                                              : OperationObjectKind::regular_file;
        }
        return {.operation_id = step_operation_id(transaction, TrashStep::purge, index),
                .action = DeleteAction::execute,
                .mode = DeleteMode::trash_purge,
                .object_kind = object_kind,
                .source = std::move(source),
                .expected_source = std::move(expected),
                .source_parent_identity_utf8 = item.storage_identity_utf8,
                .guard_path = {},
                .expected_guard = {},
                .guard_parent_identity_utf8 = {}};
    }

    [[nodiscard]] OperationResult submit_step(const TrashTransaction &transaction,
                                              const std::size_t index, const TrashStep step,
                                              const bool reconcile) {
        if (step == TrashStep::purge) {
            return wait_for_delete(purge_request(transaction, index), reconcile);
        }
        return wait_for_rename(rename_request(transaction, index, step), reconcile);
    }

    [[nodiscard]] bool apply_success(TrashTransaction &transaction, const std::size_t index,
                                     const TrashStep step, const OperationResult &result,
                                     std::string &detail) {
        auto &item = transaction.items[index];
        if (step == TrashStep::purge) {
            if (!result.ok() || result.evidence != OperationEvidence::committed) {
                detail = "trash purge succeeded without committed evidence";
                return false;
            }
            if (item.kind == TrashItemKind::directory &&
                item.directory_purge_cursor < item.directory_entries.size()) {
                ++item.directory_purge_cursor;
            } else {
                item.current.clear();
                item.location = TrashItemLocation::deleted;
                item.directory_purge_cursor = 0;
            }
            return true;
        }
        const auto expected = stable_object_identity(item.current_snapshot.source_revision_utf8);
        const auto confirmed =
            stable_object_identity(result.confirmed_snapshot.source_revision_utf8);
        auto trustworthy_identity = !expected.empty() && expected == confirmed;
#ifndef _WIN32
        trustworthy_identity =
            trustworthy_identity || same_private_trash_payload_after_remount(
                                        item.current_snapshot, result.confirmed_snapshot,
                                        item.storage_identity_utf8, item.storage_identity_utf8);
#endif
        if (!result.ok() || !trustworthy_identity) {
            detail = "trash rename succeeded without a trustworthy physical identity";
            return false;
        }
        item.current = step == TrashStep::store ? item.stored : item.restore_path;
        item.current_snapshot = result.confirmed_snapshot;
        item.location =
            step == TrashStep::store ? TrashItemLocation::stored : TrashItemLocation::source;
#ifdef _WIN32
        item.security_state =
            step == TrashStep::store ? TrashSecurityState::hardened : TrashSecurityState::original;
#else
        item.security_state = TrashSecurityState::original;
#endif
        return true;
    }

#ifdef _WIN32
    [[nodiscard]] static std::optional<detail::TrashSecurityResult>
    harden_directory_tree(TrashItem &item, const std::filesystem::path &root) {
        if (item.directory_security_descriptors_sddl_utf8.size() != item.directory_entries.size()) {
            return detail::TrashSecurityResult{.status = OperationStatus::invalid_request,
                                               .snapshot = {},
                                               .original_sddl_utf8 = {},
                                               .detail_utf8 =
                                                   "trash directory ACL manifest is incomplete"};
        }
        for (std::size_t index{}; index < item.directory_entries.size(); ++index) {
            auto &entry = item.directory_entries[index];
            const auto path = root / entry.relative_path;
            const auto directory = directory_entry_is_directory(entry);
            auto hardened = detail::verify_hardened_trash_payload(
                path, directory_entry_snapshot(entry), true, directory);
            if (hardened.ok()) {
                update_directory_entry_snapshot(entry, hardened.snapshot);
                continue;
            }
            auto original = detail::verify_restored_trash_payload(
                path, directory_entry_snapshot(entry),
                item.directory_security_descriptors_sddl_utf8[index], true, directory);
            if (!original.ok()) {
                original.detail_utf8 += ": " + diagnostic_path_utf8(path);
                return original;
            }
            auto security = detail::harden_trash_payload(path, original.snapshot, directory, true);
            if (!security.ok()) {
                security.detail_utf8 += ": " + diagnostic_path_utf8(path);
                return security;
            }
            update_directory_entry_snapshot(entry, security.snapshot);
        }
        auto hardened_root =
            detail::verify_hardened_trash_payload(root, item.current_snapshot, true, true);
        if (hardened_root.ok()) {
            item.current_snapshot = std::move(hardened_root.snapshot);
            return std::nullopt;
        }
        auto original_root = detail::verify_restored_trash_payload(
            root, item.current_snapshot, item.original_security_descriptor_sddl_utf8, true, true);
        if (!original_root.ok()) {
            original_root.detail_utf8 += ": " + diagnostic_path_utf8(root);
            return original_root;
        }
        auto root_security = detail::harden_trash_payload(root, original_root.snapshot, true, true);
        if (!root_security.ok()) {
            root_security.detail_utf8 += ": " + diagnostic_path_utf8(root);
            return root_security;
        }
        item.current_snapshot = std::move(root_security.snapshot);
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<detail::TrashSecurityResult>
    restore_directory_tree(TrashItem &item, const std::filesystem::path &root) {
        if (item.directory_security_descriptors_sddl_utf8.size() != item.directory_entries.size()) {
            return detail::TrashSecurityResult{.status = OperationStatus::invalid_request,
                                               .snapshot = {},
                                               .original_sddl_utf8 = {},
                                               .detail_utf8 =
                                                   "trash directory ACL manifest is incomplete"};
        }
        auto restored_root = detail::verify_restored_trash_payload(
            root, item.current_snapshot, item.original_security_descriptor_sddl_utf8, true, true);
        if (restored_root.ok()) {
            item.current_snapshot = std::move(restored_root.snapshot);
        } else {
            auto hardened_root =
                detail::verify_hardened_trash_payload(root, item.current_snapshot, true, true);
            if (!hardened_root.ok()) {
                return hardened_root;
            }
            auto root_security = detail::restore_trash_security(
                root, hardened_root.snapshot, item.original_security_descriptor_sddl_utf8, true,
                true);
            if (!root_security.ok()) {
                return root_security;
            }
            item.current_snapshot = std::move(root_security.snapshot);
        }
        for (std::size_t reverse = item.directory_entries.size(); reverse > 0; --reverse) {
            const auto index = reverse - 1U;
            auto &entry = item.directory_entries[index];
            const auto path = root / entry.relative_path;
            const auto directory = directory_entry_is_directory(entry);
            auto restored = detail::verify_restored_trash_payload(
                path, directory_entry_snapshot(entry),
                item.directory_security_descriptors_sddl_utf8[index], true, directory);
            if (restored.ok()) {
                update_directory_entry_snapshot(entry, restored.snapshot);
                continue;
            }
            auto hardened = detail::verify_hardened_trash_payload(
                path, directory_entry_snapshot(entry), true, directory);
            if (!hardened.ok()) {
                return hardened;
            }
            auto security = detail::restore_trash_security(
                path, hardened.snapshot, item.directory_security_descriptors_sddl_utf8[index],
                directory, true);
            if (!security.ok()) {
                return security;
            }
            update_directory_entry_snapshot(entry, security.snapshot);
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<detail::TrashSecurityResult>
    verify_hardened_directory_tree(const TrashItem &item, const std::filesystem::path &root,
                                   const bool allow_revision_advance) {
        for (std::size_t index = item.directory_purge_cursor; index < item.directory_entries.size();
             ++index) {
            const auto &entry = item.directory_entries[index];
            auto security = detail::verify_hardened_trash_payload(
                root / entry.relative_path, directory_entry_snapshot(entry),
                allow_revision_advance && directory_entry_is_directory(entry),
                directory_entry_is_directory(entry));
            if (!security.ok()) {
                return security;
            }
        }
        auto root_security = detail::verify_hardened_trash_payload(root, item.current_snapshot,
                                                                   allow_revision_advance, true);
        if (!root_security.ok()) {
            return root_security;
        }
        return std::nullopt;
    }
#endif

    [[nodiscard]] bool apply_security_after_rename(TrashTransaction &transaction,
                                                   const std::size_t index, const TrashStep step,
                                                   OperationResult &result, std::string &detail) {
#ifndef _WIN32
        static_cast<void>(transaction);
        static_cast<void>(index);
        static_cast<void>(step);
        static_cast<void>(result);
        static_cast<void>(detail);
        return true;
#else
        if (step == TrashStep::purge) {
            return true;
        }
        auto &item = transaction.items[index];
        const auto destination = step == TrashStep::store ? item.stored : item.restore_path;
        if (item.kind == TrashItemKind::directory) {
            item.current_snapshot = result.confirmed_snapshot;
            auto security = step == TrashStep::store
                                ? verify_hardened_directory_tree(item, destination, false)
                                : restore_directory_tree(item, destination);
            if (security) {
                detail = std::move(security->detail_utf8);
                return false;
            }
            result.confirmed_snapshot = item.current_snapshot;
        } else {
            auto security = step == TrashStep::store
                                ? vove::fileops::detail::verify_hardened_trash_payload(
                                      destination, result.confirmed_snapshot)
                                : vove::fileops::detail::restore_trash_security(
                                      destination, result.confirmed_snapshot,
                                      item.original_security_descriptor_sddl_utf8);
            if (!security.ok()) {
                detail = std::move(security.detail_utf8);
                return false;
            }
            result.confirmed_snapshot = std::move(security.snapshot);
        }
        return true;
#endif
    }

    static void clear_active(TrashTransaction &transaction, const bool clear_failure = true) {
        transaction.active_step = TrashStep::none;
        transaction.active_index = 0;
        transaction.active_evidence = OperationEvidence::none;
        if (clear_failure) {
            transaction.failure_status = OperationStatus::success;
            transaction.failure_detail_utf8.clear();
        }
    }

    [[nodiscard]] StepResult prepare_store_security(TrashTransaction &transaction,
                                                    const std::size_t index) {
#ifndef _WIN32
        static_cast<void>(transaction);
        static_cast<void>(index);
        return {StepOutcome::committed, OperationStatus::success, {}};
#else
        auto &item = transaction.items[index];
        if (item.kind == TrashItemKind::directory) {
            auto security = harden_directory_tree(item, item.restore_path);
            if (security) {
                return {StepOutcome::recovery_required, security->status,
                        std::move(security->detail_utf8)};
            }
        } else {
            auto security = vove::fileops::detail::harden_trash_payload(item.restore_path,
                                                                        item.current_snapshot);
            if (!security.ok()) {
                return {StepOutcome::recovery_required, security.status,
                        std::move(security.detail_utf8)};
            }
            item.current_snapshot = std::move(security.snapshot);
        }
        item.security_state = TrashSecurityState::hardened;
        std::string detail;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        return {StepOutcome::committed, OperationStatus::success, {}};
#endif
    }

    [[nodiscard]] StepResult restore_uncommitted_store_security(TrashTransaction &transaction,
                                                                const std::size_t index) {
#ifndef _WIN32
        static_cast<void>(transaction);
        static_cast<void>(index);
        return {StepOutcome::committed, OperationStatus::success, {}};
#else
        auto &item = transaction.items[index];
        if (item.security_state == TrashSecurityState::original) {
            return {StepOutcome::committed, OperationStatus::success, {}};
        }
        if (item.kind == TrashItemKind::directory) {
            auto security = restore_directory_tree(item, item.restore_path);
            if (security) {
                return {StepOutcome::recovery_required, security->status,
                        std::move(security->detail_utf8)};
            }
        } else {
            auto security = vove::fileops::detail::restore_trash_security(
                item.restore_path, item.current_snapshot,
                item.original_security_descriptor_sddl_utf8);
            if (!security.ok()) {
                return {StepOutcome::recovery_required, security.status,
                        std::move(security.detail_utf8)};
            }
            item.current_snapshot = std::move(security.snapshot);
        }
        item.security_state = TrashSecurityState::original;
        std::string detail;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        return {StepOutcome::committed, OperationStatus::success, {}};
#endif
    }

    [[nodiscard]] StepResult recover_store_security_intent(TrashTransaction &transaction,
                                                           const std::size_t index) {
#ifndef _WIN32
        static_cast<void>(transaction);
        static_cast<void>(index);
        return {StepOutcome::recovery_required, OperationStatus::unsupported,
                "VO-VE Trash security is not enabled on this platform"};
#else
        auto &item = transaction.items[index];
        const auto directory = item.kind == TrashItemKind::directory;
        auto stored = vove::fileops::detail::verify_hardened_trash_payload(
            item.stored, item.current_snapshot, true, directory);
        auto source_hardened = vove::fileops::detail::verify_hardened_trash_payload(
            item.restore_path, item.current_snapshot, true, directory);
        auto source_original = vove::fileops::detail::verify_restored_trash_payload(
            item.restore_path, item.current_snapshot, item.original_security_descriptor_sddl_utf8,
            true, directory);
        const auto source_matches = source_hardened.ok() || source_original.ok();
        if (stored.ok() == source_matches) {
            return {StepOutcome::recovery_required, OperationStatus::conflict,
                    "active trash store is not anchored to exactly one owned payload"};
        }
        if (stored.ok()) {
            item.current_snapshot = std::move(stored.snapshot);
            item.security_state = TrashSecurityState::hardened;
        } else if (source_hardened.ok()) {
            item.current_snapshot = std::move(source_hardened.snapshot);
            item.security_state = TrashSecurityState::hardened;
        } else {
            if (directory) {
                item.current_snapshot = std::move(source_original.snapshot);
                auto hardened = harden_directory_tree(item, item.restore_path);
                if (hardened) {
                    return {StepOutcome::recovery_required, hardened->status,
                            std::move(hardened->detail_utf8)};
                }
            } else {
                auto hardened = vove::fileops::detail::harden_trash_payload(
                    item.restore_path, source_original.snapshot);
                if (!hardened.ok()) {
                    return {StepOutcome::recovery_required, hardened.status,
                            std::move(hardened.detail_utf8)};
                }
                item.current_snapshot = std::move(hardened.snapshot);
            }
            item.security_state = TrashSecurityState::hardened;
        }
        if (directory) {
            const auto root = stored.ok() ? item.stored : item.restore_path;
            auto hardened = harden_directory_tree(item, root);
            if (hardened) {
                return {StepOutcome::recovery_required, hardened->status,
                        std::move(hardened->detail_utf8)};
            }
            item.security_state = TrashSecurityState::hardened;
        }
        std::string detail;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        return {StepOutcome::committed, OperationStatus::success, {}};
#endif
    }

    [[nodiscard]] StepResult execute_step(TrashTransaction &transaction, const std::size_t index,
                                          const TrashStep step, const Progress &progress,
                                          const std::stop_token &stop) {
        transaction.active_step = step;
        transaction.active_index = static_cast<std::uint32_t>(index);
        transaction.active_evidence = OperationEvidence::none;
        std::string detail;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        if (step == TrashStep::store) {
            const auto secured = prepare_store_security(transaction, index);
            if (secured.outcome != StepOutcome::committed) {
                return secured;
            }
        }
        if (progress) {
            progress({.phase = transaction.phase,
                      .completed = progress_completed(transaction),
                      .total = transaction.items.size(),
                      .source = transaction.items[index].original});
        }
        auto result = submit_step(transaction, index, step, false);
        if (result.status == OperationStatus::not_found && !stop.stop_requested()) {
            result = submit_step(transaction, index, step, true);
        }
        if (result.ok()) {
            transaction.active_evidence = result.evidence;
            if (!apply_security_after_rename(transaction, index, step, result, detail) ||
                !apply_success(transaction, index, step, result, detail)) {
                transaction.failure_status = OperationStatus::unknown_outcome;
                transaction.failure_detail_utf8 = detail;
                if (!persist(transaction, detail)) {
                    return {StepOutcome::journal_error, OperationStatus::io_error,
                            std::move(detail)};
                }
                return {StepOutcome::recovery_required, OperationStatus::unknown_outcome,
                        transaction.failure_detail_utf8};
            }
            clear_active(transaction, transaction.phase != TrashPhase::rollback &&
                                          transaction.phase != TrashPhase::restore_rollback);
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return stop.stop_requested()
                       ? StepResult{StepOutcome::stopped, OperationStatus::success,
                                    "trash operation stopped after recording a completed step"}
                       : StepResult{StepOutcome::committed, OperationStatus::success, {}};
        }

        transaction.failure_status = result.status;
        transaction.active_evidence = result.evidence;
        if (step == TrashStep::purge) {
            result.detail_utf8 +=
                ": " + diagnostic_path_utf8(purge_request(transaction, index).source);
        }
        transaction.failure_detail_utf8 = result.detail_utf8;
        if (result.evidence == OperationEvidence::no_commit) {
            if (step == TrashStep::store) {
                const auto restored = restore_uncommitted_store_security(transaction, index);
                if (restored.outcome != StepOutcome::committed) {
                    return restored;
                }
            }
            if (transaction.phase == TrashPhase::moving && step == TrashStep::store) {
                transaction.phase = TrashPhase::rollback;
            } else if (transaction.phase == TrashPhase::restoring && step == TrashStep::restore) {
                transaction.phase = TrashPhase::restore_rollback;
            }
            transaction.active_step = TrashStep::none;
            transaction.active_index = 0;
            transaction.active_evidence = OperationEvidence::none;
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return stop.stop_requested()
                       ? StepResult{StepOutcome::stopped, result.status,
                                    "trash operation stopped after recording a non-commit"}
                       : StepResult{StepOutcome::no_commit, result.status, result.detail_utf8};
        }
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        return stop.stop_requested()
                   ? StepResult{StepOutcome::stopped, result.status,
                                "trash operation stopped with durable reconciliation evidence"}
                   : StepResult{StepOutcome::recovery_required, result.status, result.detail_utf8};
    }

    [[nodiscard]] StepResult reconcile_active(TrashTransaction &transaction,
                                              const std::stop_token &stop) {
        const auto index = static_cast<std::size_t>(transaction.active_index);
        const auto step = transaction.active_step;
        auto result = submit_step(transaction, index, step, true);
        if (transaction.active_evidence != OperationEvidence::none) {
            result = detail::merge_reconciliation(
                {.operation_id = step_operation_id(transaction, step, index),
                 .status = transaction.failure_status,
                 .evidence = transaction.active_evidence,
                 .confirmed_snapshot = {},
                 .detail_utf8 = transaction.failure_detail_utf8},
                std::move(result));
        }
        std::string detail;
        if (result.ok()) {
            if (!apply_security_after_rename(transaction, index, step, result, detail) ||
                !apply_success(transaction, index, step, result, detail)) {
                return {StepOutcome::recovery_required, OperationStatus::unknown_outcome, detail};
            }
            clear_active(transaction, transaction.phase != TrashPhase::rollback &&
                                          transaction.phase != TrashPhase::restore_rollback);
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return stop.stop_requested()
                       ? StepResult{StepOutcome::stopped, OperationStatus::success,
                                    "trash recovery stopped after reconciliation"}
                       : StepResult{StepOutcome::committed, OperationStatus::success, {}};
        }
        if (result.evidence == OperationEvidence::no_commit) {
            if (step == TrashStep::store) {
                const auto restored = restore_uncommitted_store_security(transaction, index);
                if (restored.outcome != StepOutcome::committed) {
                    return restored;
                }
            }
            if (transaction.phase == TrashPhase::moving && step == TrashStep::store) {
                transaction.phase = TrashPhase::rollback;
            } else if (transaction.phase == TrashPhase::restoring && step == TrashStep::restore) {
                transaction.phase = TrashPhase::restore_rollback;
            }
            transaction.failure_status = result.status;
            transaction.failure_detail_utf8 = result.detail_utf8;
            transaction.active_step = TrashStep::none;
            transaction.active_index = 0;
            transaction.active_evidence = OperationEvidence::none;
            if (!persist(transaction, detail)) {
                return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
            }
            return {StepOutcome::no_commit, result.status, result.detail_utf8};
        }
        transaction.failure_status = result.status;
        transaction.active_evidence = result.evidence;
        transaction.failure_detail_utf8 = result.detail_utf8;
        if (!persist(transaction, detail)) {
            return {StepOutcome::journal_error, OperationStatus::io_error, std::move(detail)};
        }
        return {StepOutcome::recovery_required, result.status, result.detail_utf8};
    }

    [[nodiscard]] TrashResult recovery_result(const TrashTransaction &transaction,
                                              const StepResult &step) const {
        const bool restoring = transaction.phase == TrashPhase::restore_prepared ||
                               transaction.phase == TrashPhase::restoring ||
                               transaction.phase == TrashPhase::restore_rollback ||
                               transaction.phase == TrashPhase::manifest_remove_intent ||
                               transaction.phase == TrashPhase::restore_container_remove_intent ||
                               transaction.phase == TrashPhase::restored;
        const bool purging = transaction.phase == TrashPhase::purge_prepared ||
                             transaction.phase == TrashPhase::purging ||
                             transaction.phase == TrashPhase::purge_manifest_remove_intent ||
                             transaction.phase == TrashPhase::purge_container_remove_intent ||
                             transaction.phase == TrashPhase::purged;
        return {.status = step.outcome == StepOutcome::journal_error ? TrashRunStatus::journal_error
                          : step.outcome == StepOutcome::stopped
                              ? TrashRunStatus::stopped
                              : TrashRunStatus::recovery_required,
                .operation_status = step.status,
                .completed = count_location(transaction, restoring ? TrashItemLocation::source
                                                         : purging ? TrashItemLocation::deleted
                                                                   : TrashItemLocation::stored),
                .total = transaction.items.size(),
                .manifest_path = {},
                .stored_paths = {},
                .detail_utf8 = step.detail};
    }

    [[nodiscard]] TrashResult rollback(TrashTransaction &transaction, const Progress &progress,
                                       const std::stop_token &stop,
                                       TrashNamespaceLease &namespace_lease) {
        for (std::size_t reverse = transaction.items.size(); reverse > 0; --reverse) {
            const auto index = reverse - 1U;
            if (transaction.items[index].location != TrashItemLocation::stored) {
                continue;
            }
            const auto step = execute_step(transaction, index, TrashStep::restore, progress, stop);
            if (step.outcome != StepOutcome::committed) {
                return recovery_result(transaction, step);
            }
        }
        std::string detail;
        if (transaction.phase != TrashPhase::rollback_container_remove_intent) {
            transaction.phase = TrashPhase::rollback_container_remove_intent;
            clear_active(transaction, false);
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
#ifdef _WIN32
        if (!remove_owned_manifest_copy(trash_rescue_plan_path(transaction), transaction,
                                        TrashPhase::prepared, "trash rescue plan", detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
#endif

        namespace_lease.release_container();
        if (!retire_container(transaction, detail, true)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        invoke_cleanup_crash_hook(detail::TrashCleanupCrashPoint::rollback_container_retired);
        if (!remove_journal(detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return {.status = TrashRunStatus::rolled_back,
                .operation_status = transaction.failure_status,
                .completed = 0,
                .total = transaction.items.size(),
                .manifest_path = {},
                .stored_paths = {},
                .detail_utf8 = transaction.failure_detail_utf8};
    }

    [[nodiscard]] bool publish_manifest_copy(const std::filesystem::path &path,
                                             const std::span<const std::byte> payload,
                                             const std::string_view label, std::string &detail,
                                             const std::span<const std::byte> trusted_previous = {},
                                             const bool require_trusted_previous = false) {
        DurableJournalStore store(path);
        if (require_trusted_previous) {
            invoke_manifest_republish_hook(path);
        }
        const auto existing = store.read();
        if (existing.ok()) {
            if (!std::ranges::equal(existing.payload, payload) &&
                (trusted_previous.empty() ||
                 !std::ranges::equal(existing.payload, trusted_previous))) {
                detail = std::string(label) + " path is occupied by a different operation";
                return false;
            }
        } else if (require_trusted_previous) {
            detail = std::string(label) + " changed before migration could be published";
            return false;
        } else if (existing.status != DurableJournalStatus::not_found &&
                   existing.status != DurableJournalStatus::corrupt) {
            detail = journal_error_detail(existing, label);
            return false;
        }
        // Two writes intentionally establish primary and previous generations in production.
        for (std::size_t generation{}; generation < 2; ++generation) {
            const auto stored = store.write(payload);
            if (!stored.ok()) {
                detail = journal_error_detail(stored, label);
                return false;
            }
        }
        const auto verified = store.read();
        if (!verified.ok() || !std::ranges::equal(verified.payload, payload)) {
            detail = std::string(label) + " could not be verified after publication";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool publish_manifest(TrashTransaction &transaction, std::string &detail) {
        auto published = transaction;
        published.phase = TrashPhase::published;
        clear_active(published);
        const auto payload = encode_trash_transaction(published);
#ifdef _WIN32
        if (!publish_manifest_copy(trash_rescue_manifest_path(published), payload,
                                   "trash rescue manifest", detail) ||
            !publish_manifest_copy(
                manifest_path(options.manifest_directory, transaction.operation_id), payload,
                "trash manifest", detail)) {
            return false;
        }
#else
        if (!publish_manifest_copy(
                manifest_path(options.manifest_directory, transaction.operation_id), payload,
                "trash manifest", detail)) {
            return false;
        }
#endif
        transaction = std::move(published);
        return true;
    }

    [[nodiscard]] TrashResult continue_transaction(TrashTransaction transaction,
                                                   const Progress &progress,
                                                   const std::stop_token &stop) {
        std::string detail;
        if (transaction.phase == TrashPhase::prepared) {
            const auto prepared = ensure_container(transaction);
            if (!prepared.ok() || prepared.evidence != OperationEvidence::committed) {
                auto failure_detail = prepared.detail_utf8;
#ifndef _WIN32
                if (!prepared.ok() && prepared.evidence == OperationEvidence::no_commit &&
                    !prepared.destination_present &&
                    prepared.status != OperationStatus::unknown_outcome &&
                    prepared.status != OperationStatus::timed_out &&
                    prepared.status != OperationStatus::disconnected) {
                    if (abort_untouched_preparation(transaction, stop, detail)) {
                        return {.status = TrashRunStatus::storage_unavailable,
                                .operation_status = prepared.status,
                                .completed = 0,
                                .total = transaction.items.size(),
                                .manifest_path = {},
                                .stored_paths = {},
                                .detail_utf8 = std::move(failure_detail)};
                    }
                    failure_detail += "; " + detail;
                }
#endif
#ifdef _WIN32
                constexpr auto outcome = StepOutcome::journal_error;
#else
                constexpr auto outcome = StepOutcome::recovery_required;
#endif
                return recovery_result(transaction,
                                       {outcome, prepared.status, std::move(failure_detail)});
            }
        }
        TrashNamespaceLease namespace_lease;
#ifdef _WIN32
        const auto allow_missing_container =
            transaction.phase == TrashPhase::rollback_container_remove_intent;
        auto pinned_namespace = pin_trash_namespace(transaction, detail, allow_missing_container);
        if (!pinned_namespace) {
            return recovery_result(transaction,
                                   {StepOutcome::recovery_required,
                                    OperationStatus::permission_denied, std::move(detail)});
        }
        namespace_lease = std::move(*pinned_namespace);
        if (transaction.active_step == TrashStep::store) {
            const auto secured =
                recover_store_security_intent(transaction, transaction.active_index);
            if (secured.outcome != StepOutcome::committed) {
                return recovery_result(transaction, secured);
            }
        }
        OperationStatus security_status{};
        if (!preflight_trash_security(transaction, false, security_status, detail)) {
            return recovery_result(
                transaction, {StepOutcome::recovery_required, security_status, std::move(detail)});
        }
#endif
        if (transaction.active_step != TrashStep::none) {
            const auto reconciled = reconcile_active(transaction, stop);
            if (reconciled.outcome != StepOutcome::committed &&
                reconciled.outcome != StepOutcome::no_commit) {
                return recovery_result(transaction, reconciled);
            }
        }
        if (transaction.phase == TrashPhase::prepared) {
#ifdef _WIN32
            const auto plan_payload = encode_trash_transaction(transaction);
            if (!publish_manifest_copy(trash_rescue_plan_path(transaction), plan_payload,
                                       "trash rescue plan", detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
#endif
            transaction.phase = TrashPhase::moving;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::moving) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                if (transaction.items[index].location != TrashItemLocation::source) {
                    continue;
                }
                const auto step =
                    execute_step(transaction, index, TrashStep::store, progress, stop);
                if (step.outcome == StepOutcome::committed) {
                    continue;
                }
                if (step.outcome == StepOutcome::no_commit) {
                    return rollback(transaction, progress, stop, namespace_lease);
                }
                return recovery_result(transaction, step);
            }
            transaction.phase = TrashPhase::manifest_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::rollback ||
            transaction.phase == TrashPhase::rollback_container_remove_intent) {
            return rollback(transaction, progress, stop, namespace_lease);
        }
        if (transaction.phase == TrashPhase::manifest_intent) {
            if (!publish_manifest(transaction, detail) || !persist(transaction, detail)) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = count_location(transaction, TrashItemLocation::stored),
                        .total = transaction.items.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
        }
        if (transaction.phase == TrashPhase::published) {
            const auto path = manifest_path(options.manifest_directory, transaction.operation_id);
            const auto expected = encode_trash_transaction(transaction);
#ifdef _WIN32
            if (!publish_manifest_copy(trash_rescue_manifest_path(transaction), expected,
                                       "trash rescue manifest", detail) ||
                !publish_manifest_copy(path, expected, "trash manifest", detail)) {
#else
            if (!publish_manifest_copy(path, expected, "trash manifest", detail)) {
#endif
                return {.status = TrashRunStatus::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .completed = transaction.items.size(),
                        .total = transaction.items.size(),
                        .manifest_path = path,
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            if (!remove_journal(detail)) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = transaction.items.size(),
                        .total = transaction.items.size(),
                        .manifest_path = path,
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            std::vector<std::filesystem::path> stored_paths;
            stored_paths.reserve(transaction.items.size());
            for (const auto &item : transaction.items) {
                stored_paths.push_back(item.stored);
            }
            return {.status = TrashRunStatus::success,
                    .operation_status = OperationStatus::success,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .manifest_path = path,
                    .stored_paths = std::move(stored_paths),
                    .detail_utf8 = {}};
        }
        return {.status = TrashRunStatus::recovery_required,
                .operation_status = OperationStatus::io_error,
                .completed = count_location(transaction, TrashItemLocation::stored),
                .total = transaction.items.size(),
                .manifest_path = {},
                .stored_paths = {},
                .detail_utf8 = "trash transaction phase is not recoverable"};
    }

    [[nodiscard]] TrashResult restore_rollback(TrashTransaction &transaction,
                                               const Progress &progress,
                                               const std::stop_token &stop) {
        for (std::size_t reverse = transaction.items.size(); reverse > 0; --reverse) {
            const auto index = reverse - 1U;
            if (transaction.items[index].location != TrashItemLocation::source) {
                continue;
            }
            const auto step = execute_step(transaction, index, TrashStep::store, progress, stop);
            if (step.outcome != StepOutcome::committed) {
                return recovery_result(transaction, step);
            }
        }
        std::string detail;
        if (!remove_journal(detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .manifest_path =
                        manifest_path(options.manifest_directory, transaction.operation_id),
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return {.status = TrashRunStatus::rolled_back,
                .operation_status = transaction.failure_status,
                .completed = transaction.items.size(),
                .total = transaction.items.size(),
                .manifest_path =
                    manifest_path(options.manifest_directory, transaction.operation_id),
                .stored_paths = {},
                .detail_utf8 = transaction.failure_detail_utf8};
    }

    [[nodiscard]] bool remove_owned_manifest_copy(const std::filesystem::path &path,
                                                  const TrashTransaction &transaction,
                                                  const TrashPhase expected_phase,
                                                  const std::string_view label,
                                                  std::string &detail) {
        DurableJournalStore store(path);
        const auto loaded = store.read();
        if (loaded.status == DurableJournalStatus::not_found) {
            return true;
        }
        if (!loaded.ok()) {
            detail = journal_error_detail(loaded, label);
            return false;
        }
        TrashTransaction manifest;
        if (!decode_trash_transaction(loaded.payload, manifest, detail) ||
            manifest.phase != expected_phase ||
            !same_manifest_identity(manifest, transaction, true)) {
            if (detail.empty()) {
                detail = std::string(label) + " identity differs from the restore operation";
            }
            return false;
        }
        const auto removed = store.remove_if_payload_matches(loaded.payload);
        if (!removed.ok()) {
            detail = journal_error_detail(removed, label);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool remove_owned_manifests(const TrashTransaction &transaction,
                                              std::string &detail) {
        if (!remove_owned_manifest_copy(
                manifest_path(options.manifest_directory, transaction.operation_id), transaction,
                TrashPhase::published, "trash manifest", detail)
#ifdef _WIN32
            ||
            !remove_owned_manifest_copy(trash_rescue_manifest_path(transaction), transaction,
                                        TrashPhase::published, "trash rescue manifest", detail) ||
            !remove_owned_manifest_copy(trash_rescue_plan_path(transaction), transaction,
                                        TrashPhase::prepared, "trash rescue plan", detail)
#endif
        ) {
            return false;
        }
        return true;
    }

    [[nodiscard]] TrashResult continue_restore(TrashTransaction transaction,
                                               const Progress &progress,
                                               const std::stop_token &stop) {
        std::string detail;
        TrashNamespaceLease namespace_lease;
#ifdef _WIN32
        const auto allow_missing_container =
            transaction.phase == TrashPhase::restore_container_remove_intent ||
            transaction.phase == TrashPhase::restored;
        auto pinned_namespace = pin_trash_namespace(transaction, detail, allow_missing_container);
        if (!pinned_namespace) {
            return recovery_result(transaction,
                                   {StepOutcome::recovery_required,
                                    OperationStatus::permission_denied, std::move(detail)});
        }
        namespace_lease = std::move(*pinned_namespace);
        OperationStatus security_status{};
        if (!preflight_trash_security(transaction, true, security_status, detail)) {
            return recovery_result(
                transaction, {StepOutcome::recovery_required, security_status, std::move(detail)});
        }
#endif
        if (transaction.active_step != TrashStep::none) {
            const auto reconciled = reconcile_active(transaction, stop);
            if (reconciled.outcome != StepOutcome::committed &&
                reconciled.outcome != StepOutcome::no_commit) {
                return recovery_result(transaction, reconciled);
            }
        }
        if (transaction.phase == TrashPhase::restore_prepared) {
            transaction.phase = TrashPhase::restoring;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::restoring) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                if (transaction.items[index].location != TrashItemLocation::stored) {
                    continue;
                }
                const auto step =
                    execute_step(transaction, index, TrashStep::restore, progress, stop);
                if (step.outcome == StepOutcome::committed) {
                    continue;
                }
                if (step.outcome == StepOutcome::no_commit) {
                    return restore_rollback(transaction, progress, stop);
                }
                return recovery_result(transaction, step);
            }
            transaction.phase = TrashPhase::manifest_remove_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::restore_rollback) {
            return restore_rollback(transaction, progress, stop);
        }
        if (transaction.phase == TrashPhase::manifest_remove_intent) {
            if (!remove_owned_manifests(transaction, detail)) {
                return {.status = TrashRunStatus::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .completed = count_location(transaction, TrashItemLocation::source),
                        .total = transaction.items.size(),
                        .manifest_path =
                            manifest_path(options.manifest_directory, transaction.operation_id),
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            transaction.phase = TrashPhase::restore_container_remove_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::restore_container_remove_intent) {
            namespace_lease.release_container();
            if (!retire_container(transaction, detail)) {
                return {.status = TrashRunStatus::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .completed = count_location(transaction, TrashItemLocation::source),
                        .total = transaction.items.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            invoke_cleanup_crash_hook(detail::TrashCleanupCrashPoint::restore_container_retired);
            transaction.phase = TrashPhase::restored;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::restored) {
            if (!remove_journal(detail)) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = transaction.items.size(),
                        .total = transaction.items.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            std::vector<std::filesystem::path> restored_paths;
            restored_paths.reserve(transaction.items.size());
            for (const auto &item : transaction.items) {
                restored_paths.push_back(item.restore_path);
            }
            return {.status = TrashRunStatus::success,
                    .operation_status = OperationStatus::success,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = std::move(restored_paths),
                    .detail_utf8 = {}};
        }
        return {.status = TrashRunStatus::recovery_required,
                .operation_status = OperationStatus::io_error,
                .completed = count_location(transaction, TrashItemLocation::source),
                .total = transaction.items.size(),
                .manifest_path =
                    manifest_path(options.manifest_directory, transaction.operation_id),
                .stored_paths = {},
                .detail_utf8 = "trash restore phase is not recoverable"};
    }

    [[nodiscard]] TrashResult continue_purge(TrashTransaction transaction, const Progress &progress,
                                             const std::stop_token &stop) {
        std::string detail;
        TrashNamespaceLease namespace_lease;
#ifdef _WIN32
        const auto allow_missing_container =
            transaction.phase == TrashPhase::purge_container_remove_intent ||
            transaction.phase == TrashPhase::purged;
        auto pinned_namespace = pin_trash_namespace(transaction, detail, allow_missing_container);
        if (!pinned_namespace) {
            return recovery_result(transaction,
                                   {StepOutcome::recovery_required,
                                    OperationStatus::permission_denied, std::move(detail)});
        }
        namespace_lease = std::move(*pinned_namespace);
        OperationStatus security_status{};
        if (!preflight_trash_security(transaction, false, security_status, detail)) {
            return recovery_result(
                transaction, {StepOutcome::recovery_required, security_status, std::move(detail)});
        }
#endif
        if (transaction.active_step != TrashStep::none) {
            const auto reconciled = reconcile_active(transaction, stop);
            if (reconciled.outcome != StepOutcome::committed &&
                reconciled.outcome != StepOutcome::no_commit) {
                return recovery_result(transaction, reconciled);
            }
        }
        if (transaction.phase == TrashPhase::purge_prepared) {
            transaction.phase = TrashPhase::purging;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::purging) {
            for (std::size_t index{}; index < transaction.items.size(); ++index) {
                while (transaction.items[index].location == TrashItemLocation::stored) {
                    const auto step =
                        execute_step(transaction, index, TrashStep::purge, progress, stop);
                    if (step.outcome != StepOutcome::committed) {
                        return recovery_result(transaction, step);
                    }
                }
            }
            transaction.phase = TrashPhase::purge_manifest_remove_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::purge_manifest_remove_intent) {
            if (!remove_owned_manifests(transaction, detail)) {
                return {.status = TrashRunStatus::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .completed = count_location(transaction, TrashItemLocation::deleted),
                        .total = transaction.items.size(),
                        .manifest_path =
                            manifest_path(options.manifest_directory, transaction.operation_id),
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            transaction.phase = TrashPhase::purge_container_remove_intent;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::purge_container_remove_intent) {
            namespace_lease.release_container();
            if (!retire_container(transaction, detail)) {
                return {.status = TrashRunStatus::recovery_required,
                        .operation_status = OperationStatus::unknown_outcome,
                        .completed = count_location(transaction, TrashItemLocation::deleted),
                        .total = transaction.items.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            invoke_cleanup_crash_hook(detail::TrashCleanupCrashPoint::purge_container_retired);
            transaction.phase = TrashPhase::purged;
            if (!persist(transaction, detail)) {
                return recovery_result(
                    transaction, {StepOutcome::journal_error, OperationStatus::io_error, detail});
            }
        }
        if (transaction.phase == TrashPhase::purged) {
            if (!remove_journal(detail)) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = transaction.items.size(),
                        .total = transaction.items.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
            return {.status = TrashRunStatus::success,
                    .operation_status = OperationStatus::success,
                    .completed = transaction.items.size(),
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = {}};
        }
        return {.status = TrashRunStatus::recovery_required,
                .operation_status = OperationStatus::io_error,
                .completed = count_location(transaction, TrashItemLocation::deleted),
                .total = transaction.items.size(),
                .manifest_path =
                    manifest_path(options.manifest_directory, transaction.operation_id),
                .stored_paths = {},
                .detail_utf8 = "trash purge phase is not recoverable"};
    }

    [[nodiscard]] TrashResult run_new(const std::vector<TrashSource> &sources,
                                      const Progress &progress, const std::stop_token &stop) {
#ifndef _WIN32
        if (!initialization_error.empty() || options.journal_path.empty() ||
            options.manifest_directory.empty() || sources.empty()) {
            return {
                .status = initialization_error.empty() ? TrashRunStatus::invalid_request
                                                       : TrashRunStatus::journal_error,
                .operation_status = initialization_error.empty() ? OperationStatus::invalid_request
                                                                 : OperationStatus::io_error,
                .completed = 0,
                .total = sources.size(),
                .manifest_path = {},
                .stored_paths = {},
                .detail_utf8 = initialization_error.empty() ? "trash request or state path is empty"
                                                            : initialization_error};
        }
        if (journal.read().status != DurableJournalStatus::not_found) {
            recovery_pending.store(true, std::memory_order_release);
            return {.status = TrashRunStatus::recovery_required,
                    .operation_status = OperationStatus::unknown_outcome,
                    .completed = 0,
                    .total = sources.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = "an unfinished file operation must be recovered first"};
        }
        const auto operation_id = next_trash_operation_id();
        auto prepared_sources = sources;
        try {
            for (auto &source : prepared_sources) {
                source.original_security_descriptor_sddl_utf8.clear();
                source.restore_path = source.path.lexically_normal();
                // Capture the mount identity before quota filtering so a later SMB remount cannot
                // hide existing Trash data merely because its transient device number changed.
                source.storage_identity_utf8 = platform::storage_identity(source.path);
            }
        } catch (const std::exception &error) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = sources.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = error.what()};
        }
        if (const auto captured = capture_trash_directories(prepared_sources, operation_id)) {
            return *captured;
        }
        if (const auto quota = verify_quota(prepared_sources, stop)) {
            return *quota;
        }
        const auto created = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        TrashTransaction transaction;
        try {
            transaction = prepare_trash_transaction(prepared_sources, operation_id, created);
            if (const auto budget =
                    verify_transaction_journal_budget(transaction, sources.size())) {
                return *budget;
            }
            if (DurableJournalStore(manifest_path(options.manifest_directory, operation_id))
                    .read()
                    .status != DurableJournalStatus::not_found) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::conflict,
                        .completed = 0,
                        .total = sources.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = "trash operation id already exists"};
            }
        } catch (const std::exception &error) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = sources.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = error.what()};
        }
        std::string detail;
        if (!persist(transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_transaction(std::move(transaction), progress, stop);
#else
        if (!initialization_error.empty() || options.journal_path.empty() ||
            options.manifest_directory.empty() || sources.empty()) {
            return {
                .status = initialization_error.empty() ? TrashRunStatus::invalid_request
                                                       : TrashRunStatus::journal_error,
                .operation_status = initialization_error.empty() ? OperationStatus::invalid_request
                                                                 : OperationStatus::io_error,
                .completed = 0,
                .total = sources.size(),
                .manifest_path = {},
                .stored_paths = {},
                .detail_utf8 = initialization_error.empty() ? "trash request or state path is empty"
                                                            : initialization_error};
        }
        if (journal.read().status != DurableJournalStatus::not_found) {
            recovery_pending.store(true, std::memory_order_release);
            return {.status = TrashRunStatus::recovery_required,
                    .operation_status = OperationStatus::unknown_outcome,
                    .completed = 0,
                    .total = sources.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = "an unfinished file operation must be recovered first"};
        }
        const auto operation_id = next_trash_operation_id();
        auto prepared_sources = sources;
        if (const auto captured = capture_trash_directories(prepared_sources, operation_id)) {
            return *captured;
        }
        for (const auto &source : prepared_sources) {
            if (classify_delete_target(source.path) != DeleteTargetKind::local) {
                return {.status = TrashRunStatus::unsupported,
                        .operation_status = OperationStatus::unsupported,
                        .completed = 0,
                        .total = sources.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = "VO-VE Trash is restricted to local filesystems"};
            }
        }
        if (const auto storage = prepare_vault_owner(prepared_sources, stop)) {
            return *storage;
        }
        if (const auto quota = verify_quota(prepared_sources, stop)) {
            return *quota;
        }
        const auto created = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        TrashTransaction transaction;
        try {
            auto vault =
                select_trash_vault_root(prepared_sources.front().path, options.manifest_directory);
            if (!vault.ok()) {
                return {.status = vault.status,
                        .operation_status = vault.operation_status,
                        .completed = 0,
                        .total = sources.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = std::move(vault.detail)};
            }
            auto secured_sources = std::move(prepared_sources);
            for (auto &source : secured_sources) {
                std::string canonical_detail;
                const auto restore_path = canonical_volume_path(source.path, canonical_detail);
                if (!restore_path) {
                    return {.status = TrashRunStatus::invalid_request,
                            .operation_status = OperationStatus::invalid_request,
                            .completed = 0,
                            .total = sources.size(),
                            .manifest_path = {},
                            .stored_paths = {},
                            .detail_utf8 = std::move(canonical_detail)};
                }
                auto security = capture_trash_source_security(source);
                if (security) {
                    return {.status = TrashRunStatus::invalid_request,
                            .operation_status = security->status,
                            .completed = 0,
                            .total = sources.size(),
                            .manifest_path = {},
                            .stored_paths = {},
                            .detail_utf8 = std::move(security->detail_utf8)};
                }
                source.restore_path = *restore_path;
            }
            transaction =
                prepare_trash_transaction(secured_sources, operation_id, created, vault.path);
            if (const auto budget =
                    verify_transaction_journal_budget(transaction, sources.size())) {
                return *budget;
            }
            DurableJournalStore manifest(manifest_path(options.manifest_directory, operation_id));
            std::error_code container_error;
            const auto container = transaction.items.front().stored.parent_path();
            const auto container_exists = std::filesystem::exists(container, container_error);
            if (container_error || container_exists ||
                manifest.read().status != DurableJournalStatus::not_found) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::conflict,
                        .completed = 0,
                        .total = sources.size(),
                        .manifest_path = {},
                        .stored_paths = {},
                        .detail_utf8 = "trash operation id already exists or cannot be checked"};
            }
        } catch (const std::exception &error) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = sources.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = error.what()};
        }
        std::string detail;
        if (!persist(transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_transaction(std::move(transaction), progress, stop);
#endif
    }

    [[nodiscard]] TrashResult
    run_restore_new(const std::filesystem::path &requested_manifest,
                    const std::optional<std::filesystem::path> &destination_directory,
                    const Progress &progress, const std::stop_token &stop) {
#ifndef _WIN32
        if (!initialization_error.empty() || requested_manifest.empty() ||
            !requested_manifest.is_absolute() || requested_manifest.extension() != ".vtrash") {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = initialization_error.empty()
                                       ? "trash restore manifest path is invalid"
                                       : initialization_error};
        }
        const auto loaded = DurableJournalStore(requested_manifest).read();
        TrashTransaction transaction;
        std::string detail;
        if (!loaded.ok() || !decode_trash_transaction(loaded.payload, transaction, detail) ||
            transaction.phase != TrashPhase::published) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = detail.empty() ? "trash manifest is invalid or unavailable"
                                                  : std::move(detail)};
        }
        if (!trusted_posix_trash_storage(transaction)) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = "trash manifest storage layout is not trusted"};
        }
        const auto central = manifest_path(options.manifest_directory, transaction.operation_id);
        if (requested_manifest.lexically_normal() != central.lexically_normal()) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = "trash manifest is outside its owned location"};
        }
        if (destination_directory &&
            !rebase_restore_paths(transaction, *destination_directory, detail)) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        if (!destination_directory) {
            const auto published_payload = encode_trash_transaction(transaction);
            if (!publish_manifest_copy(central, published_payload, "trash manifest", detail,
                                       loaded.payload, true)) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = 0,
                        .total = transaction.items.size(),
                        .manifest_path = requested_manifest,
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
        }
        transaction.phase = TrashPhase::restore_prepared;
        clear_active(transaction);
        if (!persist(transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_restore(std::move(transaction), progress, stop);
#else
        if (!initialization_error.empty() || requested_manifest.empty() ||
            !requested_manifest.is_absolute() || requested_manifest.extension() != ".vtrash") {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = initialization_error.empty()
                                       ? "trash restore manifest path is invalid"
                                       : initialization_error};
        }
        const auto loaded = DurableJournalStore(requested_manifest).read();
        TrashTransaction transaction;
        std::string detail;
        if (!loaded.ok() || !decode_trash_transaction(loaded.payload, transaction, detail) ||
            transaction.phase != TrashPhase::published) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = detail.empty() ? "trash manifest is invalid or unavailable"
                                                  : std::move(detail)};
        }
        const auto central = manifest_path(options.manifest_directory, transaction.operation_id);
        const auto rescue = trash_rescue_manifest_path(transaction);
        if (requested_manifest.lexically_normal() != central.lexically_normal() &&
            requested_manifest.lexically_normal() != rescue.lexically_normal()) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = "trash manifest is outside its owned locations"};
        }
        OperationStatus security_status{};
        if (!preflight_trash_security(transaction, true, security_status, detail)) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = security_status,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        if (destination_directory &&
            !rebase_restore_paths(transaction, *destination_directory, detail)) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        if (!destination_directory) {
            const auto published_payload = encode_trash_transaction(transaction);
            const auto requested = requested_manifest.lexically_normal();
            if (!publish_manifest_copy(rescue, published_payload, "trash rescue manifest", detail,
                                       loaded.payload, requested == rescue.lexically_normal()) ||
                !publish_manifest_copy(central, published_payload, "trash manifest", detail,
                                       loaded.payload, requested == central.lexically_normal())) {
                return {.status = TrashRunStatus::journal_error,
                        .operation_status = OperationStatus::io_error,
                        .completed = 0,
                        .total = transaction.items.size(),
                        .manifest_path = requested_manifest,
                        .stored_paths = {},
                        .detail_utf8 = std::move(detail)};
            }
        }
        transaction.phase = TrashPhase::restore_prepared;
        clear_active(transaction);
        if (!persist(transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_restore(std::move(transaction), progress, stop);
#endif
    }

    [[nodiscard]] TrashResult run_purge_new(const std::filesystem::path &requested_manifest,
                                            const Progress &progress, const std::stop_token &stop) {
#ifndef _WIN32
        if (!initialization_error.empty() || requested_manifest.empty() ||
            !requested_manifest.is_absolute() || requested_manifest.extension() != ".vtrash") {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = initialization_error.empty()
                                       ? "trash purge manifest path is invalid"
                                       : initialization_error};
        }
        const auto loaded = DurableJournalStore(requested_manifest).read();
        TrashTransaction transaction;
        std::string detail;
        if (!loaded.ok() || !decode_trash_transaction(loaded.payload, transaction, detail) ||
            transaction.phase != TrashPhase::published) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = detail.empty() ? "trash manifest is invalid or unavailable"
                                                  : std::move(detail)};
        }
        if (!trusted_posix_trash_storage(transaction)) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = "trash manifest storage layout is not trusted"};
        }
        const auto central = manifest_path(options.manifest_directory, transaction.operation_id);
        if (requested_manifest.lexically_normal() != central.lexically_normal()) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = "trash manifest is outside its owned location"};
        }
        const auto published_payload = encode_trash_transaction(transaction);
        if (!publish_manifest_copy(central, published_payload, "trash manifest", detail,
                                   loaded.payload, true)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        transaction.phase = TrashPhase::purge_prepared;
        clear_active(transaction);
        if (!persist(transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_purge(std::move(transaction), progress, stop);
#else
        if (!initialization_error.empty() || requested_manifest.empty() ||
            !requested_manifest.is_absolute() || requested_manifest.extension() != ".vtrash") {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = initialization_error.empty()
                                       ? "trash purge manifest path is invalid"
                                       : initialization_error};
        }
        const auto loaded = DurableJournalStore(requested_manifest).read();
        TrashTransaction transaction;
        std::string detail;
        if (!loaded.ok() || !decode_trash_transaction(loaded.payload, transaction, detail) ||
            transaction.phase != TrashPhase::published) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = detail.empty() ? "trash manifest is invalid or unavailable"
                                                  : std::move(detail)};
        }
        const auto central = manifest_path(options.manifest_directory, transaction.operation_id);
        const auto rescue = trash_rescue_manifest_path(transaction);
        if (requested_manifest.lexically_normal() != central.lexically_normal() &&
            requested_manifest.lexically_normal() != rescue.lexically_normal()) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = "trash manifest is outside its owned locations"};
        }
        OperationStatus security_status{};
        if (!preflight_trash_security(transaction, false, security_status, detail)) {
            return {.status = TrashRunStatus::invalid_request,
                    .operation_status = security_status,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        const auto published_payload = encode_trash_transaction(transaction);
        const auto requested = requested_manifest.lexically_normal();
        if (!publish_manifest_copy(rescue, published_payload, "trash rescue manifest", detail,
                                   loaded.payload, requested == rescue.lexically_normal()) ||
            !publish_manifest_copy(central, published_payload, "trash manifest", detail,
                                   loaded.payload, requested == central.lexically_normal())) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        transaction.phase = TrashPhase::purge_prepared;
        clear_active(transaction);
        if (!persist(transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = requested_manifest,
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        return continue_purge(std::move(transaction), progress, stop);
#endif
    }

    [[nodiscard]] TrashResult run_resume(const Progress &progress, const std::stop_token &stop) {
        const auto loaded = journal.read();
        if (!loaded.ok() || loaded.encoding != CurrentOperationJournalEncoding::typed ||
            (loaded.kind != CurrentOperationKind::trash_move &&
             loaded.kind != CurrentOperationKind::trash_restore &&
             loaded.kind != CurrentOperationKind::trash_purge)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = loaded.status == DurableJournalStatus::not_found
                                       ? "there is no trash operation to recover"
                                       : "current operation journal is not a trash operation"};
        }
        TrashTransaction transaction;
        std::string detail;
        if (!decode_trash_transaction(loaded.payload, transaction, detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::io_error,
                    .completed = 0,
                    .total = 0,
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
#ifndef _WIN32
        const auto allow_restore_rebase =
            loaded.kind == CurrentOperationKind::trash_restore &&
            std::ranges::any_of(transaction.items, [](const auto &item) {
                return item.restore_path.lexically_normal() != item.original.lexically_normal();
            });
        if (!trusted_posix_trash_storage(transaction, allow_restore_rebase)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = "trash recovery storage layout is not trusted"};
        }
#endif
        if (loaded.kind == CurrentOperationKind::trash_restore &&
            std::ranges::any_of(transaction.items,
                                [](const auto &item) {
                                    return item.restore_path.lexically_normal() !=
                                           item.original.lexically_normal();
                                }) &&
            !validate_restore_destination(
                transaction, transaction.items.front().restore_path.parent_path(), detail)) {
            return {.status = TrashRunStatus::journal_error,
                    .operation_status = OperationStatus::invalid_request,
                    .completed = 0,
                    .total = transaction.items.size(),
                    .manifest_path = {},
                    .stored_paths = {},
                    .detail_utf8 = std::move(detail)};
        }
        if (loaded.kind == CurrentOperationKind::trash_move) {
            return continue_transaction(std::move(transaction), progress, stop);
        }
        if (loaded.kind == CurrentOperationKind::trash_restore) {
            return continue_restore(std::move(transaction), progress, stop);
        }
        return continue_purge(std::move(transaction), progress, stop);
    }

    template <typename Job>
    [[nodiscard]] bool launch(CurrentOperationLease lease, Job job, Progress progress,
                              Completion completion) {
        if (!completion) {
            return false;
        }
        const auto self = shared_from_this();
        std::scoped_lock lock(worker_mutex);
        if (stopped.load(std::memory_order_acquire) || running.load(std::memory_order_acquire)) {
            return false;
        }
        worker_stop = std::stop_source{};
        running.store(true, std::memory_order_release);
        const auto stop = worker_stop.get_token();
        try {
            std::thread([self, lease = std::move(lease), job = std::move(job),
                         progress = std::move(progress), completion = std::move(completion),
                         stop]() mutable {
                {
                    std::scoped_lock worker_lock(self->worker_mutex);
                    self->worker_id = std::this_thread::get_id();
                }
                TrashResult result;
                try {
                    result = job(progress, stop);
                } catch (const std::exception &error) {
                    result.detail_utf8 = error.what();
                } catch (...) {
                    result.detail_utf8 = "trash operation failed unexpectedly";
                }
                lease.release();
                {
                    std::scoped_lock worker_lock(self->worker_mutex);
                    self->worker_id = {};
                    self->running.store(false, std::memory_order_release);
                }
                self->worker_completed.notify_all();
                if (!stop.stop_requested()) {
                    try {
                        completion(std::move(result));
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                    }
                }
            }).detach();
        } catch (...) {
            running.store(false, std::memory_order_release);
            worker_completed.notify_all();
            return false;
        }
        return true;
    }

    void stop() noexcept {
        bool called_from_worker{};
        {
            std::scoped_lock lock(worker_mutex);
            stopped.store(true, std::memory_order_release);
            static_cast<void>(worker_stop.request_stop());
            called_from_worker =
                running.load(std::memory_order_acquire) && worker_id == std::this_thread::get_id();
        }
        file_operations.stop();
        if (!called_from_worker) {
            std::unique_lock lock(worker_mutex);
            worker_completed.wait(lock,
                                  [this] { return !running.load(std::memory_order_acquire); });
        }
    }
};

TrashCoordinator::TrashCoordinator(TrashCoordinatorOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}

TrashCoordinator::~TrashCoordinator() {
    stop();
}

bool TrashCoordinator::start(std::vector<TrashSource> sources, Progress progress,
                             Completion completion) {
    const auto state = state_;
    if (sources.empty()) {
        return false;
    }
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, sources = std::move(sources)](const Progress &callback,
                                              const std::stop_token &stop) mutable {
            return state->run_new(sources, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool TrashCoordinator::restore(std::filesystem::path manifest_path, Progress progress,
                               Completion completion) {
    const auto state = state_;
    if (manifest_path.empty()) {
        return false;
    }
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, manifest_path = std::move(manifest_path)](const Progress &callback,
                                                          const std::stop_token &stop) {
            return state->run_restore_new(manifest_path, std::nullopt, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool TrashCoordinator::restore_to(std::filesystem::path manifest_path,
                                  std::filesystem::path destination_directory, Progress progress,
                                  Completion completion) {
    const auto state = state_;
    if (manifest_path.empty() || destination_directory.empty()) {
        return false;
    }
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, manifest_path = std::move(manifest_path),
         destination_directory = std::move(destination_directory)](const Progress &callback,
                                                                   const std::stop_token &stop) {
            return state->run_restore_new(manifest_path, destination_directory, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool TrashCoordinator::purge(std::filesystem::path manifest_path, Progress progress,
                             Completion completion) {
    const auto state = state_;
    if (manifest_path.empty()) {
        return false;
    }
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || state->refresh_recovery_pending()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state, manifest_path = std::move(manifest_path)](const Progress &callback,
                                                          const std::stop_token &stop) {
            return state->run_purge_new(manifest_path, callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool TrashCoordinator::resume(Progress progress, Completion completion) {
    const auto state = state_;
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(state->options.journal_path, lease_error);
    if (!lease.owns_lock() || !state->owns_recovery()) {
        return false;
    }
    return state->launch(
        std::move(lease),
        [state](const Progress &callback, const std::stop_token &stop) {
            return state->run_resume(callback, stop);
        },
        std::move(progress), std::move(completion));
}

bool TrashCoordinator::busy() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

bool TrashCoordinator::stopping() const noexcept {
    return state_->stopped.load(std::memory_order_acquire);
}

bool TrashCoordinator::recovery_pending() const noexcept {
    return state_->refresh_recovery_pending();
}

bool TrashCoordinator::owns_recovery() const noexcept {
    return state_->owns_recovery();
}

void TrashCoordinator::set_maximum_bytes(const std::uintmax_t maximum_bytes) noexcept {
    state_->maximum_bytes.store(maximum_bytes, std::memory_order_release);
}

void TrashCoordinator::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

} // namespace vove::fileops
