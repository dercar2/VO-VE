#include "../file_transfer_executor.hpp"

#include "../sha256.hpp"
#include "posix/file_identity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __linux__
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

namespace vove::fileops::detail {
namespace {

constexpr std::size_t transferBufferBytes =
    static_cast<std::size_t>(kFileTransferProgressChunkBytes);

class ScopedDescriptor final {
  public:
    explicit ScopedDescriptor(const int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        close();
    }

    ScopedDescriptor(const ScopedDescriptor &) = delete;
    ScopedDescriptor &operator=(const ScopedDescriptor &) = delete;

    ScopedDescriptor(ScopedDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    ScopedDescriptor &operator=(ScopedDescriptor &&other) noexcept {
        if (this != &other) {
            close();
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

    void close() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(std::exchange(descriptor_, -1)));
        }
    }

  private:
    int descriptor_{-1};
};

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

[[nodiscard]] OperationStatus status_from_error(const int code, const bool knownCifs) noexcept {
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
    case ELOOP:
#ifdef EOPNOTSUPP
    case EOPNOTSUPP:
#endif
        return OperationStatus::unsupported;
    default:
        if (disconnected_error(code) || (knownCifs && code == EIO)) {
            return OperationStatus::disconnected;
        }
        return OperationStatus::io_error;
    }
}

[[nodiscard]] bool cifs_timestamp_fallback_allowed(const int descriptor, const int code) noexcept {
#ifdef __linux__
    if (code != EPERM && code != EACCES && code != EINVAL && code != EOPNOTSUPP) {
        return false;
    }
    struct statfs filesystem{};
    return descriptor >= 0 && ::fstatfs(descriptor, &filesystem) == 0 &&
           (filesystem.f_type == CIFS_SUPER_MAGIC || filesystem.f_type == SMB2_SUPER_MAGIC);
#else
    static_cast<void>(descriptor);
    static_cast<void>(code);
    return false;
#endif
}

[[nodiscard]] FileTransferStreamResult failure(const FileTransferStreamRequest &request,
                                               const int code, const OperationEvidence evidence,
                                               std::string detail, const bool knownCifs = false) {
    if (detail.empty()) {
        detail = std::strerror(code);
    }
    FileTransferStreamResult result;
    result.operation_id = request.operation_id;
    result.item_index = request.item_index;
    result.request_token = request.request_token;
    result.status = status_from_error(code, knownCifs);
    result.evidence = evidence;
    result.platform_code = code;
    result.detail_utf8 = std::move(detail);
    return result;
}

[[nodiscard]] FileTransferStreamResult rejected(const FileTransferStreamRequest &request,
                                                const OperationStatus status, std::string detail) {
    FileTransferStreamResult result;
    result.operation_id = request.operation_id;
    result.item_index = request.item_index;
    result.request_token = request.request_token;
    result.status = status;
    result.evidence = OperationEvidence::no_commit;
    result.detail_utf8 = std::move(detail);
    return result;
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

[[nodiscard]] std::string revision(const struct stat &status, const int descriptor) {
    return platform::posix_identity::revision_from_descriptor(descriptor, status);
}

[[nodiscard]] SourceSnapshot snapshot(const struct stat &status, const int descriptor) {
    return {.size_bytes = static_cast<std::uint64_t>(status.st_size),
            .modified_unix_ns = timestamp_ns(status.st_mtim),
            .source_revision_utf8 = revision(status, descriptor)};
}

struct SnapshotQuery {
    SourceSnapshot snapshot;
    struct stat status{};
    int error{};

    [[nodiscard]] bool ok() const noexcept {
        return error == 0 && !snapshot.source_revision_utf8.empty();
    }
};

[[nodiscard]] SnapshotQuery snapshot_for_descriptor(const int descriptor) {
    SnapshotQuery result;
    if (::fstat(descriptor, &result.status) != 0) {
        result.error = errno;
        return result;
    }
    if (!S_ISREG(result.status.st_mode) || result.status.st_size < 0 ||
        result.status.st_nlink != 1) {
        result.error = EOPNOTSUPP;
        return result;
    }
    result.snapshot = snapshot(result.status, descriptor);
    return result;
}

enum class PathState : std::uint8_t {
    matches,
    mismatch,
    error,
};

struct PathResult {
    PathState state{PathState::error};
    int error{};
};

[[nodiscard]] PathResult path_names_descriptor(const std::filesystem::path &path,
                                               const int descriptor, const bool directory) {
    struct stat pinned{};
    if (::fstat(descriptor, &pinned) != 0) {
        return {.state = PathState::error, .error = errno};
    }
    auto flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    if (directory) {
        flags |= O_DIRECTORY;
    }
    ScopedDescriptor observed(::open(path.c_str(), flags));
    if (!observed) {
        return {.state = PathState::error, .error = errno};
    }
    struct stat current{};
    if (::fstat(observed.get(), &current) != 0) {
        return {.state = PathState::error, .error = errno};
    }
    const auto expectedKind = directory ? S_ISDIR(current.st_mode) : S_ISREG(current.st_mode);
    return {.state = expectedKind && same_source_revision(revision(pinned, descriptor),
                                                          revision(current, observed.get()))
                         ? PathState::matches
                         : PathState::mismatch};
}

[[nodiscard]] PathResult name_names_descriptor(const int parent, const std::filesystem::path &name,
                                               const int descriptor) {
    struct stat pinned{};
    if (::fstat(descriptor, &pinned) != 0) {
        return {.state = PathState::error, .error = errno};
    }
    struct stat current{};
    if (::fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
        return {.state = PathState::error, .error = errno};
    }
    return {.state = S_ISREG(current.st_mode) &&
                             same_source_revision(
                                 revision(pinned, descriptor),
                                 platform::posix_identity::revision_at(parent, name, current))
                         ? PathState::matches
                         : PathState::mismatch};
}

[[nodiscard]] bool descriptor_is_cifs(const int descriptor) noexcept {
#ifdef __linux__
    struct statfs filesystem{};
    if (::fstatfs(descriptor, &filesystem) != 0) {
        return false;
    }
    constexpr auto cifsMagic = static_cast<decltype(filesystem.f_type)>(0xFF534D42UL);
    constexpr auto smb2Magic = static_cast<decltype(filesystem.f_type)>(0xFE534D42UL);
    return filesystem.f_type == cifsMagic || filesystem.f_type == smb2Magic;
#else
    static_cast<void>(descriptor);
    return false;
#endif
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

[[nodiscard]] ExactNameResult exact_name(const int directory, const std::filesystem::path &name) {
    auto flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto scanDescriptor = ::openat(directory, ".", flags);
    if (scanDescriptor < 0) {
        return {.state = ExactNameState::error, .error = errno};
    }
    auto *stream = ::fdopendir(scanDescriptor);
    if (stream == nullptr) {
        const auto code = errno;
        static_cast<void>(::close(scanDescriptor));
        return {.state = ExactNameState::error, .error = code};
    }
    const auto expected = name.native();
    errno = 0;
    while (const auto *entry = ::readdir(stream)) {
        if (expected == entry->d_name) {
            static_cast<void>(::closedir(stream));
            return {.state = ExactNameState::present};
        }
        errno = 0;
    }
    const auto code = errno;
    static_cast<void>(::closedir(stream));
    return code == 0 ? ExactNameResult{.state = ExactNameState::absent}
                     : ExactNameResult{.state = ExactNameState::error, .error = code};
}

struct PinnedDirectory {
    ScopedDescriptor descriptor;
    std::filesystem::path path;
    bool cifs{};
};

struct PinnedChainResult {
    std::vector<PinnedDirectory> chain;
    FileTransferStreamResult failure;
};

[[nodiscard]] bool same_path(const std::filesystem::path &left,
                             const std::filesystem::path &right) {
    return left.lexically_normal() == right.lexically_normal();
}

[[nodiscard]] PinnedChainResult pin_directory_chain(const FileTransferStreamRequest &request,
                                                    const std::filesystem::path &leafDirectory,
                                                    const std::string &expectedLeafIdentity,
                                                    const std::string_view purpose,
                                                    const bool requireDestinationAnchor) {
    PinnedChainResult result;
    auto directory = leafDirectory.lexically_normal();
    const auto root = directory.root_path();
    bool anchorSeen = !requireDestinationAnchor;
    if (directory.empty() || root.empty() || same_path(directory, root)) {
        result.failure = rejected(request, OperationStatus::unsupported,
                                  std::string(purpose) + " directory has no pinnable parent");
        return result;
    }
    while (!same_path(directory, root)) {
        auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        ScopedDescriptor descriptor(::open(directory.c_str(), flags));
        if (!descriptor) {
            result.failure = failure(request, errno, OperationEvidence::no_commit,
                                     std::string(purpose) + " directory could not be pinned");
            result.chain.clear();
            return result;
        }
        struct stat status{};
        if (::fstat(descriptor.get(), &status) != 0) {
            result.failure = failure(request, errno, OperationEvidence::no_commit,
                                     std::string(purpose) + " directory identity is unavailable");
            result.chain.clear();
            return result;
        }
        const auto pathState = path_names_descriptor(directory, descriptor.get(), true);
        if (pathState.state != PathState::matches) {
            result.failure =
                pathState.state == PathState::error
                    ? failure(request, pathState.error, OperationEvidence::no_commit,
                              std::string(purpose) + " directory path is unavailable")
                    : rejected(request, OperationStatus::source_changed,
                               std::string(purpose) + " directory path changed while open");
            result.chain.clear();
            return result;
        }
        if (result.chain.empty() &&
            !same_object_identity(revision(status, descriptor.get()), expectedLeafIdentity)) {
            result.failure = rejected(request, OperationStatus::source_changed,
                                      std::string(purpose) + " directory identity changed");
            result.chain.clear();
            return result;
        }
        if (requireDestinationAnchor && same_path(directory, request.destination_anchor_path)) {
            if (!same_object_identity(revision(status, descriptor.get()),
                                      request.destination_anchor_identity_utf8)) {
                result.failure = rejected(request, OperationStatus::source_changed,
                                          "destination staging-root identity changed");
                result.chain.clear();
                return result;
            }
            anchorSeen = true;
        }
        const auto cifs = descriptor_is_cifs(descriptor.get());
        result.chain.push_back(
            {.descriptor = std::move(descriptor), .path = directory, .cifs = cifs});
        const auto parent = directory.parent_path();
        if (parent.empty() || same_path(parent, directory)) {
            result.failure = rejected(request, OperationStatus::unsupported,
                                      std::string(purpose) + " directory chain has no stable root");
            result.chain.clear();
            return result;
        }
        directory = parent;
    }
    if (!anchorSeen) {
        result.failure = rejected(request, OperationStatus::invalid_request,
                                  "destination staging root is not an ancestor of the transfer");
        result.chain.clear();
    }
    return result;
}

[[nodiscard]] PathResult chain_state(const std::vector<PinnedDirectory> &chain) {
    for (const auto &directory : chain) {
        const auto state = path_names_descriptor(directory.path, directory.descriptor.get(), true);
        if (state.state != PathState::matches) {
            return state;
        }
    }
    return {.state = PathState::matches};
}

[[nodiscard]] bool chain_on_cifs(const std::vector<PinnedDirectory> &chain) noexcept {
    return std::ranges::any_of(chain,
                               [](const PinnedDirectory &directory) { return directory.cifs; });
}

[[nodiscard]] bool unsupported_directory_sync_error(const int code) noexcept {
    return code == EINVAL || code == ENOSYS
#ifdef EOPNOTSUPP
           || code == EOPNOTSUPP
#endif
        ;
}

[[nodiscard]] int sync_directory(const int descriptor, const bool knownCifs) noexcept {
    int result{};
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    if (result == 0 || (knownCifs && unsupported_directory_sync_error(errno))) {
        return 0;
    }
    return errno;
}

[[nodiscard]] int write_all(const int descriptor, const std::span<const std::byte> bytes) noexcept {
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return written < 0 ? errno : EIO;
        }
        offset += static_cast<std::size_t>(written);
    }
    return 0;
}

void remove_exact_name(const int parent, const std::filesystem::path &name,
                       const int descriptor) noexcept {
    const auto named = name_names_descriptor(parent, name, descriptor);
    if (named.state == PathState::matches) {
        static_cast<void>(::unlinkat(parent, name.c_str(), 0));
    }
}

class ReservationCleanup final {
  public:
    ReservationCleanup(const int parent, std::filesystem::path name, const int descriptor,
                       const bool armed)
        : parent_(parent), name_(std::move(name)), descriptor_(descriptor), armed_(armed) {}

    ~ReservationCleanup() {
        if (armed_) {
            remove_exact_name(parent_, name_, descriptor_);
        }
    }

    ReservationCleanup(const ReservationCleanup &) = delete;
    ReservationCleanup &operator=(const ReservationCleanup &) = delete;

    void release() noexcept {
        armed_ = false;
    }

  private:
    int parent_{};
    std::filesystem::path name_;
    int descriptor_{};
    bool armed_{};
};

[[nodiscard]] FileTransferStreamResult path_failure(const FileTransferStreamRequest &request,
                                                    const PathResult &path,
                                                    std::string changedDetail,
                                                    std::string errorDetail, const bool knownCifs,
                                                    const OperationEvidence evidence) {
    return path.state == PathState::error
               ? failure(request, path.error, evidence, std::move(errorDetail), knownCifs)
               : rejected(request, OperationStatus::source_changed, std::move(changedDetail));
}

class PosixFileAuditSession final : public FileTransferStreamSession {
  public:
    PosixFileAuditSession(FileTransferStreamRequest request, ScopedDescriptor source,
                          std::vector<PinnedDirectory> sourceChain, SnapshotQuery sourceSnapshot,
                          FileTransferReservation reservation)
        : request_(std::move(request)), source_(std::move(source)),
          source_chain_(std::move(sourceChain)), source_snapshot_(std::move(sourceSnapshot)),
          reservation_(std::move(reservation)), known_cifs_(chain_on_cifs(source_chain_)) {}

    [[nodiscard]] const FileTransferReservation &reservation() const noexcept override {
        return reservation_;
    }

    [[nodiscard]] FileTransferStreamResult stream(const Progress &progress) override {
        if (::lseek(source_.get(), 0, SEEK_SET) < 0) {
            return audit_failure(errno, "published file could not be rewound for audit");
        }
        Sha256 digest;
        std::vector<std::byte> buffer(transferBufferBytes);
        std::uint64_t total{};
        std::uint64_t lastProgress{};
        while (true) {
            const auto read = ::read(source_.get(), buffer.data(), buffer.size());
            if (read < 0 && errno == EINTR) {
                continue;
            }
            if (read < 0) {
                return audit_failure(errno, "published file audit read failed");
            }
            if (read == 0) {
                break;
            }
            const auto count = static_cast<std::size_t>(read);
            digest.update(std::span<const std::byte>(buffer.data(), count));
            total += static_cast<std::uint64_t>(count);
            if (total > request_.expected_source.size_bytes) {
                return audit_rejected("published file grew during audit");
            }
            if (total - lastProgress >= kFileTransferProgressChunkBytes ||
                total == request_.expected_source.size_bytes) {
                const FileTransferProgress update{.operation_id = request_.operation_id,
                                                  .item_index = request_.item_index,
                                                  .request_token = request_.request_token,
                                                  .bytes_written = total};
                if (progress && !progress(update)) {
                    return audit_rejected("published file audit progress channel closed");
                }
                lastProgress = total;
            }
        }

        const auto current = snapshot_for_descriptor(source_.get());
        const auto named = name_names_descriptor(source_chain_.front().descriptor.get(),
                                                 request_.source.filename(), source_.get());
        const auto chain = chain_state(source_chain_);
        if (named.state == PathState::error) {
            return audit_failure(named.error, "published file path could not be confirmed");
        }
        if (chain.state == PathState::error) {
            return audit_failure(chain.error, "published file directory became unavailable");
        }
        if (total != request_.expected_source.size_bytes || !current.ok() ||
            current.snapshot.size_bytes != request_.expected_source.size_bytes ||
            current.snapshot.modified_unix_ns != request_.expected_source.modified_unix_ns ||
            !same_source_revision(current.snapshot.source_revision_utf8,
                                  request_.expected_source.source_revision_utf8) ||
            named.state != PathState::matches || chain.state != PathState::matches) {
            return audit_rejected("published file identity changed during audit");
        }
        return {.operation_id = request_.operation_id,
                .item_index = request_.item_index,
                .request_token = request_.request_token,
                .status = OperationStatus::success,
                .evidence = OperationEvidence::committed,
                .source_snapshot = current.snapshot,
                .temp_snapshot = current.snapshot,
                .content_sha256 = digest.digest(),
                .bytes_written = total,
                .detail_utf8 = {}};
    }

  private:
    [[nodiscard]] FileTransferStreamResult audit_failure(const int code, std::string detail) const {
        auto result =
            failure(request_, code, OperationEvidence::no_commit, std::move(detail), known_cifs_);
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = source_snapshot_.snapshot;
        return result;
    }

    [[nodiscard]] FileTransferStreamResult audit_rejected(std::string detail) const {
        auto result = rejected(request_, OperationStatus::source_changed, std::move(detail));
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = source_snapshot_.snapshot;
        return result;
    }

    FileTransferStreamRequest request_;
    ScopedDescriptor source_;
    std::vector<PinnedDirectory> source_chain_;
    SnapshotQuery source_snapshot_;
    FileTransferReservation reservation_;
    bool known_cifs_{};
};

class PosixFileTransferSession final : public FileTransferStreamSession {
  public:
    PosixFileTransferSession(FileTransferStreamRequest request, ScopedDescriptor source,
                             ScopedDescriptor temp, std::vector<PinnedDirectory> sourceChain,
                             std::vector<PinnedDirectory> destinationChain,
                             SnapshotQuery sourceSnapshot, FileTransferReservation reservation,
                             const bool reservationPending)
        : request_(std::move(request)), source_(std::move(source)), temp_(std::move(temp)),
          source_chain_(std::move(sourceChain)), destination_chain_(std::move(destinationChain)),
          source_snapshot_(std::move(sourceSnapshot)), reservation_(std::move(reservation)),
          reservation_pending_(reservationPending), source_cifs_(chain_on_cifs(source_chain_)),
          known_cifs_(source_cifs_ || chain_on_cifs(destination_chain_)) {}

    ~PosixFileTransferSession() override {
        if (reservation_pending_) {
            remove_exact_name(destination_chain_.front().descriptor.get(),
                              request_.temp_destination.filename(), temp_.get());
        }
    }

    [[nodiscard]] const FileTransferReservation &reservation() const noexcept override {
        return reservation_;
    }

    [[nodiscard]] FileTransferStreamResult stream(const Progress &progress) override {
        reservation_pending_ = false;
        if (::lseek(source_.get(), 0, SEEK_SET) < 0 || ::lseek(temp_.get(), 0, SEEK_SET) < 0 ||
            ::ftruncate(temp_.get(), 0) != 0) {
            return stream_failure(errno, "transfer streams could not be reset");
        }

        Sha256 digest;
        std::vector<std::byte> buffer(transferBufferBytes);
        std::uint64_t total{};
        std::uint64_t lastProgress{};
        while (true) {
            const auto read = ::read(source_.get(), buffer.data(), buffer.size());
            if (read < 0 && errno == EINTR) {
                continue;
            }
            if (read < 0) {
                return stream_failure(errno, "source read failed");
            }
            if (read == 0) {
                break;
            }
            const auto count = static_cast<std::size_t>(read);
            const auto writeError =
                write_all(temp_.get(), std::span<const std::byte>(buffer.data(), count));
            if (writeError != 0) {
                return stream_failure(writeError, "temporary destination write failed");
            }
            digest.update(std::span<const std::byte>(buffer.data(), count));
            total += static_cast<std::uint64_t>(count);
            if (total > request_.expected_source.size_bytes) {
                return rejected_after_ack(OperationStatus::source_changed,
                                          "source grew during transfer");
            }
            if (total - lastProgress >= kFileTransferProgressChunkBytes ||
                total == request_.expected_source.size_bytes) {
                const FileTransferProgress update{.operation_id = request_.operation_id,
                                                  .item_index = request_.item_index,
                                                  .request_token = request_.request_token,
                                                  .bytes_written = total};
                if (progress && !progress(update)) {
                    return rejected_after_ack(OperationStatus::unknown_outcome,
                                              "transfer progress channel closed");
                }
                lastProgress = total;
            }
        }
        if (total != source_snapshot_.snapshot.size_bytes) {
            return rejected_after_ack(source_cifs_ ? OperationStatus::disconnected
                                                   : OperationStatus::source_changed,
                                      "source ended before the expected content was read");
        }

        const auto sourceState = verify_source();
        if (!sourceState.ok()) {
            return sourceState;
        }
        const std::array<timespec, 2> times{timespec{.tv_sec = 0, .tv_nsec = UTIME_OMIT},
                                            source_snapshot_.status.st_mtim};
        bool timestampPreserved = true;
        if (::futimens(temp_.get(), times.data()) != 0) {
            const auto code = errno;
            if (!cifs_timestamp_fallback_allowed(temp_.get(), code)) {
                return stream_failure(code, "temporary destination timestamp could not be set");
            }
            timestampPreserved = false;
        }
        int syncResult{};
        do {
            syncResult = ::fsync(temp_.get());
        } while (syncResult != 0 && errno == EINTR);
        if (syncResult != 0) {
            return stream_failure(errno, "temporary destination flush failed");
        }

        const auto tempSnapshot = snapshot_for_descriptor(temp_.get());
        const auto tempNamed =
            name_names_descriptor(destination_chain_.front().descriptor.get(),
                                  request_.temp_destination.filename(), temp_.get());
        const auto sourceChain = chain_state(source_chain_);
        const auto destinationChain = chain_state(destination_chain_);
        if (tempNamed.state == PathState::error) {
            return stream_failure(tempNamed.error,
                                  "temporary destination path could not be confirmed");
        }
        if (sourceChain.state == PathState::error || destinationChain.state == PathState::error) {
            return stream_failure(sourceChain.state == PathState::error ? sourceChain.error
                                                                        : destinationChain.error,
                                  "transfer directory became unavailable");
        }
        if (!tempSnapshot.ok() ||
            !same_object_identity(reservation_.temp_snapshot.source_revision_utf8,
                                  tempSnapshot.snapshot.source_revision_utf8) ||
            tempSnapshot.snapshot.size_bytes != total || tempNamed.state != PathState::matches ||
            sourceChain.state != PathState::matches ||
            destinationChain.state != PathState::matches) {
            return rejected_after_ack(OperationStatus::source_changed,
                                      "transfer identity changed before content became ready");
        }
        FileTransferStreamResult result;
        result.operation_id = request_.operation_id;
        result.item_index = request_.item_index;
        result.request_token = request_.request_token;
        result.status = OperationStatus::success;
        result.evidence = OperationEvidence::committed;
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = tempSnapshot.snapshot;
        result.content_sha256 = digest.digest();
        result.bytes_written = total;
        if (!timestampPreserved) {
            result.detail_utf8 = "SMB storage did not permit preserving the source timestamp";
        }
        return result;
    }

  private:
    [[nodiscard]] FileTransferStreamResult verify_source() const {
        const auto current = snapshot_for_descriptor(source_.get());
        const auto named = name_names_descriptor(source_chain_.front().descriptor.get(),
                                                 request_.source.filename(), source_.get());
        const auto chain = chain_state(source_chain_);
        if (named.state == PathState::error || chain.state == PathState::error) {
            return stream_failure(named.state == PathState::error ? named.error : chain.error,
                                  "source path could not be confirmed after transfer");
        }
        if (!current.ok() || current.snapshot.size_bytes != request_.expected_source.size_bytes ||
            current.snapshot.modified_unix_ns != request_.expected_source.modified_unix_ns ||
            !same_source_revision(current.snapshot.source_revision_utf8,
                                  request_.expected_source.source_revision_utf8) ||
            named.state != PathState::matches || chain.state != PathState::matches) {
            return rejected_after_ack(OperationStatus::source_changed,
                                      "source identity changed during transfer");
        }
        FileTransferStreamResult result;
        result.operation_id = request_.operation_id;
        result.item_index = request_.item_index;
        result.request_token = request_.request_token;
        result.status = OperationStatus::success;
        result.evidence = OperationEvidence::no_commit;
        return result;
    }

    [[nodiscard]] FileTransferStreamResult stream_failure(const int code,
                                                          std::string detail) const {
        auto result =
            failure(request_, code, OperationEvidence::none, std::move(detail), known_cifs_);
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = reservation_.temp_snapshot;
        return result;
    }

    [[nodiscard]] FileTransferStreamResult rejected_after_ack(const OperationStatus status,
                                                              std::string detail) const {
        auto result = rejected(request_, status, std::move(detail));
        result.evidence = OperationEvidence::none;
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = reservation_.temp_snapshot;
        return result;
    }

    FileTransferStreamRequest request_;
    ScopedDescriptor source_;
    ScopedDescriptor temp_;
    std::vector<PinnedDirectory> source_chain_;
    std::vector<PinnedDirectory> destination_chain_;
    SnapshotQuery source_snapshot_;
    FileTransferReservation reservation_;
    bool reservation_pending_{};
    bool source_cifs_{};
    bool known_cifs_{};
};

} // namespace

FileTransferBeginResult begin_file_transfer_stream(const FileTransferStreamRequest &request) {
    std::string validation;
    if (!valid_file_transfer_stream_request(request, validation)) {
        return {.session = nullptr,
                .failure =
                    rejected(request, OperationStatus::invalid_request, std::move(validation))};
    }

    const auto audit = request.mode == FileTransferStreamMode::audit_existing;
    const auto probe = request.mode == FileTransferStreamMode::probe_reservation;
    auto sourceChain = pin_directory_chain(request, request.source.parent_path(),
                                           request.source_parent_identity_utf8, "source", audit);
    if (sourceChain.chain.empty()) {
        return {.session = nullptr, .failure = std::move(sourceChain.failure)};
    }
    const auto sourceCifs = chain_on_cifs(sourceChain.chain);
    if (sourceCifs) {
        const auto exact =
            exact_name(sourceChain.chain.front().descriptor.get(), request.source.filename());
        if (exact.state != ExactNameState::present) {
            return {.session = nullptr,
                    .failure = exact.state == ExactNameState::error
                                   ? failure(request, exact.error, OperationEvidence::no_commit,
                                             "source name could not be enumerated", true)
                                   : rejected(request, OperationStatus::source_changed,
                                              "source spelling changed before transfer")};
        }
    }
    auto sourceFlags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    sourceFlags |= O_CLOEXEC;
#endif
    ScopedDescriptor source(::openat(sourceChain.chain.front().descriptor.get(),
                                     request.source.filename().c_str(), sourceFlags));
    if (!source) {
        return {.session = nullptr,
                .failure = failure(request, errno, OperationEvidence::no_commit,
                                   "source could not be opened for stable transfer", sourceCifs)};
    }
    const auto sourceSnapshot = snapshot_for_descriptor(source.get());
    const auto namedSource = name_names_descriptor(sourceChain.chain.front().descriptor.get(),
                                                   request.source.filename(), source.get());
    if (!sourceSnapshot.ok()) {
        return {.session = nullptr,
                .failure = failure(request, sourceSnapshot.error, OperationEvidence::no_commit,
                                   "source is not a supported ordinary file", sourceCifs)};
    }
    if (namedSource.state == PathState::error) {
        return {.session = nullptr,
                .failure = failure(request, namedSource.error, OperationEvidence::no_commit,
                                   "source path could not be confirmed", sourceCifs)};
    }
    const auto exactSource =
        sourceSnapshot.snapshot.size_bytes == request.expected_source.size_bytes &&
        sourceSnapshot.snapshot.modified_unix_ns == request.expected_source.modified_unix_ns &&
        same_source_revision(sourceSnapshot.snapshot.source_revision_utf8,
                             request.expected_source.source_revision_utf8);
    const auto refreshedCifsSource =
        !audit && sourceCifs &&
        same_object_after_rename(request.expected_source, sourceSnapshot.snapshot);
    if (namedSource.state != PathState::matches ||
        (!probe && !exactSource && !refreshedCifsSource)) {
        return {.session = nullptr,
                .failure = rejected(request, OperationStatus::source_changed,
                                    "source identity changed before transfer reservation")};
    }
    auto effectiveRequest = request;
    if (probe || refreshedCifsSource) {
        effectiveRequest.expected_source = sourceSnapshot.snapshot;
    }

    if (audit || probe) {
        const auto sourceState = chain_state(sourceChain.chain);
        if (sourceState.state != PathState::matches) {
            return {.session = nullptr,
                    .failure = path_failure(request, sourceState,
                                            "directory chain changed before file audit",
                                            "file-audit directory became unavailable", sourceCifs,
                                            OperationEvidence::no_commit)};
        }
        struct stat parentStatus{};
        if (::fstat(sourceChain.chain.front().descriptor.get(), &parentStatus) != 0) {
            return {.session = nullptr,
                    .failure =
                        failure(request, errno, OperationEvidence::no_commit,
                                "published file parent identity is unavailable", sourceCifs)};
        }
        FileTransferReservation reservation{
            .operation_id = request.operation_id,
            .item_index = request.item_index,
            .request_token = request.request_token,
            .temp_snapshot = sourceSnapshot.snapshot,
            .destination_parent_identity_utf8 = stable_object_identity(
                revision(parentStatus, sourceChain.chain.front().descriptor.get())),
            .destination_parent_revision_utf8 =
                revision(parentStatus, sourceChain.chain.front().descriptor.get()),
        };
        return {.session = std::make_unique<PosixFileAuditSession>(
                    std::move(effectiveRequest), std::move(source), std::move(sourceChain.chain),
                    sourceSnapshot, std::move(reservation)),
                .failure = {}};
    }

    auto destinationChain = pin_directory_chain(
        request, request.temp_destination.parent_path(), request.destination_parent_identity_utf8,
        "destination", !request.destination_anchor_path.empty());
    if (destinationChain.chain.empty()) {
        return {.session = nullptr, .failure = std::move(destinationChain.failure)};
    }
    const auto destinationCifs = chain_on_cifs(destinationChain.chain);
    const auto reserveNew = request.mode == FileTransferStreamMode::reserve_new;
    auto tempFlags = O_RDWR | O_NOFOLLOW;
#ifdef O_CLOEXEC
    tempFlags |= O_CLOEXEC;
#endif
    if (reserveNew) {
        tempFlags |= O_CREAT | O_EXCL;
    }
    ScopedDescriptor temp(::openat(destinationChain.chain.front().descriptor.get(),
                                   request.temp_destination.filename().c_str(), tempFlags, 0666));
    if (!temp) {
        return {.session = nullptr,
                .failure = failure(request, errno, OperationEvidence::no_commit,
                                   "temporary destination could not be reserved", destinationCifs)};
    }
    ReservationCleanup cleanup(destinationChain.chain.front().descriptor.get(),
                               request.temp_destination.filename(), temp.get(), reserveNew);
    if (reserveNew) {
        const auto marker =
            make_file_transfer_reservation_marker({.operation_id = request.operation_id,
                                                   .item_index = request.item_index,
                                                   .request_token = request.request_token});
        const auto markerError = write_all(temp.get(), std::span<const std::byte>(marker));
        int fileSync{};
        do {
            fileSync = markerError == 0 ? ::fsync(temp.get()) : -1;
        } while (markerError == 0 && fileSync != 0 && errno == EINTR);
        const auto syncError = markerError != 0 ? markerError : (fileSync == 0 ? 0 : errno);
        if (syncError != 0) {
            auto result =
                failure(request, syncError, OperationEvidence::none,
                        "temporary reservation marker could not be committed", destinationCifs);
            return {.session = nullptr, .failure = std::move(result)};
        }
        const auto directorySync =
            sync_directory(destinationChain.chain.front().descriptor.get(), destinationCifs);
        if (directorySync != 0) {
            auto result =
                failure(request, directorySync, OperationEvidence::none,
                        "temporary reservation namespace could not be committed", destinationCifs);
            return {.session = nullptr, .failure = std::move(result)};
        }
    }

    const auto tempSnapshot = snapshot_for_descriptor(temp.get());
    const auto namedTemp = name_names_descriptor(destinationChain.chain.front().descriptor.get(),
                                                 request.temp_destination.filename(), temp.get());
    if (!tempSnapshot.ok()) {
        return {.session = nullptr,
                .failure =
                    failure(request, tempSnapshot.error,
                            reserveNew ? OperationEvidence::none : OperationEvidence::no_commit,
                            "temporary destination is not an ordinary file", destinationCifs)};
    }
    if (namedTemp.state == PathState::error) {
        return {.session = nullptr,
                .failure =
                    failure(request, namedTemp.error,
                            reserveNew ? OperationEvidence::none : OperationEvidence::no_commit,
                            "temporary destination path could not be confirmed", destinationCifs)};
    }
    const auto tempMatches =
        namedTemp.state == PathState::matches &&
        ((reserveNew && tempSnapshot.snapshot.size_bytes == kFileTransferReservationMarkerBytes) ||
         (!reserveNew && tempSnapshot.snapshot.size_bytes == request.expected_temp.size_bytes &&
          tempSnapshot.snapshot.modified_unix_ns == request.expected_temp.modified_unix_ns &&
          same_source_revision(tempSnapshot.snapshot.source_revision_utf8,
                               request.expected_temp.source_revision_utf8)));
    if (!tempMatches) {
        auto result = rejected(request, OperationStatus::source_changed,
                               "temporary destination identity is not the reserved object");
        if (reserveNew) {
            result.evidence = OperationEvidence::none;
        }
        return {.session = nullptr, .failure = std::move(result)};
    }
    const auto sourceState = chain_state(sourceChain.chain);
    const auto destinationState = chain_state(destinationChain.chain);
    if (sourceState.state == PathState::error || destinationState.state == PathState::error) {
        const auto path = sourceState.state == PathState::error ? sourceState : destinationState;
        return {.session = nullptr,
                .failure =
                    failure(request, path.error,
                            reserveNew ? OperationEvidence::none : OperationEvidence::no_commit,
                            "transfer directory became unavailable during reservation",
                            sourceCifs || destinationCifs)};
    }
    if (sourceState.state != PathState::matches || destinationState.state != PathState::matches) {
        auto result = rejected(request, OperationStatus::source_changed,
                               "directory chain changed during transfer reservation");
        if (reserveNew) {
            result.evidence = OperationEvidence::none;
        }
        return {.session = nullptr, .failure = std::move(result)};
    }
    struct stat destinationParent{};
    if (::fstat(destinationChain.chain.front().descriptor.get(), &destinationParent) != 0) {
        return {.session = nullptr,
                .failure = failure(
                    request, errno,
                    reserveNew ? OperationEvidence::none : OperationEvidence::no_commit,
                    "destination directory identity changed after reservation", destinationCifs)};
    }
    FileTransferReservation reservation{
        .operation_id = request.operation_id,
        .item_index = request.item_index,
        .request_token = request.request_token,
        .temp_snapshot = tempSnapshot.snapshot,
        .destination_parent_identity_utf8 = stable_object_identity(
            revision(destinationParent, destinationChain.chain.front().descriptor.get())),
        .destination_parent_revision_utf8 =
            revision(destinationParent, destinationChain.chain.front().descriptor.get()),
    };
    auto session = std::make_unique<PosixFileTransferSession>(
        std::move(effectiveRequest), std::move(source), std::move(temp),
        std::move(sourceChain.chain), std::move(destinationChain.chain), sourceSnapshot,
        std::move(reservation), reserveNew);
    cleanup.release();
    return {.session = std::move(session), .failure = {}};
}

} // namespace vove::fileops::detail
