#include "smooth_scroll.hpp"

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vove::ui {
namespace {

class SmoothScroll final : public QObject {
  public:
    explicit SmoothScroll(QAbstractItemView &view, const int duration_ms) : QObject(&view), view_(view) {
        view_.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view_.installEventFilter(this);
        view_.viewport()->installEventFilter(this);
        view_.verticalScrollBar()->installEventFilter(this);
        animation_.setDuration(std::clamp(duration_ms, 50, 250));
        animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            settingValue_ = true;
            view_.verticalScrollBar()->setValue(value.toInt());
            settingValue_ = false;
        });
        connect(view_.verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            if (!settingValue_) {
                animation_.stop();
            }
        });
        connect(view_.verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this] { animation_.stop(); });
        if (view_.model() != nullptr) {
            connect(view_.model(), &QAbstractItemModel::modelAboutToBeReset, this,
                    [this] { animation_.stop(); });
        }
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Wheel && watched != &view_) {
            auto &wheel = *static_cast<QWheelEvent *>(event);
            auto *bar = view_.verticalScrollBar();
            if (!wheel.pixelDelta().isNull() || wheel.phase() != Qt::NoScrollPhase ||
                wheel.modifiers() != Qt::NoModifier || wheel.angleDelta().x() != 0 ||
                wheel.angleDelta().y() == 0 || bar->isSliderDown()) {
                animation_.stop();
                fraction_ = 0;
                return false;
            }
            const int current = bar->value();
            int target = animation_.state() == QAbstractAnimation::Running
                             ? animation_.endValue().toInt()
                             : current;
            const auto direction =
                bar->invertedControls() ? -wheel.angleDelta().y() : wheel.angleDelta().y();
            if ((target > current && direction < 0) || (target < current && direction > 0)) {
                animation_.stop();
                target = current;
            }
            // Keep Qt's configured pixel step and OS wheel-line count. Accessibility observes
            // only displayed positions, never a temporary probe of the destination.
            const qreal steps = wheel.angleDelta().y() / 120.0 * QApplication::wheelScrollLines() *
                                bar->singleStep();
            if (fraction_ * steps < 0) {
                fraction_ = 0;
            }
            fraction_ += steps;
            qreal distance = std::trunc(fraction_);
            fraction_ -= distance;
            if (bar->invertedControls()) {
                distance = -distance;
            }
            distance = std::clamp(distance, -qreal(bar->pageStep()), qreal(bar->pageStep()));
            if (distance == 0) {
                const bool can_scroll =
                    fraction_ != 0 && ((direction > 0 && target < bar->maximum()) ||
                                       (direction < 0 && target > bar->minimum()));
                if (!can_scroll) {
                    fraction_ = 0;
                }
                wheel.setAccepted(can_scroll);
                return true;
            }
            const int destination = static_cast<int>(
                std::clamp(target + distance, qreal(bar->minimum()), qreal(bar->maximum())));
            if (destination == target) {
                fraction_ = 0;
                wheel.ignore();
                return true;
            }
            target = destination;
            {
                const QSignalBlocker blocker(&animation_);
                animation_.stop();
                if (target != current) {
                    animation_.setStartValue(current);
                    animation_.setEndValue(target);
                    animation_.start();
                }
            }
            wheel.accept();
            return true;
        }
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
        case QEvent::Hide:
        case QEvent::Resize:
        case QEvent::LayoutDirectionChange:
            animation_.stop();
            break;
        default:
            break;
        }
        return false;
    }

  private:
    QAbstractItemView &view_;
    QVariantAnimation animation_;
    bool settingValue_{};
    qreal fraction_{};
};

} // namespace

void install_smooth_scroll(QAbstractItemView &view, const int duration_ms) {
    new SmoothScroll(view, duration_ms);
}

} // namespace vove::ui
