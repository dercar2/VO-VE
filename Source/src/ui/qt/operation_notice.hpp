#pragma once

#include <QWidget>
#include <QTimer>

class QLabel;
class QGraphicsOpacityEffect;
class QPropertyAnimation;

namespace vove::ui {

class OperationNotice final : public QWidget {
  public:
    explicit OperationNotice(QWidget *parent);
    void show_file_in_use();
    void dismiss();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void position_notice();
    QLabel *title_{};
    QLabel *detail_{};
    QGraphicsOpacityEffect *opacity_{};
    QPropertyAnimation *animation_{};
    QTimer lifetime_;
    bool fadingOut_{};
};

} // namespace vove::ui
