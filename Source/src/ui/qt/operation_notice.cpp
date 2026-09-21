#include "operation_notice.hpp"

#include <QApplication>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QVBoxLayout>

#include <algorithm>

namespace vove::ui {

OperationNotice::OperationNotice(QWidget *parent) : QWidget(parent), lifetime_(this) {
    setObjectName(QStringLiteral("operationNotice"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_StyledBackground, false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(8);
    title_ = new QLabel(this);
    detail_ = new QLabel(this);
    for (auto *label : {title_, detail_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignCenter);
        label->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(label);
    }
    title_->setObjectName(QStringLiteral("operationNoticeTitle"));
    detail_->setObjectName(QStringLiteral("operationNoticeDetail"));
    opacity_ = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(opacity_);
    animation_ = new QPropertyAnimation(opacity_, "opacity", this);
    animation_->setDuration(220);
    animation_->setEasingCurve(QEasingCurve::InOutQuad);
    connect(animation_, &QPropertyAnimation::finished, this, [this] {
        if (fadingOut_) dismiss();
    });
    lifetime_.setSingleShot(true);
    lifetime_.setTimerType(Qt::PreciseTimer);
    connect(&lifetime_, &QTimer::timeout, this, [this] {
        fadingOut_ = true;
        animation_->stop();
        animation_->setStartValue(opacity_->opacity());
        animation_->setEndValue(0.0);
        animation_->start();
    });
    qApp->installEventFilter(this);
    hide();
}

void OperationNotice::position_notice() {
    const auto available = parentWidget()->rect().adjusted(12, 12, -12, -12);
    setFixedWidth(std::max(1, std::min(available.width(), 430)));
    const auto height = std::min(available.height(), sizeHint().height());
    setFixedHeight(std::max(1, height));
    move(available.center() - QPoint(width() / 2, this->height() / 2));
}

void OperationNotice::show_file_in_use() {
    animation_->stop();
    lifetime_.stop();
    fadingOut_ = false;
    title_->setText(QCoreApplication::translate("OperationNotice", "File is in use by another application"));
    detail_->setText(QCoreApplication::translate("OperationNotice", "Cannot complete the operation"));
    setAccessibleName(title_->text() + QLatin1Char('\n') + detail_->text());
    setFont(parentWidget()->font());
    setPalette(parentWidget()->palette());
    position_notice();
    opacity_->setOpacity(0.0);
    show();
    raise();
    animation_->setStartValue(0.0);
    animation_->setEndValue(1.0);
    animation_->start();
    lifetime_.start(5000);
}

void OperationNotice::dismiss() {
    lifetime_.stop();
    animation_->stop();
    fadingOut_ = false;
    hide();
}

bool OperationNotice::eventFilter(QObject *watched, QEvent *event) {
    if (isVisible()) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto point = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
            if (!rect().contains(mapFromGlobal(point))) dismiss();
        } else if (watched == parentWidget() &&
                   (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate)) {
            dismiss();
        } else if (watched == parentWidget() && event->type() == QEvent::Resize) {
            position_notice();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OperationNotice::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    painter.setPen(QPen(QColor(QStringLiteral("#FF8A00")), 2));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));
}

} // namespace vove::ui
