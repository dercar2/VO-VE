#pragma once

#include "vove/cache/cache_types.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace vove::cache {

[[nodiscard]] std::vector<std::byte> serialize_cache_key(const CacheKey &key);
[[nodiscard]] std::vector<std::byte> serialize_cache_locator(const CacheLocatorKey &key);
[[nodiscard]] CacheDigest hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] CacheDigest hash_cache_key(const CacheKey &key);
[[nodiscard]] CacheDigest hash_cache_locator(const CacheLocatorKey &key);
[[nodiscard]] CacheDigest hash_locator_binding(std::string_view locator_digest_hex,
                                               std::string_view artifact_digest_hex);

} // namespace vove::cache
