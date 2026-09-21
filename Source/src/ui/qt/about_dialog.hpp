#pragma once

#include <QDialog>
#include <QString>

namespace vove::ui {

class AboutDialog final : public QDialog {
  public:
    explicit AboutDialog(const QString &version, QWidget *parent = nullptr);
};

} // namespace vove::ui
