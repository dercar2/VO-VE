#include "theme_switch.hpp"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QTransform>
#include <QWidget>

#include <algorithm>
#include <array>

namespace vove::ui {
namespace {

// Original Bezier contours and RGB colors from design/theme-connector.ai.
constexpr qreal stroke = 1.417;
constexpr qreal first_center = 10.2843;
constexpr qreal center_step = 20.8723;
constexpr qreal swatch_bottom = 1.1153;

const QPainterPath &capsule_path() {
    static const auto path = [] {
        QPainterPath result;
        result.moveTo(0, 0);
        result.lineTo(-62.913, 0);
        result.cubicTo(-68.552, 0, -73.166, 4.614, -73.166, 10.253);
        result.cubicTo(-73.166, 15.892, -68.552, 20.505, -62.913, 20.505);
        result.lineTo(0, 20.505);
        result.cubicTo(5.639, 20.505, 10.253, 15.892, 10.253, 10.253);
        result.cubicTo(10.253, 4.614, 5.639, 0, 0, 0);
        result.closeSubpath();
        return QTransform::fromTranslate(73.166, 0).map(result);
    }();
    return path;
}

const QPainterPath &swatch_path() {
    static const auto path = [] {
        QPainterPath result;
        result.moveTo(0, 0);
        result.cubicTo(-5, 0, -9.053, 4.053, -9.053, 9.053);
        result.lineTo(-9.053, 9.222);
        result.cubicTo(-9.053, 14.222, -5, 18.275, 0, 18.275);
        result.cubicTo(5, 18.275, 9.053, 14.222, 9.053, 9.222);
        result.lineTo(9.053, 9.053);
        result.cubicTo(9.053, 4.053, 5, 0, 0, 0);
        result.closeSubpath();
        return result;
    }();
    return path;
}

class SwatchButton final : public QToolButton {
  public:
    explicit SwatchButton(QWidget *parent) : QToolButton(parent) {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        // The parent paints the artwork. Only keyboard focus is an overlay.
        if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(palette().color(QPalette::WindowText), 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(rect()).adjusted(4, 4, -4, -4));
        }
    }
};

class ThemeSwitch final : public QWidget {
  public:
    ThemeSwitch(QButtonGroup *buttons, QWidget *parent) : QWidget(parent), buttons_(buttons) {
        setObjectName(QStringLiteral("themeSwitch"));
        setFixedSize(72, 18);
        setLayoutDirection(Qt::LeftToRight);
        buttons_->setExclusive(true);
        const std::array names{"themeWhite", "themeVanilla", "themeBlue", "themeDarkGray"};
        // Preserve translation-pack identities; the shell applies the branded names afterward.
        const std::array labels{QCoreApplication::translate("MainWindow", "White theme"),
                                QCoreApplication::translate("MainWindow", "Vanilla theme"),
                                QCoreApplication::translate("MainWindow", "Blue theme"),
                                QCoreApplication::translate("MainWindow", "Dark gray theme")};
        for (int index = 0; index < static_cast<int>(names.size()); ++index) {
            auto *button = new SwatchButton(this);
            button->setObjectName(QString::fromLatin1(names[static_cast<std::size_t>(index)]));
            button->setToolTip(labels[static_cast<std::size_t>(index)]);
            button->setAccessibleName(labels[static_cast<std::size_t>(index)]);
            const auto bounds = swatch_path().boundingRect().adjusted(-stroke / 2, -stroke / 2,
                                                                      stroke / 2, stroke / 2);
            button->setGeometry(
                artwork_transform()
                    .mapRect(bounds.translated(first_center + center_step * index, swatch_bottom))
                    .toAlignedRect());
            buttons_->addButton(button, index);
        }
        connect(buttons_, &QButtonGroup::idToggled, this, [this] { update(); });
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        const auto selected = std::clamp(buttons_->checkedId(), 0, 3);
        const auto vanilla = QColor::fromRgbF(0.945F, 0.894F, 0.733F);
        const auto blue = QColor::fromRgbF(0.129F, 0.22F, 0.251F);
        const std::array fills{QColor(242, 242, 242), vanilla, blue, QColor(51, 51, 51)};
        const std::array backgrounds{QColor(51, 51, 51), blue, vanilla, QColor(128, 128, 128)};
        const std::array outlines{QColor(204, 204, 204), vanilla, blue, QColor(77, 77, 77)};
        const auto orange = QColor::fromRgbF(1, 0.498F, 0);
        const auto theme = static_cast<std::size_t>(selected);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(artwork_transform());
        painter.setBrush(backgrounds[theme]);
        painter.setPen(QPen(backgrounds[theme], selected < 2 ? 1 : stroke));
        painter.drawPath(capsule_path());
        for (int index = 0; index < static_cast<int>(fills.size()); ++index) {
            painter.save();
            painter.translate(first_center + center_step * index, swatch_bottom);
            painter.setBrush(fills[static_cast<std::size_t>(index)]);
            painter.setPen(QPen(index == selected ? orange : outlines[theme], stroke));
            painter.drawPath(swatch_path());
            painter.restore();
        }
    }

  private:
    QTransform artwork_transform() const {
        const auto scale = std::min(width() / (83.419 + stroke), height() / (20.505 + stroke));
        QTransform transform;
        transform.translate(width() / 2.0, height() / 2.0);
        transform.scale(scale, -scale);
        transform.translate(-83.419 / 2, -20.505 / 2);
        return transform;
    }

    QButtonGroup *buttons_;
};

} // namespace

QWidget *create_theme_switch(QButtonGroup *buttons, QWidget *parent) {
    return new ThemeSwitch(buttons, parent);
}

} // namespace vove::ui
