#pragma once

#include "release_configuration.hpp"
#include "update_checker.hpp"

#include <QDialog>

class QLabel;
class QPushButton;

namespace vove::ui::updates {

class UpdateChecker;

class UpdateDialog final : public QDialog {
  public:
    UpdateDialog(ReleaseConfiguration configuration, QString current_version,
                 QWidget *parent = nullptr);
    UpdateDialog(ReleaseConfiguration configuration, QString current_version,
                 QNetworkAccessManager *manager, UpdateNetworkLimits limits,
                 QWidget *parent = nullptr);

  private:
    void start_check();
    void apply_result(const UpdateCheckResult &result);
    void confirm_release_page();
    void confirm_repository_page();

    ReleaseConfiguration configuration_;
    QString current_version_;
    UpdateChecker *checker_{};
    QLabel *availableValue_{};
    QLabel *statusLabel_{};
    QLabel *notesLabel_{};
    QPushButton *checkButton_{};
    QPushButton *releaseButton_{};
    QPushButton *repositoryButton_{};
    QUrl releaseUrl_;
    QUrl repositoryUrl_;
};

} // namespace vove::ui::updates
