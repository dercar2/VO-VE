#include "scrollbar_style.hpp"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleOptionMenuItem>
#include <QStyleOptionSlider>
#include <QVariant>

#include <algorithm>
#include <array>

namespace vove::ui {
namespace {

constexpr auto ink_property = "voveScrollbarInk";
// Original dimensions from design/scrollbar.ai, fitted to a 20-point-wide control.
constexpr qreal rail_width = 1.417;
constexpr qreal rail_spacing = 18.5806;
constexpr qreal handle_width = 9.806;

class ScrollbarStyle final : public QProxyStyle {
  public:
    ScrollbarStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    int pixelMetric(const PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
        if (metric == PM_ScrollBarExtent) {
            return 20;
        }
        if (metric == PM_ScrollBarSliderMin) {
            return 28;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    int styleHint(const StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *return_data) const override {
        if (hint == SH_ScrollBar_Transient) {
            return 0;
        }
#ifdef Q_OS_LINUX
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons) {
            return 0;
        }
#endif
        return QProxyStyle::styleHint(hint, option, widget, return_data);
    }

    void drawControl(const ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget) const override {
        const auto *menu = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        const auto tab = menu ? menu->text.indexOf(QLatin1Char('\t')) : -1;
        if (element != CE_MenuItem || !menu || tab < 0 ||
            menu->menuItemType == QStyleOptionMenuItem::Separator) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        // Keep native menu chrome, command labels and sizing; draw only the shortcut separately.
        auto command = *menu;
        command.text.truncate(tab);
        QProxyStyle::drawControl(element, &command, painter, widget);
        const bool selected = menu->state.testFlag(State_Enabled) && menu->state.testFlag(State_Selected);
        const auto group = menu->state.testFlag(State_Enabled) ? QPalette::Active : QPalette::Disabled;
        const auto foreground = menu->palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        const auto background = menu->palette.color(group, selected ? QPalette::Highlight : QPalette::Window);
        constexpr qreal strength = 0.65;
        const auto blend = [](int ink, int paper) { return qRound(ink * strength + paper * (1 - strength)); };
        const QColor muted(blend(foreground.red(), background.red()),
                           blend(foreground.green(), background.green()),
                           blend(foreground.blue(), background.blue()));
        const auto margin = std::max(8, pixelMetric(PM_MenuHMargin, menu, widget) +
            pixelMetric(PM_MenuPanelWidth, menu, widget) + pixelMetric(PM_MenuButtonIndicator, menu, widget));
        const auto shortcut = menu->text.mid(tab + 1);
        const auto width = std::max(menu->reservedShortcutWidth, menu->fontMetrics.horizontalAdvance(shortcut));
        const auto logical = QRect(menu->rect.right() - margin - width + 1,
                                   menu->rect.top(), width, menu->rect.height());
        const auto bounds = visualRect(menu->direction, menu->rect, logical);
        painter->save();
        painter->setClipRect(bounds, Qt::IntersectClip);
        painter->setFont(menu->font);
        painter->setPen(muted);
        painter->drawText(bounds, static_cast<int>(Qt::AlignVCenter |
            (menu->direction == Qt::RightToLeft ? Qt::AlignLeft : Qt::AlignRight)), shortcut);
        painter->restore();
    }

    void drawPrimitive(const PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget) const override {
        if (element != PE_IndicatorBranch || widget == nullptr ||
            widget->objectName() != QStringLiteral("directoryTree")) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        if (!option->state.testFlag(State_Children)) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(QRectF(option->rect).center());
        const auto scale = std::min(qreal(1), option->rect.width() / qreal(16));
        painter->scale(scale, scale);
        painter->rotate(option->state.testFlag(State_Open)
                            ? 180
                            : (option->direction == Qt::RightToLeft ? -90 : 90));
        QPainterPath arrow;
        arrow.moveTo(-5.837, 2.9185);
        arrow.lineTo(5.837, 2.9185);
        arrow.lineTo(0, -2.9185);
        arrow.closeSubpath();
        const auto ink = qApp->property(ink_property).value<QColor>();
        painter->fillPath(arrow, ink.isValid() ? ink : option->palette.color(QPalette::WindowText));
        painter->restore();
    }

    void drawComplexControl(const ComplexControl control, const QStyleOptionComplex *option,
                            QPainter *painter, const QWidget *widget) const override {
        const auto *scroll = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (control != CC_ScrollBar || scroll == nullptr) {
            QProxyStyle::drawComplexControl(control, option, painter, widget);
            return;
        }
        const auto bounds = QRectF(scroll->rect);
        const bool vertical = scroll->orientation == Qt::Vertical;
        const auto cross_size = vertical ? bounds.width() : bounds.height();
        if (bounds.isEmpty() || cross_size <= 0) {
            return;
        }
        const auto scale = std::min(qreal(1), cross_size / 20);
        const auto center = bounds.center();
        const auto configured_ink = qApp->property(ink_property).value<QColor>();
        const auto ink =
            configured_ink.isValid() ? configured_ink : scroll->palette.color(QPalette::WindowText);
        painter->save();
        painter->setClipRect(bounds, Qt::IntersectClip);
        painter->fillRect(bounds, scroll->palette.color(QPalette::Window));
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(ink, rail_width * scale));
        // The rails span the whole bar, including both arrow-button regions.
        for (const auto side : {-1, 1}) {
            // An adjacent splitter supplies this rail; do not double its visible thickness.
            if (vertical && widget != nullptr && widget->property("voveOuterRailShared").toBool() &&
                side == (scroll->direction == Qt::RightToLeft ? -1 : 1)) {
                continue;
            }
            const auto offset = side * rail_spacing * scale / 2;
            if (vertical) {
                painter->drawLine(QPointF(center.x() + offset, bounds.top()),
                                  QPointF(center.x() + offset, bounds.bottom()));
            } else {
                painter->drawLine(QPointF(bounds.left(), center.y() + offset),
                                  QPointF(bounds.right(), center.y() + offset));
            }
        }
        const bool enabled =
            scroll->state.testFlag(State_Enabled) && scroll->maximum > scroll->minimum;
        if (enabled && scroll->subControls.testFlag(SC_ScrollBarSlider)) {
            auto handle = QRectF(subControlRect(CC_ScrollBar, scroll, SC_ScrollBarSlider, widget));
            if (vertical) {
                handle.setLeft(center.x() - handle_width * scale / 2);
                handle.setWidth(handle_width * scale);
                handle.adjust(0, 1, 0, -1);
            } else {
                handle.setTop(center.y() - handle_width * scale / 2);
                handle.setHeight(handle_width * scale);
                handle.adjust(1, 0, -1, 0);
            }
            if (!handle.isEmpty()) {
                const auto radius = std::min(handle.width(), handle.height()) / 2;
                painter->setPen(Qt::NoPen);
                painter->setBrush(ink);
                painter->drawRoundedRect(handle, radius, radius);
                if (scroll->state.testFlag(State_HasFocus)) {
                    painter->setPen(QPen(scroll->palette.color(QPalette::Window), 1, Qt::DotLine));
                    painter->setBrush(Qt::NoBrush);
                    painter->drawRoundedRect(handle.adjusted(2, 2, -2, -2),
                                             std::max(qreal(0), radius - 2),
                                             std::max(qreal(0), radius - 2));
                }
            }
        }
        for (const auto part : {SC_ScrollBarSubLine, SC_ScrollBarAddLine}) {
            if (!scroll->subControls.testFlag(part)) {
                continue;
            }
            const auto arrow_rect = QRectF(subControlRect(CC_ScrollBar, scroll, part, widget));
            if (arrow_rect.isEmpty()) {
                continue;
            }
            const bool add = part == SC_ScrollBarAddLine;
            const bool available = enabled && (add ? scroll->sliderValue < scroll->maximum
                                                   : scroll->sliderValue > scroll->minimum);
            painter->save();
            painter->setClipRect(arrow_rect, Qt::IntersectClip);
            painter->setOpacity(available ? 1 : 0.35);
            painter->translate(arrow_rect.center());
            const auto arrow_scale =
                std::min({scale, arrow_rect.width() / 12, arrow_rect.height() / 12});
            painter->scale(arrow_scale, arrow_scale);
            painter->rotate(vertical
                                ? (add ? 180 : 0)
                                : ((add != (scroll->direction == Qt::RightToLeft)) ? 90 : -90));
            QPainterPath arrow;
            arrow.moveTo(-5.837, 2.9185);
            arrow.lineTo(5.837, 2.9185);
            arrow.lineTo(0, -2.9185);
            arrow.closeSubpath();
            painter->fillPath(arrow, ink);
            painter->restore();
        }
        painter->restore();
    }
};

} // namespace

QStyle *create_application_style() {
    return new ScrollbarStyle;
}

void apply_scrollbar_theme(const AppTheme theme) {
    // Shared application appearance, like the existing global palette and stylesheet.
    constexpr std::array inks{qRgb(91, 91, 91), qRgb(47, 82, 91), qRgb(241, 228, 187),
                              qRgb(178, 178, 178)};
    qApp->setProperty(ink_property, QColor(inks.at(static_cast<std::size_t>(theme))));
}

} // namespace vove::ui
