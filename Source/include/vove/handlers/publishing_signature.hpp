#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace vove::handlers {

enum class PublishingContainer { unknown, indesign, affinity };

[[nodiscard]] inline PublishingContainer publishing_container(std::span<const std::byte> bytes) {
    constexpr std::array indesign{
        std::byte{0x06}, std::byte{0x06}, std::byte{0xed}, std::byte{0xf5},
        std::byte{0xd8}, std::byte{0x1d}, std::byte{0x46}, std::byte{0xe5},
        std::byte{0xbd}, std::byte{0x31}, std::byte{0xef}, std::byte{0xe7},
        std::byte{0xfe}, std::byte{0x74}, std::byte{0xb7}, std::byte{0x1d}};
    constexpr std::array affinity{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x4b}, std::byte{0x41}};
    if (bytes.size() >= indesign.size() &&
        std::equal(indesign.begin(), indesign.end(), bytes.begin())) {
        return PublishingContainer::indesign;
    }
    if (bytes.size() >= affinity.size() &&
        std::equal(affinity.begin(), affinity.end(), bytes.begin())) {
        return PublishingContainer::affinity;
    }
    return PublishingContainer::unknown;
}

} // namespace vove::handlers
