#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace vove::fileops::detail {

inline bool parse_unsigned_identity_field(const std::string_view field,
                                          const bool require_nonzero = false) noexcept {
    if (field.empty()) {
        return false;
    }
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    return error == std::errc{} && end == field.data() + field.size() &&
           (!require_nonzero || value != 0);
}

inline bool valid_strong_delete_revision(const std::string_view revision) noexcept {
    const auto validate_tail = [&](const std::size_t prefix_size, const bool file_id_is_hex) {
        const auto volume_end = revision.find(':', prefix_size);
        const auto id_end = volume_end == std::string_view::npos
                                ? std::string_view::npos
                                : revision.find(':', volume_end + 1U);
        if (volume_end == std::string_view::npos || id_end == std::string_view::npos ||
            revision.find(':', id_end + 1U) != std::string_view::npos ||
            !parse_unsigned_identity_field(
                revision.substr(prefix_size, volume_end - prefix_size))) {
            return false;
        }
        const auto file_id = revision.substr(volume_end + 1U, id_end - volume_end - 1U);
        if (file_id_is_hex) {
            if (file_id.size() != 32U) {
                return false;
            }
            bool any_nonzero{};
            for (const auto character : file_id) {
                const auto hex = (character >= '0' && character <= '9') ||
                                 (character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F');
                if (!hex) {
                    return false;
                }
                any_nonzero = any_nonzero || character != '0';
            }
            if (!any_nonzero) {
                return false;
            }
        } else if (!parse_unsigned_identity_field(file_id, true)) {
            return false;
        }
        return parse_unsigned_identity_field(revision.substr(id_end + 1U), true);
    };

    constexpr std::string_view strong_prefix{"win-file128:"};
    constexpr std::string_view smb_prefix{"win-smb64:"};
    if (revision.starts_with(strong_prefix)) {
        return validate_tail(strong_prefix.size(), true);
    }
    return revision.starts_with(smb_prefix) && validate_tail(smb_prefix.size(), false);
}

} // namespace vove::fileops::detail
