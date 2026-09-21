#include "main_menu_dialog.hpp"
#include "window_frame.hpp"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWindow>

#include <algorithm>

namespace vove::ui {
namespace {

class MainMenuDialogFrame final : public QObject {
  public:
    explicit MainMenuDialogFrame(QDialog *dialog, QVBoxLayout *layout)
        : QObject(dialog), dialog_(dialog), layout_(layout), minimum_(dialog->minimumSize()) {
        dialog->setProperty("mainMenuFrameInstalled", true);
        dialog->setWindowFlag(Qt::FramelessWindowHint);
        title_ = new QLabel(dialog->windowTitle(), dialog);
        title_->setObjectName(QStringLiteral("mainMenuDialogTitle"));
        title_->setTextFormat(Qt::PlainText);
        title_->setTextInteractionFlags(Qt::NoTextInteraction);
        title_->setProperty("windowDragArea", true);
        title_->setWordWrap(true);
        title_->setCursor(Qt::SizeAllCursor);
        auto font = title_->font();
        font.setBold(true);
        title_->setFont(font);
        layout->insertWidget(0, title_);
        connect(dialog, &QWidget::windowTitleChanged, title_, &QLabel::setText);

        // Keep QDialog's reject/close semantics, including overrides and unsaved-settings handling.
        auto *close = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_F4), dialog);
        connect(close, &QShortcut::activated, dialog, [dialog] { dialog->close(); });
        qApp->installEventFilter(this);
        for (auto *screen : QGuiApplication::screens()) {
            watch_screen(screen);
        }
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
            watch_screen(screen);
            schedule_fit();
        });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] { schedule_fit(); });
        decorate_close_buttons();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (!dialog_) {
            return false;
        }
        if (watched == dialog_) {
            switch (event->type()) {
            case QEvent::Show:
                decorate_close_buttons();
                fit_to_screen();
                schedule_fit();
                break;
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::LayoutRequest:
                schedule_fit();
                break;
            case QEvent::Hide:
            case QEvent::WindowDeactivate:
            case QEvent::WindowBlocked:
                dragging_ = false;
                break;
            default:
                break;
            }
        }
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || widget->window() != dialog_ || !dialog_->isEnabled() ||
            (QApplication::activeModalWidget() && QApplication::activeModalWidget() != dialog_)) {
            return false;
        }
        if (event->type() == QEvent::MouseButtonRelease && dragging_) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                dragging_ = false;
                fit_to_screen();
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && dragging_) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (!mouse->buttons().testFlag(Qt::LeftButton)) {
                dragging_ = false;
                return false;
            }
            dialog_->move(mouse->globalPosition().toPoint() - drag_offset_);
            fit_to_screen();
            return true;
        }
        if (event->type() != QEvent::MouseButtonPress) {
            return false;
        }
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || !dialog_->windowHandle()) {
            return false;
        }
        const auto position = widget->mapTo(dialog_, mouse->position().toPoint());
        auto edges = window_frame_resize_edges(dialog_->rect(), position, QSize(5, 5));
        if (dialog_->minimumWidth() >= dialog_->maximumWidth()) {
            edges &= ~(Qt::LeftEdge | Qt::RightEdge);
        }
        if (dialog_->minimumHeight() >= dialog_->maximumHeight()) {
            edges &= ~(Qt::TopEdge | Qt::BottomEdge);
        }
        if (edges) {
            return dialog_->windowHandle()->startSystemResize(edges);
        }
        if (window_frame_is_drag_area(dialog_, title_, position)) {
            // Offscreen/minimal plugins do not support system moves; retain direct dragging.
            drag_offset_ = mouse->globalPosition().toPoint() - dialog_->pos();
            dragging_ = !dialog_->windowHandle()->startSystemMove();
            return true;
        }
        return false;
    }

  private:
    void decorate_close_buttons() {
        for (auto *box : dialog_->findChildren<QDialogButtonBox *>()) {
            if (box->window() != dialog_) {
                continue;
            }
            if (auto *button = box->button(QDialogButtonBox::Close);
                button && !button->property("mainMenuCloseButton").toBool()) {
                button->setProperty("mainMenuCloseButton", true);
                button->style()->unpolish(button);
                button->style()->polish(button);
                button->update();
            }
        }
    }

    void watch_screen(QScreen *screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, [this] { schedule_fit(); });
    }

    void schedule_fit() {
        if (fitting_ || fit_pending_ || !dialog_->isVisible()) {
            return;
        }
        fit_pending_ = true;
        QTimer::singleShot(0, this, [this] {
            fit_pending_ = false;
            if (dialog_ && dialog_->isVisible()) {
                fit_to_screen();
            }
        });
    }

    void fit_to_screen() {
        if (fitting_) {
            return;
        }
        QScopedValueRollback<bool> fitting(fitting_, true);
        auto *screen = QGuiApplication::screenAt(dialog_->geometry().center());
        if (!screen) {
            screen = dialog_->screen();
        }
        if (!screen) {
            return;
        }
        const auto available = screen->availableGeometry();
        const auto margins = layout_->contentsMargins();
        const auto content_width = available.width() - margins.left() - margins.right();
        for (auto *box : dialog_->findChildren<QDialogButtonBox *>()) {
            if (box->parentWidget() == dialog_ && box->sizeHint().width() > content_width) {
                box->setOrientation(Qt::Vertical);
            }
        }
        layout_->activate();
        if (!overflow_ && (dialog_->minimumSizeHint().width() > available.width() ||
                           dialog_->minimumSizeHint().height() > available.height())) {
            make_content_scrollable();
            layout_->activate();
        }
        dialog_->setMinimumSize(minimum_.expandedTo(dialog_->minimumSizeHint())
                                    .boundedTo(available.size()));
        const auto size = dialog_->size().boundedTo(available.size());
        const QRect fitted(
            QPoint(std::clamp(dialog_->x(), available.left(), available.right() - size.width() + 1),
                   std::clamp(dialog_->y(), available.top(), available.bottom() - size.height() + 1)),
            size);
        if (dialog_->geometry() != fitted) {
            dialog_->setGeometry(fitted);
        }
    }

    void make_content_scrollable() {
        auto *content = new QWidget;
        auto *content_layout = new QVBoxLayout(content);
        content_layout->setContentsMargins(0, 0, 0, 0);
        content_layout->setSpacing(layout_->spacing());
        content_layout->setSizeConstraint(QLayout::SetMinAndMaxSize);
        // Only overflowing content scrolls; the title and dialog commands remain reachable.
        for (int index = 0; index < layout_->count();) {
            auto *item = layout_->itemAt(index);
            auto *widget = item->widget();
            if (widget == title_ || qobject_cast<QDialogButtonBox *>(widget)) {
                ++index;
                continue;
            }
            const auto stretch = layout_->stretch(index);
            item = layout_->takeAt(index);
            if (widget) {
                content_layout->addWidget(widget, stretch, item->alignment());
                delete item;
            } else if (auto *child_layout = item->layout()) {
                child_layout->setParent(nullptr);
                content_layout->addLayout(child_layout, stretch);
            } else {
                content_layout->addItem(item);
            }
        }
        overflow_ = new QScrollArea(dialog_);
        overflow_->setObjectName(QStringLiteral("mainMenuDialogOverflow"));
        overflow_->setFrameShape(QFrame::NoFrame);
        overflow_->setWidgetResizable(true);
        overflow_->setWidget(content);
        layout_->insertWidget(1, overflow_, 1);
    }

    QPointer<QDialog> dialog_;
    QPointer<QLabel> title_;
    QVBoxLayout *layout_{};
    QPointer<QScrollArea> overflow_;
    QSize minimum_;
    QPoint drag_offset_;
    bool dragging_{};
    bool fit_pending_{};
    bool fitting_{};
};

class MainMenuDialogInstaller final : public QObject {
  public:
    explicit MainMenuDialogInstaller(QWidget *owner) : QObject(owner), owner_(owner) {
        qApp->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Polish) {
            return false;
        }
        auto *dialog = qobject_cast<QDialog *>(watched);
        if (dialog && belongs_to_owner(dialog) && dialog->isWindow() &&
            dialog->property("mainMenuDialog").toBool() &&
            !dialog->property("mainMenuFrameInstalled").toBool()) {
            if (auto *layout = qobject_cast<QVBoxLayout *>(dialog->layout())) {
                new MainMenuDialogFrame(dialog, layout);
            }
        }
        return false;
    }

  private:
    bool belongs_to_owner(const QDialog *dialog) const {
        // QWidget::isAncestorOf stops at top-level windows; dialogs are owned windows.
        for (auto *parent = dialog->parentWidget(); parent; parent = parent->parentWidget()) {
            if (parent == owner_) {
                return true;
            }
        }
        return false;
    }

    QPointer<QWidget> owner_;
};

} // namespace

void install_main_menu_dialog_frames(QWidget *owner) {
    if (owner && !owner->property("mainMenuFramesInstalled").toBool()) {
        owner->setProperty("mainMenuFramesInstalled", true);
        new MainMenuDialogInstaller(owner);
    }
}

} // namespace vove::ui
