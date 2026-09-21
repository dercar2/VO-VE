#pragma once

#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace vove::search::protocol::detail {

template <typename Integer>
void append_integer(std::vector<std::byte> &output, const Integer value) {
    static_assert(std::is_integral_v<Integer> || std::is_enum_v<Integer>);
    const auto *first = reinterpret_cast<const std::byte *>(&value);
    output.insert(output.end(), first, first + sizeof(value));
}

inline void append_string(std::vector<std::byte> &output, const std::string &text) {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("search protocol string is too large");
    }
    append_integer(output, static_cast<std::uint32_t>(text.size()));
    const auto *first = reinterpret_cast<const std::byte *>(text.data());
    output.insert(output.end(), first, first + text.size());
}

class Cursor final {
  public:
    explicit Cursor(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename Integer> bool read(Integer &value) {
        static_assert(std::is_integral_v<Integer> || std::is_enum_v<Integer>);
        if (bytes_.size() < sizeof(value)) {
            return false;
        }
        std::memcpy(&value, bytes_.data(), sizeof(value));
        bytes_ = bytes_.subspan(sizeof(value));
        return true;
    }

    bool read_string(std::string &text, const std::size_t maximum = 1U << 20U) {
        std::uint32_t size{};
        if (!read(size) || size > maximum || bytes_.size() < size) {
            return false;
        }
        text.assign(reinterpret_cast<const char *>(bytes_.data()), size);
        bytes_ = bytes_.subspan(size);
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size();
    }

  private:
    std::span<const std::byte> bytes_;
};

} // namespace vove::search::protocol::detail
