#pragma once

#include "remote_protocol.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace vove::platform::windows_detail {

struct WindowsFileIdentity {
    std::string strong_revision;
    std::string smb_revision;
    std::string legacy_revision;
    DWORD error{ERROR_SUCCESS};

    [[nodiscard]] bool has_strong_identity() const noexcept {
        return !strong_revision.empty();
    }

    [[nodiscard]] bool has_remote_identity() const noexcept {
        return !smb_revision.empty();
    }
};

inline std::string hex_file_id(const FILE_ID_128 &file_id) {
    constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(32);
    for (const auto byte : file_id.Identifier) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

inline WindowsFileIdentity query_file_identity(const HANDLE handle,
                                               const LARGE_INTEGER &change_time) {
    WindowsFileIdentity result;
    BY_HANDLE_FILE_INFORMATION legacy{};
    if (GetFileInformationByHandle(handle, &legacy) == FALSE) {
        result.error = GetLastError();
        return result;
    }
    const auto legacy_id = (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32U) |
                           static_cast<std::uint64_t>(legacy.nFileIndexLow);
    if (legacy_id != 0 && change_time.QuadPart > 0) {
        result.legacy_revision = "win-file:" + std::to_string(legacy.dwVolumeSerialNumber) + ':' +
                                 std::to_string(legacy_id) + ':' +
                                 std::to_string(static_cast<std::uint64_t>(change_time.QuadPart));

        FILE_REMOTE_PROTOCOL_INFO remote{};
        if (GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote)) !=
                FALSE &&
            is_smb_remote_protocol(remote)) {
            // SMB exposes FileInternalInformation as a stable 64-bit file-system ID. Keep it in a
            // distinct namespace so it is never mistaken for a local ReFS fallback.
            result.smb_revision = "win-smb64:" + std::to_string(legacy.dwVolumeSerialNumber) + ':' +
                                  std::to_string(legacy_id) + ':' +
                                  std::to_string(static_cast<std::uint64_t>(change_time.QuadPart));
        }
    }

    FILE_ID_INFO strong{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &strong, sizeof(strong)) != FALSE &&
        change_time.QuadPart > 0) {
        result.strong_revision = "win-file128:" + std::to_string(strong.VolumeSerialNumber) + ':' +
                                 hex_file_id(strong.FileId) + ':' +
                                 std::to_string(static_cast<std::uint64_t>(change_time.QuadPart));
        return result;
    }
    result.error = GetLastError();
    return result;
}

inline constexpr bool identity_fallback_is_usable(const DWORD error) noexcept {
    return error == ERROR_SUCCESS || error == ERROR_INVALID_FUNCTION ||
           error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
           error == ERROR_CALL_NOT_IMPLEMENTED;
}

inline std::string preferred_revision(const WindowsFileIdentity &identity) {
    if (identity.has_strong_identity()) {
        return identity.strong_revision;
    }
    if (!identity_fallback_is_usable(identity.error)) {
        return {};
    }
    return identity.has_remote_identity() ? identity.smb_revision : identity.legacy_revision;
}

inline const std::string &matching_revision(const WindowsFileIdentity &identity,
                                            const std::string_view expected) noexcept {
    if (expected.starts_with("win-file128:")) {
        return identity.strong_revision;
    }
    static const std::string empty;
    if (!identity_fallback_is_usable(identity.error)) {
        return empty;
    }
    if (expected.starts_with("win-smb64:")) {
        return identity.smb_revision;
    }
    return identity.legacy_revision;
}

inline constexpr std::uint64_t kMaximumZoneIdentifierBytes = 64U * 1024U;

struct WindowsTransferDataStreams {
    bool has_zone_identifier{};
    std::uint64_t zone_identifier_bytes{};
    bool has_empty_encryptable{};
    DWORD error{ERROR_SUCCESS};

    [[nodiscard]] bool ok() const noexcept {
        return error == ERROR_SUCCESS;
    }
};

inline bool stream_name_equals(const std::wstring_view left,
                               const std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

inline WindowsTransferDataStreams query_supported_transfer_data_streams(const HANDLE handle) {
    constexpr std::size_t initial_bytes = 4U * 1024U;
    constexpr std::size_t maximum_bytes = 1024U * 1024U;
    std::vector<std::byte> buffer(initial_bytes);
    while (GetFileInformationByHandleEx(handle, FileStreamInfo, buffer.data(),
                                        static_cast<DWORD>(buffer.size())) == FALSE) {
        const auto error = GetLastError();
        if ((error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) ||
            buffer.size() >= maximum_bytes) {
            return {.error = error};
        }
        buffer.resize(std::min(buffer.size() * 2U, maximum_bytes));
    }

    WindowsTransferDataStreams result;
    constexpr std::wstring_view unnamed = L"::$DATA";
    constexpr std::wstring_view zone_identifier = L":Zone.Identifier:$DATA";
    constexpr std::wstring_view encryptable = L":encryptable:$DATA";
    std::size_t offset{};
    bool saw_unnamed{};
    while (offset + offsetof(FILE_STREAM_INFO, StreamName) <= buffer.size()) {
        const auto *entry = reinterpret_cast<const FILE_STREAM_INFO *>(buffer.data() + offset);
        const auto name_bytes = static_cast<std::size_t>(entry->StreamNameLength);
        if ((name_bytes % sizeof(wchar_t)) != 0 ||
            name_bytes > buffer.size() - offset - offsetof(FILE_STREAM_INFO, StreamName) ||
            entry->StreamSize.QuadPart < 0) {
            return {.error = ERROR_INVALID_DATA};
        }
        const std::wstring_view name(entry->StreamName, name_bytes / sizeof(wchar_t));
        if (name == unnamed) {
            if (saw_unnamed) {
                return {.error = ERROR_INVALID_DATA};
            }
            saw_unnamed = true;
        } else if (stream_name_equals(name, zone_identifier)) {
            const auto bytes = static_cast<std::uint64_t>(entry->StreamSize.QuadPart);
            if (result.has_zone_identifier || bytes > kMaximumZoneIdentifierBytes) {
                return {.error = ERROR_NOT_SUPPORTED};
            }
            result.has_zone_identifier = true;
            result.zone_identifier_bytes = bytes;
        } else if (stream_name_equals(name, encryptable)) {
            if (result.has_empty_encryptable || entry->StreamSize.QuadPart != 0) {
                return {.error = ERROR_NOT_SUPPORTED};
            }
            result.has_empty_encryptable = true;
        } else {
            return {.error = ERROR_NOT_SUPPORTED};
        }

        if (entry->NextEntryOffset == 0) {
            if (!saw_unnamed) {
                return {.error = ERROR_INVALID_DATA};
            }
            return result;
        }
        if (entry->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName) ||
            entry->NextEntryOffset > buffer.size() - offset) {
            return {.error = ERROR_INVALID_DATA};
        }
        offset += entry->NextEntryOffset;
    }
    return {.error = ERROR_INVALID_DATA};
}

inline bool has_only_supported_transfer_data_streams(const HANDLE handle, DWORD &error) {
    const auto streams = query_supported_transfer_data_streams(handle);
    error = streams.error;
    return streams.ok();
}

} // namespace vove::platform::windows_detail
