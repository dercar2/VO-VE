#include "toolbar_fields.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QToolButton>

#include <algorithm>
#include <array>

namespace vove::ui {
namespace {

struct Inks {
    QRgb background;
    QRgb border;
    QRgb placeholder;
    QRgb input;
    QRgb sort;
    QRgb arrow;
    QRgb direction;
    QRgb clear;
};

// RGB inks and vector proportions from design/fields.ai, in application theme order.
constexpr std::array inks{
    Inks{qRgb(242, 242, 242), qRgb(178, 178, 178), qRgb(178, 178, 178), qRgb(26, 26, 26),
         qRgb(91, 91, 91), qRgb(178, 178, 178), qRgb(91, 91, 91), qRgb(91, 91, 91)},
    Inks{qRgb(241, 228, 187), qRgb(47, 82, 91), qRgb(237, 196, 95), qRgb(91, 169, 179),
         qRgb(8, 133, 161), qRgb(91, 169, 179), qRgb(47, 82, 91), qRgb(91, 169, 179)},
    Inks{qRgb(33, 56, 64), qRgb(241, 228, 187), qRgb(47, 82, 91), qRgb(241, 228, 187),
         qRgb(94, 165, 182), qRgb(91, 169, 179), qRgb(241, 228, 187), qRgb(91, 169, 179)},
    Inks{qRgb(51, 51, 51), qRgb(178, 178, 178), qRgb(91, 91, 91), qRgb(230, 230, 230),
         qRgb(178, 178, 178), qRgb(91, 91, 91), qRgb(178, 178, 178), qRgb(178, 178, 178)}};

constexpr QRgb active_border = qRgb(255, 127, 0);

void draw_frame(QPainter &painter, const QRect &rect, const QColor &border) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, 1.4));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect).adjusted(0.7, 0.7, -0.7, -0.7), 4, 4);
}

class ClearIcon final : public QIconEngine {
  public:
    explicit ClearIcon(const QRgb color) : color_(color) {}

    QIconEngine *clone() const override {
        return new ClearIcon(color_);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(QRectF(rect).center());
        const auto scale = std::min(rect.width(), rect.height()) / 18.0;
        painter->scale(scale, scale);
        QPainterPath cross;
        cross.moveTo(-5.975, -4.973);
        cross.lineTo(-4.973, -5.975);
        cross.lineTo(0, -1.002);
        cross.lineTo(4.973, -5.975);
        cross.lineTo(5.975, -4.973);
        cross.lineTo(1.002, 0);
        cross.lineTo(5.975, 4.973);
        cross.lineTo(4.973, 5.975);
        cross.lineTo(0, 1.002);
        cross.lineTo(-4.973, 5.975);
        cross.lineTo(-5.975, 4.973);
        cross.lineTo(-1.002, 0);
        cross.closeSubpath();
        painter->fillPath(cross, QColor(color_));
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, const QIcon::Mode mode, const QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint{}, size), mode, state);
        return result;
    }

  private:
    QRgb color_;
};

class SortField final : public QComboBox {
  public:
    explicit SortField(QWidget *parent) : QComboBox(parent) {
        setStyleSheet(QStringLiteral("QComboBox { background: transparent; border: 0; "
                                     "padding: 0; min-height: 0; }"));
        // Use item-view styling consistently, including platforms with menu-like combo popups.
        setItemDelegate(new QStyledItemDelegate(this));
        view()->setMouseTracking(true);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto rtl = isRightToLeft();
        const auto arrow_x = rtl ? 13.0 : width() - 13.0;
        QPainterPath arrow;
        arrow.moveTo(arrow_x - 5.837, height() / 2.0 - 2.9185);
        arrow.lineTo(arrow_x + 5.837, height() / 2.0 - 2.9185);
        arrow.lineTo(arrow_x, height() / 2.0 + 2.9185);
        arrow.closeSubpath();
        painter.fillPath(arrow, palette().color(QPalette::ButtonText));
        painter.setPen(palette().color(QPalette::Text));
        const auto text_rect = rect().adjusted(rtl ? 26 : 9, 0, rtl ? -9 : -26, 0);
        painter.drawText(
            text_rect, static_cast<int>(Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft)),
            fontMetrics().elidedText(currentText(), Qt::ElideRight, text_rect.width()));
        if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
            painter.setPen(QPen(palette().color(QPalette::Text), 1, Qt::DotLine));
            painter.drawLine(text_rect.bottomLeft() - QPoint(0, 4),
                             text_rect.bottomRight() - QPoint(0, 4));
        }
    }
};

class DirectionButton final : public QToolButton {
  public:
    explicit DirectionButton(QWidget *parent) : QToolButton(parent) {
        setCheckable(true); // Checked means descending; no second menu.
        setFocusPolicy(Qt::StrongFocus);
        setFixedWidth(26);
        setStyleSheet(QStringLiteral("QToolButton { border: 0; padding: 0; }"));
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(width() / 2.0, height() / 2.0);
        if (!isChecked()) {
            painter.rotate(180);
        }
        QPainterPath arrow;
        arrow.moveTo(5.837, -5.0995);
        arrow.lineTo(-5.837, -5.0995);
        arrow.lineTo(-1.475, -0.7375);
        arrow.lineTo(-5.837, -0.7375);
        arrow.lineTo(0, 5.0995);
        arrow.lineTo(5.837, -0.7375);
        arrow.lineTo(1.475, -0.7375);
        arrow.closeSubpath();
        const auto color = palette().color(QPalette::ButtonText);
        painter.fillPath(arrow, color);
        if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
            painter.setPen(QPen(color, 1, Qt::DotLine));
            painter.drawRect(QRectF(-9, -9, 18, 18));
        }
    }
};

class SortGroup final : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Base));
        painter.drawRoundedRect(QRectF(rect()), 4, 4);
        const auto border = palette().color(QPalette::Mid);
        draw_frame(painter, rect(), border);
        const auto x = isRightToLeft() ? 26.0 : width() - 26.0;
        painter.drawLine(QPointF(x, 1), QPointF(x, height() - 1));
    }
};

} // namespace

AddressField::AddressField(QWidget *parent) : QLineEdit(parent) {
    setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: 0; "
                                 "padding: 1px 5px; min-height: 20px; }"));
}

void AddressField::set_theme(const AppTheme theme) {
    theme_ = theme;
    update();
}

void AddressField::paintEvent(QPaintEvent *event) {
    const auto &colors = inks.at(static_cast<std::size_t>(theme_));
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(colors.background));
        painter.drawRoundedRect(QRectF(rect()), 4, 4);
    }
    QLineEdit::paintEvent(event);
    QPainter painter(this);
    draw_frame(painter, rect(), QColor(hasFocus() ? active_border : colors.border));
}

SearchField::SearchField(QWidget *parent) : QLineEdit(parent) {
    setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: 0; "
                                 "padding: 1px 8px; min-height: 0; }"));
    clear_ = addAction(QIcon{}, QLineEdit::TrailingPosition);
    clear_->setObjectName(QStringLiteral("searchClearAction"));
    clear_->setVisible(false);
    connect(clear_, &QAction::triggered, this, &QLineEdit::clear);
    connect(this, &QLineEdit::textChanged, this, [this] {
        clear_->setVisible(!text().isEmpty());
        update();
    });
    set_theme(static_cast<AppTheme>(2));
}

void SearchField::set_theme(const AppTheme theme) {
    theme_ = theme;
    clear_->setIcon(QIcon(new ClearIcon(inks.at(static_cast<std::size_t>(theme_)).clear)));
    apply_colors();
}

void SearchField::fit_placeholder() {
    // Reserve the clear affordance even while hidden: typing cannot stretch the toolbar.
    const auto ideal_width = fontMetrics().horizontalAdvance(placeholderText()) + 38;
    setMinimumWidth(std::min(ideal_width, fontMetrics().horizontalAdvance(QStringLiteral("MMM")) + 38));
    setMaximumWidth(ideal_width);
    setFixedHeight(std::max(24, fontMetrics().height() + 6));
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    resize(sizeHint());
    clear_->setText(QCoreApplication::translate("QLineEdit", "Clear"));
}

QSize SearchField::sizeHint() const { return {maximumWidth(), minimumHeight()}; }

QSize SearchField::minimumSizeHint() const { return {minimumWidth(), minimumHeight()}; }

void SearchField::apply_colors() {
    const auto &colors = inks.at(static_cast<std::size_t>(theme_));
    setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: 0; "
                                 "padding: 1px 8px; min-height: 0; color: %1; "
                                 "placeholder-text-color: %2; }")
                      .arg(QColor(colors.input).name(),
                           QColor(hasFocus() ? colors.input : colors.placeholder).name()));
    update();
}

void SearchField::focusInEvent(QFocusEvent *event) {
    QLineEdit::focusInEvent(event);
    apply_colors();
}

void SearchField::focusOutEvent(QFocusEvent *event) {
    QLineEdit::focusOutEvent(event);
    apply_colors();
}

void SearchField::paintEvent(QPaintEvent *event) {
    const auto &colors = inks.at(static_cast<std::size_t>(theme_));
    // Navigation also clears queries under QSignalBlocker. Paint from the actual text, not a flag.
    clear_->setVisible(!text().isEmpty());
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(colors.background));
        painter.drawRoundedRect(QRectF(rect()), 4, 4);
    }
    QLineEdit::paintEvent(event);
    QPainter painter(this);
    draw_frame(painter, rect(),
               QColor(hasFocus() || !text().isEmpty() ? active_border : colors.border));
}

SortControls create_sort_controls(QWidget *parent) {
    auto *widget = new SortGroup(parent);
    widget->setObjectName(QStringLiteral("sortControls"));
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *field = new SortField(widget);
    auto *direction = new DirectionButton(widget);
    layout->addWidget(field);
    layout->addWidget(direction);
    return {widget, field, direction};
}

void apply_sort_controls_theme(const SortControls &controls, const AppTheme theme) {
    const auto &colors = inks.at(static_cast<std::size_t>(theme));
    const QColor background(colors.background);
    const QColor accent = static_cast<std::size_t>(theme) < 2 ? QColor(91, 169, 179)
                                                            : QColor(active_border);
    const auto blend = [](const int base, const int tint) { return (base * 86 + tint * 14) / 100; };
    const QColor hover(blend(background.red(), accent.red()),
                       blend(background.green(), accent.green()),
                       blend(background.blue(), accent.blue()));
    auto palette = controls.widget->palette();
    palette.setColor(QPalette::Base, QColor(colors.background));
    palette.setColor(QPalette::Mid, QColor(colors.border));
    palette.setColor(QPalette::Text, QColor(colors.sort));
    controls.widget->setPalette(palette);
    controls.field->setStyleSheet(QStringLiteral("QComboBox { background: transparent; border: 0; "
                                                 "padding: 0; min-height: 0; color: %1; }")
                                      .arg(QColor(colors.sort).name()));
    palette.setColor(QPalette::ButtonText, QColor(colors.arrow));
    controls.field->setPalette(palette);
    controls.field->view()->setStyleSheet(
        QStringLiteral("QAbstractItemView { background: %1; color: %2; "
                       "selection-background-color: %3; selection-color: %2; "
                       "border: 1px solid %4; outline: 0; }"
                       "QAbstractItemView::item:selected { "
                       "background: %3; color: %2; }")
            .arg(background.name(), QColor(colors.sort).name(), hover.name(),
                 QColor(colors.border).name()));
    palette.setColor(QPalette::ButtonText, QColor(colors.direction));
    controls.direction->setPalette(palette);
    controls.widget->update();
}

void fit_sort_controls(const SortControls &controls) {
    int width{};
    for (int index = 0; index < controls.field->count(); ++index) {
        width = std::max(width, controls.field->fontMetrics().horizontalAdvance(
                                    controls.field->itemText(index)));
    }
    const auto height = std::max(24, controls.field->fontMetrics().height() + 6);
    controls.field->setFixedSize(width + 36, height);
    controls.direction->setFixedHeight(height);
    controls.widget->setFixedSize(controls.field->width() + controls.direction->width(), height);
}

} // namespace vove::ui
