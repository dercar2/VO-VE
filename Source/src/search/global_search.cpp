#include "vove/search/global_search.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vove::search {
namespace {

std::string path_utf8(const std::filesystem::path &path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

std::filesystem::path path_from_utf8(const std::string_view encoded) {
    const auto *first = reinterpret_cast<const char8_t *>(encoded.data());
    return std::filesystem::path(std::u8string(first, first + encoded.size()));
}

std::string normalize_path(const std::string_view encoded) {
    if (encoded.empty()) {
        return {};
    }
    auto path = path_from_utf8(encoded).lexically_normal();
    if (!path.is_absolute()) {
        return {};
    }
    auto normalized = path_utf8(path);
    while (normalized.size() > 1U && (normalized.back() == '/' || normalized.back() == '\\')) {
#ifdef _WIN32
        if (normalized.size() == 3U && normalized[1] == ':') {
            break;
        }
#endif
        normalized.pop_back();
    }
    return normalized;
}

#ifdef _WIN32
std::wstring wide_from_utf8(const std::string_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool path_prefix(const std::string_view candidate, const std::string_view root) {
    const auto candidate_wide = wide_from_utf8(candidate);
    const auto root_wide = wide_from_utf8(root);
    if (candidate_wide.size() < root_wide.size() || root_wide.empty() ||
        CompareStringOrdinal(candidate_wide.data(), static_cast<int>(root_wide.size()),
                             root_wide.data(), static_cast<int>(root_wide.size()),
                             TRUE) != CSTR_EQUAL) {
        return false;
    }
    if (candidate_wide.size() == root_wide.size()) {
        return true;
    }
    if (root_wide.back() == L'/' || root_wide.back() == L'\\') {
        return true;
    }
    return candidate_wide[root_wide.size()] == L'/' || candidate_wide[root_wide.size()] == L'\\';
}
#else
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool path_prefix(const std::string_view candidate, const std::string_view root) noexcept {
    if (candidate.size() < root.size() ||
        !std::equal(root.begin(), root.end(), candidate.begin())) {
        return false;
    }
    if (candidate.size() == root.size()) {
        return true;
    }
    if (!root.empty() && root.back() == '/') {
        return true;
    }
    return candidate[root.size()] == '/';
}
#endif

} // namespace

std::vector<std::string> normalize_allowed_roots(const std::span<const std::string> roots_utf8) {
    std::vector<std::string> roots;
    roots.reserve(std::min(roots_utf8.size(), kMaximumSearchRoots));
    for (const auto &root : roots_utf8) {
        auto normalized = normalize_path(root);
        if (normalized.empty()) {
            continue;
        }
        if (std::ranges::any_of(
                roots, [&](const auto &existing) { return path_prefix(normalized, existing); })) {
            continue;
        }
        std::erase_if(roots,
                      [&](const auto &existing) { return path_prefix(existing, normalized); });
        roots.push_back(std::move(normalized));
        if (roots.size() == kMaximumSearchRoots) {
            break;
        }
    }
    return roots;
}

bool path_is_within_allowed_roots(const std::string &path_utf8_value,
                                  const std::span<const std::string> normalized_roots_utf8) {
    const auto normalized = normalize_path(path_utf8_value);
    return !normalized.empty() && std::ranges::any_of(normalized_roots_utf8, [&](const auto &root) {
        return path_prefix(normalized, root);
    });
}

} // namespace vove::search
