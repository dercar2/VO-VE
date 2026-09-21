#pragma once

#include <QDialog>
#include <QString>

class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace vove::ui {

class RenameDialog final : public QDialog {
  public:
    explicit RenameDialog(const QString &file_name, QWidget *parent = nullptr,
                          bool directory = false);

    [[nodiscard]] QString file_name() const;
    void accept() override;

  private:
    [[nodiscard]] QString validation_error() const;
    bool update_validation();

    QString originalFileName_;
    QLineEdit *nameEdit_{};
    QLineEdit *extensionEdit_{};
    QLabel *validationLabel_{};
    QDialogButtonBox *buttons_{};
};

} // namespace vove::ui
