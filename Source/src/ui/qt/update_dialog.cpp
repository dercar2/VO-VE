#include "update_dialog.hpp"

#include "ui_text.hpp"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace vove::ui::updates {

UpdateDialog::UpdateDialog(ReleaseConfiguration configuration, QString current_version,
                           QWidget *parent)
    : UpdateDialog(std::move(configuration), std::move(current_version), nullptr, {}, parent) {}

UpdateDialog::UpdateDialog(ReleaseConfiguration configuration, QString current_version,
                           QNetworkAccessManager *manager, const UpdateNetworkLimits limits,
                           QWidget *parent)
    : QDialog(parent), configuration_(std::move(configuration)),
      current_version_(std::move(current_version)) {
    setObjectName(QStringLiteral("updateDialog"));
    setProperty("mainMenuDialog", true);
    setWindowTitle(ui_text(UiTextId::update_title));
    setMinimumWidth(600);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *versions = new QFormLayout;
    auto *installed = new QLabel(current_version_, this);
    installed->setTextInteractionFlags(Qt::TextSelectableByMouse);
    availableValue_ = new QLabel(QStringLiteral("-"), this);
    availableValue_->setObjectName(QStringLiteral("availableVersion"));
    availableValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    versions->addRow(ui_text(UiTextId::update_installed_version), installed);
    versions->addRow(ui_text(UiTextId::update_available_version), availableValue_);
    layout->addLayout(versions);

    statusLabel_ = new QLabel(ui_text(UiTextId::update_checking), this);
    statusLabel_->setObjectName(QStringLiteral("updateStatus"));
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    notesLabel_ = new QLabel(this);
    notesLabel_->setObjectName(QStringLiteral("updateNotes"));
    notesLabel_->setTextFormat(Qt::PlainText);
    notesLabel_->setWordWrap(true);
    notesLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    notesLabel_->hide();
    layout->addWidget(notesLabel_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    checkButton_ =
        buttons->addButton(ui_text(UiTextId::check_again), QDialogButtonBox::ActionRole);
    checkButton_->setObjectName(QStringLiteral("updateCheckAgain"));
    connect(checkButton_, &QPushButton::clicked, this, &UpdateDialog::start_check);
    repositoryUrl_ = configuration_.repository_url;
    repositoryButton_ =
        buttons->addButton(ui_text(UiTextId::update_open_repository), QDialogButtonBox::ActionRole);
    repositoryButton_->setObjectName(QStringLiteral("updateRepositoryPage"));
    repositoryButton_->setVisible(is_secure_https_url(repositoryUrl_));
    connect(repositoryButton_, &QPushButton::clicked, this, &UpdateDialog::confirm_repository_page);
    releaseButton_ =
        buttons->addButton(ui_text(UiTextId::update_open_release), QDialogButtonBox::ActionRole);
    releaseButton_->setObjectName(QStringLiteral("updateReleasePage"));
    releaseButton_->hide();
    connect(releaseButton_, &QPushButton::clicked, this, &UpdateDialog::confirm_release_page);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    checker_ = new UpdateChecker(configuration_, manager, limits, this);
    start_check();
}

void UpdateDialog::start_check() {
    if (!checkButton_->isEnabled()) {
        return;
    }
    checkButton_->setEnabled(false);
    availableValue_->setText(QStringLiteral("-"));
    statusLabel_->setText(ui_text(UiTextId::update_checking));
    notesLabel_->clear();
    notesLabel_->hide();
    releaseUrl_.clear();
    releaseButton_->hide();
    checker_->start(current_version_,
                    [this](const UpdateCheckResult &result) { apply_result(result); });
}

void UpdateDialog::apply_result(const UpdateCheckResult &result) {
    checkButton_->setEnabled(true);
    if (!result.manifest.version_text.isEmpty()) {
        availableValue_->setText(result.manifest.version_text);
    }
    const auto verified_release = result.status == UpdateCheckStatus::current ||
                                  result.status == UpdateCheckStatus::update_available ||
                                  result.status == UpdateCheckStatus::repository_current ||
                                  result.status == UpdateCheckStatus::repository_update_available;
    if (verified_release && is_secure_https_url(result.manifest.release_url)) {
        releaseUrl_ = result.manifest.release_url;
        releaseButton_->show();
    }
    switch (result.status) {
    case UpdateCheckStatus::current:
        statusLabel_->setText(ui_text(UiTextId::update_current));
        break;
    case UpdateCheckStatus::update_available:
        statusLabel_->setText(ui_text(UiTextId::update_available));
        if (!result.manifest.notes.isEmpty()) {
            QStringList lines;
            lines.reserve(result.manifest.notes.size() + 1);
            lines.push_back(ui_text(UiTextId::update_changes));
            for (const auto &note : result.manifest.notes) {
                lines.push_back(QStringLiteral("- %1").arg(note));
            }
            notesLabel_->setText(lines.join(QLatin1Char('\n')));
            notesLabel_->show();
        }
        break;
    case UpdateCheckStatus::repository_current:
        statusLabel_->setText(ui_text(UiTextId::update_repository_current));
        break;
    case UpdateCheckStatus::repository_update_available:
        statusLabel_->setText(ui_text(UiTextId::update_repository_available));
        break;
    case UpdateCheckStatus::no_published_release:
        statusLabel_->setText(ui_text(UiTextId::update_no_published_release));
        break;
    case UpdateCheckStatus::not_configured:
        statusLabel_->setText(ui_text(UiTextId::update_not_configured));
        break;
    case UpdateCheckStatus::network_error:
        statusLabel_->setText(ui_text(UiTextId::update_network_failed));
        break;
    case UpdateCheckStatus::invalid_manifest:
    case UpdateCheckStatus::invalid_signature:
        statusLabel_->setText(ui_text(UiTextId::update_security_failed));
        break;
    case UpdateCheckStatus::wrong_platform:
        statusLabel_->setText(ui_text(UiTextId::update_platform_mismatch));
        break;
    case UpdateCheckStatus::unsupported_verifier:
        statusLabel_->setText(ui_text(UiTextId::update_verifier_unavailable));
        break;
    }
}

void UpdateDialog::confirm_release_page() {
    if (!is_secure_https_url(releaseUrl_)) {
        return;
    }
    if (QMessageBox::question(this, ui_text(UiTextId::update_title),
                              ui_text(UiTextId::confirm_external_link)) == QMessageBox::Yes) {
        QDesktopServices::openUrl(releaseUrl_);
    }
}

void UpdateDialog::confirm_repository_page() {
    if (!is_secure_https_url(repositoryUrl_)) {
        return;
    }
    if (QMessageBox::question(this, ui_text(UiTextId::update_title),
                              ui_text(UiTextId::confirm_external_link)) == QMessageBox::Yes) {
        QDesktopServices::openUrl(repositoryUrl_);
    }
}

} // namespace vove::ui::updates
