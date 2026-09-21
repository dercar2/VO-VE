#pragma once

#include <QToolButton>

namespace vove::ui {

enum class AppTheme : int;
enum class CatalogToolKind { recursive_view, refresh };

class CatalogToolButton final : public QToolButton {
  public:
    explicit CatalogToolButton(CatalogToolKind kind, QWidget *parent);
    void set_theme(AppTheme theme);

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    CatalogToolKind kind_;
    AppTheme theme_{};
};

} // namespace vove::ui
