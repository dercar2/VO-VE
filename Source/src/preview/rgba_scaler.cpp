#include "vove/preview/rgba_scaler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vove::preview {
namespace {

inline constexpr std::uint64_t kMaximumInputPixels = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumOutputEdge = 2048;

[[nodiscard]] std::uint8_t channel(const std::span<const std::byte> source, const std::size_t pixel,
                                   const std::size_t component) noexcept {
    return std::to_integer<std::uint8_t>(source[pixel * 4U + component]);
}

[[nodiscard]] std::uint8_t rounded_byte(const double value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

} // namespace

ScaledRgbaImage scale_rgba8_to_edge(const std::span<const std::byte> source,
                                    const std::uint32_t width, const std::uint32_t height,
                                    const std::uint32_t maximum_edge) {
    if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > kMaximumInputPixels ||
        maximum_edge == 0 || maximum_edge > kMaximumOutputEdge ||
        static_cast<std::uint64_t>(width) * height * 4ULL != source.size()) {
        return {
            .width = 0, .height = 0, .rgba8 = {}, .error = "RGBA scaler input is outside limits"};
    }
    if (width <= maximum_edge && height <= maximum_edge) {
        return {.width = width,
                .height = height,
                .rgba8 = std::vector<std::byte>(source.begin(), source.end()),
                .error = {}};
    }

    const auto scale =
        static_cast<double>(maximum_edge) / static_cast<double>(std::max(width, height));
    const auto target_width = std::max<std::uint32_t>(
        1U, static_cast<std::uint32_t>(std::lround(static_cast<double>(width) * scale)));
    const auto target_height = std::max<std::uint32_t>(
        1U, static_cast<std::uint32_t>(std::lround(static_cast<double>(height) * scale)));
    std::vector<std::byte> output(static_cast<std::size_t>(target_width) * target_height * 4U);

    for (std::uint32_t target_y = 0; target_y < target_height; ++target_y) {
        const auto source_y = (static_cast<double>(target_y) + 0.5) * height / target_height - 0.5;
        const auto y0 = static_cast<std::uint32_t>(
            std::clamp(std::floor(source_y), 0.0, static_cast<double>(height - 1U)));
        const auto y1 = std::min(y0 + 1U, height - 1U);
        const auto fy = std::clamp(source_y - std::floor(source_y), 0.0, 1.0);
        for (std::uint32_t target_x = 0; target_x < target_width; ++target_x) {
            const auto source_x =
                (static_cast<double>(target_x) + 0.5) * width / target_width - 0.5;
            const auto x0 = static_cast<std::uint32_t>(
                std::clamp(std::floor(source_x), 0.0, static_cast<double>(width - 1U)));
            const auto x1 = std::min(x0 + 1U, width - 1U);
            const auto fx = std::clamp(source_x - std::floor(source_x), 0.0, 1.0);
            const std::array pixels{static_cast<std::size_t>(y0) * width + x0,
                                    static_cast<std::size_t>(y0) * width + x1,
                                    static_cast<std::size_t>(y1) * width + x0,
                                    static_cast<std::size_t>(y1) * width + x1};
            const std::array weights{(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy,
                                     fx * fy};
            double alpha{};
            std::array<double, 3> premultiplied{};
            for (std::size_t sample = 0; sample < pixels.size(); ++sample) {
                const auto sample_alpha = static_cast<double>(channel(source, pixels[sample], 3));
                alpha += weights[sample] * sample_alpha;
                for (std::size_t component = 0; component < premultiplied.size(); ++component) {
                    premultiplied[component] +=
                        weights[sample] *
                        static_cast<double>(channel(source, pixels[sample], component)) *
                        sample_alpha / 255.0;
                }
            }
            const auto target_pixel =
                (static_cast<std::size_t>(target_y) * target_width + target_x) * 4U;
            for (std::size_t component = 0; component < premultiplied.size(); ++component) {
                output[target_pixel + component] = static_cast<std::byte>(
                    alpha > 0.0 ? rounded_byte(premultiplied[component] * 255.0 / alpha) : 0U);
            }
            output[target_pixel + 3U] = static_cast<std::byte>(rounded_byte(alpha));
        }
    }

    return {
        .width = target_width, .height = target_height, .rgba8 = std::move(output), .error = {}};
}

} // namespace vove::preview
