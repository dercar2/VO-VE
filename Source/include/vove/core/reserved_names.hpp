#pragma once

#include <cstddef>
#include <string_view>

namespace vove::core {

template <typename Character>
[[nodiscard]] inline bool ascii_iequal(const std::basic_string_view<Character> left,
                                       const std::basic_string_view<Character> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    const auto folded = [](const Character value) noexcept {
        constexpr auto upper_a = static_cast<Character>('A');
        constexpr auto upper_z = static_cast<Character>('Z');
        constexpr auto lower_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + lower_delta)
                                                    : value;
    };
    for (std::size_t index{}; index < left.size(); ++index) {
        if (folded(left[index]) != folded(right[index])) {
            return false;
        }
    }
    return true;
}

template <typename Character>
[[nodiscard]] inline bool
ascii_istarts_with(const std::basic_string_view<Character> value,
                   const std::basic_string_view<Character> prefix) noexcept {
    return value.size() >= prefix.size() && ascii_iequal(value.substr(0U, prefix.size()), prefix);
}

inline constexpr std::u8string_view kTrashFilenamePrefix = u8".vove-trash-564f5645-";
inline constexpr std::wstring_view kTrashFilenamePrefixWide = L".vove-trash-564f5645-";
inline constexpr std::u8string_view kTrashVaultDirectoryName = u8".vove-trash-564f5645-vault";
inline constexpr std::wstring_view kTrashVaultDirectoryNameWide = L".vove-trash-564f5645-vault";
inline constexpr std::u8string_view kTrashItemFilenamePrefix = u8"item-";
inline constexpr std::u8string_view kTrashRescueManifestFilename = u8"metadata.vtrash";
inline constexpr std::u8string_view kTransferSourceFilenamePrefix =
    u8".vove-transfer-564f5645-source-";
inline constexpr std::wstring_view kTransferSourceFilenamePrefixWide =
    L".vove-transfer-564f5645-source-";
inline constexpr std::u8string_view kTransferDestinationFilenamePrefix =
    u8".vove-transfer-564f5645-destination-";
inline constexpr std::wstring_view kTransferDestinationFilenamePrefixWide =
    L".vove-transfer-564f5645-destination-";
inline constexpr std::u8string_view kTransferOverwriteBackupFilenamePrefix =
    u8".vove-transfer-564f5645-overwrite-backup-";
inline constexpr std::wstring_view kTransferOverwriteBackupFilenamePrefixWide =
    L".vove-transfer-564f5645-overwrite-backup-";
inline constexpr std::u8string_view kTransferOwnershipMarkerFilename = u8".vove-owner-564f5645";
inline constexpr std::wstring_view kTransferOwnershipMarkerFilenameWide = L".vove-owner-564f5645";

[[nodiscard]] inline bool is_trash_filename(const std::u8string_view filename) noexcept {
#ifdef _WIN32
    return ascii_istarts_with(filename, kTrashVaultDirectoryName) ||
           ascii_istarts_with(filename, kTrashFilenamePrefix);
#else
    return filename.starts_with(kTrashVaultDirectoryName) ||
           filename.starts_with(kTrashFilenamePrefix);
#endif
}

[[nodiscard]] inline bool is_trash_filename(const std::wstring_view filename) noexcept {
    return ascii_istarts_with(filename, kTrashVaultDirectoryNameWide) ||
           ascii_istarts_with(filename, kTrashFilenamePrefixWide);
}

[[nodiscard]] inline bool is_trash_item_filename(const std::u8string_view filename) noexcept {
    return filename.starts_with(kTrashItemFilenamePrefix);
}

[[nodiscard]] inline bool is_transfer_filename(const std::u8string_view filename) noexcept {
#ifdef _WIN32
    return ascii_istarts_with(filename, kTransferSourceFilenamePrefix) ||
           ascii_istarts_with(filename, kTransferDestinationFilenamePrefix) ||
           ascii_istarts_with(filename, kTransferOverwriteBackupFilenamePrefix) ||
           ascii_iequal(filename, kTransferOwnershipMarkerFilename);
#else
    return filename.starts_with(kTransferSourceFilenamePrefix) ||
           filename.starts_with(kTransferDestinationFilenamePrefix) ||
           filename.starts_with(kTransferOverwriteBackupFilenamePrefix) ||
           filename == kTransferOwnershipMarkerFilename;
#endif
}

[[nodiscard]] inline bool is_transfer_filename(const std::wstring_view filename) noexcept {
    return ascii_istarts_with(filename, kTransferSourceFilenamePrefixWide) ||
           ascii_istarts_with(filename, kTransferDestinationFilenamePrefixWide) ||
           ascii_istarts_with(filename, kTransferOverwriteBackupFilenamePrefixWide) ||
           ascii_iequal(filename, kTransferOwnershipMarkerFilenameWide);
}

[[nodiscard]] inline bool is_internal_filename(const std::u8string_view filename) noexcept {
    return is_trash_filename(filename) || is_transfer_filename(filename);
}

[[nodiscard]] inline bool is_internal_filename(const std::wstring_view filename) noexcept {
    return is_trash_filename(filename) || is_transfer_filename(filename);
}

} // namespace vove::core
