#include "../file_operation_platform.hpp"
#include "../sha256.hpp"

#include "posix/file_identity.hpp"
#include "vove/platform/read_only_source.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#ifdef __linux__
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif

namespace vove::fileops::detail {
namespace {

class ScopedDescriptor {
  public:
    explicit ScopedDescriptor(const int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    ScopedDescriptor(const ScopedDescriptor &) = delete;
    ScopedDescriptor &operator=(const ScopedDescriptor &) = delete;

    ScopedDescriptor(ScopedDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    ScopedDescriptor &operator=(ScopedDescriptor &&other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                static_cast<void>(::close(descriptor_));
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor_ >= 0;
    }

  private:
    int descriptor_{-1};
};

#ifdef VOVE_FILEOP_TEST_HOOKS
std::atomic<BeforeTransferRenameSyscallHook> before_transfer_rename_syscall_hook{};
std::atomic<AfterTransferRenameSyscallHook> after_transfer_rename_syscall_hook{};
std::atomic<BeforeTransferSourceDeleteHook> before_transfer_source_delete_hook{};
std::atomic<BeforeCreateDirectoryCommitHook> before_create_directory_commit_hook{};
std::atomic<AfterCreateDirectoryCommitHook> after_create_directory_commit_hook{};
#endif

[[nodiscard]] bool disconnected_error(const int code) noexcept {
    switch (code) {
    case ENETDOWN:
    case ENETUNREACH:
    case ECONNABORTED:
    case ECONNRESET:
    case EHOSTUNREACH:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
#ifdef ESTALE
    case ESTALE:
#endif
        return true;
    default:
        return false;
    }
}

[[nodiscard]] OperationStatus status_for_errno(const int code) noexcept {
    switch (code) {
    case ENOENT:
    case ENOTDIR:
        return OperationStatus::not_found;
    case EEXIST:
    case ENOTEMPTY:
        return OperationStatus::conflict;
    case EBUSY:
    case ETXTBSY:
        return OperationStatus::file_in_use;
    case EACCES:
    case EPERM:
    case EROFS:
        return OperationStatus::permission_denied;
    case ETIMEDOUT:
        return OperationStatus::timed_out;
    case EXDEV:
        return OperationStatus::cross_device;
    case ENOSYS:
    case EINVAL:
    case ELOOP:
#ifdef EOPNOTSUPP
    case EOPNOTSUPP:
#endif
        return OperationStatus::unsupported;
    default:
        return disconnected_error(code) ? OperationStatus::disconnected : OperationStatus::io_error;
    }
}

[[nodiscard]] bool uncertain_errno(const int code) noexcept {
    return code == EIO || code == ETIMEDOUT || disconnected_error(code);
}

[[nodiscard]] OperationResult failure(const std::uint64_t operation_id, const int code,
                                      std::string detail, const bool mutation_attempted = false) {
    auto status = status_for_errno(code);
    auto evidence = OperationEvidence::no_commit;
    if (mutation_attempted && uncertain_errno(code)) {
        status = OperationStatus::unknown_outcome;
        evidence = OperationEvidence::none;
    }
    if (detail.empty()) {
        detail = std::strerror(code);
    }
    return {.operation_id = operation_id,
            .status = status,
            .evidence = evidence,
            .platform_code = code,
            .confirmed_snapshot = {},
            .detail_utf8 = std::move(detail)};
}

[[nodiscard]] std::int64_t timestamp_ns(const timespec &timestamp) noexcept {
    constexpr auto billion = std::int64_t{1'000'000'000};
    const auto seconds = static_cast<std::int64_t>(timestamp.tv_sec);
    if (seconds > std::numeric_limits<std::int64_t>::max() / billion) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (seconds < std::numeric_limits<std::int64_t>::min() / billion) {
        return std::numeric_limits<std::int64_t>::min();
    }
    const auto base = seconds * billion;
    if (timestamp.tv_nsec > 0 &&
        base > std::numeric_limits<std::int64_t>::max() - timestamp.tv_nsec) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return base + timestamp.tv_nsec;
}

struct DescriptorDigest {
    Sha256Digest digest{};
    std::uint64_t bytes{};
    int error{};

    [[nodiscard]] bool ok() const noexcept {
        return error == 0;
    }
};

[[nodiscard]] DescriptorDigest digest_descriptor(const int descriptor) noexcept {
    constexpr std::size_t buffer_size = 64U * 1024U;
    std::array<std::byte, buffer_size> buffer{};
    Sha256 digest;
    std::uint64_t total{};
    off_t offset{};
    for (;;) {
        const auto count = ::pread(descriptor, buffer.data(), buffer.size(), offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {.error = errno};
        }
        if (count == 0) {
            break;
        }
        const auto bytes = static_cast<std::size_t>(count);
        if (total > std::numeric_limits<std::uint64_t>::max() - bytes ||
            offset > std::numeric_limits<off_t>::max() - count) {
            return {.error = EOVERFLOW};
        }
        digest.update(std::span<const std::byte>(buffer.data(), bytes));
        total += bytes;
        offset += count;
    }
    return {.digest = digest.digest(), .bytes = total};
}

[[nodiscard]] std::string revision(const struct stat &status, const int descriptor) {
    return platform::posix_identity::revision_from_descriptor(descriptor, status);
}

[[nodiscard]] SourceSnapshot snapshot(const struct stat &status, const int descriptor) {
    return {.size_bytes = static_cast<std::uint64_t>(status.st_size),
            .modified_unix_ns = timestamp_ns(status.st_mtim),
            .source_revision_utf8 = revision(status, descriptor)};
}

[[nodiscard]] bool same_directory(const struct stat &left, const struct stat &right) noexcept {
    return S_ISDIR(left.st_mode) && S_ISDIR(right.st_mode) && left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino;
}

[[nodiscard]] bool same_directory_descriptors(const int left, const int right) noexcept {
    struct stat left_status{};
    struct stat right_status{};
    return ::fstat(left, &left_status) == 0 && ::fstat(right, &right_status) == 0 &&
           S_ISDIR(left_status.st_mode) && S_ISDIR(right_status.st_mode) &&
           same_source_revision(revision(left_status, left), revision(right_status, right));
}

enum class ExactNameState : std::uint8_t {
    present,
    absent,
    error,
};

struct ExactNameResult {
    ExactNameState state{ExactNameState::error};
    int error{};
};

[[nodiscard]] ExactNameResult directory_exact_name(const int directory,
                                                   const std::filesystem::path &name) {
    auto flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto scan_descriptor = ::openat(directory, ".", flags);
    if (scan_descriptor < 0) {
        return {.state = ExactNameState::error, .error = errno};
    }
    auto *stream = ::fdopendir(scan_descriptor);
    if (stream == nullptr) {
        const auto code = errno;
        static_cast<void>(::close(scan_descriptor));
        return {.state = ExactNameState::error, .error = code};
    }
    const auto expected = name.native();
    errno = 0;
    while (const auto *entry = ::readdir(stream)) {
        if (expected == entry->d_name) {
            const auto close_result = ::closedir(stream);
            return close_result == 0
                       ? ExactNameResult{.state = ExactNameState::present}
                       : ExactNameResult{.state = ExactNameState::error, .error = errno};
        }
        errno = 0;
    }
    const auto read_error = errno;
    const auto close_result = ::closedir(stream);
    if (read_error != 0 || close_result != 0) {
        return {.state = ExactNameState::error, .error = read_error != 0 ? read_error : errno};
    }
    return {.state = ExactNameState::absent};
}

[[nodiscard]] bool snapshot_matches(const struct stat &status, const int descriptor,
                                    const SourceSnapshot &expected) {
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        return false;
    }
    const auto observed = snapshot(status, descriptor);
    return observed.size_bytes == expected.size_bytes &&
           observed.modified_unix_ns == expected.modified_unix_ns &&
           same_source_revision(observed.source_revision_utf8, expected.source_revision_utf8);
}

[[nodiscard]] ScopedDescriptor open_directory(const std::filesystem::path &path, int &error) {
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::open(path.c_str(), flags);
    error = descriptor < 0 ? errno : 0;
    return ScopedDescriptor(descriptor);
}

[[nodiscard]] ScopedDescriptor
open_directory_without_symlink_chain(const std::filesystem::path &path, int &error) {
    const auto normalized = path.lexically_normal();
    if (!normalized.is_absolute() || normalized.has_relative_path() == false) {
        error = EINVAL;
        return ScopedDescriptor{};
    }
    auto current = open_directory(normalized.root_path(), error);
    if (!current) {
        return ScopedDescriptor{};
    }
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    for (const auto &component : normalized.relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            error = EINVAL;
            return ScopedDescriptor{};
        }
        const auto descriptor = ::openat(current.get(), component.c_str(), flags);
        if (descriptor < 0) {
            error = errno;
            return ScopedDescriptor{};
        }
        current = ScopedDescriptor(descriptor);
    }
    error = 0;
    return current;
}

[[nodiscard]] ScopedDescriptor open_regular_at(const int parent, const std::filesystem::path &name,
                                               struct stat &status, int &error) {
    auto flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(parent, name.c_str(), flags);
    if (descriptor < 0) {
        error = errno;
        return ScopedDescriptor{};
    }
    if (::fstat(descriptor, &status) != 0) {
        error = errno;
        static_cast<void>(::close(descriptor));
        return ScopedDescriptor{};
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        error = EOPNOTSUPP;
        static_cast<void>(::close(descriptor));
        return ScopedDescriptor{};
    }
    error = 0;
    return ScopedDescriptor(descriptor);
}

[[nodiscard]] ScopedDescriptor open_object_at(const int parent, const std::filesystem::path &name,
                                              struct stat &status, int &error,
                                              const OperationObjectKind kind) {
    if (kind == OperationObjectKind::regular_file) {
        return open_regular_at(parent, name, status, error);
    }
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(parent, name.c_str(), flags);
    if (descriptor < 0) {
        error = errno;
        return ScopedDescriptor{};
    }
    if (::fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) {
        error = errno != 0 ? errno : EOPNOTSUPP;
        static_cast<void>(::close(descriptor));
        return ScopedDescriptor{};
    }
    error = 0;
    return ScopedDescriptor(descriptor);
}

[[nodiscard]] SourceSnapshot snapshot_for_object(const struct stat &status, const int descriptor,
                                                 const OperationObjectKind kind) {
    auto result = snapshot(status, descriptor);
    if (kind == OperationObjectKind::directory) {
        result.size_bytes = 0;
    }
    return result;
}

[[nodiscard]] bool snapshot_matches_object(const struct stat &status, const int descriptor,
                                           const SourceSnapshot &expected,
                                           const OperationObjectKind kind) {
    if (kind == OperationObjectKind::regular_file) {
        return snapshot_matches(status, descriptor, expected);
    }
    return S_ISDIR(status.st_mode) && expected.size_bytes == 0 &&
           same_object_identity(revision(status, descriptor), expected.source_revision_utf8);
}

enum class NamedObjectState : std::uint8_t {
    matches,
    mismatch,
    error,
};

struct NamedObjectResult {
    NamedObjectState state{NamedObjectState::error};
    int error{};
};

[[nodiscard]] NamedObjectResult name_still_names_regular(const int parent,
                                                         const std::filesystem::path &name,
                                                         const int pinned_descriptor) {
    struct stat pinned_status{};
    if (::fstat(pinned_descriptor, &pinned_status) != 0) {
        return {.state = NamedObjectState::error, .error = errno};
    }

    struct stat named_status{};
    int error = 0;
    auto named = open_regular_at(parent, name, named_status, error);
    if (!named) {
        switch (error) {
        case ENOENT:
        case ENOTDIR:
        case ELOOP:
        case EOPNOTSUPP:
            return {.state = NamedObjectState::mismatch};
        default:
            return {.state = NamedObjectState::error, .error = error};
        }
    }

    if (!same_source_revision(revision(pinned_status, pinned_descriptor),
                              revision(named_status, named.get()))) {
        return {.state = NamedObjectState::mismatch};
    }
    return {.state = NamedObjectState::matches};
}

[[nodiscard]] NamedObjectResult name_still_names_object(const int parent,
                                                        const std::filesystem::path &name,
                                                        const int pinned_descriptor,
                                                        const OperationObjectKind kind) {
    if (kind == OperationObjectKind::regular_file) {
        return name_still_names_regular(parent, name, pinned_descriptor);
    }
    struct stat pinned_status{};
    if (::fstat(pinned_descriptor, &pinned_status) != 0) {
        return {.state = NamedObjectState::error, .error = errno};
    }
    struct stat named_status{};
    int error{};
    auto named = open_object_at(parent, name, named_status, error, kind);
    if (!named) {
        return error == ENOENT || error == ENOTDIR || error == ELOOP || error == EOPNOTSUPP
                   ? NamedObjectResult{.state = NamedObjectState::mismatch}
                   : NamedObjectResult{.state = NamedObjectState::error, .error = error};
    }
    return named_status.st_dev == pinned_status.st_dev &&
                   named_status.st_ino == pinned_status.st_ino
               ? NamedObjectResult{.state = NamedObjectState::matches}
               : NamedObjectResult{.state = NamedObjectState::mismatch};
}

enum class DirectoryPathState : std::uint8_t {
    matches,
    mismatch,
    error,
};

struct DirectoryPathResult {
    DirectoryPathState state{DirectoryPathState::error};
    int error{};
};

[[nodiscard]] DirectoryPathResult path_still_names_directory(const std::filesystem::path &path,
                                                             const int pinned_descriptor) {
    struct stat pinned{};
    if (::fstat(pinned_descriptor, &pinned) != 0) {
        return {.state = DirectoryPathState::error, .error = errno};
    }
    int error = 0;
    auto observed = open_directory(path, error);
    if (!observed) {
        return {.state = DirectoryPathState::error, .error = error};
    }
    struct stat current{};
    if (::fstat(observed.get(), &current) != 0) {
        return {.state = DirectoryPathState::error, .error = errno};
    }
    if (!S_ISDIR(pinned.st_mode) || !S_ISDIR(current.st_mode) ||
        !same_source_revision(revision(pinned, pinned_descriptor),
                              revision(current, observed.get()))) {
        return {.state = DirectoryPathState::mismatch};
    }
    return {.state = DirectoryPathState::matches};
}

struct DirectoryMatchResult {
    bool matches{};
    int error{};
};

[[nodiscard]] DirectoryMatchResult directory_matches_expected(const int descriptor,
                                                              const std::string &expected) {
    if (expected.empty()) {
        return {.matches = true};
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        return {.error = errno};
    }
    return {.matches = S_ISDIR(status.st_mode) &&
                       same_object_identity(revision(status, descriptor), expected)};
}

[[nodiscard]] bool lexical_ancestor(const std::filesystem::path &ancestor,
                                    const std::filesystem::path &path) {
    const auto normalized_ancestor = ancestor.lexically_normal();
    const auto normalized_path = path.lexically_normal();
    auto left = normalized_ancestor.begin();
    auto right = normalized_path.begin();
    for (; left != normalized_ancestor.end() && right != normalized_path.end(); ++left, ++right) {
        if (*left != *right) {
            return false;
        }
    }
    return left == normalized_ancestor.end();
}

[[nodiscard]] bool transfer_rename_mode(const RenameMode mode) noexcept {
    return mode == RenameMode::transfer_atomic || mode == RenameMode::transfer_stage ||
           mode == RenameMode::transfer_publish || mode == RenameMode::transfer_restore ||
           mode == RenameMode::transfer_publish_replace ||
           mode == RenameMode::transfer_atomic_replace ||
           mode == RenameMode::transfer_overwrite_stage ||
           mode == RenameMode::transfer_overwrite_restore;
}

[[nodiscard]] OperationResult changed_result(const std::uint64_t operation_id, std::string detail) {
    return {.operation_id = operation_id,
            .status = OperationStatus::source_changed,
            .evidence = OperationEvidence::no_commit,
            .confirmed_snapshot = {},
            .detail_utf8 = std::move(detail)};
}

enum class RemoteFilesystemState : std::uint8_t {
    smb,
    other,
    error,
};

struct RemoteFilesystemResult {
    RemoteFilesystemState state{RemoteFilesystemState::error};
    int error{};
};

[[nodiscard]] RemoteFilesystemResult remote_smb_descriptor(const int descriptor) noexcept {
#ifdef __linux__
    struct statfs filesystem{};
    if (::fstatfs(descriptor, &filesystem) != 0) {
        return {.state = RemoteFilesystemState::error, .error = errno};
    }
    constexpr auto cifs_magic = static_cast<decltype(filesystem.f_type)>(0xFF534D42UL);
    constexpr auto smb2_magic = static_cast<decltype(filesystem.f_type)>(0xFE534D42UL);
    return {.state = filesystem.f_type == cifs_magic || filesystem.f_type == smb2_magic
                         ? RemoteFilesystemState::smb
                         : RemoteFilesystemState::other};
#else
    static_cast<void>(descriptor);
    return {.state = RemoteFilesystemState::other};
#endif
}

} // namespace

#ifdef VOVE_FILEOP_TEST_HOOKS
void set_before_transfer_rename_syscall_hook(const BeforeTransferRenameSyscallHook hook) noexcept {
    before_transfer_rename_syscall_hook.store(hook, std::memory_order_release);
}

void set_after_transfer_rename_syscall_hook(const AfterTransferRenameSyscallHook hook) noexcept {
    after_transfer_rename_syscall_hook.store(hook, std::memory_order_release);
}

void set_before_transfer_source_delete_hook(const BeforeTransferSourceDeleteHook hook) noexcept {
    before_transfer_source_delete_hook.store(hook, std::memory_order_release);
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
    if (!lexical_ancestor(request.destination_anchor_path, request.destination.parent_path())) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::invalid_request,
                .evidence = OperationEvidence::no_commit,
                .confirmed_snapshot = {},
                .detail_utf8 = "destination is outside its supplied anchor"};
    }
    const auto observed = platform::query_directory_revision(request.destination_anchor_path);
    if (!observed) {
        return {.operation_id = request.operation_id,
                .status = status_for_errno(static_cast<int>(observed.error.platform_code)),
                .evidence = OperationEvidence::no_commit,
                .platform_code = observed.error.platform_code,
                .confirmed_snapshot = {},
                .detail_utf8 = observed.error.detail};
    }
    if (!same_object_identity(observed.revision_utf8, request.destination_anchor_identity_utf8)) {
        return changed_result(request.operation_id, "destination anchor identity changed");
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::no_commit,
            .confirmed_snapshot = {},
            .detail_utf8 = {}};
}

OperationResult rename_no_replace(const RenameRequest &request) {
#if !defined(__linux__) || !defined(SYS_renameat2)
    return {.operation_id = request.operation_id,
            .status = OperationStatus::unsupported,
            .evidence = OperationEvidence::no_commit,
            .confirmed_snapshot = {},
            .detail_utf8 = "atomic no-replace rename is unavailable on this POSIX platform"};
#else
    auto anchor = verify_rename_destination_anchor(request);
    if (!anchor.ok()) {
        return anchor;
    }

    int error = 0;
    auto source_parent = open_directory(request.source.parent_path(), error);
    if (!source_parent) {
        return failure(request.operation_id, error, "source parent is unavailable");
    }
    auto destination_parent = open_directory(request.destination.parent_path(), error);
    if (!destination_parent) {
        return failure(request.operation_id, error, "destination parent is unavailable");
    }
    const auto trash_mode =
        request.mode == RenameMode::trash_internal || request.mode == RenameMode::trash_restore;
    if (trash_mode && !request.source_parent_identity_utf8.empty() &&
        platform::storage_identity(static_cast<platform::NativeFileObject>(
            destination_parent.get())) != request.source_parent_identity_utf8) {
        return changed_result(request.operation_id, "trash destination storage identity changed");
    }
    if (!trash_mode) {
        const auto source_parent_match =
            directory_matches_expected(source_parent.get(), request.source_parent_identity_utf8);
        if (source_parent_match.error != 0) {
            return failure(request.operation_id, source_parent_match.error,
                           "source parent identity could not be read");
        }
        const auto destination_parent_match = directory_matches_expected(
            destination_parent.get(), request.destination_parent_identity_utf8);
        if (destination_parent_match.error != 0) {
            return failure(request.operation_id, destination_parent_match.error,
                           "destination parent identity could not be read");
        }
        if (!source_parent_match.matches || !destination_parent_match.matches) {
            return changed_result(request.operation_id,
                                  "source or destination parent identity changed");
        }
    }

    struct stat source_status{};
    auto source = open_object_at(source_parent.get(), request.source.filename(), source_status,
                                 error, request.object_kind);
    if (!source) {
        return failure(request.operation_id, error, "source file is unavailable");
    }
    if (trash_mode && !request.source_parent_identity_utf8.empty() &&
        platform::storage_identity(static_cast<platform::NativeFileObject>(source.get())) !=
            request.source_parent_identity_utf8) {
        return changed_result(request.operation_id, "trash storage identity changed");
    }
    const auto stale_ordinary_directory =
        !trash_mode && request.object_kind == OperationObjectKind::directory &&
        (timestamp_ns(source_status.st_mtim) != request.expected_source.modified_unix_ns ||
         !same_source_revision(revision(source_status, source.get()),
                               request.expected_source.source_revision_utf8));
    if (!snapshot_matches_object(source_status, source.get(), request.expected_source,
                                 request.object_kind) || stale_ordinary_directory) {
        return changed_result(request.operation_id, "source identity changed before rename");
    }
    if (request.object_kind == OperationObjectKind::regular_file &&
        (transfer_rename_mode(request.mode) || trash_mode) && source_status.st_nlink != 1) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .source_matches_expected = true,
                .confirmed_snapshot =
                    snapshot_for_object(source_status, source.get(), request.object_kind),
                .detail_utf8 =
                    trash_mode ? "VO-VE Trash does not accept files with multiple hard links"
                               : "file transfer does not accept files with multiple hard links"};
    }
    const auto source_parent_path =
        path_still_names_directory(request.source.parent_path(), source_parent.get());
    if (source_parent_path.state != DirectoryPathState::matches) {
        return source_parent_path.state == DirectoryPathState::mismatch
                   ? changed_result(request.operation_id, "source parent path changed")
                   : failure(request.operation_id, source_parent_path.error,
                             "source parent path is unavailable");
    }
    const auto destination_parent_path =
        path_still_names_directory(request.destination.parent_path(), destination_parent.get());
    if (destination_parent_path.state != DirectoryPathState::matches) {
        return destination_parent_path.state == DirectoryPathState::mismatch
                   ? changed_result(request.operation_id, "destination parent path changed")
                   : failure(request.operation_id, destination_parent_path.error,
                             "destination parent path is unavailable");
    }

    constexpr unsigned int rename_no_replace_flag = 1U;
    const auto replacement = request.mode == RenameMode::transfer_publish_replace ||
                             request.mode == RenameMode::transfer_atomic_replace;
    ScopedDescriptor existing;
    struct stat existing_status{};
    if (replacement) {
        existing = open_regular_at(destination_parent.get(), request.destination.filename(),
                                   existing_status, error);
        if (!existing) {
            return failure(request.operation_id, error, "overwrite destination is unavailable");
        }
        if (existing_status.st_nlink != 1 ||
            !snapshot_matches(existing_status, existing.get(), request.expected_destination) ||
            (existing_status.st_dev == source_status.st_dev &&
             existing_status.st_ino == source_status.st_ino)) {
            return changed_result(request.operation_id,
                                  "authorized overwrite destination changed before publication");
        }

        const auto remote = remote_smb_descriptor(existing.get());
        if (remote.state == RemoteFilesystemState::error) {
            return failure(request.operation_id, remote.error,
                           "overwrite destination filesystem could not be identified");
        }
        if (remote.state == RemoteFilesystemState::smb) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .source_matches_expected = true,
                    .destination_present = true,
                    .confirmed_snapshot = snapshot(existing_status, existing.get()),
                    .detail_utf8 =
                        "direct CIFS replacement requires a journalled destination evacuation"};
        }
    }
    std::optional<DescriptorDigest> overwrite_digest_before;
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (transfer_rename_mode(request.mode)) {
        if (const auto hook = before_transfer_rename_syscall_hook.load(std::memory_order_acquire)) {
            hook();
        }
    }
#endif
    const auto source_name = name_still_names_object(source_parent.get(), request.source.filename(),
                                                     source.get(), request.object_kind);
    if (source_name.state != NamedObjectState::matches) {
        return source_name.state == NamedObjectState::mismatch
                   ? changed_result(request.operation_id,
                                    "source name changed before rename commit")
                   : failure(request.operation_id, source_name.error,
                             "source name could not be confirmed before rename commit");
    }
    if (replacement) {
        const auto destination_name = name_still_names_regular(
            destination_parent.get(), request.destination.filename(), existing.get());
        if (destination_name.state != NamedObjectState::matches) {
            return destination_name.state == NamedObjectState::mismatch
                       ? changed_result(request.operation_id,
                                        "overwrite destination changed before rename commit")
                       : failure(
                             request.operation_id, destination_name.error,
                             "overwrite destination could not be confirmed before rename commit");
        }
    }
    if (request.mode == RenameMode::transfer_overwrite_stage) {
        overwrite_digest_before = digest_descriptor(source.get());
        struct stat hashed_status{};
        if (!overwrite_digest_before->ok()) {
            return failure(request.operation_id, overwrite_digest_before->error,
                           "overwrite destination could not be hashed before evacuation");
        }
        if (::fstat(source.get(), &hashed_status) != 0) {
            return failure(request.operation_id, errno,
                           "overwrite destination could not be confirmed after hashing");
        }
        if (overwrite_digest_before->bytes != request.expected_source.size_bytes ||
            !snapshot_matches(hashed_status, source.get(), request.expected_source)) {
            return changed_result(request.operation_id,
                                  "overwrite destination changed while being hashed");
        }
    }
    auto case_alias_rename = false;
    auto committed =
        ::syscall(SYS_renameat2, source_parent.get(), request.source.filename().c_str(),
                  destination_parent.get(), request.destination.filename().c_str(),
                  replacement ? 0U : rename_no_replace_flag) == 0;
    if (!committed) {
        auto code = errno;
        if (code == EEXIST) {
            struct stat existing_status{};
            // CIFS may expose one case-insensitive object with a different pseudo-inode for each
            // requested spelling. Directory enumeration remains authoritative for the published
            // spelling and prevents this fallback from replacing a distinct entry.
            const auto destination_resolves_to_expected_shape =
                ::fstatat(destination_parent.get(), request.destination.filename().c_str(),
                          &existing_status, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(existing_status.st_mode) &&
                existing_status.st_size == source_status.st_size &&
                timestamp_ns(existing_status.st_mtim) == timestamp_ns(source_status.st_mtim);
            const auto source_exact =
                directory_exact_name(source_parent.get(), request.source.filename());
            const auto destination_exact =
                directory_exact_name(destination_parent.get(), request.destination.filename());
            if (source_exact.state == ExactNameState::error ||
                destination_exact.state == ExactNameState::error) {
                const auto scan_error = source_exact.state == ExactNameState::error
                                            ? source_exact.error
                                            : destination_exact.error;
                return failure(request.operation_id, scan_error,
                               "directory entries could not be verified for case-only rename");
            }
            case_alias_rename =
                request.object_kind == OperationObjectKind::regular_file &&
                destination_resolves_to_expected_shape &&
                same_directory_descriptors(source_parent.get(), destination_parent.get()) &&
                source_exact.state == ExactNameState::present &&
                destination_exact.state == ExactNameState::absent;
            if (case_alias_rename) {
                const auto source_name_before_alias = name_still_names_regular(
                    source_parent.get(), request.source.filename(), source.get());
                if (source_name_before_alias.state != NamedObjectState::matches) {
                    return source_name_before_alias.state == NamedObjectState::mismatch
                               ? changed_result(request.operation_id,
                                                "source name changed before case-only rename")
                               : failure(
                                     request.operation_id, source_name_before_alias.error,
                                     "source name could not be confirmed before case-only rename");
                }
                committed = ::syscall(SYS_renameat2, source_parent.get(),
                                      request.source.filename().c_str(), destination_parent.get(),
                                      request.destination.filename().c_str(), 0U) == 0;
            }
            if (case_alias_rename && !committed) {
                code = errno;
            }
        }
        if (!committed) {
            return failure(request.operation_id, code, {}, true);
        }
    }
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (request.mode == RenameMode::transfer_overwrite_stage) {
        if (const auto hook = after_transfer_rename_syscall_hook.load(std::memory_order_acquire)) {
            hook();
        }
    }
#endif

    struct stat destination_status{};
    auto destination = open_object_at(destination_parent.get(), request.destination.filename(),
                                      destination_status, error, request.object_kind);
    const auto exact_identity = destination && destination_status.st_dev == source_status.st_dev &&
                                destination_status.st_ino == source_status.st_ino;
    auto confirmed_case_alias = false;
    if (destination && case_alias_rename && destination_status.st_size == source_status.st_size &&
        timestamp_ns(destination_status.st_mtim) == timestamp_ns(source_status.st_mtim)) {
        const auto destination_exact =
            directory_exact_name(destination_parent.get(), request.destination.filename());
        const auto source_exact =
            directory_exact_name(source_parent.get(), request.source.filename());
        confirmed_case_alias = destination_exact.state == ExactNameState::present &&
                               source_exact.state == ExactNameState::absent;
    }
    if (!exact_identity && !confirmed_case_alias) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::committed,
                .platform_code = error,
                .source_present = false,
                .destination_present = static_cast<bool>(destination),
                .confirmed_snapshot =
                    destination ? snapshot_for_object(destination_status, destination.get(),
                                                      request.object_kind)
                                : SourceSnapshot{},
                .detail_utf8 = "rename committed but destination identity could not be confirmed"};
    }
    if (overwrite_digest_before) {
        const auto digest_after = digest_descriptor(source.get());
        struct stat hashed_status{};
        const auto status_read = ::fstat(source.get(), &hashed_status) == 0;
        const auto confirmed = snapshot(destination_status, destination.get());
        if (!digest_after.ok() || !status_read) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unknown_outcome,
                    .evidence = OperationEvidence::committed,
                    .source_present = false,
                    .destination_present = true,
                    .destination_matches_source = false,
                    .confirmed_snapshot = confirmed,
                    .detail_utf8 =
                        "overwrite destination was evacuated but its content proof failed"};
        }
        const auto hashed_snapshot = snapshot(hashed_status, source.get());
        const auto unchanged = digest_after.bytes == overwrite_digest_before->bytes &&
                               digest_after.digest == overwrite_digest_before->digest &&
                               hashed_snapshot.size_bytes == digest_after.bytes;
        if (!unchanged) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::source_changed,
                    .evidence = OperationEvidence::committed,
                    .source_present = false,
                    .destination_present = true,
                    .destination_matches_source = false,
                    .confirmed_snapshot = hashed_snapshot,
                    .detail_utf8 = "overwrite destination content changed during evacuation"};
        }
        const auto destination_name = name_still_names_regular(
            destination_parent.get(), request.destination.filename(), source.get());
        if (destination_name.state != NamedObjectState::matches) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unknown_outcome,
                    .evidence = OperationEvidence::committed,
                    .source_present = false,
                    .destination_present = false,
                    .destination_matches_source = false,
                    .confirmed_snapshot = {},
                    .detail_utf8 =
                        "overwrite destination was evacuated but its backup name changed"};
        }
        destination_status = hashed_status;
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .source_present = false,
            .source_matches_expected = false,
            .destination_present = true,
            .destination_matches_source = true,
            .confirmed_snapshot =
                snapshot_for_object(destination_status, destination.get(), request.object_kind),
            .detail_utf8 = {}};
#endif
}

OperationResult permanent_delete_remote(const DeleteRequest &request) {
    int error = 0;
    auto source_parent =
        request.mode == DeleteMode::trash_purge
            ? open_directory_without_symlink_chain(request.source.parent_path(), error)
            : open_directory(request.source.parent_path(), error);
    if (!source_parent) {
        return failure(request.operation_id, error, "source parent is unavailable");
    }
    if (request.mode != DeleteMode::trash_purge) {
        const auto source_parent_match =
            directory_matches_expected(source_parent.get(), request.source_parent_identity_utf8);
        if (source_parent_match.error != 0) {
            return failure(request.operation_id, source_parent_match.error,
                           "source parent identity could not be read");
        }
        if (!source_parent_match.matches) {
            return changed_result(request.operation_id, "source parent identity changed");
        }
    }

    struct stat source_status{};
    auto source = open_object_at(source_parent.get(), request.source.filename(), source_status,
                                 error, request.object_kind);
    if (!source) {
        return failure(request.operation_id, error, "source file is unavailable");
    }
    if (request.mode == DeleteMode::trash_purge && !request.source_parent_identity_utf8.empty() &&
        platform::storage_identity(static_cast<platform::NativeFileObject>(source.get())) !=
            request.source_parent_identity_utf8) {
        return changed_result(request.operation_id, "trash storage identity changed");
    }
    if (!snapshot_matches_object(source_status, source.get(), request.expected_source,
                                 request.object_kind)) {
        return changed_result(request.operation_id, "source identity changed before delete");
    }
    if (request.object_kind == OperationObjectKind::regular_file &&
        request.mode == DeleteMode::trash_purge && source_status.st_nlink != 1) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unsupported,
                .evidence = OperationEvidence::no_commit,
                .source_present = true,
                .source_matches_expected = true,
                .confirmed_snapshot =
                    snapshot_for_object(source_status, source.get(), request.object_kind),
                .detail_utf8 = "VO-VE Trash does not accept files with multiple hard links"};
    }
    if (request.mode == DeleteMode::permanent_remote) {
        const auto remote = remote_smb_descriptor(source.get());
        if (remote.state == RemoteFilesystemState::error) {
            return failure(request.operation_id, remote.error,
                           "source filesystem could not be identified");
        }
        if (remote.state != RemoteFilesystemState::smb) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::unsupported,
                    .evidence = OperationEvidence::no_commit,
                    .source_present = true,
                    .source_matches_expected = true,
                    .confirmed_snapshot = snapshot(source_status, source.get()),
                    .detail_utf8 = "permanent delete is limited to mounted SMB filesystems"};
        }
    }

    ScopedDescriptor guard;
    ScopedDescriptor guard_parent;
    if (request.mode == DeleteMode::transfer_source_commit) {
        guard_parent = open_directory(request.guard_path.parent_path(), error);
        if (!guard_parent) {
            return failure(request.operation_id, error, "publication guard parent is unavailable");
        }
        const auto guard_parent_match =
            directory_matches_expected(guard_parent.get(), request.guard_parent_identity_utf8);
        if (guard_parent_match.error != 0) {
            return failure(request.operation_id, guard_parent_match.error,
                           "publication guard parent identity could not be read");
        }
        if (!guard_parent_match.matches) {
            return changed_result(request.operation_id,
                                  "publication guard parent identity changed");
        }
        struct stat guard_status{};
        guard =
            open_regular_at(guard_parent.get(), request.guard_path.filename(), guard_status, error);
        if (!guard) {
            return failure(request.operation_id, error, "publication guard is unavailable");
        }
        if (!snapshot_matches(guard_status, guard.get(), request.expected_guard)) {
            return changed_result(request.operation_id, "publication guard identity changed");
        }
        const auto guard_parent_path =
            path_still_names_directory(request.guard_path.parent_path(), guard_parent.get());
        if (guard_parent_path.state != DirectoryPathState::matches) {
            return guard_parent_path.state == DirectoryPathState::mismatch
                       ? changed_result(request.operation_id,
                                        "publication guard parent path changed")
                       : failure(request.operation_id, guard_parent_path.error,
                                 "publication guard parent path is unavailable");
        }
    }
    const auto source_parent_path =
        path_still_names_directory(request.source.parent_path(), source_parent.get());
    if (source_parent_path.state != DirectoryPathState::matches) {
        return source_parent_path.state == DirectoryPathState::mismatch
                   ? changed_result(request.operation_id, "source parent path changed")
                   : failure(request.operation_id, source_parent_path.error,
                             "source parent path is unavailable");
    }
    if (request.mode == DeleteMode::transfer_source_commit) {
#ifdef VOVE_FILEOP_TEST_HOOKS
        if (const auto hook = before_transfer_source_delete_hook.load(std::memory_order_acquire)) {
            hook();
        }
#endif
        struct stat guard_status{};
        if (::fstat(guard.get(), &guard_status) != 0) {
            return failure(request.operation_id, errno, "publication guard could not be reread");
        }
        if (!snapshot_matches(guard_status, guard.get(), request.expected_guard)) {
            return changed_result(request.operation_id,
                                  "publication guard changed before source deletion");
        }
        struct stat named_guard_status{};
        auto named_guard = open_regular_at(guard_parent.get(), request.guard_path.filename(),
                                           named_guard_status, error);
        if (!named_guard) {
            return uncertain_errno(error)
                       ? failure(request.operation_id, error,
                                 "publication guard could not be confirmed")
                       : changed_result(request.operation_id,
                                        "publication guard changed before source deletion");
        }
        if (named_guard_status.st_dev != guard_status.st_dev ||
            named_guard_status.st_ino != guard_status.st_ino ||
            !snapshot_matches(named_guard_status, named_guard.get(), request.expected_guard)) {
            return changed_result(request.operation_id,
                                  "publication guard changed before source deletion");
        }
    }
    if (request.mode == DeleteMode::transfer_overwrite_cleanup) {
#ifdef VOVE_FILEOP_TEST_HOOKS
        if (const auto hook = before_transfer_source_delete_hook.load(std::memory_order_acquire)) {
            hook();
        }
#endif
        struct stat protected_status{};
        if (::fstat(source.get(), &protected_status) != 0) {
            return failure(request.operation_id, errno,
                           "overwrite backup could not be reread before cleanup");
        }
        if (!snapshot_matches(protected_status, source.get(), request.expected_source)) {
            return changed_result(request.operation_id, "overwrite backup changed before cleanup");
        }
        source_status = protected_status;
    }
    const auto source_name = name_still_names_object(source_parent.get(), request.source.filename(),
                                                     source.get(), request.object_kind);
    if (source_name.state != NamedObjectState::matches) {
        return source_name.state == NamedObjectState::mismatch
                   ? changed_result(request.operation_id,
                                    "source name changed before delete commit")
                   : failure(request.operation_id, source_name.error,
                             "source name could not be confirmed before delete commit");
    }
    const auto unlink_flags =
        request.object_kind == OperationObjectKind::directory ? AT_REMOVEDIR : 0;
    if (::unlinkat(source_parent.get(), request.source.filename().c_str(), unlink_flags) != 0) {
        const auto code = errno;
        return failure(request.operation_id, code, {}, true);
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .source_present = false,
            .source_matches_expected = false,
            .confirmed_snapshot =
                snapshot_for_object(source_status, source.get(), request.object_kind),
            .detail_utf8 = {}};
}

OperationResult create_directory_relative(const CreateDirectoryRequest &request) {
    int error = 0;
    auto parent = open_directory(request.destination.parent_path(), error);
    if (!parent) {
        return failure(request.operation_id, error, "destination parent is unavailable");
    }
    if (!request.destination_parent_revision_utf8.empty()) {
        const auto parent_match =
            directory_matches_expected(parent.get(), request.destination_parent_revision_utf8);
        if (parent_match.error != 0) {
            return failure(request.operation_id, parent_match.error,
                           "destination parent identity could not be read");
        }
        if (!parent_match.matches) {
            return changed_result(request.operation_id, "destination parent identity changed");
        }
    }
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = before_create_directory_commit_hook.load(std::memory_order_acquire)) {
        hook();
    }
#endif
    const auto destination_parent_path =
        path_still_names_directory(request.destination.parent_path(), parent.get());
    if (destination_parent_path.state != DirectoryPathState::matches) {
        return destination_parent_path.state == DirectoryPathState::mismatch
                   ? changed_result(request.operation_id, "destination parent path changed")
                   : failure(request.operation_id, destination_parent_path.error,
                             "destination parent path is unavailable");
    }
    const mode_t mode = request.mode == CreateDirectoryMode::trash_internal ? 0700U : 0777U;
    if (::mkdirat(parent.get(), request.destination.filename().c_str(), mode) != 0) {
        const auto code = errno;
        return failure(request.operation_id, code, {}, true);
    }

    auto child_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    child_flags |= O_CLOEXEC;
#endif
    auto child = ScopedDescriptor(
        ::openat(parent.get(), request.destination.filename().c_str(), child_flags));
    if (!child) {
        error = errno;
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::committed,
                .platform_code = error,
                .destination_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "directory was created but its identity could not be confirmed"};
    }
#ifdef VOVE_FILEOP_TEST_HOOKS
    if (const auto hook = after_create_directory_commit_hook.load(std::memory_order_acquire)) {
        hook();
    }
#endif
    struct stat child_status{};
    if (::fstat(child.get(), &child_status) != 0 || !S_ISDIR(child_status.st_mode)) {
        error = errno;
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::committed,
                .platform_code = error,
                .destination_present = true,
                .confirmed_snapshot = {},
                .detail_utf8 = "directory was created but confirmation failed"};
    }
    struct stat published_status{};
    errno = 0;
    const auto published = ::fstatat(parent.get(), request.destination.filename().c_str(),
                                     &published_status, AT_SYMLINK_NOFOLLOW) == 0;
    if (!published || !same_directory(child_status, published_status)) {
        error = published ? 0 : errno;
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::committed,
                .platform_code = error,
                .destination_present = published,
                .confirmed_snapshot = {.source_revision_utf8 = revision(child_status, child.get())},
                .detail_utf8 =
                    "directory was created but its requested name changed before confirmation"};
    }
    if (request.mode == CreateDirectoryMode::trash_internal &&
        (child_status.st_uid != ::geteuid() || (child_status.st_mode & 0777U) != 0700U)) {
        if (::unlinkat(parent.get(), request.destination.filename().c_str(), AT_REMOVEDIR) == 0) {
            return {.operation_id = request.operation_id,
                    .status = OperationStatus::permission_denied,
                    .evidence = OperationEvidence::no_commit,
                    .destination_present = false,
                    .confirmed_snapshot = {},
                    .detail_utf8 = "filesystem could not create a private VO-VE Trash container"};
        }
        return {.operation_id = request.operation_id,
                .status = OperationStatus::unknown_outcome,
                .evidence = OperationEvidence::committed,
                .platform_code = errno,
                .destination_present = true,
                .confirmed_snapshot = {.source_revision_utf8 = revision(child_status, child.get())},
                .detail_utf8 = "non-private Trash container was created and could not be retired"};
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .destination_present = true,
            .confirmed_snapshot = {.source_revision_utf8 = revision(child_status, child.get())},
            .detail_utf8 = {}};
}

OperationResult verify_private_empty_directory_relative(const CreateDirectoryRequest &request) {
    int error = 0;
    auto parent = open_directory(request.destination.parent_path(), error);
    if (!parent) {
        return failure(request.operation_id, error, "trash container parent is unavailable");
    }
    const auto parent_path =
        path_still_names_directory(request.destination.parent_path(), parent.get());
    if (parent_path.state != DirectoryPathState::matches) {
        return parent_path.state == DirectoryPathState::mismatch
                   ? changed_result(request.operation_id, "trash container parent path changed")
                   : failure(request.operation_id, parent_path.error,
                             "trash container parent path is unavailable");
    }

    auto child_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    child_flags |= O_CLOEXEC;
#endif
    auto child = ScopedDescriptor(
        ::openat(parent.get(), request.destination.filename().c_str(), child_flags));
    if (!child) {
        return failure(request.operation_id, errno, "trash container is unavailable");
    }
    struct stat child_status{};
    struct stat named_status{};
    if (::fstat(child.get(), &child_status) != 0 ||
        ::fstatat(parent.get(), request.destination.filename().c_str(), &named_status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_directory(child_status, named_status)) {
        return changed_result(request.operation_id, "trash container identity changed");
    }
    if (child_status.st_uid != ::geteuid() || (child_status.st_mode & 0777U) != 0700U) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::permission_denied,
                .evidence = OperationEvidence::conflicting,
                .destination_present = true,
                .confirmed_snapshot = {.source_revision_utf8 = revision(child_status, child.get())},
                .detail_utf8 = "trash container is not private to the current user"};
    }

    const auto enumeration_descriptor = ::dup(child.get());
    if (enumeration_descriptor < 0) {
        return failure(request.operation_id, errno, "trash container could not be inspected");
    }
    auto *directory = ::fdopendir(enumeration_descriptor);
    if (directory == nullptr) {
        const auto code = errno;
        static_cast<void>(::close(enumeration_descriptor));
        return failure(request.operation_id, code, "trash container could not be inspected");
    }
    bool empty = true;
    errno = 0;
    while (const auto *entry = ::readdir(directory)) {
        const std::string_view name(entry->d_name);
        if (name != "." && name != "..") {
            empty = false;
            break;
        }
    }
    const auto enumeration_error = errno;
    static_cast<void>(::closedir(directory));
    if (enumeration_error != 0) {
        return failure(request.operation_id, enumeration_error,
                       "trash container contents could not be inspected");
    }
    if (!empty) {
        return {.operation_id = request.operation_id,
                .status = OperationStatus::conflict,
                .evidence = OperationEvidence::conflicting,
                .destination_present = true,
                .confirmed_snapshot = {.source_revision_utf8 = revision(child_status, child.get())},
                .detail_utf8 = "trash container contains unowned entries"};
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .destination_present = true,
            .confirmed_snapshot = {.source_revision_utf8 = revision(child_status, child.get())},
            .detail_utf8 = {}};
}

OperationResult remove_empty_directory_relative(const CreateDirectoryRequest &request) {
    int error = 0;
    auto parent = open_directory(request.destination.parent_path(), error);
    if (!parent) {
        return failure(request.operation_id, error, "trash container parent is unavailable");
    }
    if (!request.destination_parent_revision_utf8.empty()) {
        const auto parent_match =
            directory_matches_expected(parent.get(), request.destination_parent_revision_utf8);
        if (parent_match.error != 0) {
            return failure(request.operation_id, parent_match.error,
                           "trash container parent identity could not be read");
        }
        if (!parent_match.matches) {
            return changed_result(request.operation_id, "trash container parent identity changed");
        }
    }
    const auto parent_path =
        path_still_names_directory(request.destination.parent_path(), parent.get());
    if (parent_path.state != DirectoryPathState::matches) {
        return parent_path.state == DirectoryPathState::mismatch
                   ? changed_result(request.operation_id, "trash container parent path changed")
                   : failure(request.operation_id, parent_path.error,
                             "trash container parent path is unavailable");
    }

    auto child_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    child_flags |= O_CLOEXEC;
#endif
    auto child = ScopedDescriptor(
        ::openat(parent.get(), request.destination.filename().c_str(), child_flags));
    if (!child) {
        return failure(request.operation_id, errno, "trash container is unavailable");
    }
    struct stat child_status{};
    struct stat named_status{};
    if (::fstat(child.get(), &child_status) != 0 ||
        ::fstatat(parent.get(), request.destination.filename().c_str(), &named_status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_directory(child_status, named_status)) {
        return changed_result(request.operation_id, "trash container identity changed");
    }
    if (::unlinkat(parent.get(), request.destination.filename().c_str(), AT_REMOVEDIR) != 0) {
        return failure(request.operation_id, errno, "trash container could not be retired", true);
    }
    return {.operation_id = request.operation_id,
            .status = OperationStatus::success,
            .evidence = OperationEvidence::committed,
            .destination_present = false,
            .confirmed_snapshot = {},
            .detail_utf8 = {}};
}

} // namespace vove::fileops::detail
