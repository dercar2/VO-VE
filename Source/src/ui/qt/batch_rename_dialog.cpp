#include "batch_rename_dialog.hpp"

#include "format_style.hpp"
#include "vove/fileops/file_operation.hpp"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <utility>

namespace vove::ui {
namespace {

QString filename_text(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.filename().native());
#else
    const auto text = path.filename().u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(text.data()),
                             static_cast<qsizetype>(text.size()));
#endif
}

QString issue_text(const fileops::BatchRenameIssue issue, const qsizetype row_count) {
    using Issue = fileops::BatchRenameIssue;
    switch (issue) {
    case Issue::none:
        return QCoreApplication::translate("BatchRenameDialog", "Ready to rename: %1")
            .arg(row_count);
    case Issue::empty_selection:
        return QCoreApplication::translate("BatchRenameDialog", "No files selected");
    case Issue::mixed_directories:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "All files must be in the same folder");
    case Issue::invalid_source:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "The file list changed or contains a duplicate file");
    case Issue::invalid_template:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "Check the template and counter settings");
    case Issue::unknown_token:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "The template contains an unknown variable");
    case Issue::counter_overflow:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "The counter exceeds the allowed range");
    case Issue::invalid_name:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "The generated file name is invalid");
    case Issue::duplicate_destination:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "Multiple files would have the same name");
    case Issue::occupied_destination:
        return QCoreApplication::translate("BatchRenameDialog",
                                           "One of the new names is already in use");
    }
    return QCoreApplication::translate("BatchRenameDialog", "Check the new file names");
}

QHBoxLayout *extension_buttons(QLineEdit &edit, QWidget &parent) {
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    for (const auto &extension :
         {QStringLiteral("jpg"), QStringLiteral("psd"), QStringLiteral("cdr"), QStringLiteral("ai"),
          QStringLiteral("pdf")}) {
        auto *button = new QPushButton(extension, &parent);
        button->setObjectName(QStringLiteral("batchExtensionQuick_%1").arg(extension));
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

class BatchRenamePreviewModel final : public QAbstractTableModel {
  public:
    explicit BatchRenamePreviewModel(QObject *parent) : QAbstractTableModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(plan_.rows.size());
    }

    [[nodiscard]] int columnCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : 3;
    }

    [[nodiscard]] QVariant data(const QModelIndex &index, const int role) const override {
        if (!index.isValid() || index.row() < 0 ||
            static_cast<std::size_t>(index.row()) >= plan_.rows.size()) {
            return {};
        }
        const auto &row = plan_.rows[static_cast<std::size_t>(index.row())];
        if (role == Qt::DisplayRole) {
            if (index.column() == 0) {
                return filename_text(row.source);
            }
            if (index.column() == 1) {
                return filename_text(row.destination);
            }
            if (!row.valid()) {
                return issue_text(row.issue, 1);
            }
            return row.destination == row.source
                       ? QCoreApplication::translate("BatchRenameDialog", "Unchanged")
                       : QCoreApplication::translate("BatchRenameDialog", "Ready");
        }
        if (role == Qt::ToolTipRole && !row.valid()) {
            return QString::fromUtf8(row.detail_utf8);
        }
        if (role == Qt::ForegroundRole && !row.valid()) {
            return QColor(QStringLiteral("#D94848"));
        }
        return {};
    }

    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        if (section == 0) {
            return QCoreApplication::translate("BatchRenameDialog", "Old name");
        }
        if (section == 1) {
            return QCoreApplication::translate("BatchRenameDialog", "New name");
        }
        return QCoreApplication::translate("BatchRenameDialog", "Status");
    }

    void set_plan(fileops::BatchRenamePlan plan) {
        beginResetModel();
        plan_ = std::move(plan);
        endResetModel();
    }

  private:
    fileops::BatchRenamePlan plan_;
};

BatchRenameDialog::BatchRenameDialog(std::vector<fileops::BatchRenameSource> sources,
                                     std::vector<std::filesystem::path> directory_names,
                                     QWidget *parent)
    : QDialog(parent), sources_(std::move(sources)), directoryNames_(std::move(directory_names)) {
    using namespace std::chrono;
    options_.current_unix_ns =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    build_ui();
    rebuild_plan();
}

BatchRenameDialog::~BatchRenameDialog() = default;

const fileops::BatchRenamePlan &BatchRenameDialog::plan() const noexcept {
    return plan_;
}

const fileops::BatchRenameOptions &BatchRenameDialog::options() const noexcept {
    return options_;
}

void BatchRenameDialog::build_ui() {
    setObjectName(QStringLiteral("batchRenameDialog"));
    setWindowTitle(QCoreApplication::translate("BatchRenameDialog", "Batch Rename"));
    resize(780, 560);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    templateEdit_ = new QLineEdit(QStringLiteral("{name}"), this);
    templateEdit_->setObjectName(QStringLiteral("batchTemplate"));
    form->addRow(QCoreApplication::translate("BatchRenameDialog", "Name template"), templateEdit_);

    auto *tokens = new QHBoxLayout;
    tokens->setContentsMargins(0, 0, 0, 0);
    struct TokenButton {
        QString object_name;
        QString label;
        QString token;
        QString tooltip;
    };
    const auto add_token = [this, tokens](const TokenButton &specification) {
        auto *button = new QToolButton(this);
        button->setObjectName(specification.object_name);
        button->setText(specification.label);
        button->setToolTip(specification.tooltip);
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this,
                [this, token = specification.token] { insert_token(token); });
        tokens->addWidget(button);
    };
    add_token(
        {.object_name = QStringLiteral("batchTokenName"),
         .label = QCoreApplication::translate("BatchRenameDialog", "Name"),
         .token = QStringLiteral("{name}"),
         .tooltip = QCoreApplication::translate("BatchRenameDialog", "Insert the original name")});
    add_token({.object_name = QStringLiteral("batchTokenCounter"),
               .label = QCoreApplication::translate("BatchRenameDialog", "Counter"),
               .token = QStringLiteral("{counter}"),
               .tooltip =
                   QCoreApplication::translate("BatchRenameDialog", "Insert the sequence number")});
    add_token({.object_name = QStringLiteral("batchTokenDate"),
               .label = QCoreApplication::translate("BatchRenameDialog", "Date"),
               .token = QStringLiteral("{date}"),
               .tooltip = QCoreApplication::translate("BatchRenameDialog", "Insert the date")});
    add_token({.object_name = QStringLiteral("batchTokenTime"),
               .label = QCoreApplication::translate("BatchRenameDialog", "Time"),
               .token = QStringLiteral("{time}"),
               .tooltip = QCoreApplication::translate("BatchRenameDialog", "Insert the time")});
    tokens->addStretch(1);
    form->addRow(QCoreApplication::translate("BatchRenameDialog", "Variables"), tokens);

    auto *counter = new QHBoxLayout;
    counterStart_ = new QSpinBox(this);
    counterStart_->setObjectName(QStringLiteral("batchCounterStart"));
    counterStart_->setRange(0, 999'999'999);
    counterStart_->setValue(1);
    counterStep_ = new QSpinBox(this);
    counterStep_->setObjectName(QStringLiteral("batchCounterStep"));
    counterStep_->setRange(1, 999'999'999);
    counterStep_->setValue(1);
    counterPadding_ = new QSpinBox(this);
    counterPadding_->setObjectName(QStringLiteral("batchCounterPadding"));
    counterPadding_->setRange(1, 20);
    counterPadding_->setValue(1);
    counter->addWidget(new QLabel(QCoreApplication::translate("BatchRenameDialog", "Start"), this));
    counter->addWidget(counterStart_);
    counter->addWidget(new QLabel(QCoreApplication::translate("BatchRenameDialog", "Step"), this));
    counter->addWidget(counterStep_);
    counter->addWidget(
        new QLabel(QCoreApplication::translate("BatchRenameDialog", "Padding"), this));
    counter->addWidget(counterPadding_);
    counter->addStretch(1);
    form->addRow(QCoreApplication::translate("BatchRenameDialog", "Counter"), counter);

    auto *date_time = new QHBoxLayout;
    dateTimeSource_ = new QComboBox(this);
    dateTimeSource_->setObjectName(QStringLiteral("batchDateTimeSource"));
    dateTimeSource_->addItems(
        {QCoreApplication::translate("BatchRenameDialog", "File modification time"),
         QCoreApplication::translate("BatchRenameDialog", "Current date and time")});
    dateFormat_ = new QComboBox(this);
    dateFormat_->setObjectName(QStringLiteral("batchDateFormat"));
    dateFormat_->addItems(
        {QStringLiteral("2026-08-14"), QStringLiteral("20260814"), QStringLiteral("14-08-2026")});
    timeFormat_ = new QComboBox(this);
    timeFormat_->setObjectName(QStringLiteral("batchTimeFormat"));
    timeFormat_->addItems(
        {QStringLiteral("21-05"), QStringLiteral("2105"), QStringLiteral("21-05-37")});
    date_time->addWidget(dateTimeSource_, 2);
    date_time->addWidget(dateFormat_, 1);
    date_time->addWidget(timeFormat_, 1);
    form->addRow(QCoreApplication::translate("BatchRenameDialog", "Date and time"), date_time);

    const auto has_files = std::any_of(sources_.begin(), sources_.end(), [](const auto &source) {
        return source.object_kind == fileops::OperationObjectKind::regular_file;
    });
    if (has_files) {
        auto *extension = new QHBoxLayout;
        extensionEdit_ = new QLineEdit(this);
        extensionEdit_->setObjectName(QStringLiteral("batchExtension"));
        extensionEdit_->setPlaceholderText(
            QCoreApplication::translate("BatchRenameDialog", "without a dot; leave blank to keep"));
        extension->addWidget(new QLabel(QStringLiteral("."), this));
        extension->addWidget(extensionEdit_, 1);
        form->addRow(QCoreApplication::translate("BatchRenameDialog", "Extension"), extension);
        form->addRow(QString{}, extension_buttons(*extensionEdit_, *this));
    }
    root->addLayout(form);

    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName(QStringLiteral("batchValidation"));
    root->addWidget(validationLabel_);

    previewModel_ = new BatchRenamePreviewModel(this);
    previewTable_ = new QTableView(this);
    previewTable_->setObjectName(QStringLiteral("batchPreview"));
    previewTable_->setModel(previewModel_);
    previewTable_->setAlternatingRowColors(true);
    previewTable_->setSelectionMode(QAbstractItemView::NoSelection);
    previewTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    previewTable_->verticalHeader()->setVisible(false);
    previewTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    previewTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    previewTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    root->addWidget(previewTable_, 1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->setObjectName(QStringLiteral("batchButtons"));
    buttons_->button(QDialogButtonBox::Ok)
        ->setText(QCoreApplication::translate("BatchRenameDialog", "Rename"));
    buttons_->button(QDialogButtonBox::Cancel)
        ->setText(QCoreApplication::translate("BatchRenameDialog", "Cancel"));
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (plan_.valid()) {
            accept();
        }
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons_);

    connect(templateEdit_, &QLineEdit::textChanged, this, [this] { rebuild_plan(); });
    connect(counterStart_, &QSpinBox::valueChanged, this, [this] { rebuild_plan(); });
    connect(counterStep_, &QSpinBox::valueChanged, this, [this] { rebuild_plan(); });
    connect(counterPadding_, &QSpinBox::valueChanged, this, [this] { rebuild_plan(); });
    connect(dateTimeSource_, &QComboBox::currentIndexChanged, this, [this] { rebuild_plan(); });
    connect(dateFormat_, &QComboBox::currentIndexChanged, this, [this] { rebuild_plan(); });
    connect(timeFormat_, &QComboBox::currentIndexChanged, this, [this] { rebuild_plan(); });
    if (extensionEdit_) {
        connect(extensionEdit_, &QLineEdit::textChanged, this, [this] { rebuild_plan(); });
    }
}

void BatchRenameDialog::insert_token(const QString &token) {
    const auto selection_start = templateEdit_->selectionStart();
    const auto insertion_position =
        selection_start >= 0 ? selection_start : templateEdit_->cursorPosition();
    auto insertion = token;
    if (insertion_position > 0 &&
        templateEdit_->text().at(insertion_position - 1) != QLatin1Char('_')) {
        insertion.prepend(QLatin1Char('_'));
    }
    templateEdit_->insert(insertion);
    templateEdit_->setFocus(Qt::OtherFocusReason);
}

void BatchRenameDialog::rebuild_plan() {
    options_.name_template_utf8 = templateEdit_->text().toUtf8().toStdString();
    options_.counter_start = static_cast<std::uint64_t>(counterStart_->value());
    options_.counter_step = static_cast<std::uint64_t>(counterStep_->value());
    options_.counter_padding = static_cast<std::uint8_t>(counterPadding_->value());
    options_.date_time_source = dateTimeSource_->currentIndex() == 0
                                    ? fileops::BatchDateTimeSource::modified
                                    : fileops::BatchDateTimeSource::current;
    options_.date_format = static_cast<fileops::BatchDateFormat>(dateFormat_->currentIndex());
    options_.time_format = static_cast<fileops::BatchTimeFormat>(timeFormat_->currentIndex());
    const auto extension = extensionEdit_ ? extensionEdit_->text() : QString{};
    options_.extension_mode = extension.trimmed().isEmpty()
                                  ? fileops::BatchExtensionMode::preserve
                                  : fileops::BatchExtensionMode::replace;
    options_.replacement_extension_utf8 = extension.toUtf8().toStdString();
    plan_ = fileops::plan_batch_rename(sources_, directoryNames_, options_);
    previewModel_->set_plan(plan_);
    const auto changed_count =
        std::count_if(plan_.rows.begin(), plan_.rows.end(), [](const auto &row) {
            return row.valid() && row.destination != row.source;
        });
    validationLabel_->setText(
        plan_.valid() && changed_count == 0
            ? QCoreApplication::translate("BatchRenameDialog", "Unchanged")
            : issue_text(plan_.issue, static_cast<qsizetype>(changed_count)));
    validationLabel_->setStyleSheet(plan_.valid() ? QString{} : QStringLiteral("color: #D94848;"));
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(plan_.valid());
}

} // namespace vove::ui
