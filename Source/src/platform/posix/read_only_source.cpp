#include "vove/platform/read_only_source.hpp"

#include "file_identity.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace vove::platform {
namespace {

[[nodiscard]] bool is_disconnected_error(const int code) noexcept {
    switch (code) {
    case ENETDOWN:
    case ENETUNREACH:
    case ECONNRESET:
    case ECONNABORTED:
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

[[nodiscard]] SourceOpenErrorKind posix_error_kind(const int code) noexcept {
    SourceOpenErrorKind kind = SourceOpenErrorKind::io_error;
    if (code == ENOENT || code == ENOTDIR) {
        kind = SourceOpenErrorKind::not_found;
    } else if (code == EACCES || code == EPERM) {
        kind = SourceOpenErrorKind::access_denied;
    } else if (code == ETIMEDOUT) {
        kind = SourceOpenErrorKind::timed_out;
    } else if (is_disconnected_error(code)) {
        kind = SourceOpenErrorKind::disconnected;
    }
    return kind;
}

[[nodiscard]] SourceOpenError posix_error(const int code) {
    return {.kind = posix_error_kind(code), .platform_code = code, .detail = std::strerror(code)};
}

[[nodiscard]] std::string hex_encode(const std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

[[nodiscard]] std::string decode_mount_field(const std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index{}; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 3U < value.size() && value[index + 1U] >= '0' &&
            value[index + 1U] <= '7' && value[index + 2U] >= '0' && value[index + 2U] <= '7' &&
            value[index + 3U] >= '0' && value[index + 3U] <= '7') {
            decoded.push_back(static_cast<char>(((value[index + 1U] - '0') << 6U) |
                                                ((value[index + 2U] - '0') << 3U) |
                                                (value[index + 3U] - '0')));
            index += 3U;
            continue;
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

[[nodiscard]] bool path_is_within(const std::filesystem::path &path,
                                  const std::filesystem::path &root) {
    auto path_part = path.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string linux_mount_identity(const std::filesystem::path &path) {
#ifdef __linux__
    auto absolute = path;
    if (!absolute.is_absolute()) {
        absolute = std::filesystem::current_path() / absolute;
    }
    absolute = absolute.lexically_normal();

    std::ifstream mounts("/proc/self/mountinfo");
    std::string line;
    std::size_t best_depth{};
    std::string best_identity;
    while (std::getline(mounts, line)) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) {
            continue;
        }
        std::istringstream left(line.substr(0, separator));
        std::string mount_id;
        std::string parent_id;
        std::string device;
        std::string root;
        std::string mount_point;
        if (!(left >> mount_id >> parent_id >> device >> root >> mount_point)) {
            continue;
        }
        const auto decoded_mount = std::filesystem::path(decode_mount_field(mount_point));
        if (!path_is_within(absolute, decoded_mount)) {
            continue;
        }
        const auto depth =
            static_cast<std::size_t>(std::distance(decoded_mount.begin(), decoded_mount.end()));
        if (depth < best_depth) {
            continue;
        }
        std::istringstream right(line.substr(separator + 3U));
        std::string filesystem;
        std::string source;
        if (!(right >> filesystem >> source)) {
            continue;
        }
        best_depth = depth;
        best_identity = filesystem == "cifs" || filesystem == "smb3"
                            ? "linux-smb:" + hex_encode(root) + ':' + hex_encode(mount_point) +
                                  ':' + hex_encode(source)
                            : std::string{};
    }
    return best_identity;
#else
    static_cast<void>(path);
    return {};
#endif
}

[[nodiscard]] std::string linux_mount_identity(const int descriptor) {
#ifdef __linux__
    std::ifstream descriptor_info("/proc/self/fdinfo/" + std::to_string(descriptor));
    std::string line;
    std::string mount_id;
    while (std::getline(descriptor_info, line)) {
        constexpr std::string_view prefix = "mnt_id:";
        if (line.starts_with(prefix)) {
            mount_id = line.substr(prefix.size());
            const auto first = mount_id.find_first_not_of(" \t");
            mount_id = first == std::string::npos ? std::string{} : mount_id.substr(first);
            break;
        }
    }
    if (mount_id.empty()) {
        return {};
    }

    std::ifstream mounts("/proc/self/mountinfo");
    while (std::getline(mounts, line)) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) {
            continue;
        }
        std::istringstream left(line.substr(0, separator));
        std::string candidate_id;
        std::string parent_id;
        std::string device;
        std::string root;
        std::string mount_point;
        if (!(left >> candidate_id >> parent_id >> device >> root >> mount_point) ||
            candidate_id != mount_id) {
            continue;
        }
        std::istringstream right(line.substr(separator + 3U));
        std::string filesystem;
        std::string source;
        if (!(right >> filesystem >> source) || (filesystem != "cifs" && filesystem != "smb3")) {
            return {};
        }
        return "linux-smb:" + hex_encode(root) + ':' + hex_encode(mount_point) + ':' +
               hex_encode(source);
    }
#else
    static_cast<void>(descriptor);
#endif
    return {};
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
                .platform_code = EBADF};
    }
    struct stat status{};
    if (::fstat(static_cast<int>(object_), &status) != 0) {
        const auto code = errno;
        return {.status = SourceIdentityStatus::unavailable,
                .error_kind = posix_error_kind(code),
                .platform_code = code};
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        return {.status = SourceIdentityStatus::changed};
    }
    const auto modified =
        static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL + status.st_mtim.tv_nsec;
    try {
        const auto unchanged = static_cast<std::uint64_t>(status.st_size) == size_bytes_ &&
                               modified == modified_unix_ns_ &&
                               posix_identity::revision_from_descriptor(
                                   static_cast<int>(object_), status) == source_revision_utf8_;
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
        static_cast<void>(::close(static_cast<int>(object_)));
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
    auto flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        result.error = posix_error(errno);
        return result;
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        const auto code = errno;
        static_cast<void>(::close(descriptor));
        result.error = posix_error(code);
        return result;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        static_cast<void>(::close(descriptor));
        result.error = {.kind = SourceOpenErrorKind::not_regular_file,
                        .platform_code = 0,
                        .detail = "source is not a regular file"};
        return result;
    }
    const auto size = static_cast<std::uint64_t>(status.st_size);
    if (size > maximum_bytes) {
        static_cast<void>(::close(descriptor));
        result.error = {.kind = SourceOpenErrorKind::too_large,
                        .platform_code = 0,
                        .detail = "source exceeds the configured byte limit"};
        return result;
    }
    const auto modified =
        static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL + status.st_mtim.tv_nsec;
    result.source = ReadOnlySource(ReadOnlySource::Snapshot{
        .object = static_cast<NativeFileObject>(descriptor),
        .size_bytes = size,
        .modified_unix_ns = modified,
        .source_revision_utf8 = posix_identity::revision_from_descriptor(descriptor, status)});
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
    auto flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        const auto code = errno;
        result.error = code == ENOTDIR
                           ? SourceOpenError{.kind = SourceOpenErrorKind::not_regular_file,
                                             .platform_code = code,
                                             .detail = "path is not a direct directory"}
                           : posix_error(code);
        return result;
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        const auto code = errno;
        static_cast<void>(::close(descriptor));
        result.error = posix_error(code);
        return result;
    }
    if (!S_ISDIR(status.st_mode)) {
        static_cast<void>(::close(descriptor));
        result.error = {.kind = SourceOpenErrorKind::not_regular_file,
                        .platform_code = 0,
                        .detail = "path is not a direct directory"};
        return result;
    }
    result.revision_utf8 = posix_identity::revision_from_descriptor(descriptor, status);
    static_cast<void>(::close(descriptor));
    return result;
}

std::string storage_identity(const NativeFileObject object) noexcept {
    try {
        return object == kInvalidNativeFileObject ? std::string{}
                                                  : linux_mount_identity(static_cast<int>(object));
    } catch (...) {
        return {};
    }
}

std::string storage_identity(const std::filesystem::path &path) noexcept {
    try {
        return path.empty() ? std::string{} : linux_mount_identity(path);
    } catch (...) {
        return {};
    }
}

} // namespace vove::platform
