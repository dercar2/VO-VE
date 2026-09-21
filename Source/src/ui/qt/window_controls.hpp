#pragma once

#include <QWidget>

class QToolButton;

namespace vove::ui {

enum class AppTheme : int;

class WindowControls final : public QWidget {
  public:
    WindowControls(QWidget *window, QWidget *parent);

    void set_theme(AppTheme theme);
    [[nodiscard]] QToolButton *maximize_button() const noexcept;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    void update_state();

    QWidget *window_;
    QToolButton *minimize_;
    QToolButton *maximize_;
    QToolButton *close_;
};

} // namespace vove::ui
