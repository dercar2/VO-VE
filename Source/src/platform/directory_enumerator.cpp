#include "directory_enumerator.hpp"

namespace vove::platform::detail {

std::string path_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::uint64_t stable_entry_id(const std::string_view text) {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offsetBasis;
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    return hash;
}

} // namespace vove::platform::detail
