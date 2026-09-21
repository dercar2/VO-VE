#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vove::fileops::detail {

inline constexpr std::size_t kSha256DigestBytes = 32;
using Sha256Digest = std::array<std::uint8_t, kSha256DigestBytes>;

class Sha256 final {
  public:
    void update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] Sha256Digest digest() const noexcept;

  private:
    void transform(std::span<const std::byte, 64> block) noexcept;

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_{};
    std::uint64_t total_bytes_{};
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> bytes) noexcept;

} // namespace vove::fileops::detail
