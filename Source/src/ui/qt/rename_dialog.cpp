#include "rename_dialog.hpp"

#include "format_style.hpp"
#include "vove/fileops/file_operation.hpp"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <filesystem>
#include <string>

namespace vove::ui {
namespace {

QString normalized_extension(QString extension) {
    if (extension.startsWith(QLatin1Char('.'))) {
        extension.remove(0, 1);
    }
    return extension;
}

bool valid_filename(const QString &filename) {
    if (!filename.isValidUtf16()) {
        return false;
    }
    try {
#ifdef Q_OS_WIN
        // Narrow Windows paths would interpret Unicode through the process code page.
        const auto path = std::filesystem::path(filename.toStdWString());
#else
        const auto utf8 = filename.toUtf8();
        const auto path = std::filesystem::path(
            std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
        std::string detail;
        return fileops::valid_destination_filename(path, detail);
    } catch (const std::filesystem::filesystem_error &) {
        return false;
    }
}

QHBoxLayout *extension_buttons(QLineEdit &edit, QWidget &parent) {
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    for (const auto &extension :
         {QStringLiteral("jpg"), QStringLiteral("psd"), QStringLiteral("cdr"), QStringLiteral("ai"),
          QStringLiteral("pdf")}) {
        auto *button = new QPushButton(extension, &parent);
        button->setObjectName(QStringLiteral("renameExtensionQuick_%1").arg(extension));
        const auto color = format_color(extension, parent.palette().window().color()).name();
        button->setStyleSheet(QStringLiteral("QPushButton { color: %1; border: 1px solid %1; "
                                             "padding: 2px 7px; background: transparent; } "
                                             "QPushButton:hover { background: %1; color: white; }")
                                  .arg(color));
        QObject::connect(button, &QPushButton::clicked, &edit,
                         [&edit, extension] { edit.setText(extension); });
        layout->addWidget(button);
    }
    layout->addStretch(1);
    return layout;
}

} // namespace

RenameDialog::RenameDialog(const QString &file_name, QWidget *parent, const bool directory)
    : QDialog(parent), originalFileName_(file_name) {
    setObjectName(QStringLiteral("renameDialog"));
    setWindowTitle(QCoreApplication::translate("RenameDialog", "Rename"));

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    const auto dot = file_name.lastIndexOf(QLatin1Char('.'));
    const auto has_extension = !directory && dot > 0 && dot + 1 < file_name.size();
    nameEdit_ = new QLineEdit(has_extension ? file_name.left(dot) : file_name, this);
    nameEdit_->setObjectName(QStringLiteral("renameName"));
    const auto name_label = QCoreApplication::translate("RenameDialog", "Name");
    nameEdit_->setAccessibleName(name_label);
    form->addRow(name_label, nameEdit_);

    if (!directory) {
        extensionEdit_ = new QLineEdit(has_extension ? file_name.mid(dot + 1) : QString{}, this);
        extensionEdit_->setObjectName(QStringLiteral("renameExtension"));
        const auto extension_label = QCoreApplication::translate("RenameDialog", "Extension");
        extensionEdit_->setAccessibleName(extension_label);
        form->addRow(extension_label, extensionEdit_);
        form->addRow(QString{}, extension_buttons(*extensionEdit_, *this));
    }
    root->addLayout(form);

    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName(QStringLiteral("renameValidation"));
    validationLabel_->setTextFormat(Qt::PlainText);
    validationLabel_->setWordWrap(true);
    root->addWidget(validationLabel_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->setObjectName(QStringLiteral("renameButtons"));
    buttons_->button(QDialogButtonBox::Ok)
        ->setText(QCoreApplication::translate("RenameDialog", "Rename"));
    buttons_->button(QDialogButtonBox::Ok)->setDefault(true);
    buttons_->button(QDialogButtonBox::Cancel)
        ->setText(QCoreApplication::translate("RenameDialog", "Cancel"));
    connect(buttons_, &QDialogButtonBox::accepted, this, &RenameDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons_);

    connect(nameEdit_, &QLineEdit::textChanged, this, [this] { update_validation(); });
    if (extensionEdit_) {
        connect(extensionEdit_, &QLineEdit::textChanged, this, [this] { update_validation(); });
        setTabOrder(nameEdit_, extensionEdit_);
    }
    update_validation();
    nameEdit_->setFocus(Qt::OtherFocusReason);
    nameEdit_->selectAll();
    resize(400, sizeHint().height());
}

QString RenameDialog::file_name() const {
    if (!extensionEdit_) {
        return nameEdit_->text();
    }
    const auto extension = normalized_extension(extensionEdit_->text());
    return extension.isEmpty() ? nameEdit_->text()
                               : nameEdit_->text() + QLatin1Char('.') + extension;
}

QString RenameDialog::validation_error() const {
    const auto name = nameEdit_->text();
    if (name.isEmpty()) {
        return extensionEdit_ ? QCoreApplication::translate("RenameDialog", "Enter a file name")
                              : QCoreApplication::translate("RenameDialog", "Enter a folder name");
    }
    if (extensionEdit_) {
        const auto extension = normalized_extension(extensionEdit_->text());
        if ((!extensionEdit_->text().isEmpty() && extension.isEmpty()) ||
            extension.contains(QLatin1Char('.'))) {
            return QCoreApplication::translate("RenameDialog", "Enter a single extension");
        }
    }
    const auto candidate = file_name();
    if (name == QStringLiteral(".") || name == QStringLiteral("..") ||
        !valid_filename(candidate)) {
        return extensionEdit_
                   ? QCoreApplication::translate("RenameDialog", "Enter a valid file name")
                   : QCoreApplication::translate("RenameDialog", "Enter a valid folder name");
    }
    return {};
}

bool RenameDialog::update_validation() {
    const auto error = validation_error();
    validationLabel_->setText(error);
    const auto acceptable = error.isEmpty() && file_name() != originalFileName_;
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(acceptable);
    return acceptable;
}

void RenameDialog::accept() {
    if (update_validation()) {
        QDialog::accept();
    }
}

} // namespace vove::ui
