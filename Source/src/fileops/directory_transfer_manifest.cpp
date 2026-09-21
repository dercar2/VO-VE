#include "vove/fileops/directory_transfer_manifest.hpp"

#include "vove/core/reserved_names.hpp"

#include "catalog_protocol_codec.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace vove::fileops {
namespace {

using namespace vove::platform::detail::protocol;

constexpr std::uint32_t manifestMagic = 0x314d4456U;
static_assert(detail::kSha256DigestBytes == kDirectoryTransferManifestDigestBytes);

std::string path_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    const auto *first = reinterpret_cast<const char8_t *>(text.data());
    return std::filesystem::path(std::u8string(first, first + text.size()));
}

bool valid_utf8(const std::string_view text) noexcept {
    if (text.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t count{};
        std::uint32_t code_point{};
        if ((first & 0xe0U) == 0xc0U) {
            count = 1U;
            code_point = first & 0x1fU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            count = 2U;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= count; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((count == 2U && code_point < 0x800U) || (count == 3U && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        index += count + 1U;
    }
    return true;
}

std::string hexadecimal(const std::uint64_t value) {
    std::array<char, 32> encoded{};
    const auto [end, error] =
        std::to_chars(encoded.data(), encoded.data() + encoded.size(), value, 16);
    if (error != std::errc{}) {
        throw std::runtime_error("directory-transfer identifier could not be generated");
    }
    return {encoded.data(), end};
}

std::string hexadecimal(const DirectoryTransferOwnershipToken &value) {
    constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const auto byte : value) {
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

template <std::size_t Size> bool bytes_empty(const std::array<std::uint8_t, Size> &bytes) noexcept {
    return std::ranges::all_of(bytes, [](const auto value) { return value == 0U; });
}

template <std::size_t Size>
void append_bytes(std::vector<std::byte> &output, const std::array<std::uint8_t, Size> &bytes) {
    std::ranges::transform(bytes, std::back_inserter(output),
                           [](const auto value) { return static_cast<std::byte>(value); });
}

template <std::size_t Size> bool read_bytes(Cursor &cursor, std::array<std::uint8_t, Size> &bytes) {
    for (auto &value : bytes) {
        if (!cursor.read(value)) {
            return false;
        }
    }
    return true;
}

std::filesystem::path transfer_root_path(const std::filesystem::path &user_path,
                                         const std::u8string_view prefix,
                                         const std::uint64_t operation_id,
                                         const std::size_t root_index,
                                         const DirectoryTransferOwnershipToken &ownership_token) {
    const auto leaf = std::string(reinterpret_cast<const char *>(prefix.data()), prefix.size()) +
                      hexadecimal(operation_id) + '-' + hexadecimal(root_index) + '-' +
                      hexadecimal(ownership_token) + ".tree";
    return user_path.parent_path() / path_from_utf8(leaf);
}

bool valid_kind(const FileTransferKind kind) noexcept {
    return kind == FileTransferKind::copy || kind == FileTransferKind::move;
}

bool valid_entry_kind(const DirectoryManifestEntryKind kind) noexcept {
    return kind == DirectoryManifestEntryKind::directory ||
           kind == DirectoryManifestEntryKind::regular_file;
}

#ifdef _WIN32
bool starts_with_ascii_folded(const std::wstring_view text,
                              const std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    const auto fold = [](const wchar_t value) noexcept {
        return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
    };
    return std::equal(prefix.cbegin(), prefix.cend(), text.cbegin(),
                      [fold](const wchar_t left, const wchar_t right) noexcept {
                          return fold(left) == fold(right);
                      });
}

std::string windows_path_key(const std::filesystem::path &path) {
    auto text = path.native();
    std::ranges::replace(text, L'/', L'\\');
    constexpr std::wstring_view extended_unc = LR"(\\?\UNC\)";
    constexpr std::wstring_view extended = LR"(\\?\)";
    constexpr std::wstring_view device = LR"(\\.\)";
    if (starts_with_ascii_folded(text, device)) {
        return {};
    }
    if (starts_with_ascii_folded(text, extended_unc)) {
        text = LR"(\\)" + text.substr(extended_unc.size());
    } else if (starts_with_ascii_folded(text, extended)) {
        text.erase(0, extended.size());
    }
    std::ranges::replace(text, L'\\', L'/');

    const auto normalized_size =
        NormalizeString(NormalizationC, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (normalized_size <= 0) {
        return {};
    }
    std::wstring normalized(static_cast<std::size_t>(normalized_size), L'\0');
    const auto normalized_actual =
        NormalizeString(NormalizationC, text.data(), static_cast<int>(text.size()),
                        normalized.data(), normalized_size);
    if (normalized_actual <= 0 || normalized_actual > normalized_size) {
        return {};
    }
    normalized.resize(static_cast<std::size_t>(normalized_actual));
    const auto folded_size =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(), normalized_actual,
                      nullptr, 0, nullptr, nullptr, 0);
    if (folded_size <= 0) {
        return {};
    }
    std::wstring folded(static_cast<std::size_t>(folded_size), L'\0');
    const auto folded_actual =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(), normalized_actual,
                      folded.data(), folded_size, nullptr, nullptr, 0);
    if (folded_actual <= 0 || folded_actual > folded_size) {
        return {};
    }
    folded.resize(static_cast<std::size_t>(folded_actual));
    const auto utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folded.data(),
                                               folded_actual, nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        return {};
    }
    std::string key(static_cast<std::size_t>(utf8_size), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folded.data(), folded_actual,
                               key.data(), utf8_size, nullptr, nullptr) == utf8_size
               ? key
               : std::string{};
}
#endif

bool namespace_key_is_ancestor_or_same(const std::string_view ancestor_key,
                                       const std::string_view candidate_key) {
    if (ancestor_key.empty() || candidate_key.empty() || !candidate_key.starts_with(ancestor_key)) {
        return false;
    }
    return candidate_key.size() == ancestor_key.size() || ancestor_key.ends_with('/') ||
           candidate_key[ancestor_key.size()] == '/';
}

struct ParsedRevision {
    std::string stable_identity;
    std::string volume_identity;
    DirectoryPathSemantics path_semantics{};
    bool has_change{};
};

std::optional<std::uint64_t> parse_unsigned_decimal(const std::string_view text,
                                                    const bool require_nonzero) noexcept {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
        (require_nonzero && value == 0)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> canonical_hex128(const std::string_view text) {
    if (text.size() != 32U) {
        return std::nullopt;
    }
    bool nonzero{};
    std::string canonical;
    canonical.reserve(text.size());
    for (const char value : text) {
        const bool digit = value >= '0' && value <= '9';
        const bool lower = value >= 'a' && value <= 'f';
        const bool upper = value >= 'A' && value <= 'F';
        if (!digit && !lower && !upper) {
            return std::nullopt;
        }
        nonzero = nonzero || value != '0';
        canonical.push_back(upper ? static_cast<char>(value + ('a' - 'A')) : value);
    }
    return nonzero ? std::optional<std::string>(std::move(canonical)) : std::nullopt;
}

std::optional<ParsedRevision> parse_revision(const std::string_view text,
                                             const bool require_change) {
    std::array<std::string_view, 5> parts{};
    std::size_t count{};
    std::size_t begin{};
    for (;;) {
        if (count == parts.size()) {
            return std::nullopt;
        }
        const auto end = text.find(':', begin);
        parts[count++] = text.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    const auto strong_posix = parts[0] == "posix2";
    const auto expected_count =
        strong_posix ? (require_change ? 5U : 4U) : (require_change ? 4U : 3U);
    if (count != expected_count || (parts[0] != "win-file128" && parts[0] != "win-smb64" &&
                                    parts[0] != "posix" && !strong_posix)) {
        return std::nullopt;
    }
    const auto volume = parse_unsigned_decimal(parts[1], false);
    if (!volume) {
        return std::nullopt;
    }
    std::string object_identity;
    if (parts[0] == "win-file128") {
        const auto canonical = canonical_hex128(parts[2]);
        if (!canonical) {
            return std::nullopt;
        }
        object_identity = *canonical;
    } else {
        const auto object = parse_unsigned_decimal(parts[2], true);
        if (!object) {
            return std::nullopt;
        }
        object_identity = std::to_string(*object);
    }
    const auto birth =
        strong_posix ? parse_unsigned_decimal(parts[3], false) : std::optional<std::uint64_t>{};
    if (strong_posix && !birth) {
        return std::nullopt;
    }
    const auto change_index = strong_posix ? 4U : 3U;
    if (require_change && !parse_unsigned_decimal(parts[change_index], true)) {
        return std::nullopt;
    }
    const auto volume_identity = std::string(parts[0]) + ':' + std::to_string(*volume);
    auto stable_identity = volume_identity + ':' + object_identity;
    if (strong_posix) {
        stable_identity += ':' + std::to_string(*birth);
    }
    return ParsedRevision{.stable_identity = std::move(stable_identity),
                          .volume_identity = volume_identity,
                          .path_semantics = parts[0] == "posix" || strong_posix
                                                ? DirectoryPathSemantics::posix_exact
                                                : DirectoryPathSemantics::windows_ordinal_nfc,
                          .has_change = require_change};
}

std::optional<ParsedRevision> valid_snapshot_revision(const SourceSnapshot &snapshot,
                                                      const DirectoryPathSemantics semantics) {
    auto revision = parse_revision(snapshot.source_revision_utf8, true);
    if (!revision || revision->path_semantics != semantics) {
        return std::nullopt;
    }
    return revision;
}

bool parent_anchor_matches(const std::string &identity, const std::string &revision,
                           const DirectoryPathSemantics semantics) {
    const auto parsed_identity = parse_revision(identity, false);
    const auto parsed_revision = parse_revision(revision, true);
    return parsed_identity && parsed_revision && parsed_identity->path_semantics == semantics &&
           parsed_revision->path_semantics == semantics &&
           parsed_identity->stable_identity == parsed_revision->stable_identity;
}

bool valid_path_semantics(const DirectoryPathSemantics semantics) noexcept {
    return semantics == DirectoryPathSemantics::windows_ordinal_nfc ||
           semantics == DirectoryPathSemantics::posix_exact;
}

bool supported_path_semantics(const DirectoryPathSemantics semantics) noexcept {
#ifdef _WIN32
    return semantics == DirectoryPathSemantics::windows_ordinal_nfc;
#else
    return semantics == DirectoryPathSemantics::posix_exact;
#endif
}

bool contains_noncanonical_component(const std::filesystem::path &path) {
    for (const auto &component : path) {
        if (component == "." || component == "..") {
            return true;
        }
    }
    return false;
}

std::string_view utf8_view(const std::u8string_view text) noexcept {
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

bool is_internal_namespace_name(const std::filesystem::path &name,
                                const DirectoryPathSemantics semantics) {
    const auto key = directory_transfer_namespace_key(name, semantics);
    const auto marker_key = directory_transfer_namespace_key(
        path_from_utf8(utf8_view(core::kTransferOwnershipMarkerFilename)), semantics);
    return key.starts_with(utf8_view(core::kTrashFilenamePrefix)) ||
           key.starts_with(utf8_view(core::kTransferSourceFilenamePrefix)) ||
           key.starts_with(utf8_view(core::kTransferDestinationFilenamePrefix)) ||
           key == marker_key;
}

bool contains_internal_component(const std::filesystem::path &path,
                                 const DirectoryPathSemantics semantics) {
    return std::ranges::any_of(path, [semantics](const auto &component) {
        return is_internal_namespace_name(component, semantics);
    });
}

bool namespace_key_less(const std::string_view left, const std::string_view right) noexcept {
    const auto rank = [](const char value) noexcept {
        return value == '/' ? 0U : static_cast<unsigned>(static_cast<unsigned char>(value)) + 1U;
    };
    const auto common = std::min(left.size(), right.size());
    for (std::size_t index{}; index < common; ++index) {
        const auto left_rank = rank(left[index]);
        const auto right_rank = rank(right[index]);
        if (left_rank != right_rank) {
            return left_rank < right_rank;
        }
    }
    return left.size() < right.size();
}

bool add_size(std::size_t &total, const std::size_t amount) noexcept {
    if (amount > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += amount;
    return total <= kMaximumDirectoryTransferManifestBytes;
}

bool add_encoded_string_size(std::size_t &total, const std::string_view value) noexcept {
    return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
           add_size(total, sizeof(std::uint32_t)) && add_size(total, value.size());
}

bool encoded_size(const DirectoryTransferManifest &manifest, std::size_t &size) {
    size = sizeof(std::uint32_t) * 4U + sizeof(std::uint64_t) * 2U + sizeof(std::uint8_t) * 2U +
           sizeof(std::uint16_t) + kDirectoryTransferManifestDigestBytes;
    if (size > kMaximumDirectoryTransferManifestBytes) {
        return false;
    }
    for (const auto &root : manifest.roots) {
        if (!add_size(size, sizeof(std::uint32_t) * 2U + sizeof(std::uint64_t) * 2U +
                                sizeof(std::int64_t) + kDirectoryTransferOwnershipTokenBytes)) {
            return false;
        }
        const std::array strings{
            path_utf8(root.source),
            path_utf8(root.staged_source),
            path_utf8(root.staging_destination),
            path_utf8(root.destination),
            root.source_snapshot.source_revision_utf8,
            root.source_parent_identity_utf8,
            root.destination_parent_identity_utf8,
            root.source_parent_revision_utf8,
            root.destination_parent_revision_utf8,
            root.source_namespace_key_utf8,
            root.staged_source_namespace_key_utf8,
            root.staging_destination_namespace_key_utf8,
            root.destination_namespace_key_utf8,
        };
        for (const auto &value : strings) {
            if (!add_encoded_string_size(size, value)) {
                return false;
            }
        }
    }
    for (const auto &entry : manifest.entries) {
        if (!add_size(size, sizeof(std::uint32_t) * 2U + sizeof(std::uint16_t) +
                                sizeof(std::uint8_t) * 2U + sizeof(std::uint64_t) * 2U +
                                sizeof(std::int64_t)) ||
            !add_encoded_string_size(size, path_utf8(entry.name)) ||
            !add_encoded_string_size(size, entry.namespace_key_utf8) ||
            !add_encoded_string_size(size, entry.source_snapshot.source_revision_utf8)) {
            return false;
        }
    }
    return true;
}

bool valid_root_paths(const DirectoryTransferManifest &manifest, const std::size_t root_index,
                      const DirectoryTransferManifestRoot &root, std::string &detail) {
    if (!root.source.is_absolute() || !root.destination.is_absolute() ||
        !root.staged_source.is_absolute() || !root.staging_destination.is_absolute()) {
        detail = "directory-transfer root paths must be absolute";
        return false;
    }
    if (contains_internal_component(root.source, manifest.path_semantics) ||
        contains_internal_component(root.destination, manifest.path_semantics)) {
        detail = "directory-transfer root uses a reserved internal name";
        return false;
    }
    const std::array paths{root.source, root.staged_source, root.staging_destination,
                           root.destination};
    if (std::ranges::any_of(paths, [](const auto &path) {
            return contains_noncanonical_component(path) || path != path.lexically_normal();
        })) {
        detail = "directory-transfer root path is not canonical";
        return false;
    }
    const std::array<std::string_view, 4> stored_keys{
        root.source_namespace_key_utf8,
        root.staged_source_namespace_key_utf8,
        root.staging_destination_namespace_key_utf8,
        root.destination_namespace_key_utf8,
    };
    std::array<std::string, 4> computed_keys;
    std::ranges::transform(paths, computed_keys.begin(), [&](const auto &path) {
        return directory_transfer_namespace_key(path, manifest.path_semantics);
    });
    for (std::size_t index{}; index < paths.size(); ++index) {
        if (computed_keys[index].empty() || !valid_utf8(stored_keys[index]) ||
            computed_keys[index] != stored_keys[index]) {
            detail = "directory-transfer root namespace key is invalid";
            return false;
        }
    }
    if (bytes_empty(root.ownership_token)) {
        detail = "directory-transfer root ownership token is empty";
        return false;
    }
    const auto expected_staged_source = directory_transfer_staged_source_path(
        root.source, manifest.operation_id, root_index, root.ownership_token);
    const auto expected_staging_destination = directory_transfer_staging_destination_path(
        root.destination, manifest.operation_id, root_index, root.ownership_token);
    if (root.staged_source != expected_staged_source ||
        root.staging_destination != expected_staging_destination) {
        detail = "directory-transfer root has non-deterministic staging paths";
        return false;
    }
    std::string filename_detail;
    if (!valid_destination_filename(root.destination.filename(), filename_detail)) {
        detail = "directory-transfer destination is invalid: " + filename_detail;
        return false;
    }
    for (std::size_t left{}; left < paths.size(); ++left) {
        for (std::size_t right = left + 1U; right < paths.size(); ++right) {
            if (namespace_key_is_ancestor_or_same(computed_keys[left], computed_keys[right]) ||
                namespace_key_is_ancestor_or_same(computed_keys[right], computed_keys[left])) {
                detail = "directory-transfer root namespace paths overlap";
                return false;
            }
        }
    }
    return true;
}

} // namespace

std::string
directory_transfer_canonical_object_identity(const std::string_view source_revision_utf8,
                                             const DirectoryPathSemantics semantics) {
    auto parsed = parse_revision(source_revision_utf8, false);
    if (!parsed) {
        parsed = parse_revision(source_revision_utf8, true);
    }
    return parsed && parsed->path_semantics == semantics ? parsed->stable_identity : std::string{};
}

std::string directory_transfer_namespace_key(const std::filesystem::path &path,
                                             const DirectoryPathSemantics semantics) {
    if (semantics == DirectoryPathSemantics::posix_exact) {
        const auto key = path.lexically_normal().generic_u8string();
        return {reinterpret_cast<const char *>(key.data()), key.size()};
    }
#ifdef _WIN32
    if (semantics == DirectoryPathSemantics::windows_ordinal_nfc) {
        const auto &native = path.native();
        constexpr std::wstring_view extended = LR"(\\?\)";
        return windows_path_key(
            starts_with_ascii_folded(native, extended) ? path : path.lexically_normal());
    }
#else
    static_cast<void>(path);
#endif
    return {};
}

std::filesystem::path directory_transfer_staged_source_path(
    const std::filesystem::path &source, const std::uint64_t operation_id,
    const std::size_t root_index, const DirectoryTransferOwnershipToken &ownership_token) {
    return transfer_root_path(source, core::kTransferSourceFilenamePrefix, operation_id, root_index,
                              ownership_token);
}

std::filesystem::path directory_transfer_staging_destination_path(
    const std::filesystem::path &destination, const std::uint64_t operation_id,
    const std::size_t root_index, const DirectoryTransferOwnershipToken &ownership_token) {
    return transfer_root_path(destination, core::kTransferDestinationFilenamePrefix, operation_id,
                              root_index, ownership_token);
}

bool generate_directory_transfer_ownership_token(DirectoryTransferOwnershipToken &token,
                                                 std::error_code &error) noexcept {
    token = {};
    error.clear();
    for (unsigned int attempt{}; attempt < 4U; ++attempt) {
#ifdef _WIN32
        const auto status =
            BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(token.data()),
                            static_cast<ULONG>(token.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) {
            token = {};
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
#else
        std::size_t offset{};
        while (offset < token.size()) {
            const auto count = ::getrandom(token.data() + offset, token.size() - offset, 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                token = {};
                error = count < 0 ? std::error_code(errno, std::generic_category())
                                  : std::make_error_code(std::errc::io_error);
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
#endif
        if (!bytes_empty(token)) {
            return true;
        }
    }
    error = std::make_error_code(std::errc::io_error);
    return false;
}

bool valid_directory_transfer_manifest(const DirectoryTransferManifest &manifest,
                                       std::string &detail_utf8) {
    detail_utf8.clear();
    if (manifest.version != kDirectoryTransferManifestVersion || manifest.operation_id == 0 ||
        !valid_kind(manifest.kind)) {
        detail_utf8 = "directory-transfer manifest identity header is invalid";
        return false;
    }
    if (!valid_path_semantics(manifest.path_semantics) ||
        !supported_path_semantics(manifest.path_semantics)) {
        detail_utf8 = "directory-transfer manifest pathname model is unsupported";
        return false;
    }
    if (manifest.roots.empty()) {
        detail_utf8 = "directory-transfer manifest has no roots";
        return false;
    }
    if (manifest.roots.size() > kMaximumDirectoryTransferRoots) {
        detail_utf8 = "directory-transfer manifest has too many roots: " +
                      std::to_string(manifest.roots.size());
        return false;
    }
    if (manifest.entries.size() > kMaximumDirectoryTransferEntries) {
        detail_utf8 = "directory-transfer manifest has too many entries";
        return false;
    }

    std::unordered_set<std::string> occupied_paths;
    std::unordered_set<std::string> physical_objects;
    std::unordered_set<std::string> ownership_tokens;
    std::vector<std::string_view> root_path_keys;
    root_path_keys.reserve(manifest.roots.size() * 4U);
    std::size_t expected_first{};
    for (std::size_t root_index{}; root_index < manifest.roots.size(); ++root_index) {
        const auto &root = manifest.roots[root_index];
        const auto root_revision =
            valid_snapshot_revision(root.source_snapshot, manifest.path_semantics);
        const auto source_parent = parse_revision(root.source_parent_identity_utf8, false);
        if (root.first_entry != expected_first ||
            static_cast<std::size_t>(root.entry_count) > manifest.entries.size() - expected_first ||
            !valid_root_paths(manifest, root_index, root, detail_utf8) ||
            root.source_snapshot.size_bytes != 0 || !root_revision || root.link_count != 0 ||
            root.unsupported_shape_flags != 0 || !source_parent ||
            source_parent->path_semantics != manifest.path_semantics ||
            !parent_anchor_matches(root.source_parent_identity_utf8,
                                   root.source_parent_revision_utf8, manifest.path_semantics) ||
            !parent_anchor_matches(root.destination_parent_identity_utf8,
                                   root.destination_parent_revision_utf8,
                                   manifest.path_semantics) ||
            source_parent->volume_identity != root_revision->volume_identity) {
            if (detail_utf8.empty()) {
                detail_utf8 = "directory-transfer root metadata is invalid";
            }
            return false;
        }
        expected_first += root.entry_count;

        if (!physical_objects.insert(root_revision->stable_identity).second) {
            detail_utf8 = "directory-transfer roots contain a duplicate physical object";
            return false;
        }
        const std::string token_key(reinterpret_cast<const char *>(root.ownership_token.data()),
                                    root.ownership_token.size());
        if (!ownership_tokens.emplace(token_key).second) {
            detail_utf8 = "directory-transfer roots contain a duplicate ownership token";
            return false;
        }
        const std::array<std::string_view, 4> keys{
            root.source_namespace_key_utf8,
            root.staged_source_namespace_key_utf8,
            root.staging_destination_namespace_key_utf8,
            root.destination_namespace_key_utf8,
        };
        for (const auto &key : keys) {
            if (key.empty() || !occupied_paths.emplace(key).second) {
                detail_utf8 = "directory-transfer roots contain a path collision";
                return false;
            }
            root_path_keys.emplace_back(key);
        }
    }
    if (expected_first != manifest.entries.size()) {
        detail_utf8 = "directory-transfer root slices do not cover the manifest";
        return false;
    }
    std::ranges::sort(root_path_keys, namespace_key_less);
    for (std::size_t index = 1U; index < root_path_keys.size(); ++index) {
        if (namespace_key_is_ancestor_or_same(root_path_keys[index - 1U], root_path_keys[index])) {
            detail_utf8 = "directory-transfer root namespace paths overlap";
            return false;
        }
    }

    std::unordered_set<std::string> sibling_names;
    std::uint64_t total_bytes{};
    for (std::size_t index{}; index < manifest.entries.size(); ++index) {
        const auto &entry = manifest.entries[index];
        const auto entry_revision =
            valid_snapshot_revision(entry.source_snapshot, manifest.path_semantics);
        if (entry.root_index >= manifest.roots.size() || !valid_entry_kind(entry.kind) ||
            entry.depth == 0 || entry.depth > kMaximumDirectoryTransferDepth || !entry_revision) {
            detail_utf8 = "directory-transfer entry metadata is invalid";
            return false;
        }
        const auto &root = manifest.roots[entry.root_index];
        const auto root_end = static_cast<std::size_t>(root.first_entry) + root.entry_count;
        if (index < root.first_entry || index >= root_end) {
            detail_utf8 = "directory-transfer entry is outside its root slice";
            return false;
        }
        if ((entry.depth == 1U && entry.parent_index != kDirectoryTransferRootParent) ||
            (entry.depth > 1U &&
             (entry.parent_index == kDirectoryTransferRootParent || entry.parent_index >= index))) {
            detail_utf8 = "directory-transfer entry parent ordering is invalid";
            return false;
        }
        if (entry.parent_index != kDirectoryTransferRootParent) {
            const auto &parent = manifest.entries[entry.parent_index];
            if (parent.root_index != entry.root_index ||
                parent.kind != DirectoryManifestEntryKind::directory ||
                entry.depth != static_cast<std::uint32_t>(parent.depth) + 1U) {
                detail_utf8 = "directory-transfer entry parent is invalid";
                return false;
            }
        }
        std::string filename_detail;
        if (!valid_destination_filename(entry.name, filename_detail) ||
            is_internal_namespace_name(entry.name, manifest.path_semantics)) {
            detail_utf8 = "directory-transfer entry name is invalid: " + filename_detail;
            return false;
        }
        const auto entry_name_key =
            directory_transfer_namespace_key(entry.name, manifest.path_semantics);
        if (entry_name_key.empty() || !valid_utf8(entry.namespace_key_utf8) ||
            entry_name_key != entry.namespace_key_utf8) {
            detail_utf8 = "directory-transfer entry name cannot be normalized";
            return false;
        }
        const auto sibling_key = std::to_string(entry.root_index) + ':' +
                                 std::to_string(entry.parent_index) + ':' + entry_name_key;
        if (!sibling_names.insert(sibling_key).second) {
            detail_utf8 = "directory-transfer manifest contains duplicate sibling names";
            return false;
        }
        const auto root_revision =
            valid_snapshot_revision(root.source_snapshot, manifest.path_semantics);
        if (!root_revision || entry_revision->volume_identity != root_revision->volume_identity) {
            detail_utf8 = "directory-transfer entry crosses the source root volume";
            return false;
        }
        if (!physical_objects.insert(entry_revision->stable_identity).second) {
            detail_utf8 = "directory-transfer manifest contains a duplicate physical object";
            return false;
        }
        if (entry.kind == DirectoryManifestEntryKind::directory) {
            if (entry.source_snapshot.size_bytes != 0 || entry.link_count != 0 ||
                entry.unsupported_shape_flags != 0) {
                detail_utf8 = "directory-transfer directory has a file size";
                return false;
            }
        } else {
            if (entry.link_count != 1U || entry.unsupported_shape_flags != 0) {
                detail_utf8 = "directory-transfer file has an unsupported filesystem shape";
                return false;
            }
            if (entry.source_snapshot.size_bytes >
                std::numeric_limits<std::uint64_t>::max() - total_bytes) {
                detail_utf8 = "directory-transfer byte count overflows";
                return false;
            }
            total_bytes += entry.source_snapshot.size_bytes;
        }
    }
    if (total_bytes != manifest.total_bytes) {
        detail_utf8 = "directory-transfer total byte count is inconsistent";
        return false;
    }
    std::size_t payload_size{};
    if (!encoded_size(manifest, payload_size)) {
        detail_utf8 = "directory-transfer manifest exceeds its 64 MiB limit";
        return false;
    }
    return true;
}

std::vector<std::byte>
encode_directory_transfer_manifest(const DirectoryTransferManifest &manifest) {
    std::string detail;
    if (!valid_directory_transfer_manifest(manifest, detail)) {
        throw std::invalid_argument(detail);
    }
    std::size_t payload_size{};
    if (!encoded_size(manifest, payload_size)) {
        throw std::length_error("directory-transfer manifest exceeds its size limit");
    }
    std::vector<std::byte> payload;
    payload.reserve(payload_size);
    append_integer(payload, manifestMagic);
    append_integer(payload, manifest.version);
    append_integer(payload, manifest.operation_id);
    append_integer(payload, static_cast<std::uint8_t>(manifest.kind));
    append_integer(payload, static_cast<std::uint8_t>(manifest.path_semantics));
    append_integer(payload, std::uint16_t{});
    append_integer(payload, static_cast<std::uint32_t>(manifest.roots.size()));
    append_integer(payload, static_cast<std::uint32_t>(manifest.entries.size()));
    append_integer(payload, manifest.total_bytes);
    for (const auto &root : manifest.roots) {
        append_integer(payload, root.first_entry);
        append_integer(payload, root.entry_count);
        append_integer(payload, root.source_snapshot.size_bytes);
        append_integer(payload, root.source_snapshot.modified_unix_ns);
        append_integer(payload, root.link_count);
        append_integer(payload, root.unsupported_shape_flags);
        append_bytes(payload, root.ownership_token);
        append_string(payload, path_utf8(root.source));
        append_string(payload, path_utf8(root.staged_source));
        append_string(payload, path_utf8(root.staging_destination));
        append_string(payload, path_utf8(root.destination));
        append_string(payload, root.source_snapshot.source_revision_utf8);
        append_string(payload, root.source_parent_identity_utf8);
        append_string(payload, root.destination_parent_identity_utf8);
        append_string(payload, root.source_parent_revision_utf8);
        append_string(payload, root.destination_parent_revision_utf8);
        append_string(payload, root.source_namespace_key_utf8);
        append_string(payload, root.staged_source_namespace_key_utf8);
        append_string(payload, root.staging_destination_namespace_key_utf8);
        append_string(payload, root.destination_namespace_key_utf8);
    }
    for (const auto &entry : manifest.entries) {
        append_integer(payload, entry.root_index);
        append_integer(payload, entry.parent_index);
        append_integer(payload, entry.depth);
        append_integer(payload, static_cast<std::uint8_t>(entry.kind));
        append_integer(payload, std::uint8_t{});
        append_integer(payload, entry.source_snapshot.size_bytes);
        append_integer(payload, entry.source_snapshot.modified_unix_ns);
        append_integer(payload, entry.link_count);
        append_integer(payload, entry.unsupported_shape_flags);
        append_string(payload, path_utf8(entry.name));
        append_string(payload, entry.namespace_key_utf8);
        append_string(payload, entry.source_snapshot.source_revision_utf8);
    }
    if (payload.size() + kDirectoryTransferManifestDigestBytes != payload_size) {
        throw std::logic_error("directory-transfer manifest size accounting failed");
    }
    const auto digest = detail::sha256(payload);
    for (const auto value : digest) {
        payload.push_back(static_cast<std::byte>(value));
    }
    return payload;
}

namespace {

bool encoded_manifest_has_incompatible_version(const std::span<const std::byte> payload) {
    if (payload.size() < kDirectoryTransferManifestDigestBytes + sizeof(std::uint32_t) * 2U ||
        payload.size() > kMaximumDirectoryTransferManifestBytes) {
        return false;
    }
    const auto body = payload.first(payload.size() - kDirectoryTransferManifestDigestBytes);
    const auto expected_digest = detail::sha256(body);
    for (std::size_t index{}; index < expected_digest.size(); ++index) {
        if (payload[body.size() + index] != static_cast<std::byte>(expected_digest[index])) {
            return false;
        }
    }
    Cursor cursor(body);
    std::uint32_t magic{};
    std::uint32_t version{};
    return cursor.read(magic) && cursor.read(version) && magic == manifestMagic &&
           version != kDirectoryTransferManifestVersion;
}

bool decode_directory_transfer_manifest_impl(const std::span<const std::byte> payload,
                                             DirectoryTransferManifest &manifest,
                                             std::string &detail_utf8) {
    detail_utf8.clear();
    if (payload.size() > kMaximumDirectoryTransferManifestBytes ||
        payload.size() < kDirectoryTransferManifestDigestBytes) {
        detail_utf8 = "directory-transfer manifest size is invalid";
        return false;
    }
    const auto body = payload.first(payload.size() - kDirectoryTransferManifestDigestBytes);
    const auto expected_digest = detail::sha256(body);
    for (std::size_t index{}; index < expected_digest.size(); ++index) {
        if (payload[body.size() + index] != static_cast<std::byte>(expected_digest[index])) {
            detail_utf8 = "directory-transfer manifest digest is invalid";
            return false;
        }
    }
    Cursor cursor(body);
    DirectoryTransferManifest decoded;
    std::uint32_t magic{};
    std::uint8_t kind{};
    std::uint8_t path_semantics{};
    std::uint16_t reserved16{};
    std::uint32_t root_count{};
    std::uint32_t entry_count{};
    if (!cursor.read(magic) || !cursor.read(decoded.version) ||
        !cursor.read(decoded.operation_id) || !cursor.read(kind) || !cursor.read(path_semantics) ||
        !cursor.read(reserved16) || !cursor.read(root_count) || !cursor.read(entry_count) ||
        !cursor.read(decoded.total_bytes) || magic != manifestMagic ||
        !valid_kind(static_cast<FileTransferKind>(kind)) ||
        !valid_path_semantics(static_cast<DirectoryPathSemantics>(path_semantics)) ||
        reserved16 != 0 || root_count == 0 || root_count > kMaximumDirectoryTransferRoots ||
        entry_count > kMaximumDirectoryTransferEntries) {
        detail_utf8 = "directory-transfer manifest header is invalid or truncated";
        return false;
    }
    decoded.kind = static_cast<FileTransferKind>(kind);
    decoded.path_semantics = static_cast<DirectoryPathSemantics>(path_semantics);
    constexpr std::size_t minimum_root_bytes =
        sizeof(std::uint32_t) * 2U + sizeof(std::uint64_t) * 2U + sizeof(std::int64_t) +
        kDirectoryTransferOwnershipTokenBytes + sizeof(std::uint32_t) * 13U;
    constexpr std::size_t minimum_entry_bytes =
        sizeof(std::uint32_t) * 2U + sizeof(std::uint16_t) + sizeof(std::uint8_t) * 2U +
        sizeof(std::uint64_t) * 2U + sizeof(std::int64_t) + sizeof(std::uint32_t) * 3U;
    const auto minimum_body = static_cast<std::size_t>(root_count) * minimum_root_bytes +
                              static_cast<std::size_t>(entry_count) * minimum_entry_bytes;
    if (cursor.remaining() < minimum_body) {
        detail_utf8 = "directory-transfer manifest counts exceed its body";
        return false;
    }
    decoded.roots.reserve(root_count);
    for (std::uint32_t index{}; index < root_count; ++index) {
        DirectoryTransferManifestRoot root;
        std::array<std::string, 13> strings;
        if (!cursor.read(root.first_entry) || !cursor.read(root.entry_count) ||
            !cursor.read(root.source_snapshot.size_bytes) ||
            !cursor.read(root.source_snapshot.modified_unix_ns) || !cursor.read(root.link_count) ||
            !cursor.read(root.unsupported_shape_flags) ||
            !read_bytes(cursor, root.ownership_token)) {
            detail_utf8 = "directory-transfer root header is invalid or truncated";
            return false;
        }
        for (auto &value : strings) {
            if (!cursor.read_string(value) || !valid_utf8(value)) {
                detail_utf8 = "directory-transfer root text is invalid or truncated";
                return false;
            }
        }
        root.source = path_from_utf8(strings[0]);
        root.staged_source = path_from_utf8(strings[1]);
        root.staging_destination = path_from_utf8(strings[2]);
        root.destination = path_from_utf8(strings[3]);
        root.source_snapshot.source_revision_utf8 = std::move(strings[4]);
        root.source_parent_identity_utf8 = std::move(strings[5]);
        root.destination_parent_identity_utf8 = std::move(strings[6]);
        root.source_parent_revision_utf8 = std::move(strings[7]);
        root.destination_parent_revision_utf8 = std::move(strings[8]);
        root.source_namespace_key_utf8 = std::move(strings[9]);
        root.staged_source_namespace_key_utf8 = std::move(strings[10]);
        root.staging_destination_namespace_key_utf8 = std::move(strings[11]);
        root.destination_namespace_key_utf8 = std::move(strings[12]);
        decoded.roots.push_back(std::move(root));
    }
    decoded.entries.reserve(entry_count);
    for (std::uint32_t index{}; index < entry_count; ++index) {
        DirectoryTransferManifestEntry entry;
        std::uint8_t entry_kind{};
        std::uint8_t entry_reserved{};
        std::string name;
        std::string namespace_key;
        if (!cursor.read(entry.root_index) || !cursor.read(entry.parent_index) ||
            !cursor.read(entry.depth) || !cursor.read(entry_kind) || !cursor.read(entry_reserved) ||
            !cursor.read(entry.source_snapshot.size_bytes) ||
            !cursor.read(entry.source_snapshot.modified_unix_ns) ||
            !cursor.read(entry.link_count) || !cursor.read(entry.unsupported_shape_flags) ||
            !cursor.read_string(name) || !cursor.read_string(namespace_key) ||
            !cursor.read_string(entry.source_snapshot.source_revision_utf8) ||
            !valid_entry_kind(static_cast<DirectoryManifestEntryKind>(entry_kind)) ||
            entry_reserved != 0 || !valid_utf8(name) || !valid_utf8(namespace_key) ||
            !valid_utf8(entry.source_snapshot.source_revision_utf8)) {
            detail_utf8 = "directory-transfer entry is invalid or truncated";
            return false;
        }
        entry.kind = static_cast<DirectoryManifestEntryKind>(entry_kind);
        entry.name = path_from_utf8(name);
        entry.namespace_key_utf8 = std::move(namespace_key);
        decoded.entries.push_back(std::move(entry));
    }
    if (cursor.remaining() != 0 || !valid_directory_transfer_manifest(decoded, detail_utf8)) {
        if (detail_utf8.empty()) {
            detail_utf8 = "directory-transfer manifest contains trailing or invalid data";
        }
        return false;
    }
    manifest = std::move(decoded);
    return true;
}

} // namespace

bool decode_directory_transfer_manifest(const std::span<const std::byte> payload,
                                        DirectoryTransferManifest &manifest,
                                        std::string &detail_utf8) {
    return decode_directory_transfer_manifest_status(payload, manifest, detail_utf8) ==
           DirectoryTransferManifestDecodeStatus::success;
}

DirectoryTransferManifestDecodeStatus
decode_directory_transfer_manifest_status(const std::span<const std::byte> payload,
                                          DirectoryTransferManifest &manifest,
                                          std::string &detail_utf8) {
    try {
        if (encoded_manifest_has_incompatible_version(payload)) {
            detail_utf8 = "directory-transfer manifest version is incompatible";
            return DirectoryTransferManifestDecodeStatus::incompatible_version;
        }
        return decode_directory_transfer_manifest_impl(payload, manifest, detail_utf8)
                   ? DirectoryTransferManifestDecodeStatus::success
                   : DirectoryTransferManifestDecodeStatus::corrupt;
    } catch (const std::bad_alloc &) {
        detail_utf8 = "directory-transfer manifest allocation failed";
        return DirectoryTransferManifestDecodeStatus::corrupt;
    } catch (const std::exception &error) {
        detail_utf8 = std::string("directory-transfer manifest decode failed: ") + error.what();
        return DirectoryTransferManifestDecodeStatus::corrupt;
    }
}

} // namespace vove::fileops
