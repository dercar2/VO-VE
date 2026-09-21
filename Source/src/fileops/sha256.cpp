#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace vove::fileops::detail {
namespace {

constexpr std::array<std::uint32_t, 64> roundConstants{
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

std::uint32_t load_big_endian(const std::byte *bytes) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
}

void store_big_endian(const std::uint32_t value, std::uint8_t *output) noexcept {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

} // namespace

void Sha256::transform(const std::span<const std::byte, 64> block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index{}; index < 16U; ++index) {
        words[index] = load_big_endian(block.data() + index * 4U);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
        const auto s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^
                        (words[index - 15U] >> 3U);
        const auto s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^
                        (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index{}; index < words.size(); ++index) {
        const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto first = h + sigma1 + choose + roundConstants[index] + words[index];
        const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto second = sigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> bytes) noexcept {
    total_bytes_ += static_cast<std::uint64_t>(bytes.size());
    if (buffered_ != 0) {
        const auto copied = std::min(buffer_.size() - buffered_, bytes.size());
        std::ranges::copy(bytes.first(copied),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += copied;
        bytes = bytes.subspan(copied);
        if (buffered_ == buffer_.size()) {
            transform(std::span<const std::byte, 64>(buffer_));
            buffered_ = 0;
        }
    }
    while (bytes.size() >= buffer_.size()) {
        transform(std::span<const std::byte, 64>(bytes.data(), buffer_.size()));
        bytes = bytes.subspan(buffer_.size());
    }
    if (!bytes.empty()) {
        std::ranges::copy(bytes, buffer_.begin());
        buffered_ = bytes.size();
    }
}

Sha256Digest Sha256::digest() const noexcept {
    auto finished = *this;
    finished.buffer_[finished.buffered_++] = std::byte{0x80};
    if (finished.buffered_ > 56U) {
        std::fill(finished.buffer_.begin() + static_cast<std::ptrdiff_t>(finished.buffered_),
                  finished.buffer_.end(), std::byte{});
        finished.transform(std::span<const std::byte, 64>(finished.buffer_));
        finished.buffered_ = 0;
    }
    std::fill(finished.buffer_.begin() + static_cast<std::ptrdiff_t>(finished.buffered_),
              finished.buffer_.begin() + 56, std::byte{});
    const auto bit_count = finished.total_bytes_ * 8U;
    for (std::size_t index{}; index < 8U; ++index) {
        finished.buffer_[63U - index] = static_cast<std::byte>((bit_count >> (index * 8U)) & 0xffU);
    }
    finished.transform(std::span<const std::byte, 64>(finished.buffer_));

    Sha256Digest output{};
    for (std::size_t index{}; index < finished.state_.size(); ++index) {
        store_big_endian(finished.state_[index], output.data() + index * 4U);
    }
    return output;
}

Sha256Digest sha256(const std::span<const std::byte> bytes) noexcept {
    Sha256 state;
    state.update(bytes);
    return state.digest();
}

} // namespace vove::fileops::detail
