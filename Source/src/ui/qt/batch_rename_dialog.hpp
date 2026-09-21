#pragma once

#include "vove/fileops/batch_rename.hpp"

#include <QDialog>

#include <filesystem>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableView;

namespace vove::ui {

class BatchRenamePreviewModel;

class BatchRenameDialog final : public QDialog {
  public:
    BatchRenameDialog(std::vector<fileops::BatchRenameSource> sources,
                      std::vector<std::filesystem::path> directory_names,
                      QWidget *parent = nullptr);
    ~BatchRenameDialog() override;

    [[nodiscard]] const fileops::BatchRenamePlan &plan() const noexcept;
    [[nodiscard]] const fileops::BatchRenameOptions &options() const noexcept;

  private:
    void build_ui();
    void insert_token(const QString &token);
    void rebuild_plan();

    std::vector<fileops::BatchRenameSource> sources_;
    std::vector<std::filesystem::path> directoryNames_;
    fileops::BatchRenameOptions options_;
    fileops::BatchRenamePlan plan_;
    BatchRenamePreviewModel *previewModel_{};
    QLineEdit *templateEdit_{};
    QSpinBox *counterStart_{};
    QSpinBox *counterStep_{};
    QSpinBox *counterPadding_{};
    QComboBox *dateTimeSource_{};
    QComboBox *dateFormat_{};
    QComboBox *timeFormat_{};
    QLineEdit *extensionEdit_{};
    QLabel *validationLabel_{};
    QTableView *previewTable_{};
    QDialogButtonBox *buttons_{};
};

} // namespace vove::ui
