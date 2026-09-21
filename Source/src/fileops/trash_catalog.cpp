#include "vove/fileops/trash_catalog.hpp"

#include "vove/fileops/durable_journal.hpp"
#include "vove/core/reserved_names.hpp"

#if defined(VOVE_TRASH_CATALOG_TEST_HOOKS)
#include "trash_catalog_test_hooks.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include "windows/trash_security.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace vove::fileops {
namespace {

#if defined(VOVE_TRASH_CATALOG_TEST_HOOKS)
std::atomic<detail::TrashCatalogAfterRootPinHook> after_root_pin_hook{};
#endif

constexpr std::string_view manifest_extension{".vtrash"};
constexpr std::string_view previous_suffix{".previous"};
constexpr std::string_view temporary_suffix{".temporary"};

bool remove_suffix(std::string &value, const std::string_view suffix) {
    if (!value.ends_with(suffix)) {
        return false;
    }
    value.resize(value.size() - suffix.size());
    return true;
}

std::optional<std::uint64_t> manifest_operation_id(const std::filesystem::path &path) {
    const auto text = path.filename().string();
    if (!text.ends_with(manifest_extension) || text.size() == manifest_extension.size()) {
        return std::nullopt;
    }
    const auto id_text = std::string_view(text).substr(0, text.size() - manifest_extension.size());
    std::uint64_t operation_id{};
    const auto [end, error] =
        std::from_chars(id_text.data(), id_text.data() + id_text.size(), operation_id, 16);
    if (error != std::errc{} || end != id_text.data() + id_text.size() || operation_id == 0) {
        return std::nullopt;
    }
    return operation_id;
}

std::optional<std::filesystem::path>
primary_manifest_path(const std::filesystem::path &entry_path) {
    auto filename = entry_path.filename().string();
    static_cast<void>(remove_suffix(filename, previous_suffix) ||
                      remove_suffix(filename, temporary_suffix));
    auto primary = entry_path.parent_path() / filename;
    if (!manifest_operation_id(primary)) {
        return std::nullopt;
    }
    return primary;
}

std::optional<std::uint64_t> trash_container_operation_id(const std::filesystem::path &path) {
    const auto filename = path.filename().u8string();
    const auto prefix = core::kTrashFilenamePrefix;
    if (!filename.starts_with(prefix) || filename.size() == prefix.size()) {
        return std::nullopt;
    }
    const auto first = reinterpret_cast<const char *>(filename.data() + prefix.size());
    const auto last = reinterpret_cast<const char *>(filename.data() + filename.size());
    std::uint64_t operation_id{};
    const auto [end, error] = std::from_chars(first, last, operation_id, 16);
    if (error != std::errc{} || end != last || operation_id == 0) {
        return std::nullopt;
    }
    return operation_id;
}

#ifdef _WIN32
std::filesystem::path rescue_manifest_path(const std::filesystem::path &container) {
    return container / std::filesystem::path(std::u8string(core::kTrashRescueManifestFilename));
}
#endif

bool is_local_recovery_root(const std::filesystem::path &root) {
#ifdef _WIN32
    std::array<wchar_t, 32'768> volume_root{};
    if (GetVolumePathNameW(root.c_str(), volume_root.data(),
                           static_cast<DWORD>(volume_root.size())) == FALSE) {
        return false;
    }
    return GetDriveTypeW(volume_root.data()) == DRIVE_FIXED;
#else
    auto candidate = root;
    struct stat status{};
    struct statfs filesystem{};
    while (::lstat(candidate.c_str(), &status) != 0) {
        if (errno != ENOENT || candidate.empty() || candidate == candidate.root_path()) {
            return false;
        }
        const auto parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) {
            return false;
        }
        candidate = parent;
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        ::statfs(candidate.c_str(), &filesystem) != 0) {
        return false;
    }
    constexpr std::uint32_t cifs_magic = 0xFF534D42U;
    constexpr std::uint32_t smb2_magic = 0xFE534D42U;
    const auto filesystem_magic = static_cast<std::uint32_t>(filesystem.f_type);
    return filesystem_magic != cifs_magic && filesystem_magic != smb2_magic;
#endif
}

std::filesystem::path storage_root_for(const std::filesystem::path &path) {
#ifdef _WIN32
    std::array<wchar_t, 32'768> volume_root{};
    if (GetVolumePathNameW(path.c_str(), volume_root.data(),
                           static_cast<DWORD>(volume_root.size())) != FALSE) {
        std::array<wchar_t, MAX_PATH + 1U> volume_guid{};
        if (GetVolumeNameForVolumeMountPointW(volume_root.data(), volume_guid.data(),
                                              static_cast<DWORD>(volume_guid.size())) != FALSE) {
            return std::filesystem::path(volume_guid.data());
        }
        return std::filesystem::path(volume_root.data());
    }
#else
    auto candidate = path;
    struct stat candidate_status{};
    while (::stat(candidate.c_str(), &candidate_status) != 0) {
        if (errno != ENOENT || candidate.empty() || candidate == candidate.root_path()) {
            return candidate.empty() ? path : candidate;
        }
        candidate = candidate.parent_path();
    }
    while (!candidate.empty() && candidate != candidate.root_path()) {
        const auto parent = candidate.parent_path();
        struct stat parent_status{};
        if (::stat(parent.c_str(), &parent_status) != 0 ||
            parent_status.st_dev != candidate_status.st_dev) {
            break;
        }
        candidate = parent;
        candidate_status = parent_status;
    }
    return candidate;
#endif
    return path.root_path();
}

#ifndef _WIN32
std::optional<std::uint64_t> revision_device(const std::string_view revision) {
    const auto prefix_size = revision.starts_with("posix2:")  ? std::string_view{"posix2:"}.size()
                             : revision.starts_with("posix:") ? std::string_view{"posix:"}.size()
                                                              : 0U;
    if (prefix_size == 0U) {
        return std::nullopt;
    }
    const auto end = revision.find(':', prefix_size);
    if (end == std::string_view::npos || end == prefix_size) {
        return std::nullopt;
    }
    std::uint64_t device{};
    const auto [parsed, error] =
        std::from_chars(revision.data() + prefix_size, revision.data() + end, device);
    if (error != std::errc{} || parsed != revision.data() + end) {
        return std::nullopt;
    }
    return device;
}
#endif

bool transaction_matches_storage_filter(const TrashTransaction &transaction,
                                        const TrashStorageFilter &filter) {
#ifdef _WIN32
    const auto expected = filter.root.lexically_normal().string();
    return std::ranges::all_of(transaction.items, [&](const auto &item) {
        return storage_root_for(item.stored).lexically_normal().string() == expected;
    });
#else
    if (!filter.storage_identity_utf8.empty()) {
        return std::ranges::all_of(transaction.items, [&](const auto &item) {
            return item.storage_identity_utf8 == filter.storage_identity_utf8;
        });
    }
    const auto expected_device = revision_device(filter.source_revision_utf8);
    return expected_device && std::ranges::all_of(transaction.items, [&](const auto &item) {
               return revision_device(item.current_snapshot.source_revision_utf8) ==
                      expected_device;
           });
#endif
}

bool rescue_manifest_matches(const std::filesystem::path &candidate,
                             const TrashTransaction &transaction,
                             const std::uint64_t operation_id) {
    if (candidate.filename().u8string() != std::u8string(core::kTrashRescueManifestFilename) ||
        trash_container_operation_id(candidate.parent_path()) != operation_id ||
        transaction.items.empty()) {
        return false;
    }
    const auto expected_container = transaction.items.front().stored.parent_path().filename();
    if (candidate.parent_path().filename() != expected_container) {
        return false;
    }
    return std::ranges::all_of(transaction.items, [&expected_container](const auto &item) {
        return item.stored.parent_path().filename() == expected_container;
    });
}

bool trusted_transaction_storage(
    const TrashTransaction &transaction
#ifdef _WIN32
    ,
    const std::wstring &sid,
    std::unordered_map<std::string, vove::fileops::detail::TrashDirectoryLease> &trusted_roots,
    std::unordered_set<std::string> &untrusted_roots
#endif
) {
#ifdef _WIN32
    if (transaction.items.empty()) {
        return false;
    }
    const auto vault = transaction.items.front().stored.parent_path().parent_path();
    if (!std::ranges::all_of(transaction.items, [&vault](const auto &item) {
            return item.stored.parent_path().parent_path() == vault;
        })) {
        return false;
    }
    const auto key = vault.lexically_normal().string();
    if (trusted_roots.contains(key)) {
        return true;
    }
    if (untrusted_roots.contains(key)) {
        return false;
    }
    std::string detail;
    auto lease = vove::fileops::detail::pin_owned_trash_directory(vault, sid, detail);
    if (lease.valid()) {
        trusted_roots.emplace(key, std::move(lease));
        return true;
    }
    untrusted_roots.insert(key);
    return false;
#else
    return trusted_posix_trash_storage(transaction);
#endif
}

} // namespace

TrashCatalogResult read_trash_catalog(const std::filesystem::path &manifest_directory,
                                      const std::span<const std::filesystem::path> recovery_roots,
                                      const std::stop_token &stop,
                                      const std::optional<TrashStorageFilter> &storage_filter) {
    TrashCatalogResult result{.status = TrashCatalogStatus::success,
                              .manifests = {},
                              .corrupt_manifests = 0,
                              .unreadable_recovery_roots = 0,
                              .low_space_roots = 0,
                              .space_check_failures = 0,
                              .total_items = 0,
                              .total_bytes = 0,
                              .oldest_unix_ns = 0,
                              .totals_saturated = false,
                              .storage_usage = {},
                              .error = {}};
    if (!is_local_recovery_root(manifest_directory)) {
        result.status = TrashCatalogStatus::io_error;
        result.error = std::make_error_code(std::errc::operation_not_supported);
        return result;
    }
    std::error_code error;
    const auto central_exists = std::filesystem::exists(manifest_directory, error);
    if (error) {
        result.status = TrashCatalogStatus::io_error;
        result.error = error;
        return result;
    }
    if (central_exists && !std::filesystem::is_directory(manifest_directory, error)) {
        result.status = TrashCatalogStatus::io_error;
        result.error = error ? error : std::make_error_code(std::errc::not_a_directory);
        return result;
    }

    std::unordered_map<std::uint64_t, std::vector<std::filesystem::path>> candidates;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> storage_roots;
    const auto filtered_storage_root =
        storage_filter ? storage_filter->root.lexically_normal().string() : std::string{};
    if (storage_filter) {
        storage_roots.insert(filtered_storage_root);
    }
    std::unordered_set<std::uint64_t> recovery_candidates;
#ifdef _WIN32
    std::wstring current_sid;
    std::string sid_detail;
    if (!vove::fileops::detail::current_user_sid_text(current_sid, sid_detail)) {
        result.status = TrashCatalogStatus::io_error;
        result.error = std::make_error_code(std::errc::permission_denied);
        return result;
    }
    std::unordered_map<std::string, vove::fileops::detail::TrashDirectoryLease> trusted_roots;
    std::unordered_set<std::string> untrusted_roots;
#endif
    const std::filesystem::directory_iterator end;
    if (central_exists) {
        std::filesystem::directory_iterator iterator(manifest_directory, error);
        while (!error && iterator != end) {
            if (stop.stop_requested()) {
                result.status = TrashCatalogStatus::cancelled;
                return result;
            }
            const auto primary = primary_manifest_path(iterator->path());
            if (primary) {
                const auto key = primary->lexically_normal().string();
                if (visited.insert(key).second) {
                    const auto operation_id = manifest_operation_id(*primary);
                    if (operation_id) {
                        candidates[*operation_id].push_back(*primary);
                    }
                }
            }
            iterator.increment(error);
        }
    }
    if (error) {
        result.status = TrashCatalogStatus::io_error;
        result.error = error;
        return result;
    }

    for (const auto &root : recovery_roots) {
        if (stop.stop_requested()) {
            result.status = TrashCatalogStatus::cancelled;
            return result;
        }
        if (!is_local_recovery_root(root)) {
            ++result.unreadable_recovery_roots;
            continue;
        }
#ifndef _WIN32
        static_cast<void>(root);
        ++result.unreadable_recovery_roots;
        continue;
#else
        std::error_code root_error;
        if (!std::filesystem::is_directory(root, root_error)) {
            if (root_error) {
                ++result.unreadable_recovery_roots;
            }
            continue;
        }
        std::string security_detail;
        auto root_lease =
            vove::fileops::detail::pin_owned_trash_directory(root, current_sid, security_detail);
        if (!root_lease.valid()) {
            ++result.unreadable_recovery_roots;
            continue;
        }
        trusted_roots.insert_or_assign(root.lexically_normal().string(), std::move(root_lease));
#if defined(VOVE_TRASH_CATALOG_TEST_HOOKS)
        if (const auto hook = after_root_pin_hook.load(std::memory_order_acquire);
            hook != nullptr) {
            hook(root);
        }
#endif
        std::filesystem::directory_iterator root_iterator(root, root_error);
        while (!root_error && root_iterator != end) {
            if (stop.stop_requested()) {
                result.status = TrashCatalogStatus::cancelled;
                return result;
            }
            const auto operation_id = root_iterator->is_directory(root_error)
                                          ? trash_container_operation_id(root_iterator->path())
                                          : std::nullopt;
            if (operation_id && !root_error) {
                const auto rescue = rescue_manifest_path(root_iterator->path());
                const auto key = rescue.lexically_normal().string();
                if (visited.insert(key).second) {
                    candidates[*operation_id].push_back(rescue);
                    recovery_candidates.insert(*operation_id);
                }
            }
            root_iterator.increment(root_error);
        }
        if (root_error) {
            ++result.unreadable_recovery_roots;
        }
#endif
    }

    for (auto &[operation_id, paths] : candidates) {
        if (stop.stop_requested()) {
            result.status = TrashCatalogStatus::cancelled;
            return result;
        }
        bool recovered{};
        bool filtered_candidate_seen{};
        for (const auto &path : paths) {
            const auto loaded = DurableJournalStore(path).read();
            TrashTransaction transaction;
            std::string detail;
            if (!loaded.ok() || !decode_trash_transaction(loaded.payload, transaction, detail) ||
                transaction.operation_id != operation_id ||
                transaction.phase != TrashPhase::published) {
                continue;
            }
            if (storage_filter) {
                if (!transaction_matches_storage_filter(transaction, *storage_filter)) {
                    continue;
                }
                filtered_candidate_seen = true;
            }
            if (!trusted_transaction_storage(transaction
#ifdef _WIN32
                                             ,
                                             current_sid, trusted_roots, untrusted_roots
#endif
                                             )) {
                continue;
            }
            const auto is_central =
                manifest_operation_id(path) == operation_id &&
                path.parent_path().lexically_normal() == manifest_directory.lexically_normal();
            const auto is_rescue = rescue_manifest_matches(path, transaction, operation_id);
            if (!is_central && !is_rescue) {
                continue;
            }
            result.manifests.push_back(
                {.manifest_path = is_central ? path : trash_rescue_manifest_path(transaction),
                 .transaction = std::move(transaction)});
            const auto &stored = result.manifests.back().transaction;
            if (stored.items.size() >
                std::numeric_limits<std::size_t>::max() - result.total_items) {
                result.total_items = std::numeric_limits<std::size_t>::max();
                result.totals_saturated = true;
            } else {
                result.total_items += stored.items.size();
            }
            for (const auto &item : stored.items) {
                storage_roots.insert(storage_root_for(item.stored).lexically_normal().string());
                if (item.payload_bytes >
                    std::numeric_limits<std::uint64_t>::max() - result.total_bytes) {
                    result.total_bytes = std::numeric_limits<std::uint64_t>::max();
                    result.totals_saturated = true;
                } else {
                    result.total_bytes += item.payload_bytes;
                }
            }
            result.oldest_unix_ns = result.oldest_unix_ns == 0
                                        ? stored.created_unix_ns
                                        : std::min(result.oldest_unix_ns, stored.created_unix_ns);
            recovered = true;
            break;
        }
        if (!recovered && (!storage_filter || filtered_candidate_seen ||
                           recovery_candidates.contains(operation_id))) {
            ++result.corrupt_manifests;
        }
    }
    std::ranges::sort(result.manifests, [](const auto &left, const auto &right) {
        if (left.transaction.created_unix_ns != right.transaction.created_unix_ns) {
            return left.transaction.created_unix_ns > right.transaction.created_unix_ns;
        }
        return left.transaction.operation_id > right.transaction.operation_id;
    });
    std::unordered_map<std::string, std::size_t> storage_indices;
    for (const auto &root : storage_roots) {
        storage_indices.emplace(root, result.storage_usage.size());
        result.storage_usage.push_back({.root = std::filesystem::path(root)});
    }
    for (const auto &manifest : result.manifests) {
        for (const auto &item : manifest.transaction.items) {
            const auto root = storage_root_for(item.stored).lexically_normal().string();
            const auto found = storage_indices.find(root);
            if (found == storage_indices.end()) {
                continue;
            }
            auto &usage = result.storage_usage[found->second];
            if (usage.total_items == std::numeric_limits<std::size_t>::max()) {
                usage.totals_saturated = true;
            } else {
                ++usage.total_items;
            }
            if (item.payload_bytes >
                std::numeric_limits<std::uint64_t>::max() - usage.total_bytes) {
                usage.total_bytes = std::numeric_limits<std::uint64_t>::max();
                usage.totals_saturated = true;
            } else {
                usage.total_bytes += item.payload_bytes;
            }
        }
    }
    for (const auto &root : storage_roots) {
        if (stop.stop_requested()) {
            result.status = TrashCatalogStatus::cancelled;
            return result;
        }
        std::error_code space_error;
        const auto space = std::filesystem::space(std::filesystem::path(root), space_error);
        if (space_error) {
            ++result.space_check_failures;
        } else {
            const auto found = storage_indices.find(root);
            if (found != storage_indices.end()) {
                auto &usage = result.storage_usage[found->second];
                usage.capacity_bytes = space.capacity;
                usage.available_bytes = space.available;
                usage.space_known = true;
            }
            if (space.available < kTrashLowSpaceWarningBytes) {
                ++result.low_space_roots;
            }
        }
    }
    std::ranges::sort(result.storage_usage, [](const auto &left, const auto &right) {
        return left.root.native() < right.root.native();
    });
    return result;
}

std::filesystem::path trash_storage_root_for(const std::filesystem::path &path) {
    return storage_root_for(path);
}

std::uintmax_t effective_trash_limit(const TrashStorageUsage &usage,
                                     const std::uintmax_t absolute_limit) noexcept {
    if (!usage.space_known || usage.capacity_bytes == 0 || absolute_limit == 0) {
        return 0;
    }
    return std::min(usage.capacity_bytes / 10U, absolute_limit);
}

TrashQuotaState classify_trash_quota(const TrashStorageUsage &usage,
                                     const std::uintmax_t absolute_limit,
                                     const std::uintmax_t additional_bytes) noexcept {
    const auto limit = effective_trash_limit(usage, absolute_limit);
    if (limit == 0 || usage.totals_saturated) {
        return TrashQuotaState::unknown;
    }
    const auto occupied =
        additional_bytes > std::numeric_limits<std::uintmax_t>::max() - usage.total_bytes
            ? std::numeric_limits<std::uintmax_t>::max()
            : static_cast<std::uintmax_t>(usage.total_bytes) + additional_bytes;
    // Trash moves are same-filesystem renames. They consume quota but do not duplicate the source
    // payload, so only the existing free-space reserve matters here.
    const auto violates_free_space_reserve = usage.available_bytes <= kTrashLowSpaceWarningBytes;
    if (occupied >= limit || violates_free_space_reserve) {
        return TrashQuotaState::exceeded;
    }
    if (occupied >= limit - limit / 5U) {
        return TrashQuotaState::warning;
    }
    return TrashQuotaState::normal;
}

} // namespace vove::fileops

#if defined(VOVE_TRASH_CATALOG_TEST_HOOKS)
namespace vove::fileops::detail {

void set_trash_catalog_after_root_pin_hook(const TrashCatalogAfterRootPinHook hook) noexcept {
    after_root_pin_hook.store(hook, std::memory_order_release);
}

} // namespace vove::fileops::detail
#endif
