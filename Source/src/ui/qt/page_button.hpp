#pragma once

#include <QToolButton>

namespace vove::ui {

enum class AppTheme : int;
enum class PageDirection { previous, next };

class PageButton final : public QToolButton {
  public:
    PageButton(PageDirection direction, const QString &tooltip, QWidget *parent);
    void set_theme(AppTheme theme);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    PageDirection direction_;
    AppTheme theme_{};
};

} // namespace vove::ui
