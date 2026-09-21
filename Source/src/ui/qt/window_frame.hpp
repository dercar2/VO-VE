#pragma once

#include <QAbstractNativeEventFilter>
#include <QCursor>
#include <QObject>
#include <QPointer>
#include <QRect>

class QAbstractButton;
class QWidget;

namespace vove::ui {

[[nodiscard]] Qt::Edges window_frame_resize_edges(const QRect &bounds, const QPoint &position,
                                                  const QSize &border) noexcept;
[[nodiscard]] bool window_frame_is_drag_area(QWidget *window, QWidget *title_bar,
                                             const QPoint &window_position);

// Window-owned; create before showing/restoring geometry. Button connections and settings
// remain with the caller. Blank QWidget containers are draggable; other passive
// title-bar children opt in via windowDragArea=true.
class WindowFrame final : public QObject, public QAbstractNativeEventFilter {
  public:
    WindowFrame(QWidget *window, QWidget *title_bar, QAbstractButton *maximize_button);
    ~WindowFrame() override;

    bool nativeEventFilter(const QByteArray &event_type, void *message, qintptr *result) override;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    [[nodiscard]] Qt::Edges resize_edges(const QPoint &position, const QSize &border) const;
    [[nodiscard]] bool maximize_area(const QPoint &position) const;
    void update_resize_cursor(Qt::Edges edges);
    void set_maximize_hover(bool hovered);
    void cancel_maximize_press();
    void update_native_window();

    QPointer<QWidget> window_;
    QPointer<QWidget> title_bar_;
    QPointer<QAbstractButton> maximize_button_;
    QCursor previous_cursor_;
    bool cursor_changed_{};
    bool had_cursor_{};
    bool had_hover_{};
    bool native_enabled_{};
    bool maximize_pressed_{};
    bool maximize_hovered_{};
    quintptr native_window_{};
    quintptr original_frame_style_{};
};

} // namespace vove::ui
