#include "add_favorite_button.hpp"

#include <QPainter>

namespace vove::ui {

AddFavoriteButton::AddFavoriteButton(QWidget *parent) : QToolButton(parent) {
    setFixedSize(31, 24);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
}

void AddFavoriteButton::paintEvent(QPaintEvent *) {
    constexpr auto passive_ink = QRgb{0xFFB2B2B2U};
    constexpr auto active_ink = QRgb{0xFF5BA9B3U};
    constexpr auto active_frame = QRgb{0xFFFF7F00U};

    const auto active = isEnabled() && (underMouse() || isDown());
    auto ink = QColor::fromRgba(active ? active_ink : passive_ink);
    if (!isEnabled()) {
        ink.setAlpha(96);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto center = QRectF(rect()).center();
    if (active) {
        painter.setPen(QPen(QColor::fromRgba(active_frame), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(center.x() - 9.5, center.y() - 9.5, 19.0, 19.0), 4.0, 4.0);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawRect(QRectF(center.x() - 8.0, center.y() - 1.0, 16.0, 2.0));
    painter.drawRect(QRectF(center.x() - 1.0, center.y() - 8.0, 2.0, 16.0));
}

} // namespace vove::ui
