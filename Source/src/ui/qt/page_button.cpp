#include "page_button.hpp"

#include <QPainter>
#include <QPainterPath>

#include <array>

namespace vove::ui {
namespace {

struct Inks {
    QRgb fill;
    QRgb ring;
    QRgb arrow;
};

// RGB fills and vector dimensions from the author's page-navigation artwork.
constexpr std::array inks{Inks{qRgb(178, 178, 178), qRgb(91, 91, 91), qRgb(242, 242, 242)},
                          Inks{qRgb(0, 50, 68), qRgb(82, 106, 112), qRgb(241, 228, 187)},
                          Inks{qRgb(91, 169, 179), qRgb(235, 200, 160), qRgb(241, 228, 187)},
                          Inks{qRgb(91, 91, 91), qRgb(178, 178, 178), qRgb(178, 178, 178)}};

const QPainterPath &disc() {
    static const auto path = [] {
        QPainterPath result;
        result.moveTo(-10, 0);
        result.cubicTo(-10, -5.523, -5.523, -10, 0, -10);
        result.cubicTo(5.523, -10, 10, -5.523, 10, 0);
        result.cubicTo(10, 5.523, 5.523, 10, 0, 10);
        result.cubicTo(-5.523, 10, -10, 5.523, -10, 0);
        result.closeSubpath();
        return result;
    }();
    return path;
}

} // namespace

PageButton::PageButton(const PageDirection direction, const QString &tooltip, QWidget *parent)
    : QToolButton(parent), direction_(direction) {
    setFixedSize(28, 28);
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tooltip);
    setAccessibleName(tooltip);
}

void PageButton::set_theme(const AppTheme theme) {
    theme_ = theme;
    update();
}

void PageButton::paintEvent(QPaintEvent *) {
    const auto colors = inks.at(static_cast<std::size_t>(theme_));
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    if (!isEnabled()) {
        painter.setOpacity(0.4);
    }
    painter.setPen(QPen(QColor(colors.ring), static_cast<int>(theme_) == 1 ? 1.417 : 1.0));
    painter.setBrush(QColor(colors.fill));
    painter.drawPath(disc());
    if ((direction_ == PageDirection::next) != isRightToLeft()) {
        painter.rotate(180);
    }
    QPainterPath arrow;
    arrow.moveTo(1.8204, -5.837);
    arrow.lineTo(1.8204, 5.837);
    arrow.lineTo(-4.0166, 0);
    arrow.closeSubpath();
    painter.fillPath(arrow, QColor(colors.arrow));
    if (isEnabled() && (underMouse() || isDown() ||
                        (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)))) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(QStringLiteral("#F5A623")), 1.4, Qt::SolidLine));
        painter.drawEllipse(QRectF(-12.5, -12.5, 25, 25));
    }
}

} // namespace vove::ui
