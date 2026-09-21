#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wnnc.h>

namespace vove::platform::windows_detail {

inline bool is_smb_remote_protocol(const FILE_REMOTE_PROTOCOL_INFO &remote) noexcept {
    return remote.Protocol == WNNC_NET_SMB;
}

} // namespace vove::platform::windows_detail
