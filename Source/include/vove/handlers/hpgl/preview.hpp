#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace vove::handlers::hpgl {

inline constexpr std::size_t kMaximumSourceBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaximumIntermediateBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaximumSvgBytes = 16U * 1024U * 1024U;

enum class Status { success, unsupported, malformed, resource_limit };

struct Result {
    Status status{Status::malformed};
    std::string svg;
    std::string diagnostic;
};

// First page only. Built-in stroked labels require no fonts or external resources.
// Calls are serialized because upstream hp2xx uses process-global parser state.
[[nodiscard]] Result render_svg(std::span<const std::byte> source);

} // namespace vove::handlers::hpgl
