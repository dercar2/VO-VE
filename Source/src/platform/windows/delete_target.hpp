#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vove::platform::windows_detail {

inline bool is_regular_delete_target(const DWORD attributes) noexcept {
    return (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

} // namespace vove::platform::windows_detail
