#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vove::platform::detail::protocol {

inline constexpr std::uint32_t requestMagic = 0x31525156U;
inline constexpr std::uint32_t batchMagic = 0x31414256U;

template <typename Integer>
void append_integer(std::vector<std::byte> &output, const Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto bits = static_cast<std::uint64_t>(std::bit_cast<Unsigned>(value));
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.push_back(static_cast<std::byte>((bits >> (index * 8U)) & 0xFFU));
    }
}

inline void append_string(std::vector<std::byte> &output, const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("catalog protocol string is too large");
    }
    append_integer(output, static_cast<std::uint32_t>(value.size()));
    const auto *first = reinterpret_cast<const std::byte *>(value.data());
    output.insert(output.end(), first, first + value.size());
}

class Cursor final {
  public:
    explicit Cursor(const std::span<const std::byte> data) : data_(data) {}

    template <typename Integer> [[nodiscard]] bool read(Integer &value) {
        static_assert(sizeof(Integer) <= sizeof(std::uint64_t));
        if (remaining() < sizeof(Integer)) {
            return false;
        }
        using Unsigned = std::make_unsigned_t<Integer>;
        std::uint64_t bits{};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            bits |=
                static_cast<std::uint64_t>(std::to_integer<unsigned char>(data_[offset_ + index]))
                << (index * 8U);
        }
        value = std::bit_cast<Integer>(static_cast<Unsigned>(bits));
        offset_ += sizeof(Integer);
        return true;
    }

    [[nodiscard]] bool read_string(std::string &value) {
        std::uint32_t length{};
        if (!read(length) || remaining() < static_cast<std::size_t>(length)) {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(data_.data() + offset_),
                     static_cast<std::size_t>(length));
        offset_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return data_.size() - offset_;
    }

  private:
    std::span<const std::byte> data_;
    std::size_t offset_{};
};

} // namespace vove::platform::detail::protocol
