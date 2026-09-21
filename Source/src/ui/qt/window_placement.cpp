#include "window_placement.hpp"

#include <QRegion>

#include <algorithm>
#include <cstdint>

namespace vove::ui {
namespace {

std::int64_t area(const QRect &rectangle) noexcept {
    return rectangle.isValid() ? static_cast<std::int64_t>(rectangle.width()) *
                                     static_cast<std::int64_t>(rectangle.height())
                               : 0;
}

long double distance_squared(const QPoint &left, const QPoint &right) noexcept {
    const auto dx = static_cast<long double>(left.x()) - right.x();
    const auto dy = static_cast<long double>(left.y()) - right.y();
    return dx * dx + dy * dy;
}

} // namespace

QRect visible_window_geometry(const QRect &requested,
                              const QList<QRect> &available_screens) noexcept {
    if (!requested.isValid()) {
        return requested;
    }

    QList<QRect> screens;
    screens.reserve(available_screens.size());
    for (const auto &screen : available_screens) {
        if (screen.isValid()) {
            screens.push_back(screen);
        }
    }
    if (screens.isEmpty()) {
        return requested;
    }

    QRegion visible_region;
    bool title_anchor_visible{};
    const QPoint title_anchor(requested.center().x(), requested.top());
    for (const auto &screen : screens) {
        visible_region += requested.intersected(screen);
        title_anchor_visible = title_anchor_visible || screen.contains(title_anchor);
    }
    std::int64_t visible_area{};
    for (const auto &visible_rectangle : visible_region) {
        visible_area += area(visible_rectangle);
    }
    const auto requested_area = area(requested);
    if (title_anchor_visible && visible_area >= requested_area * 4 / 5) {
        return requested;
    }

    const QRect *target = &screens.front();
    auto best_intersection = area(requested.intersected(*target));
    auto best_distance = distance_squared(requested.center(), target->center());
    for (const auto &screen : screens) {
        const auto intersection = area(requested.intersected(screen));
        const auto distance = distance_squared(requested.center(), screen.center());
        if (intersection > best_intersection ||
            (intersection == best_intersection && distance < best_distance)) {
            target = &screen;
            best_intersection = intersection;
            best_distance = distance;
        }
    }

    const QSize fitted_size(std::min(requested.width(), target->width()),
                            std::min(requested.height(), target->height()));
    const auto maximum_x = target->right() - fitted_size.width() + 1;
    const auto maximum_y = target->bottom() - fitted_size.height() + 1;
    return {{std::clamp(requested.x(), target->left(), maximum_x),
             std::clamp(requested.y(), target->top(), maximum_y)},
            fitted_size};
}

} // namespace vove::ui
