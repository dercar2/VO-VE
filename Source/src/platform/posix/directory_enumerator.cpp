#include "../directory_enumerator.hpp"
#include "../directory_recursive.hpp"

#include "file_identity.hpp"

#include "vove/core/reserved_names.hpp"

#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <memory>
#include <unistd.h>
#if defined(__linux__) && __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#include <sys/stat.h>
#include <system_error>
#include <utility>

namespace vove::platform::detail {

namespace {

catalog::CatalogErrorKind classify_error(const std::error_code &error) {
    const auto condition = error.default_error_condition();
    if (condition == std::errc::no_such_file_or_directory) {
        return catalog::CatalogErrorKind::not_found;
    }
    if (condition == std::errc::permission_denied) {
        return catalog::CatalogErrorKind::permission_denied;
    }
    if (condition == std::errc::timed_out) {
        return catalog::CatalogErrorKind::timed_out;
    }
    if (condition == std::errc::network_down || condition == std::errc::network_unreachable ||
        condition == std::errc::network_reset || condition == std::errc::connection_aborted ||
        condition == std::errc::connection_reset || condition == std::errc::host_unreachable) {
        return catalog::CatalogErrorKind::network_disconnected;
    }
    return catalog::CatalogErrorKind::io_error;
}

catalog::CatalogError make_error(const std::filesystem::path &path, const std::error_code &error) {
    return {.kind = classify_error(error),
            .message_utf8 = path_utf8(path) + ": " + error.message(),
            .platform_code = error.value()};
}

int open_recursive_directory(const int parent, const std::filesystem::path &name,
                             const bool root) {
#if defined(__linux__) && defined(SYS_openat2) && defined(RESOLVE_NO_XDEV)
    struct open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    if (!root) {
        how.resolve |= RESOLVE_BENEATH | RESOLVE_NO_XDEV;
    }
    return static_cast<int>(::syscall(SYS_openat2, parent, name.c_str(), &how, sizeof(how)));
#else
    // st_dev alone cannot detect bind mounts. Fail closed when the kernel cannot enforce this.
    static_cast<void>(parent);
    static_cast<void>(name);
    static_cast<void>(root);
    errno = ENOTSUP;
    return -1;
#endif
}

void walk_recursive(RecursiveTraversal &scan, const int parent,
                    const std::filesystem::path &name, const std::filesystem::path &path,
                    const std::uint32_t depth) {
    if (!scan.enter(depth)) {
        return;
    }
    const auto descriptor = open_recursive_directory(parent, name, depth == 0);
    if (descriptor < 0) {
        scan.skip(make_error(path, std::error_code(errno, std::generic_category())), depth == 0);
        return;
    }
    const auto close_directory = [](DIR *directory) { static_cast<void>(::closedir(directory)); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(::fdopendir(descriptor), close_directory);
    if (!directory) {
        const auto code = errno;
        static_cast<void>(::close(descriptor));
        scan.skip(make_error(path, std::error_code(code, std::generic_category())), depth == 0);
        return;
    }
    while (scan.tick()) {
        errno = 0;
        const auto *record = ::readdir(directory.get());
        if (!record) {
            if (errno != 0) {
                scan.skip(make_error(path, std::error_code(errno, std::generic_category())), depth == 0);
            }
            return;
        }
        const std::filesystem::path child_name(record->d_name);
        if (recursive_name_excluded(child_name)) {
            continue;
        }
        const auto child_path = path / child_name;
        struct stat status{};
        if (::fstatat(descriptor, record->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            scan.skip(make_error(child_path, std::error_code(errno, std::generic_category())));
            continue;
        }
        if (S_ISLNK(status.st_mode)) {
            scan.skip();
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            walk_recursive(scan, descriptor, child_name, child_path, depth + 1);
            continue;
        }
        if (!S_ISREG(status.st_mode)) {
            continue;
        }
        // Check file mounts too; opening O_PATH neither reads contents nor blocks on a FIFO swap.
#if defined(__linux__) && defined(SYS_openat2) && defined(RESOLVE_NO_XDEV)
        struct open_how how{};
        how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS;
        const auto file = static_cast<int>(::syscall(SYS_openat2, descriptor, record->d_name,
                                                    &how, sizeof(how)));
        if (file < 0) {
            scan.skip(make_error(child_path, std::error_code(errno, std::generic_category())));
            continue;
        }
        if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode)) {
            static_cast<void>(::close(file));
            scan.skip();
            continue;
        }
        core::DirectoryEntry entry;
        entry.state = core::EntryState::metadata_ready;
        entry.name_utf8 = path_utf8(child_name);
        entry.path_utf8 = path_utf8(child_path);
        entry.id = stable_entry_id(entry.path_utf8);
        entry.size_bytes = status.st_size >= 0 ? static_cast<std::uint64_t>(status.st_size) : 0;
        entry.modified_unix_ns = posix_identity::timestamp_ns(status.st_mtim);
        entry.source_revision_utf8 = posix_identity::revision_from_descriptor(file, status);
        static_cast<void>(::close(file));
        if (!scan.emit(std::move(entry))) {
            return;
        }
#endif
    }
}

} // namespace

EnumerationResult enumerate_directory(const catalog::CatalogRequest &request,
                                      const EntryCallback &on_entry,
                                      const ProgressCallback &on_progress) {
    if (request.recursive) {
        if (!catalog::valid_recursive_request(request)) {
            return {.error = {.kind = catalog::CatalogErrorKind::io_error,
                              .message_utf8 = "Invalid recursive catalog bounds"},
                    .truncated = true, .root_failed = true};
        }
        RecursiveTraversal scan(request, on_entry, on_progress);
        walk_recursive(scan, AT_FDCWD, request.path, request.path, 0);
        return std::move(scan.result);
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        request.path, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    if (error) {
        return {.error = make_error(request.path, error)};
    }

    EnumerationResult result;
    std::size_t count = 0;
    while (iterator != end) {
        if (request.maximum_entries != 0 && count >= request.maximum_entries) {
            result.truncated = true;
            break;
        }

        const auto source = *iterator;
        if (core::is_internal_filename(source.path().filename().u8string())) {
            iterator.increment(error);
            if (error) {
                result.error = make_error(request.path, error);
                break;
            }
            continue;
        }
        struct stat status{};
        if (::stat(source.path().c_str(), &status) != 0) {
            iterator.increment(error);
            if (error) {
                result.error = make_error(request.path, error);
                break;
            }
            continue;
        }
        core::DirectoryEntry entry;
        const bool is_directory = S_ISDIR(status.st_mode);
        entry.kind = is_directory ? core::EntryKind::directory : core::EntryKind::file;
        entry.state = core::EntryState::metadata_ready;
        entry.name_utf8 = path_utf8(source.path().filename());
        entry.path_utf8 = path_utf8(source.path());
        entry.id = stable_entry_id(entry.path_utf8);

        if (!is_directory && status.st_size >= 0) {
            entry.size_bytes = static_cast<std::uint64_t>(status.st_size);
        }
        entry.modified_unix_ns =
            static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL +
            status.st_mtim.tv_nsec;
        entry.source_revision_utf8 = posix_identity::revision_from_path(source.path(), status, 0);

        if (!on_entry(std::move(entry))) {
            result.error.kind = catalog::CatalogErrorKind::cancelled;
            break;
        }
        ++count;

        iterator.increment(error);
        if (error) {
            result.error = make_error(request.path, error);
            break;
        }
    }
    return result;
}

EntryQueryResult query_entry(const std::filesystem::path &path) {
    if (core::is_internal_filename(path.filename().u8string())) {
        return {.error = {.kind = catalog::CatalogErrorKind::not_found,
                          .message_utf8 = path_utf8(path) + ": object is not available"}};
    }

    struct stat status{};
    if (::stat(path.c_str(), &status) != 0) {
        return {.error = make_error(path, std::error_code(errno, std::generic_category()))};
    }
    const bool is_directory = S_ISDIR(status.st_mode);
    if (!is_directory && !S_ISREG(status.st_mode)) {
        return {.error = {.kind = catalog::CatalogErrorKind::io_error,
                          .message_utf8 = path_utf8(path) + ": unsupported object type"}};
    }

    core::DirectoryEntry entry;
    entry.kind = is_directory ? core::EntryKind::directory : core::EntryKind::file;
    entry.state = core::EntryState::metadata_ready;
    entry.name_utf8 = path_utf8(path.filename());
    entry.path_utf8 = path_utf8(path);
    entry.id = stable_entry_id(entry.path_utf8);
    if (!is_directory && status.st_size >= 0) {
        entry.size_bytes = static_cast<std::uint64_t>(status.st_size);
    }
    entry.modified_unix_ns =
        static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL + status.st_mtim.tv_nsec;
    entry.source_revision_utf8 = posix_identity::revision_from_path(path, status, 0);
    return {.entry = std::move(entry)};
}

} // namespace vove::platform::detail
