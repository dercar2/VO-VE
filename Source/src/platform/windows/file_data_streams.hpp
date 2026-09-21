#pragma once

#include "file_identity.hpp"

#include <filesystem>

namespace vove::platform::windows_detail {

struct WindowsDataStreamCopyResult {
    DWORD error{ERROR_SUCCESS};
    bool source_changed{};
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return error == ERROR_SUCCESS; }
};

inline WindowsDataStreamCopyResult verify_transfer_stream_identity(
    const HANDLE source, const HANDLE stream, const std::string &label) {
    LARGE_INTEGER identity_only{};
    identity_only.QuadPart = 1;
    const auto pinned = query_file_identity(source, identity_only);
    const auto opened = query_file_identity(stream, identity_only);
    const auto pinned_id = preferred_revision(pinned);
    const auto opened_id = preferred_revision(opened);
    if (pinned_id.empty() || opened_id.empty()) {
        const auto error = pinned_id.empty() ? pinned.error : opened.error;
        return {error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error, false,
                label + " file identity could not be inspected"};
    }
    if (pinned_id != opened_id)
        return {ERROR_INVALID_DATA, true, label + " belongs to a different source file"};
    return {};
}

struct WindowsTransferStreamContents {
    WindowsTransferDataStreams streams;
    std::vector<std::byte> zone_identifier;
    WindowsDataStreamCopyResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

inline WindowsTransferStreamContents read_supported_transfer_data_streams(
    const std::filesystem::path &source, const HANDLE pinned_source) {
    WindowsTransferStreamContents contents;
    contents.streams = query_supported_transfer_data_streams(pinned_source);
    if (!contents.streams.ok()) {
        contents.result = {contents.streams.error, false, "source data streams could not be inspected"};
        return contents;
    }
    for (const bool zone : {true, false}) {
        if (zone ? !contents.streams.has_zone_identifier : !contents.streams.has_empty_encryptable) continue;
        const std::string label = zone ? "Zone.Identifier" : "encryptable";
        const auto path = source.native() + (zone ? L":Zone.Identifier:$DATA" : L":encryptable:$DATA");
        struct Input {
            HANDLE handle{INVALID_HANDLE_VALUE};
            ~Input() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } input;
        input.handle = CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input.handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            contents.result = {error, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
                               label + " could not be opened"};
            return contents;
        }
        contents.result = verify_transfer_stream_identity(pinned_source, input.handle, label);
        if (!contents.ok()) return contents;
        const auto expected = zone ? contents.streams.zone_identifier_bytes : 0;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(input.handle, &size)) {
            contents.result = {GetLastError(), false, label + " size could not be inspected"};
            return contents;
        }
        if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != expected) {
            contents.result = {ERROR_INVALID_DATA, true, label + " changed during transfer"};
            return contents;
        }
        if (zone) contents.zone_identifier.resize(static_cast<std::size_t>(expected));
        std::size_t total{};
        while (total < expected) {
            DWORD read{};
            if (!ReadFile(input.handle, contents.zone_identifier.data() + total,
                          static_cast<DWORD>(expected - total), &read, nullptr)) {
                contents.result = {GetLastError(), false, label + " read failed"};
                return contents;
            }
            if (read == 0) {
                contents.result = {ERROR_INVALID_DATA, true, label + " was truncated during transfer"};
                return contents;
            }
            total += read;
        }
        std::byte extra{};
        DWORD read{};
        if (!ReadFile(input.handle, &extra, 1, &read, nullptr)) {
            contents.result = {GetLastError(), false, label + " final read failed"};
            return contents;
        }
        if (read) {
            contents.result = {ERROR_INVALID_DATA, true, label + " grew during transfer"};
            return contents;
        }
    }
    return contents;
}

inline WindowsDataStreamCopyResult copy_transfer_data_stream(
    const std::filesystem::path &source, const HANDLE pinned_source, const HANDLE destination,
    const std::wstring_view stream_name, const bool source_present,
    const std::uint64_t source_bytes) {
    const std::string label = stream_name == L":encryptable:$DATA" ? "encryptable" : "Zone.Identifier";
    struct Input {
        HANDLE handle{INVALID_HANDLE_VALUE};
        ~Input() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } input;
    if (source_present) {
        const auto path = source.native() + std::wstring(stream_name);
        input.handle = CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input.handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return {error, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
                    label + " could not be opened"};
        }
        const auto identity = verify_transfer_stream_identity(pinned_source, input.handle, label);
        if (!identity.ok()) return identity;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(input.handle, &size))
            return {GetLastError(), false, label + " size could not be inspected"};
        if (size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != source_bytes ||
            source_bytes > kMaximumZoneIdentifierBytes)
            return {ERROR_INVALID_DATA, true, label + " changed during transfer"};
    }

    struct Backup {
        HANDLE handle;
        void *context{};
        ~Backup() { static_cast<void>(close()); }
        bool close() {
            if (!context) return true;
            DWORD ignored{};
            return BackupWrite(handle, nullptr, 0, &ignored, TRUE, FALSE, &context) != FALSE;
        }
        bool write(const void *data, DWORD bytes) {
            DWORD written{};
            if (!BackupWrite(handle, reinterpret_cast<LPBYTE>(const_cast<void *>(data)),
                bytes, &written, FALSE, FALSE, &context)) return false;
            if (written == bytes) return true;
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
    } backup{destination};
    WIN32_STREAM_ID header{};
    header.dwStreamId = BACKUP_ALTERNATE_DATA;
    header.Size.QuadPart = static_cast<LONGLONG>(source_bytes);
    header.dwStreamNameSize = static_cast<DWORD>(stream_name.size() * sizeof(wchar_t));
    if (!backup.write(&header, static_cast<DWORD>(offsetof(WIN32_STREAM_ID, cStreamName))) ||
        !backup.write(stream_name.data(), header.dwStreamNameSize))
        return {GetLastError(), false, label + " stream header could not be written"};

    std::array<std::byte, 4096> buffer{};
    std::uint64_t total{};
    while (input.handle != INVALID_HANDLE_VALUE) {
        DWORD read{};
        if (!ReadFile(input.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return {GetLastError(), false, label + " read failed"};
        if (read == 0) break;
        if (read > source_bytes - total)
            return {ERROR_INVALID_DATA, true, label + " grew during transfer"};
        if (!backup.write(buffer.data(), read))
            return {GetLastError(), false, label + " write failed"};
        total += read;
    }
    if (total != source_bytes)
        return {ERROR_INVALID_DATA, true, label + " changed during transfer"};
    if (!backup.close() || !FlushFileBuffers(destination))
        return {GetLastError(), false, label + " flush failed"};
    return {};
}

// The source handle stays pinned by the caller through metadata copy and revision verification.
inline WindowsDataStreamCopyResult copy_supported_transfer_data_streams(
    const std::filesystem::path &source, const HANDLE pinned_source, const HANDLE destination,
    const WindowsTransferDataStreams &source_streams,
    const WindowsTransferDataStreams &destination_streams) {
    if (!source_streams.ok())
        return {source_streams.error, false, "source data streams could not be inspected"};
    if (!destination_streams.ok())
        return {destination_streams.error, false, "temporary data streams could not be inspected"};
    if (destination_streams.has_empty_encryptable && !source_streams.has_empty_encryptable)
        return {ERROR_INVALID_DATA, true, "temporary encryptable stream is absent from the source"};
    if (!source_streams.has_zone_identifier && !destination_streams.has_zone_identifier &&
        !source_streams.has_empty_encryptable) return {};
    if (source_streams.has_zone_identifier || destination_streams.has_zone_identifier) {
        const auto copied = copy_transfer_data_stream(source, pinned_source, destination, L":Zone.Identifier:$DATA",
            source_streams.has_zone_identifier, source_streams.zone_identifier_bytes);
        if (!copied.ok()) return copied;
    }
    if (source_streams.has_empty_encryptable) {
        const auto copied = copy_transfer_data_stream(source, pinned_source, destination, L":encryptable:$DATA", true, 0);
        if (!copied.ok()) return copied;
    }
    const auto observed = query_supported_transfer_data_streams(destination);
    if (!observed.ok())
        return {observed.error, false, "temporary data streams could not be verified"};
    if ((source_streams.has_zone_identifier && (!observed.has_zone_identifier ||
            observed.zone_identifier_bytes != source_streams.zone_identifier_bytes)) ||
        (!source_streams.has_zone_identifier && observed.has_zone_identifier && observed.zone_identifier_bytes != 0) ||
        source_streams.has_empty_encryptable != observed.has_empty_encryptable)
        return {ERROR_INVALID_DATA, true, "temporary additional data streams could not be verified"};
    return {};
}

} // namespace vove::platform::windows_detail
