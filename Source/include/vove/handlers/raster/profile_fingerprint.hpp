#pragma once

#include "vove/color/color_transform.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vove::handlers::raster {

// ICC inputs are already bounded by the color-transform contract. An empty result means that no
// actual ICC profile was embedded or assigned.
[[nodiscard]] inline std::string profile_sha256(const std::span<const std::byte> profile) {
    if (profile.empty() || profile.size() > color::kMaximumIccProfileBytes) {
        return {};
    }

    constexpr std::array<std::uint32_t, 64> constants{
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
    std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                       0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    const auto process = [&state, &constants](const std::span<const std::byte, 64> block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4U;
            words[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
                           (std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
                           (std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
                           std::to_integer<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto first = std::rotr(words[index - 15U], 7) ^
                               std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
            const auto second = std::rotr(words[index - 2U], 17) ^
                                std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + first + words[index - 7U] + second;
        }
        auto [a, b, c, d, e, f, g, h] = state;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                               ((e & f) ^ (~e & g)) + constants[index] + words[index];
            const auto second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                                ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto complete_bytes = profile.size() - (profile.size() % 64U);
    for (std::size_t offset = 0; offset < complete_bytes; offset += 64U) {
        process(std::span<const std::byte, 64>(profile.data() + offset, 64U));
    }
    std::array<std::byte, 128> tail{};
    const auto remainder = profile.size() - complete_bytes;
    std::copy_n(profile.data() + complete_bytes, remainder, tail.data());
    tail[remainder] = std::byte{0x80};
    const auto tail_bytes = remainder < 56U ? 64U : 128U;
    const auto bit_length = static_cast<std::uint64_t>(profile.size()) * 8U;
    for (std::size_t index = 0; index < 8U; ++index) {
        tail[tail_bytes - 1U - index] =
            static_cast<std::byte>((bit_length >> (index * 8U)) & 0xffU);
    }
    process(std::span<const std::byte, 64>(tail.data(), 64U));
    if (tail_bytes == 128U) {
        process(std::span<const std::byte, 64>(tail.data() + 64U, 64U));
    }

    constexpr std::string_view digits = "0123456789abcdef";
    std::string fingerprint;
    fingerprint.reserve(64U);
    for (const auto value : state) {
        for (unsigned shift = 24U;; shift -= 8U) {
            const auto byte = static_cast<unsigned>((value >> shift) & 0xffU);
            fingerprint.push_back(digits[byte >> 4U]);
            fingerprint.push_back(digits[byte & 0x0fU]);
            if (shift == 0U) {
                break;
            }
        }
    }
    return fingerprint;
}

} // namespace vove::handlers::raster
