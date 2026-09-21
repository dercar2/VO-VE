#pragma once

#include <QLineEdit>

class QAction;
class QComboBox;
class QToolButton;

namespace vove::ui {

enum class AppTheme : int;

class AddressField final : public QLineEdit {
  public:
    explicit AddressField(QWidget *parent);
    void set_theme(AppTheme theme);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    AppTheme theme_{};
};

class SearchField final : public QLineEdit {
  public:
    explicit SearchField(QWidget *parent);
    void set_theme(AppTheme theme);
    void fit_placeholder();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

  private:
    void apply_colors();
    AppTheme theme_{};
    QAction *clear_{};
};

struct SortControls {
    QWidget *widget;
    QComboBox *field;
    QToolButton *direction;
};

[[nodiscard]] SortControls create_sort_controls(QWidget *parent);
void apply_sort_controls_theme(const SortControls &controls, AppTheme theme);
void fit_sort_controls(const SortControls &controls);

} // namespace vove::ui
