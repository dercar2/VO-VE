#pragma once

#include <QPainterPath>

namespace vove::ui::artwork {

// Shared authored contour in navigation-controls.ai and create-folder.ai.
inline const QPainterPath &round_button_ring() {
    static const auto path = [] {
        QPainterPath ring;
        ring.setFillRule(Qt::OddEvenFill);
        ring.moveTo(0, -9.292);
        ring.cubicTo(-5.123, -9.292, -9.292, -5.124, -9.292, 0);
        ring.cubicTo(-9.292, 5.123, -5.123, 9.291, 0, 9.291);
        ring.cubicTo(5.123, 9.291, 9.292, 5.123, 9.292, 0);
        ring.cubicTo(9.292, -5.124, 5.123, -9.292, 0, -9.292);
        ring.closeSubpath();
        ring.moveTo(0, 10.708);
        ring.cubicTo(-5.905, 10.708, -10.708, 5.904, -10.708, 0);
        ring.cubicTo(-10.708, -5.905, -5.905, -10.709, 0, -10.709);
        ring.cubicTo(5.905, -10.709, 10.708, -5.905, 10.708, 0);
        ring.cubicTo(10.708, 5.904, 5.905, 10.708, 0, 10.708);
        ring.closeSubpath();
        return ring;
    }();
    return path;
}

} // namespace vove::ui::artwork
