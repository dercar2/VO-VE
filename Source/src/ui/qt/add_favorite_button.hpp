#pragma once

#include <QToolButton>

namespace vove::ui {

class AddFavoriteButton final : public QToolButton {
  public:
    explicit AddFavoriteButton(QWidget *parent);

  protected:
    void paintEvent(QPaintEvent *) override;
};

} // namespace vove::ui
