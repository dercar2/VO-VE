#include "window_controls.hpp"

#include "ui_text.hpp"

#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <array>

namespace vove::ui {
namespace {

enum class Symbol { minimize, maximize, close };

constexpr auto captionInkProperty = "voveCaptionInk";

// Filled vector outlines and inks from design/window-controls.ai.
QPainterPath square(const QRectF &rect) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    path.addRect(rect);
    path.addRect(rect.adjusted(1.417, 1.417, -1.417, -1.417));
    return path;
}

class CaptionButton final : public QToolButton {
  public:
    CaptionButton(const Symbol symbol, QWidget *parent) : QToolButton(parent), symbol_(symbol) {
        setFixedSize(30, 28);
        setAutoRaise(true);
        setFocusPolicy(Qt::StrongFocus);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto ink = property(captionInkProperty).value<QColor>();
        if (!ink.isValid()) {
            ink = palette().color(QPalette::ButtonText);
        }
        if (underMouse() || isDown()) {
            auto hover = ink;
            hover.setAlpha(isDown() ? 70 : 30);
            painter.fillRect(rect(), hover);
        }
        painter.translate(width() / 2.0, height() / 2.0);
        if (symbol_ == Symbol::minimize) {
            painter.fillRect(QRectF(-6.059, 4.642, 12.118, 1.417), ink);
        } else if (symbol_ == Symbol::maximize) {
            if (property("windowRestores").toBool()) {
                const QRectF front(-6.059, -2.941, 9, 9);
                QPainterPath front_area;
                front_area.addRect(front);
                painter.fillPath(square(QRectF(-2.941, -6.059, 9, 9)).subtracted(front_area), ink);
                painter.fillPath(square(front), ink);
            } else {
                painter.fillPath(square(QRectF(-6.059, -6.059, 12.118, 12.118)), ink);
            }
        } else {
            QPainterPath cross;
            cross.setFillRule(Qt::WindingFill);
            cross.moveTo(-4.973, 5.9755);
            cross.lineTo(-5.975, 4.9735);
            cross.lineTo(4.973, -5.9755);
            cross.lineTo(5.975, -4.9735);
            cross.closeSubpath();
            cross.moveTo(4.973, 5.9755);
            cross.lineTo(-5.975, -4.9735);
            cross.lineTo(-4.973, -5.9755);
            cross.lineTo(5.975, 4.9735);
            cross.closeSubpath();
            painter.fillPath(cross, ink);
        }
        if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
            painter.setPen(QPen(palette().color(QPalette::Text), 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(-12, -11, 24, 22));
        }
    }

  private:
    Symbol symbol_;
};

} // namespace

WindowControls::WindowControls(QWidget *window, QWidget *parent)
    : QWidget(parent), window_(window), minimize_(new CaptionButton(Symbol::minimize, this)),
      maximize_(new CaptionButton(Symbol::maximize, this)),
      close_(new CaptionButton(Symbol::close, this)) {
    setObjectName(QStringLiteral("windowControls"));
    setLayoutDirection(Qt::LeftToRight);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    minimize_->setObjectName(QStringLiteral("windowMinimize"));
    maximize_->setObjectName(QStringLiteral("windowMaximize"));
    close_->setObjectName(QStringLiteral("windowClose"));
    for (auto *button : {minimize_, maximize_, close_}) {
        layout->addWidget(button);
    }
    connect(minimize_, &QToolButton::clicked, window_, &QWidget::showMinimized);
    connect(maximize_, &QToolButton::clicked, window_, [this] {
        if (window_->isMaximized() || window_->isFullScreen()) {
            window_->showNormal();
        } else {
            window_->showMaximized();
        }
    });
    connect(close_, &QToolButton::clicked, window_, &QWidget::close);
    window_->installEventFilter(this);
    update_state();
}

void WindowControls::set_theme(const AppTheme theme) {
    const std::array inks{qRgb(178, 178, 178), qRgb(91, 169, 179), qRgb(91, 169, 179),
                          qRgb(91, 91, 91)};
    for (auto *button : {minimize_, maximize_, close_}) {
        button->setProperty(captionInkProperty,
                            QColor(inks.at(static_cast<std::size_t>(theme))));
        button->update();
    }
}

QToolButton *WindowControls::maximize_button() const noexcept {
    return maximize_;
}

void WindowControls::update_state() {
    const auto restores = window_->isMaximized() || window_->isFullScreen();
    maximize_->setProperty("windowRestores", restores);
    const std::array labels{QCoreApplication::translate("WindowControls", "Minimize"),
                            restores ? QCoreApplication::translate("WindowControls", "Restore")
                                     : QCoreApplication::translate("WindowControls", "Maximize"),
                            ui_text(UiTextId::close)};
    std::size_t index{};
    for (auto *button : {minimize_, maximize_, close_}) {
        button->setToolTip(labels[index]);
        button->setAccessibleName(labels[index++]);
        button->update();
    }
}

bool WindowControls::eventFilter(QObject *watched, QEvent *event) {
    if (watched == window_ && event->type() == QEvent::WindowStateChange) {
        update_state();
    }
    return QWidget::eventFilter(watched, event);
}

void WindowControls::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        update_state();
    }
    QWidget::changeEvent(event);
}

} // namespace vove::ui
