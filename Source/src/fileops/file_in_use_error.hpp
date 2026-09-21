#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#endif

namespace vove::fileops::detail {

[[nodiscard]] inline bool file_in_use_error(const std::int64_t code) noexcept {
#ifdef _WIN32
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
#else
    return code == EBUSY || code == ETXTBSY;
#endif
}

} // namespace vove::fileops::detail
