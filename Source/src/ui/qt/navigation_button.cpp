#include "navigation_button.hpp"
#include "round_button_artwork.hpp"

#include <QPainter>
#include <QPainterPath>

#include <array>

namespace vove::ui {
namespace {

struct Inks {
    QRgb arrow;
    QRgb ring;
};

struct NavigationPalette {
    Inks normal;
    Inks active;
    Inks up_normal;
    Inks up_active;
    Inks disabled;
};

// Authored fills from design/navigation-controls.ai; Up has its own ring colors.
constexpr std::array palettes{NavigationPalette{{qRgb(91, 91, 91), qRgb(91, 91, 91)},
                                                {qRgb(237, 196, 95), qRgb(91, 91, 91)},
                                                {qRgb(91, 91, 91), qRgb(178, 178, 178)},
                                                {qRgb(237, 196, 95), qRgb(178, 178, 178)},
                                                {qRgb(178, 178, 178), qRgb(178, 178, 178)}},
                              NavigationPalette{{qRgb(93, 165, 181), qRgb(33, 56, 64)},
                                                {qRgb(255, 127, 0), qRgb(142, 179, 193)},
                                                {qRgb(4, 63, 97), qRgb(93, 165, 181)},
                                                {qRgb(255, 127, 0), qRgb(93, 165, 181)},
                                                {qRgb(186, 175, 145), qRgb(82, 106, 112)}},
                              NavigationPalette{{qRgb(241, 228, 187), qRgb(91, 169, 179)},
                                                {qRgb(255, 127, 0), qRgb(91, 169, 179)},
                                                {qRgb(91, 169, 179), qRgb(241, 228, 187)},
                                                {qRgb(255, 127, 0), qRgb(241, 228, 187)},
                                                {qRgb(82, 106, 112), qRgb(82, 106, 112)}},
                              NavigationPalette{{qRgb(178, 178, 178), qRgb(178, 178, 178)},
                                                {qRgb(93, 165, 181), qRgb(178, 178, 178)},
                                                {qRgb(178, 178, 178), qRgb(91, 91, 91)},
                                                {qRgb(93, 165, 181), qRgb(91, 91, 91)},
                                                {qRgb(91, 91, 91), qRgb(91, 91, 91)}}};

} // namespace

NavigationButton::NavigationButton(const NavigationDirection direction, const QString &label,
                                   QWidget *parent)
    : QToolButton(parent), direction_(direction) {
    setFixedSize(28, 28);
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(label);
    setAccessibleName(label);
}

void NavigationButton::set_theme(const AppTheme theme) {
    theme_ = theme;
    update();
}

void NavigationButton::paintEvent(QPaintEvent *) {
    const auto &palette = palettes.at(static_cast<std::size_t>(theme_));
    const auto up = direction_ == NavigationDirection::up;
    const auto active = underMouse() || isDown() ||
                        (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange));
    const auto inks = !isEnabled() ? palette.disabled
                      : up         ? (active ? palette.up_active : palette.up_normal)
                                   : (active ? palette.active : palette.normal);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.fillPath(artwork::round_button_ring(), QColor(inks.ring));
    int rotation = direction_ == NavigationDirection::forward   ? 180
                   : direction_ == NavigationDirection::history ? -90
                   : up                                         ? 90
                                                                : 0;
    if ((direction_ == NavigationDirection::back || direction_ == NavigationDirection::forward) &&
        layoutDirection() == Qt::RightToLeft) {
        rotation += 180;
    }
    painter.rotate(rotation);
    QPainterPath triangle;
    triangle.moveTo(1.8206, -5.837);
    triangle.lineTo(1.8206, 5.837);
    triangle.lineTo(-4.0164, 0);
    triangle.closeSubpath();
    painter.fillPath(triangle, QColor(inks.arrow));
}

} // namespace vove::ui
