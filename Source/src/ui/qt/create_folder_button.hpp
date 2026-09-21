#pragma once

#include <QIcon>
#include <QToolButton>

namespace vove::ui {

enum class AppTheme : int;

[[nodiscard]] QIcon create_folder_icon(AppTheme theme);

class CreateFolderButton final : public QToolButton {
  public:
    explicit CreateFolderButton(QWidget *parent);

  protected:
    void paintEvent(QPaintEvent *) override;
};

} // namespace vove::ui
