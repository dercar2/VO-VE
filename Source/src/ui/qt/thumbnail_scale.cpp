#include "thumbnail_scale.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>

#include <array>

namespace vove::ui {
namespace {

// Logical geometry and RGB inks from design/plus-minus.ai, not font glyphs.
constexpr qreal artwork_height = 23.225;
constexpr qreal ring_radius = 10.709;
constexpr qreal line_width = 1.417;
constexpr auto lineInkProperty = "voveScaleLineInk";
constexpr auto symbolInkProperty = "voveScaleSymbolInk";
constexpr auto handleInkProperty = "voveScaleHandleInk";
constexpr auto shadowInkProperty = "voveScaleShadowInk";

QColor authored_color(const QWidget *widget, const char *name,
                       const QPalette::ColorRole fallback) {
    auto color = widget->property(name).value<QColor>();
    return color.isValid() ? color : widget->palette().color(fallback);
}

bool keyboard_focus(const QWidget *widget) {
    return widget->hasFocus() && widget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
}

class StepButton final : public QToolButton {
  public:
    StepButton(const bool increase, QWidget *parent) : QToolButton(parent), increase_(increase) {
        setFixedSize(24, 28);
        setAutoRaise(true);
        setFocusPolicy(Qt::StrongFocus);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(width() / 2.0, height() / 2.0);
        if (!isEnabled()) {
            painter.setOpacity(0.4);
        }
        QPainterPath ring;
        ring.setFillRule(Qt::OddEvenFill);
        ring.addEllipse(QPointF{}, ring_radius, ring_radius);
        ring.addEllipse(QPointF{}, 9.291, 9.291);
        painter.fillPath(ring, authored_color(this, lineInkProperty, QPalette::Mid));
        if (isDown() || underMouse()) {
            auto highlight =
                authored_color(this, symbolInkProperty, QPalette::ButtonText);
            highlight.setAlpha(isDown() ? 70 : 30);
            painter.setPen(Qt::NoPen);
            painter.setBrush(highlight);
            painter.drawEllipse(QPointF{}, 9.291, 9.291);
        }
        const auto ink = authored_color(this, symbolInkProperty, QPalette::ButtonText);
        painter.fillRect(QRectF(-7.7415, -line_width / 2, 15.483, line_width), ink);
        if (increase_) {
            painter.fillRect(QRectF(-line_width / 2, -7.7415, line_width, 15.483), ink);
        }
        if (keyboard_focus(this)) {
            painter.setPen(QPen(ink, 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF{}, 8, 8);
        }
    }

  private:
    bool increase_;
};

class ScaleSlider final : public QSlider {
  public:
    explicit ScaleSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
        setFixedHeight(28);
        setMinimumWidth(60);
        setMaximumWidth(170);
        setFocusPolicy(Qt::StrongFocus);
        // Qt still owns all input and RTL behavior. Its handle hit area includes the shadow.
        setStyleSheet(
            QStringLiteral("QSlider::groove:horizontal { height: 1px; }"
                           "QSlider::handle:horizontal { width: 17px; margin: -11px 0; }"));
    }

    QSize sizeHint() const override {
        return {104, 28};
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const auto handle =
            style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(0, (height() - artwork_height) / 2);
        if (!isEnabled()) {
            painter.setOpacity(0.4);
        }
        painter.fillRect(QRectF(0, 11, width(), line_width),
                         authored_color(this, lineInkProperty, QPalette::Mid));
        const auto left = handle.left() + (handle.width() - 16.947) / 2 + 3.387;
        const auto shadow = authored_color(this, shadowInkProperty, QPalette::Shadow);
        painter.fillRect(QRectF(left, 0, 10.926, artwork_height), shadow);
        painter.fillRect(QRectF(left - 3.387, 11, 16.947, line_width), shadow);
        painter.fillRect(QRectF(left, 0, 9.159, 21.417),
                         authored_color(this, handleInkProperty, QPalette::Highlight));
        if (keyboard_focus(this)) {
            painter.setPen(QPen(palette().color(QPalette::Shadow), 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(left + 1.5, 1.5, 6.159, 18.417));
        }
    }
};

} // namespace

ThumbnailScaleControls create_thumbnail_scale(QWidget *parent) {
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("thumbnailScale"));
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *smaller = new StepButton(false, widget);
    smaller->setObjectName(QStringLiteral("smallerThumbnails"));
    smaller->setToolTip(QCoreApplication::translate("MainWindow", "Smaller thumbnails"));
    auto *slider = new ScaleSlider(widget);
    slider->setObjectName(QStringLiteral("thumbnailSize"));
    slider->setRange(64, 320);
    slider->setPageStep(32);
    slider->setToolTip(QCoreApplication::translate("MainWindow", "Thumbnail size"));
    auto *larger = new StepButton(true, widget);
    larger->setObjectName(QStringLiteral("largerThumbnails"));
    larger->setToolTip(QCoreApplication::translate("MainWindow", "Larger thumbnails"));
    layout->addWidget(smaller);
    layout->addWidget(slider);
    layout->addWidget(larger);
    return {widget, smaller, slider, larger};
}

void apply_thumbnail_scale_theme(const ThumbnailScaleControls &controls, const AppTheme theme) {
    struct Inks {
        QRgb line;
        QRgb symbol;
        QRgb handle;
        QRgb shadow;
    };
    const std::array inks{
        Inks{qRgb(96, 96, 96), qRgb(178, 178, 178), qRgb(178, 178, 178), qRgb(25, 25, 25)},
        Inks{qRgb(33, 56, 64), qRgb(91, 169, 179), qRgb(93, 165, 181), qRgb(4, 63, 97)},
        Inks{qRgb(241, 228, 187), qRgb(91, 169, 179), qRgb(241, 228, 187), qRgb(127, 107, 61)},
        Inks{qRgb(91, 91, 91), qRgb(178, 178, 178), qRgb(178, 178, 178), qRgb(25, 25, 25)}};
    const auto colors = inks.at(static_cast<std::size_t>(theme));
    for (auto *control :
         std::array<QWidget *, 3>{controls.smaller, controls.slider, controls.larger}) {
        control->setProperty(lineInkProperty, QColor(colors.line));
        control->setProperty(symbolInkProperty, QColor(colors.symbol));
        control->setProperty(handleInkProperty, QColor(colors.handle));
        control->setProperty(shadowInkProperty, QColor(colors.shadow));
        control->update();
    }
}

} // namespace vove::ui
