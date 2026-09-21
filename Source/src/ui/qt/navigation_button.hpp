#pragma once

#include <QToolButton>

namespace vove::ui {

enum class AppTheme : int;
enum class NavigationDirection { back, forward, history, up };

class NavigationButton final : public QToolButton {
  public:
    NavigationButton(NavigationDirection direction, const QString &label, QWidget *parent);
    void set_theme(AppTheme theme);

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    NavigationDirection direction_;
    AppTheme theme_{};
};

} // namespace vove::ui
