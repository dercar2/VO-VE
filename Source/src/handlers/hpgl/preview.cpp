#include "vove/handlers/hpgl/preview.hpp"

#include "parser_bridge.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>

namespace vove::handlers::hpgl {
namespace {

// The pinned parser emits native scalar records, never external serialized data.
static_assert(sizeof(float) == 4 && sizeof(int) == 4 && sizeof(unsigned short) == 2);
enum Command : unsigned char { nop, move, draw, dot, select_pen, pen_width, pen_color, line_attr };
struct Point { float x{}, y{}; };
struct Pen { float width{0.1F}; std::array<unsigned, 3> color{}; };

class Records final {
  public:
    explicit Records(const VoveHpglParsed &parsed) : bytes_(parsed.commands, parsed.size) {}
    [[nodiscard]] bool empty() const { return bytes_.empty(); }
    template <typename T> T next() {
        if (bytes_.size() < sizeof(T)) throw Status::malformed;
        T value;
        std::memcpy(&value, bytes_.data(), sizeof(T));
        bytes_ = bytes_.subspan(sizeof(T));
        return value;
    }
  private:
    std::span<const unsigned char> bytes_;
};

void append(std::string &output, std::string_view text) {
    if (text.size() > kMaximumSvgBytes - output.size()) throw Status::resource_limit;
    output.append(text);
}

void number(std::string &output, double value) {
    if (!std::isfinite(value)) throw Status::malformed;
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general, 9);
    if (result.ec != std::errc{}) throw Status::resource_limit;
    append(output, std::string_view(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())));
}

void coordinates(std::string &output, Point point) {
    number(output, point.x);
    append(output, " ");
    number(output, -static_cast<double>(point.y));
}

std::string rgb(const Pen &pen) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string value = "#000000";
    for (std::size_t i = 0; i < pen.color.size(); ++i) {
        if (pen.color[i] > 255) throw Status::malformed;
        value[1 + i * 2] = hex[pen.color[i] >> 4U];
        value[2 + i * 2] = hex[pen.color[i] & 15U];
    }
    return value;
}

std::string serialize(const VoveHpglParsed &parsed) {
    if (parsed.size > kMaximumIntermediateBytes) throw Status::resource_limit;
    Records records(parsed);
    std::array<Pen, 256> pens{};
    pens[0].width = 0;
    constexpr std::array<std::array<unsigned, 3>, 8> colors{{
        {255,255,255}, {0,0,0}, {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255}, {255,0,255}, {255,255,0}}};
    for (std::size_t i = 0; i < colors.size(); ++i) pens[i].color = colors[i];
    unsigned active_pen{};
    int cap = 1, join = 1, miter = 5;
    Point position{};
    double xmin = std::numeric_limits<double>::infinity(), ymin = xmin;
    double xmax = -xmin, ymax = -xmin;
    std::optional<std::array<double, 2>> direction;
    std::string body, path;
    bool ink = false;
    const auto bounds = [&](double x, double y, double margin) {
        if (!std::isfinite(x) || !std::isfinite(y)) throw Status::malformed;
        xmin = std::min(xmin, x - margin);
        xmax = std::max(xmax, x + margin);
        ymin = std::min(ymin, y - margin);
        ymax = std::max(ymax, y + margin);
    };
    const auto flush = [&] {
        if (ink) {
            append(body, "<path fill=\"none\" stroke=\"");
            append(body, rgb(pens[active_pen]));
            append(body, "\" stroke-width=\"");
            number(body, static_cast<double>(pens[active_pen].width) * 40);
            append(body, "\" stroke-linecap=\"");
            append(body, cap == 4 ? "round" : cap == 2 ? "square" : "butt");
            append(body, "\" stroke-linejoin=\"");
            append(body, join == 4 ? "round" : join == 5 ? "bevel" : "miter");
            append(body, "\" stroke-miterlimit=\"");
            number(body, miter);
            append(body, "\" d=\"");
            append(body, path);
            append(body, "\"/>");
        }
        path.clear();
        ink = false;
        direction.reset();
    };
    while (!records.empty()) {
        const auto command = records.next<unsigned char>();
        switch (command) {
        case nop: break;
        case move:
            position = records.next<Point>();
            direction.reset();
            append(path, "M"); coordinates(path, position); append(path, " ");
            break;
        case draw: {
            const auto next = records.next<Point>();
            if (pens[active_pen].width > 0) {
                const auto radius = static_cast<double>(pens[active_pen].width) * 20;
                const auto margin = cap == 2 ? radius * std::sqrt(2.0) : radius;
                bounds(position.x, position.y, margin); bounds(next.x, next.y, margin);
                const double dx = static_cast<double>(next.x) - position.x;
                const double dy = static_cast<double>(next.y) - position.y;
                const auto length = std::hypot(dx, dy);
                if (length > 0) {
                    const std::array unit{dx / length, dy / length};
                    if (join == 1 && direction) {
                        const auto denominator = 1 + (*direction)[0] * unit[0] + (*direction)[1] * unit[1];
                        if (denominator > 1e-12) {
                            const auto mx = -((*direction)[1] + unit[1]) * radius / denominator;
                            const auto my = ((*direction)[0] + unit[0]) * radius / denominator;
                            if (std::hypot(mx, my) <= radius * miter) {
                                bounds(position.x + mx, position.y + my, 0);
                                bounds(position.x - mx, position.y - my, 0);
                            }
                        }
                    }
                    direction = unit;
                }
                if (path.empty()) { append(path, "M"); coordinates(path, position); }
                append(path, "L"); coordinates(path, next); append(path, " ");
                ink = true;
            }
            position = next;
            break;
        }
        case dot: {
            flush();
            position = records.next<Point>();
            if (pens[active_pen].width <= 0) break;
            bounds(position.x, position.y, static_cast<double>(pens[active_pen].width) * 20);
            append(body, "<circle cx=\""); number(body, position.x);
            append(body, "\" cy=\""); number(body, -static_cast<double>(position.y));
            append(body, "\" r=\""); number(body, static_cast<double>(pens[active_pen].width) * 20);
            append(body, "\" fill=\""); append(body, rgb(pens[active_pen])); append(body, "\"/>");
            break;
        }
        case select_pen: {
            const auto pen = records.next<unsigned char>();
            if (active_pen != pen) flush();
            active_pen = pen;
            break;
        }
        case pen_width: {
            const auto pen = records.next<std::uint16_t>();
            const auto width = records.next<float>();
            if (pen >= pens.size() || !std::isfinite(width) || width < 0) throw Status::malformed;
            if ((pen == active_pen || (pen == 0 && active_pen != 0)) && pens[active_pen].width != width) flush();
            if (pen == 0) { for (std::size_t i = 1; i < pens.size(); ++i) pens[i].width = width; }
            else pens[pen].width = width;
            break;
        }
        case pen_color: {
            const auto pen = records.next<std::uint16_t>();
            if (pen >= pens.size()) throw Status::malformed;
            std::array<unsigned, 3> color{};
            for (auto &channel : color) {
                channel = records.next<std::uint16_t>();
                if (channel > 255) throw Status::malformed;
            }
            if (pen == active_pen && pens[pen].color != color) flush();
            pens[pen].color = color;
            break;
        }
        case line_attr: {
            const auto kind = records.next<int>();
            const auto value = records.next<int>();
            int *attribute = nullptr;
            if (kind == 0 && (value == 1 || value == 2 || value == 4)) attribute = &cap;
            else if (kind == 1 && (value == 1 || value == 4 || value == 5)) attribute = &join;
            else if (kind == 2 && value >= 1 && value <= 1000) attribute = &miter;
            if (!attribute) throw Status::unsupported;
            if (*attribute != value) flush();
            *attribute = value;
            break;
        }
        default: throw Status::malformed;
        }
        if (body.size() + path.size() > kMaximumSvgBytes) throw Status::resource_limit;
    }
    flush();
    if (body.empty() || !std::isfinite(xmin)) throw Status::unsupported;
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"";
    number(svg, xmin - 1); append(svg, " "); number(svg, -ymax - 1);
    append(svg, " "); number(svg, xmax - xmin + 2);
    append(svg, " "); number(svg, ymax - ymin + 2);
    append(svg, "\">"); append(svg, body); append(svg, "</svg>");
    return svg;
}

} // namespace

Result render_svg(const std::span<const std::byte> source) {
    if (source.size() > kMaximumSourceBytes) return {Status::resource_limit, {}, "HPGL source exceeds 32 MiB"};
    if (source.empty()) return {Status::malformed, {}, "HPGL source is empty"};
    static std::mutex parser_mutex;
    const std::lock_guard guard(parser_mutex);
    VoveHpglParsed parsed{};
    struct Release { VoveHpglParsed &value; ~Release() { vove_hpgl_release(&value); } } release{parsed};
    try {
        const auto status = vove_hpgl_parse(reinterpret_cast<const unsigned char *>(source.data()), source.size(), &parsed);
        if (status != VOVE_HPGL_OK) {
            return {status == VOVE_HPGL_LIMIT ? Status::resource_limit : status == VOVE_HPGL_UNSUPPORTED ? Status::unsupported : Status::malformed,
                    {}, "HPGL contains unsupported, damaged or excessive plot commands"};
        }
        return {Status::success, serialize(parsed), {}};
    } catch (const Status status) {
        return {status, {}, "HPGL geometry or style is unsupported, damaged or exceeds preview limits"};
    } catch (const std::bad_alloc &) {
        return {Status::resource_limit, {}, "HPGL preview memory limit reached"};
    }
}

} // namespace vove::handlers::hpgl
