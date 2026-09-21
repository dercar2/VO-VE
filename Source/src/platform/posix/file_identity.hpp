#pragma once

#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <sys/stat.h>

#ifdef __linux__
#include <linux/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

namespace vove::platform::posix_identity {

[[nodiscard]] inline std::int64_t timestamp_ns(const timespec &timestamp) noexcept {
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

namespace detail {

#if defined(__linux__) && defined(SYS_statx)
[[nodiscard]] inline std::optional<std::int64_t>
birth_time_ns(const int directory, const char *path, const int flags,
              const struct stat &expected) noexcept {
    struct statx observed{};
    const auto result =
        ::syscall(SYS_statx, directory, path, flags, STATX_BASIC_STATS | STATX_BTIME, &observed);
    if (result != 0 || (observed.stx_mask & STATX_BTIME) == 0U ||
        static_cast<std::uint64_t>(observed.stx_ino) !=
            static_cast<std::uint64_t>(expected.st_ino) ||
        ::makedev(observed.stx_dev_major, observed.stx_dev_minor) != expected.st_dev ||
        observed.stx_btime.tv_sec < 0) {
        return std::nullopt;
    }
    const timespec birth{.tv_sec = static_cast<time_t>(observed.stx_btime.tv_sec),
                         .tv_nsec = static_cast<long>(observed.stx_btime.tv_nsec)};
    return timestamp_ns(birth);
}
#endif

[[nodiscard]] inline std::string revision(const struct stat &status,
                                          const std::optional<std::int64_t> birth) {
    const auto device = std::to_string(static_cast<std::uint64_t>(status.st_dev));
    const auto inode = std::to_string(static_cast<std::uint64_t>(status.st_ino));
    const auto changed = std::to_string(timestamp_ns(status.st_ctim));
    if (birth) {
        return "posix2:" + device + ':' + inode + ':' + std::to_string(*birth) + ':' + changed;
    }
    return "posix:" + device + ':' + inode + ':' + changed;
}

} // namespace detail

[[nodiscard]] inline std::string revision_from_descriptor(const int descriptor,
                                                          const struct stat &status) {
#if defined(__linux__) && defined(SYS_statx)
    return detail::revision(
        status, detail::birth_time_ns(descriptor, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, status));
#else
    static_cast<void>(descriptor);
    return detail::revision(status, std::nullopt);
#endif
}

[[nodiscard]] inline std::string revision_at(const int directory, const std::filesystem::path &name,
                                             const struct stat &status,
                                             const int flags = AT_SYMLINK_NOFOLLOW) {
#if defined(__linux__) && defined(SYS_statx)
    return detail::revision(status, detail::birth_time_ns(directory, name.c_str(), flags, status));
#else
    static_cast<void>(directory);
    static_cast<void>(name);
    static_cast<void>(flags);
    return detail::revision(status, std::nullopt);
#endif
}

[[nodiscard]] inline std::string revision_from_path(const std::filesystem::path &path,
                                                    const struct stat &status,
                                                    const int flags = AT_SYMLINK_NOFOLLOW) {
#ifdef __linux__
    return revision_at(AT_FDCWD, path, status, flags);
#else
    static_cast<void>(path);
    static_cast<void>(flags);
    return detail::revision(status, std::nullopt);
#endif
}

} // namespace vove::platform::posix_identity
