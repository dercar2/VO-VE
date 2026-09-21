#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

#include "filename_component_limit.hpp"

namespace vove::fileops::detail {

FilenameComponentLimitResult
query_filename_component_limit(const std::filesystem::path &directory) noexcept {
#ifdef _WIN32
    const auto handle =
        CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {.error = {static_cast<int>(GetLastError()), std::system_category()}};
    }
    DWORD maximum{};
    const auto succeeded =
        GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, &maximum, nullptr, nullptr, 0);
    const auto code = succeeded == FALSE ? GetLastError() : ERROR_SUCCESS;
    static_cast<void>(CloseHandle(handle));
    if (succeeded == FALSE) {
        return {.error = {static_cast<int>(code), std::system_category()}};
    }
    if (maximum == 0) {
        return {.error = {static_cast<int>(ERROR_INVALID_DATA), std::system_category()}};
    }
    return {.maximum_units = static_cast<std::size_t>(maximum), .error = {}};
#else
    errno = 0;
    const auto maximum = ::pathconf(directory.c_str(), _PC_NAME_MAX);
    if (maximum <= 0) {
        const auto code = errno == 0 ? EINVAL : errno;
        return {.error = {code, std::generic_category()}};
    }
    return {.maximum_units = static_cast<std::size_t>(maximum), .error = {}};
#endif
}

std::size_t filename_component_units(const std::filesystem::path &filename) noexcept {
    return filename.native().size();
}

} // namespace vove::fileops::detail
