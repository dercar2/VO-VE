#include "zoom_preview.hpp"

#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vove::ui {
namespace {
constexpr qreal kMinimumZoom = 1.0;
constexpr qreal kMaximumZoom = 16.0;
constexpr qreal kZoomStep = 1.2;

bool zoom_key(const QKeyEvent *event) {
    const auto modifiers = event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    return modifiers == Qt::NoModifier &&
           (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal ||
            event->key() == Qt::Key_Minus || event->key() == Qt::Key_0);
}
} // namespace

ZoomPreview::ZoomPreview(QWidget *parent) : QLabel(parent), indicator_(new QLabel(this)) {
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    indicator_->setObjectName(QStringLiteral("previewZoomIndicator"));
    indicator_->setAttribute(Qt::WA_TransparentForMouseEvents);
    indicator_->setFocusPolicy(Qt::NoFocus);
    indicator_->setTextInteractionFlags(Qt::NoTextInteraction);
    indicator_->setLayoutDirection(Qt::LeftToRight);
    indicator_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    indicator_->setContentsMargins(4, 2, 4, 2);
    indicator_->setAutoFillBackground(true);
    auto small_font = font();
    if (small_font.pointSizeF() > 0) {
        small_font.setPointSizeF(small_font.pointSizeF() * 0.9);
    } else if (small_font.pixelSize() > 0) {
        small_font.setPixelSize(std::max(1, qRound(small_font.pixelSize() * 0.9)));
    }
    set_indicator_font(small_font);
}

ZoomPreview::ZoomPreview(const QString &text, QWidget *parent) : ZoomPreview(parent) {
    setText(text);
}

void ZoomPreview::setPixmap(const QPixmap &image) {
    imageKey_ = 0;
    sourceKey_.clear();
    if (!image.isNull() && image.cacheKey() == source_.cacheKey()) {
        return;
    }
    source_ = image;
    QLabel::setPixmap(image);
    reset_zoom();
}

void ZoomPreview::set_preview(const QImage &image, const QString &stable_key) {
    const auto same_source = !image.isNull() && !source_.isNull() && sourceKey_ == stable_key &&
                             (!stable_key.isEmpty() || imageKey_ == image.cacheKey());
    if (same_source && imageKey_ == image.cacheKey()) {
        return;
    }
    const auto old_size = fitted_size();
    source_ = QPixmap::fromImage(image);
    QLabel::setPixmap(source_);
    imageKey_ = image.cacheKey();
    sourceKey_ = image.isNull() ? QString{} : stable_key;
    if (!same_source) {
        reset_zoom();
        return;
    }
    stop_pan();
    const auto size = fitted_size();
    if (!old_size.isEmpty()) {
        pan_.setX(pan_.x() * size.width() / old_size.width());
        pan_.setY(pan_.y() * size.height() / old_size.height());
    }
    clamp_pan();
    refresh_cursor();
    refresh_indicator();
    update();
}

void ZoomPreview::setText(const QString &text) {
    imageKey_ = 0;
    sourceKey_.clear();
    source_ = {};
    QLabel::setText(text);
    reset_zoom();
}

void ZoomPreview::setMovie(QMovie *movie) {
    imageKey_ = 0;
    sourceKey_.clear();
    source_ = {};
    QLabel::setMovie(movie);
    reset_zoom();
}

void ZoomPreview::clear() {
    imageKey_ = 0;
    sourceKey_.clear();
    source_ = {};
    QLabel::clear();
    reset_zoom();
}

void ZoomPreview::set_indicator_font(const QFont &font) {
    indicator_->setFont(font);
    refresh_indicator();
}

QRectF ZoomPreview::image_area() const {
    const auto inset = margin();
    return QRectF(contentsRect()).adjusted(inset, inset, -inset, -inset);
}

QSizeF ZoomPreview::fitted_size() const {
    const auto area = image_area();
    if (source_.isNull() || area.isEmpty()) {
        return {};
    }
    const auto source_size = QSizeF(source_.size()) / devicePixelRatioF();
    // 100% is the existing fit-to-pane baseline, never an upscale of a small native raster.
    const auto scale = std::min({qreal{1.0}, area.width() / source_size.width(),
                                area.height() / source_size.height()});
    return source_size * scale;
}

QRectF ZoomPreview::image_rect() const {
    const auto size = fitted_size() * zoom_;
    if (size.isEmpty()) {
        return {};
    }
    return {image_area().center() + pan_ - QPointF(size.width(), size.height()) / 2, size};
}

bool ZoomPreview::can_pan() const {
    const auto size = fitted_size() * zoom_;
    const auto area = image_area();
    return zoom_ > kMinimumZoom && !size.isEmpty() &&
           (size.width() > area.width() || size.height() > area.height());
}

void ZoomPreview::clamp_pan() {
    const auto size = fitted_size() * zoom_;
    const auto area = image_area();
    const auto limit_x = std::max(qreal{0.0}, (size.width() - area.width()) / 2);
    const auto limit_y = std::max(qreal{0.0}, (size.height() - area.height()) / 2);
    pan_.setX(std::clamp(pan_.x(), -limit_x, limit_x));
    pan_.setY(std::clamp(pan_.y(), -limit_y, limit_y));
    if (zoom_ == kMinimumZoom || size.isEmpty()) {
        pan_ = {};
    }
}

void ZoomPreview::reset_zoom() {
    stop_pan();
    zoom_ = kMinimumZoom;
    pan_ = {};
    refresh_cursor();
    refresh_indicator();
    update();
}

void ZoomPreview::zoom_at(const qreal factor, const QPointF &anchor) {
    if (source_.isNull() || !std::isfinite(factor) || image_area().isEmpty()) {
        return;
    }
    stop_pan();
    const auto next = std::clamp(factor, kMinimumZoom, kMaximumZoom);
    const auto ratio = next / zoom_;
    // Keep the image point below the pointer still until an edge requires clamping.
    const auto offset = anchor - image_area().center();
    pan_ = offset - (offset - pan_) * ratio;
    zoom_ = next;
    clamp_pan();
    refresh_cursor();
    refresh_indicator();
    update();
}

void ZoomPreview::refresh_indicator() {
    const auto area = image_area().toAlignedRect();
    const auto visible = !source_.isNull() && !area.isEmpty();
    indicator_->setVisible(visible);
    if (!visible) {
        return;
    }
    indicator_->setText(QString::number(qRound(zoom_ * 100)) + QLatin1Char('%'));
    const auto size = indicator_->sizeHint().boundedTo(area.size());
    const auto inset_x = std::min(4, std::max(0, area.width() - size.width()));
    const auto inset_y = std::min(4, std::max(0, area.height() - size.height()));
    indicator_->setGeometry(area.right() + 1 - inset_x - size.width(), area.top() + inset_y,
                            size.width(), size.height());
    indicator_->raise();
}

void ZoomPreview::refresh_cursor() {
    if (can_pan()) {
        if (!ownsCursor_) {
            hadExplicitCursor_ = testAttribute(Qt::WA_SetCursor);
            previousCursor_ = cursor();
            ownsCursor_ = true;
        }
        setCursor(panning_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    } else if (ownsCursor_) {
        ownsCursor_ = false;
        if (hadExplicitCursor_) {
            setCursor(previousCursor_);
        } else {
            unsetCursor();
        }
    }
}

void ZoomPreview::stop_pan() {
    panning_ = false;
    refresh_cursor();
}

bool ZoomPreview::event(QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride && !source_.isNull() && hasFocus() &&
        zoom_key(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide ||
        event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse) {
        stop_pan();
    }
    const auto result = QLabel::event(event);
    if (event->type() == QEvent::ContentsRectChange ||
        event->type() == QEvent::DevicePixelRatioChange) {
        clamp_pan();
        refresh_cursor();
        refresh_indicator();
        update();
    }
    return result;
}

void ZoomPreview::paintEvent(QPaintEvent *event) {
    if (source_.isNull()) {
        QLabel::paintEvent(event);
        return;
    }
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.setClipRect(image_area().intersected(event->rect()));
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(image_rect(), source_, QRectF(source_.rect()));
}

void ZoomPreview::resizeEvent(QResizeEvent *event) {
    QLabel::resizeEvent(event);
    stop_pan();
    clamp_pan();
    refresh_cursor();
    refresh_indicator();
    update();
}

void ZoomPreview::changeEvent(QEvent *event) {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::PaletteChange || event->type() == QEvent::LayoutDirectionChange) {
        refresh_indicator();
    }
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        stop_pan();
    }
}

void ZoomPreview::wheelEvent(QWheelEvent *event) {
    if (source_.isNull() || event->modifiers() != Qt::NoModifier) {
        QLabel::wheelEvent(event);
        return;
    }
    const auto delta = event->pixelDelta().y() != 0 ? event->pixelDelta().y()
                                                   : event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const auto steps = std::clamp(static_cast<qreal>(delta) / 120.0, qreal{-32.0}, qreal{32.0});
    zoom_at(zoom_ * std::pow(kZoomStep, steps), event->position());
    event->accept();
}

void ZoomPreview::keyPressEvent(QKeyEvent *event) {
    if (!source_.isNull() && hasFocus() && zoom_key(event)) {
        if (event->key() == Qt::Key_0) {
            reset_zoom();
        } else {
            zoom_at(zoom_ * (event->key() == Qt::Key_Minus ? 1.0 / kZoomStep : kZoomStep),
                    image_area().center());
        }
        event->accept();
        return;
    }
    QLabel::keyPressEvent(event);
}

void ZoomPreview::mousePressEvent(QMouseEvent *event) {
    if (!source_.isNull() && event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier) {
        setFocus(Qt::MouseFocusReason);
        if (can_pan() && image_rect().contains(event->position())) {
            panning_ = true;
            dragPosition_ = event->position();
            refresh_cursor();
            event->accept();
            return;
        }
    }
    QLabel::mousePressEvent(event);
}

void ZoomPreview::mouseMoveEvent(QMouseEvent *event) {
    if (panning_ && event->buttons().testFlag(Qt::LeftButton)) {
        pan_ += event->position() - dragPosition_;
        dragPosition_ = event->position();
        clamp_pan();
        update();
        event->accept();
        return;
    }
    if (panning_) {
        stop_pan();
    }
    QLabel::mouseMoveEvent(event);
}

void ZoomPreview::mouseReleaseEvent(QMouseEvent *event) {
    if (panning_ && event->button() == Qt::LeftButton) {
        stop_pan();
        event->accept();
        return;
    }
    QLabel::mouseReleaseEvent(event);
}

} // namespace vove::ui
