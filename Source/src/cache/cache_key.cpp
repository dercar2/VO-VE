#include "vove/cache/cache_key.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace vove::cache {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

void append_u32(std::vector<std::byte> &output, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::byte> &output, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_string(std::vector<std::byte> &output, const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("cache key string exceeds the serialization limit");
    }
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]] constexpr std::uint32_t rotate_right(const std::uint32_t value,
                                                   const unsigned count) noexcept {
    return (value >> count) | (value << (32U - count));
}

void process_block(std::array<std::uint32_t, 8> &state, const std::byte *block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4;
        words[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
                       (std::to_integer<std::uint32_t>(block[offset + 1]) << 16U) |
                       (std::to_integer<std::uint32_t>(block[offset + 2]) << 8U) |
                       std::to_integer<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const auto first = rotate_right(words[index - 15], 7) ^
                           rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3U);
        const auto second = rotate_right(words[index - 2], 17) ^
                            rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10U);
        words[index] = words[index - 16] + first + words[index - 7] + second;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto temporary1 = h + sum1 + choice + kRoundConstants[index] + words[index];
        const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

std::string CacheDigest::hex() const {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        const auto number = std::to_integer<unsigned char>(value);
        output.push_back(digits[number >> 4U]);
        output.push_back(digits[number & 0x0fU]);
    }
    return output;
}

std::vector<std::byte> serialize_cache_key(const CacheKey &key) {
    std::vector<std::byte> output;
    output.reserve(128 + key.source.source_identity_utf8.size() + key.handler_id.size() +
                   key.color_policy_version.working_profile_fingerprint.size());
    append_string(output, "VOVE-CACHE-KEY");
    append_u32(output, kCacheKeySchemaVersion);
    append_string(output, key.source.source_identity_utf8);
    append_u64(output, key.source.size_bytes);
    append_u64(output, static_cast<std::uint64_t>(key.source.modified_unix_ns));
    output.push_back(key.source.stable_file_id.has_value() ? std::byte{1} : std::byte{0});
    if (key.source.stable_file_id) {
        append_string(output, *key.source.stable_file_id);
    }
    append_string(output, key.handler_id);
    append_u32(output, key.handler_version);
    append_u32(output, key.render_policy_version);
    append_u32(output, key.color_policy_version.version);
    append_u32(output, key.canonical_edge);
    append_string(output, key.color_policy_version.working_profile_fingerprint);
    append_u32(output, key.page_index);
    return output;
}

std::vector<std::byte> serialize_cache_locator(const CacheLocatorKey &key) {
    std::vector<std::byte> output;
    output.reserve(112 + key.catalog_source.source_identity_utf8.size() +
                   key.color_policy_version.working_profile_fingerprint.size());
    append_string(output, "VOVE-CACHE-LOCATOR");
    append_u32(output, 1);
    append_string(output, key.catalog_source.source_identity_utf8);
    append_u64(output, key.catalog_source.size_bytes);
    append_u64(output, static_cast<std::uint64_t>(key.catalog_source.modified_unix_ns));
    output.push_back(key.catalog_source.stable_file_id.has_value() ? std::byte{1} : std::byte{0});
    if (key.catalog_source.stable_file_id) {
        append_string(output, *key.catalog_source.stable_file_id);
    }
    append_u32(output, key.routing_policy_version);
    append_u32(output, key.render_policy_version);
    append_u32(output, key.color_policy_version.version);
    append_string(output, key.color_policy_version.working_profile_fingerprint);
    append_u32(output, key.page_index);
    // Preserve existing 512px offline locators; larger pane artifacts have separate identities.
    if (key.canonical_edge != kThumbnailCanonicalEdge) {
        append_u32(output, key.canonical_edge);
    }
    return output;
}

CacheDigest hash_bytes(const std::span<const std::byte> bytes) {
    std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                       0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::vector<std::byte> padded(bytes.begin(), bytes.end());
    padded.push_back(std::byte{0x80});
    while ((padded.size() % 64) != 56) {
        padded.push_back(std::byte{0});
    }
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(
            static_cast<std::byte>((bit_length >> static_cast<unsigned>(shift)) & 0xffU));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        process_block(state, padded.data() + offset);
    }

    CacheDigest digest;
    for (std::size_t index = 0; index < state.size(); ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            digest.bytes[index * 4 + byte] = static_cast<std::byte>(
                (state[index] >> (24U - static_cast<unsigned>(byte * 8))) & 0xffU);
        }
    }
    return digest;
}

CacheDigest hash_cache_key(const CacheKey &key) {
    return hash_bytes(serialize_cache_key(key));
}

CacheDigest hash_cache_locator(const CacheLocatorKey &key) {
    return hash_bytes(serialize_cache_locator(key));
}

CacheDigest hash_locator_binding(const std::string_view locator_digest_hex,
                                 const std::string_view artifact_digest_hex) {
    constexpr std::string_view domain = "VOVE-LOCATOR-BINDING-V1";
    if (locator_digest_hex.size() > std::numeric_limits<std::uint32_t>::max() ||
        artifact_digest_hex.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("locator binding digest exceeds the serialization limit");
    }
    std::string serialized;
    serialized.reserve(domain.size() + locator_digest_hex.size() + artifact_digest_hex.size() + 2);
    serialized.append(domain);
    serialized.push_back('\0');
    serialized.append(locator_digest_hex);
    serialized.push_back('\0');
    serialized.append(artifact_digest_hex);
    return hash_bytes({reinterpret_cast<const std::byte *>(serialized.data()), serialized.size()});
}

} // namespace vove::cache
