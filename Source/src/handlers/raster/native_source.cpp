#include "vove/handlers/raster/native_source.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vove::handlers::raster {
namespace {

[[nodiscard]] constexpr NativeSourceError make_error(const NativeSourceErrorCode code,
                                                     const std::uint32_t system_code = 0) noexcept {
    return {.code = code, .system_code = system_code};
}

#ifdef _WIN32

[[nodiscard]] HANDLE as_windows_handle(const NativeSourceHandle handle) noexcept {
    return static_cast<HANDLE>(handle);
}

[[nodiscard]] NativeSourceError windows_error(const DWORD code) noexcept {
    auto typed_code = code == ERROR_INVALID_HANDLE ? NativeSourceErrorCode::invalid_handle
                                                   : NativeSourceErrorCode::io_error;
    if (code == ERROR_BAD_NETPATH || code == ERROR_BAD_NET_NAME ||
        code == ERROR_NETWORK_UNREACHABLE || code == ERROR_NETNAME_DELETED ||
        code == ERROR_CONNECTION_UNAVAIL || code == ERROR_UNEXP_NET_ERR ||
        code == ERROR_SEM_TIMEOUT) {
        typed_code = NativeSourceErrorCode::disconnected;
    }
    return make_error(typed_code, code);
}

struct WindowsReadResult {
    DWORD bytes_read{};
    NativeSourceError error;
};

[[nodiscard]] WindowsReadResult read_windows_chunk(const HANDLE handle, const std::uint64_t offset,
                                                   std::byte *const destination,
                                                   const DWORD byte_count) noexcept {
    LARGE_INTEGER original{};
    LARGE_INTEGER distance{};
    if (SetFilePointerEx(handle, distance, &original, FILE_CURRENT) == FALSE) {
        return {.bytes_read = 0, .error = windows_error(GetLastError())};
    }
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(handle, target, nullptr, FILE_BEGIN) == FALSE) {
        return {.bytes_read = 0, .error = windows_error(GetLastError())};
    }
    DWORD bytes_read{};
    const auto read_succeeded = ReadFile(handle, destination, byte_count, &bytes_read, nullptr);
    const auto read_error = read_succeeded == FALSE ? GetLastError() : ERROR_SUCCESS;
    if (SetFilePointerEx(handle, original, nullptr, FILE_BEGIN) == FALSE) {
        return {.bytes_read = 0, .error = windows_error(GetLastError())};
    }
    if (read_succeeded == FALSE) {
        return read_error == ERROR_HANDLE_EOF
                   ? WindowsReadResult{}
                   : WindowsReadResult{.bytes_read = 0, .error = windows_error(read_error)};
    }
    return {.bytes_read = bytes_read, .error = {}};
}

[[nodiscard]] bool windows_snapshot(const HANDLE handle, NativeSourceSnapshot &snapshot,
                                    NativeSourceError &error) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle, &information) == FALSE) {
        error = windows_error(GetLastError());
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = make_error(NativeSourceErrorCode::not_regular_file);
        return false;
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        error = windows_error(GetLastError());
        return false;
    }
    snapshot.identity_high = information.dwVolumeSerialNumber;
    snapshot.identity_low =
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) | information.nFileIndexLow;
    snapshot.size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) | information.nFileSizeLow;
    snapshot.modified = basic.LastWriteTime.QuadPart;
    snapshot.changed = basic.ChangeTime.QuadPart;
    return true;
}

#else

[[nodiscard]] NativeSourceError posix_error(const int code) noexcept {
    auto typed_code =
        code == EBADF ? NativeSourceErrorCode::invalid_handle : NativeSourceErrorCode::io_error;
    if (code == ENETDOWN || code == ENETUNREACH || code == ECONNRESET || code == ENOTCONN ||
        code == ETIMEDOUT || code == EHOSTDOWN || code == EHOSTUNREACH) {
        typed_code = NativeSourceErrorCode::disconnected;
    }
    return make_error(typed_code, static_cast<std::uint32_t>(code));
}

[[nodiscard]] bool posix_snapshot(const int descriptor, NativeSourceSnapshot &snapshot,
                                  NativeSourceError &error) noexcept {
    struct stat status{};
    int stat_result{};
    do {
        stat_result = ::fstat(descriptor, &status);
    } while (stat_result < 0 && errno == EINTR);
    if (stat_result < 0) {
        error = posix_error(errno);
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        error = make_error(NativeSourceErrorCode::not_regular_file);
        return false;
    }
    snapshot.identity_high = static_cast<std::uint64_t>(status.st_dev);
    snapshot.identity_low = static_cast<std::uint64_t>(status.st_ino);
    snapshot.size = static_cast<std::uint64_t>(status.st_size);
#if defined(__APPLE__)
    snapshot.modified = static_cast<std::int64_t>(status.st_mtimespec.tv_sec) * 1'000'000'000LL +
                        status.st_mtimespec.tv_nsec;
    snapshot.changed = static_cast<std::int64_t>(status.st_ctimespec.tv_sec) * 1'000'000'000LL +
                       status.st_ctimespec.tv_nsec;
#else
    snapshot.modified =
        static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL + status.st_mtim.tv_nsec;
    snapshot.changed =
        static_cast<std::int64_t>(status.st_ctim.tv_sec) * 1'000'000'000LL + status.st_ctim.tv_nsec;
#endif
    return true;
}

#endif

} // namespace

NativeSource::NativeSource(const State state) noexcept : state_(state) {}

NativeSource::~NativeSource() {
    release();
}

NativeSource::NativeSource(NativeSource &&other) noexcept : state_(other.state_) {
    other.state_.supplied_handle = empty_handle();
    other.state_.read_handle = empty_handle();
    other.state_.size = 0;
}

NativeSource &NativeSource::operator=(NativeSource &&other) noexcept {
    if (this != &other) {
        release();
        state_ = other.state_;
        other.state_.supplied_handle = empty_handle();
        other.state_.read_handle = empty_handle();
        other.state_.size = 0;
    }
    return *this;
}

std::uint64_t NativeSource::size() const noexcept {
    return state_.size;
}

NativeSourceReadResult
NativeSource::read_at(const std::uint64_t offset,
                      const std::span<std::byte> destination) const noexcept {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    const auto requested_size = static_cast<std::uint64_t>(destination.size());
    if (requested_size > std::numeric_limits<std::uint64_t>::max() - offset) {
        return {.error = make_error(NativeSourceErrorCode::offset_overflow)};
    }
    if (offset > state_.size) {
        return {.error = make_error(NativeSourceErrorCode::offset_out_of_range)};
    }

    const auto available_size = state_.size - offset;
    const auto bounded_size = static_cast<std::size_t>(std::min(requested_size, available_size));
    std::size_t total_read{};
#ifdef _WIN32
    const std::scoped_lock lock(read_mutex_);
#endif
    while (total_read < bounded_size) {
        const auto remaining = bounded_size - total_read;
#ifdef _WIN32
        constexpr auto maximum_chunk = std::numeric_limits<DWORD>::max();
        const auto chunk_size = static_cast<DWORD>(std::min<std::size_t>(remaining, maximum_chunk));
        const auto chunk = read_windows_chunk(as_windows_handle(state_.read_handle),
                                              offset + static_cast<std::uint64_t>(total_read),
                                              destination.data() + total_read, chunk_size);
        if (chunk.error) {
            return {.bytes_read = total_read, .error = chunk.error};
        }
        if (chunk.bytes_read == 0) {
            break;
        }
        total_read += static_cast<std::size_t>(chunk.bytes_read);
#else
        const auto maximum_chunk = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        const auto chunk_size = std::min(remaining, maximum_chunk);
        ssize_t chunk_read{};
        do {
            chunk_read = ::pread(state_.read_handle, destination.data() + total_read, chunk_size,
                                 static_cast<off_t>(offset + total_read));
        } while (chunk_read < 0 && errno == EINTR);
        if (chunk_read < 0) {
            return {.bytes_read = total_read, .error = posix_error(errno)};
        }
        if (chunk_read == 0) {
            break;
        }
        total_read += static_cast<std::size_t>(chunk_read);
#endif
    }
    return {.bytes_read = total_read, .error = {}};
}

NativeSourceValidationResult NativeSource::validate_unchanged() const noexcept {
    NativeSourceSnapshot current;
    NativeSourceError error;
#ifdef _WIN32
    const std::scoped_lock lock(read_mutex_);
    if (!windows_snapshot(as_windows_handle(state_.read_handle), current, error)) {
#else
    if (!posix_snapshot(state_.read_handle, current, error)) {
#endif
        return {.unchanged = false, .error = error};
    }
    if (current == state_.snapshot) {
        return {.unchanged = true, .error = {}};
    }
    return {.unchanged = false, .error = make_error(NativeSourceErrorCode::source_changed)};
}

void NativeSource::release() noexcept {
#ifdef _WIN32
    if (state_.read_handle != empty_handle()) {
        static_cast<void>(CloseHandle(as_windows_handle(state_.read_handle)));
    }
#endif
    state_.supplied_handle = empty_handle();
    state_.read_handle = empty_handle();
    state_.size = 0;
}

NativeSourceCreateResult make_native_source(const NativeSourceHandle supplied_handle,
                                            const std::uint64_t maximum_size) noexcept {
#ifdef _WIN32
    const auto handle = as_windows_handle(supplied_handle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return {.source = std::nullopt, .error = windows_error(ERROR_INVALID_HANDLE)};
    }

    SetLastError(ERROR_SUCCESS);
    const auto file_type = GetFileType(handle);
    const auto type_error = GetLastError();
    if (file_type != FILE_TYPE_DISK) {
        if (file_type == FILE_TYPE_UNKNOWN && type_error != ERROR_SUCCESS) {
            return {.source = std::nullopt, .error = windows_error(type_error)};
        }
        return {.source = std::nullopt,
                .error = make_error(NativeSourceErrorCode::not_regular_file)};
    }

    NativeSourceSnapshot snapshot;
    NativeSourceError snapshot_error;
    if (!windows_snapshot(handle, snapshot, snapshot_error)) {
        return {.source = std::nullopt, .error = snapshot_error};
    }
    const auto size = snapshot.size;
    if (size > maximum_size) {
        return {.source = std::nullopt,
                .error = make_error(NativeSourceErrorCode::size_limit_exceeded)};
    }

    HANDLE read_handle{};
    if (DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &read_handle, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == FALSE) {
        return {.source = std::nullopt, .error = windows_error(GetLastError())};
    }

    NativeSourceCreateResult result;
    result.source = NativeSource({.supplied_handle = supplied_handle,
                                  .read_handle = static_cast<NativeSourceHandle>(read_handle),
                                  .size = size,
                                  .snapshot = snapshot});
    return result;
#else
    if (supplied_handle < 0) {
        return {.source = std::nullopt, .error = posix_error(EBADF)};
    }

    NativeSourceSnapshot snapshot;
    NativeSourceError snapshot_error;
    if (!posix_snapshot(supplied_handle, snapshot, snapshot_error)) {
        return {.source = std::nullopt, .error = snapshot_error};
    }
    const auto size = snapshot.size;
    if (size > maximum_size) {
        return {.source = std::nullopt,
                .error = make_error(NativeSourceErrorCode::size_limit_exceeded)};
    }

    NativeSourceCreateResult result;
    result.source = NativeSource({.supplied_handle = supplied_handle,
                                  .read_handle = supplied_handle,
                                  .size = size,
                                  .snapshot = snapshot});
    return result;
#endif
}

} // namespace vove::handlers::raster
