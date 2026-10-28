#include "settings_dialog.hpp"
#include "ui_text.hpp"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace vove::ui {

std::optional<SettingsDialogResult>
show_settings_dialog(QWidget *owner, const SettingsDialogData &data,
                     const SettingsDialogActions &actions) {
    QDialog dialog(owner);
    dialog.setObjectName(QStringLiteral("settingsDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(ui_text(UiTextId::settings_title));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setObjectName(QStringLiteral("settingsScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(12, 8, 12, 8);
    content_layout->setSpacing(12);
    content_layout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    const auto add_section_break = [content_layout, content] {
        content_layout->addSpacing(4);
        auto *separator = new QFrame(content);
        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setProperty("settingsSectionSeparator", true);
        content_layout->addWidget(separator);
        content_layout->addSpacing(4);
    };
    const auto configure_compact_list = [&dialog](QListWidget *list) {
        const auto update_height = [list] {
            const auto row_height =
                std::max(list->sizeHintForRow(0), list->fontMetrics().lineSpacing() + 4);
            const auto visible_rows = std::clamp(list->count(), 2, 8);
            const auto height = visible_rows * row_height + 2 * list->frameWidth();
            list->setMinimumHeight(height);
            list->setMaximumHeight(height);
            list->setProperty("compactVisibleRows", visible_rows);
        };
        QObject::connect(list->model(), &QAbstractItemModel::rowsInserted, &dialog,
                         [update_height] { update_height(); });
        QObject::connect(list->model(), &QAbstractItemModel::rowsRemoved, &dialog,
                         [update_height] { update_height(); });
        update_height();
    };
    auto *title = new QLabel(ui_text(UiTextId::search_roots_title), &dialog);
    auto title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    content_layout->addWidget(title);
    auto *explanation = new QLabel(ui_text(UiTextId::search_roots_description), &dialog);
    explanation->setWordWrap(true);
    content_layout->addWidget(explanation);

    auto *roots = new QListWidget(&dialog);
    roots->setObjectName(QStringLiteral("searchRootsList"));
    roots->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &path : data.search_roots) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(path), roots);
        item->setData(Qt::UserRole, path);
    }
    configure_compact_list(roots);
    roots->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    roots->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_layout->addWidget(roots);

    auto *path_row = new QHBoxLayout;
    auto *path = new QLineEdit(&dialog);
    path->setObjectName(QStringLiteral("searchRootPath"));
    path->setPlaceholderText(ui_text(UiTextId::local_or_unc_path));
    auto *browse = new QPushButton(ui_text(UiTextId::browse), &dialog);
    auto *add = new QPushButton(ui_text(UiTextId::add), &dialog);
    auto *remove = new QPushButton(ui_text(UiTextId::remove), &dialog);
    path_row->addWidget(path, 1);
    path_row->addWidget(browse);
    path_row->addWidget(add);
    path_row->addWidget(remove);
    content_layout->addLayout(path_row);

    add_section_break();
    auto *applications_title = new QLabel(ui_text(UiTextId::external_applications_title), &dialog);
    auto applications_title_font = applications_title->font();
    applications_title_font.setBold(true);
    applications_title->setFont(applications_title_font);
    content_layout->addWidget(applications_title);
    auto *applications_explanation =
        new QLabel(ui_text(UiTextId::external_applications_description), &dialog);
    applications_explanation->setWordWrap(true);
    content_layout->addWidget(applications_explanation);
    auto *applications = new QListWidget(&dialog);
    applications->setObjectName(QStringLiteral("externalApplicationsList"));
    applications->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &application : data.applications) {
        auto *item = new QListWidgetItem(application.name, applications);
        item->setData(Qt::UserRole, application.path);
        item->setToolTip(QDir::toNativeSeparators(application.path));
    }
    configure_compact_list(applications);
    content_layout->addWidget(applications);
    auto *remove_application = new QPushButton(ui_text(UiTextId::remove), &dialog);
    remove_application->setObjectName(QStringLiteral("removeExternalApplication"));
    remove_application->setEnabled(false);
    content_layout->addWidget(remove_application, 0, Qt::AlignTrailing);

    add_section_break();
    auto *startup_title = new QLabel(ui_text(UiTextId::startup_title), &dialog);
    auto startup_title_font = startup_title->font();
    startup_title_font.setBold(true);
    startup_title->setFont(startup_title_font);
    content_layout->addWidget(startup_title);
    auto *start_with_system = new QCheckBox(ui_text(UiTextId::start_with_system), &dialog);
    start_with_system->setObjectName(QStringLiteral("startWithSystem"));
    start_with_system->setChecked(data.startup_enabled);
    content_layout->addWidget(start_with_system);

    add_section_break();
    auto *cache_title = new QLabel(ui_text(UiTextId::preview_cache), &dialog);
    auto cache_title_font = cache_title->font();
    cache_title_font.setBold(true);
    cache_title->setFont(cache_title_font);
    content_layout->addWidget(cache_title);
    auto *cache_row = new QHBoxLayout;
    auto *cache_occupied = new QLabel(ui_text(UiTextId::cache_occupied), &dialog);
    auto *cache_usage = new QLabel(ui_text(UiTextId::calculating), &dialog);
    cache_usage->setObjectName(QStringLiteral("previewCacheUsage"));
    auto *cache_limit_label = new QLabel(ui_text(UiTextId::cache_limit), &dialog);
    auto *cache_limit = new QComboBox(&dialog);
    cache_limit->setObjectName(QStringLiteral("previewCacheLimit"));
    constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
    for (const auto gib : {1ULL, 2ULL, 5ULL, 10ULL, 20ULL, 50ULL}) {
        cache_limit->addItem(QStringLiteral("%1 GiB").arg(gib),
                             QVariant::fromValue<qulonglong>(gib * gibibyte));
    }
    cache_limit->setCurrentIndex(
        std::max(0, cache_limit->findData(QVariant::fromValue<qulonglong>(data.cache_maximum_bytes))));
    auto *clear_cache = new QPushButton(ui_text(UiTextId::clear_cache), &dialog);
    clear_cache->setObjectName(QStringLiteral("clearPreviewCache"));
    cache_row->addWidget(cache_occupied);
    cache_row->addWidget(cache_usage, 1);
    cache_row->addWidget(cache_limit_label);
    cache_row->addWidget(cache_limit);
    cache_row->addWidget(clear_cache);
    content_layout->addLayout(cache_row);
    auto *cache_error = new QLabel(&dialog);
    cache_error->setObjectName(QStringLiteral("previewCacheError"));
    cache_error->setStyleSheet(QStringLiteral("color: #c62828;"));
    cache_error->hide();
    content_layout->addWidget(cache_error);

    add_section_break();
    auto *trash_title =
        new QLabel(QCoreApplication::translate("MainWindow", "VO-VE Trash"), &dialog);
    auto trash_title_font = trash_title->font();
    trash_title_font.setBold(true);
    trash_title->setFont(trash_title_font);
    content_layout->addWidget(trash_title);
    auto *trash_row = new QHBoxLayout;
    auto *trash_limit_label = new QLabel(
        QCoreApplication::translate("MainWindow", "Maximum per storage device"), &dialog);
    auto *trash_limit = new QComboBox(&dialog);
    trash_limit->setObjectName(QStringLiteral("trashMaximumBytes"));
    for (const auto gib : {10ULL, 20ULL, 50ULL, 100ULL, 200ULL}) {
        trash_limit->addItem(QStringLiteral("%1 GiB").arg(gib),
                             QVariant::fromValue<qulonglong>(gib * gibibyte));
    }
    trash_limit->setCurrentIndex(
        std::max(0, trash_limit->findData(QVariant::fromValue<qulonglong>(data.trash_maximum_bytes))));
    auto *empty_trash =
        new QPushButton(QCoreApplication::translate("MainWindow", "Empty Trash…"), &dialog);
    empty_trash->setObjectName(QStringLiteral("emptyTrashFromSettings"));
    trash_row->addWidget(trash_limit_label);
    trash_row->addWidget(trash_limit);
    trash_row->addStretch(1);
    trash_row->addWidget(empty_trash);
    content_layout->addLayout(trash_row);
    auto *trash_explanation = new QLabel(
        QCoreApplication::translate(
            "MainWindow",
            "The effective limit is the smaller of this value and 10% of the storage capacity. "
            "Trash is emptied only manually."),
        &dialog);
    trash_explanation->setWordWrap(true);
    content_layout->addWidget(trash_explanation);
    QObject::connect(empty_trash, &QPushButton::clicked, &dialog,
                     [&dialog, owner, action = actions.empty_trash] {
        dialog.accept();
        QTimer::singleShot(0, owner, action);
    });

    const QPointer<QLabel> cache_usage_pointer{cache_usage};
    const QPointer<QLabel> cache_error_pointer{cache_error};
    const QPointer<QPushButton> clear_cache_pointer{clear_cache};
    const auto show_cache_result = [cache_usage_pointer, cache_error_pointer,
                                    clear_cache_pointer](const SettingsCacheUsage reply) {
        if (cache_usage_pointer.isNull()) return;
        if (reply.success) {
            cache_usage_pointer->setText(
                QLocale().formattedDataSize(static_cast<qint64>(reply.bytes)));
            if (!cache_error_pointer.isNull()) cache_error_pointer->hide();
        } else {
            cache_usage_pointer->setText(QStringLiteral("—"));
            if (!cache_error_pointer.isNull()) {
                cache_error_pointer->setText(ui_text(UiTextId::cache_clear_failed));
                cache_error_pointer->show();
            }
        }
        if (!clear_cache_pointer.isNull()) clear_cache_pointer->setEnabled(true);
    };
    if (!actions.inspect_cache(show_cache_result)) show_cache_result({});
    QObject::connect(clear_cache, &QPushButton::clicked, &dialog,
            [owner, actions, cache_usage_pointer, clear_cache_pointer, show_cache_result] {
        if (!clear_cache_pointer.isNull()) clear_cache_pointer->setEnabled(false);
        if (!cache_usage_pointer.isNull()) {
            cache_usage_pointer->setText(ui_text(UiTextId::calculating));
        }
        if (!actions.clear_cache([owner, actions, cache_usage_pointer,
                                   show_cache_result](const SettingsCacheUsage reply) {
            show_cache_result(reply);
            if (!reply.success) return;
            actions.cache_cleared();
            if (!cache_usage_pointer.isNull()) {
                cache_usage_pointer->setText(ui_text(UiTextId::calculating));
            }
            QTimer::singleShot(2000, owner, [actions, show_cache_result] {
                if (!actions.inspect_cache(show_cache_result)) show_cache_result({});
            });
        })) show_cache_result({});
    });

    const auto add_path = [roots, path] {
        const auto candidate = QDir::cleanPath(QDir::fromNativeSeparators(path->text().trimmed()));
#ifdef Q_OS_WIN
        const auto native = std::filesystem::path(candidate.toStdWString());
        constexpr auto path_case = Qt::CaseInsensitive;
#else
        const auto native = std::filesystem::path(candidate.toUtf8().toStdString());
        constexpr auto path_case = Qt::CaseSensitive;
#endif
        if (candidate.isEmpty() || !native.is_absolute()) return;
        for (int index = 0; index < roots->count(); ++index) {
            if (roots->item(index)->data(Qt::UserRole).toString().compare(candidate, path_case) == 0) {
                roots->setCurrentRow(index);
                path->clear();
                return;
            }
        }
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(candidate), roots);
        item->setData(Qt::UserRole, candidate);
        roots->setCurrentItem(item);
        path->clear();
    };
    QObject::connect(path, &QLineEdit::returnPressed, &dialog, add_path);
    QObject::connect(add, &QPushButton::clicked, &dialog, add_path);
    QObject::connect(browse, &QPushButton::clicked, &dialog,
                     [path, choose = actions.choose_search_root] {
        const auto selected = choose();
        if (!selected.isEmpty()) path->setText(selected);
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [roots] {
        const auto selected = roots->selectedItems();
        for (auto *item : selected) delete roots->takeItem(roots->row(item));
    });
    QObject::connect(applications, &QListWidget::itemSelectionChanged, &dialog,
                     [applications, remove_application] {
        remove_application->setEnabled(!applications->selectedItems().isEmpty());
    });
    QObject::connect(remove_application, &QPushButton::clicked, &dialog, [applications] {
        const auto selected = applications->selectedItems();
        for (auto *item : selected) delete applications->takeItem(applications->row(item));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close,
                                         Qt::Horizontal, &dialog);
    buttons->setContentsMargins(12, 0, 12, 0);
    auto *check_updates = new QPushButton(ui_text(UiTextId::check_updates), &dialog);
    check_updates->setObjectName(QStringLiteral("settingsCheckUpdates"));
    check_updates->setAutoDefault(false);
    buttons->addButton(check_updates, QDialogButtonBox::ActionRole);
    QObject::connect(check_updates, &QPushButton::clicked, owner, actions.check_updates);
    buttons->button(QDialogButtonBox::Save)->setText(ui_text(UiTextId::save));
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     [&dialog, start_with_system, set_startup = actions.set_startup_enabled] {
        QString error;
        if (!set_startup(start_with_system->isChecked(), &error)) {
            QMessageBox message(QMessageBox::Warning, ui_text(UiTextId::settings_title),
                                ui_text(UiTextId::startup_change_failed), QMessageBox::Ok, &dialog);
            message.setInformativeText(error);
            message.exec();
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto fit_dialog_to_content = [&dialog, content, content_layout, buttons, layout] {
        content_layout->activate();
        const auto available = dialog.screen()->availableGeometry().size();
        const auto margins = layout->contentsMargins();
        const auto natural_height = content->sizeHint().height() + buttons->sizeHint().height() +
                                    margins.top() + margins.bottom() + layout->spacing();
        const auto width = std::min(700, std::max(360, available.width() - 40));
        const auto height_limit = std::max(240, available.height() - 80);
        dialog.resize(width, std::min(height_limit, std::max(360, natural_height)));
    };
    const auto schedule_dialog_fit = [&dialog, fit_dialog_to_content] {
        QTimer::singleShot(0, &dialog, fit_dialog_to_content);
    };
    QObject::connect(roots->model(), &QAbstractItemModel::rowsInserted, &dialog,
                     [schedule_dialog_fit] { schedule_dialog_fit(); });
    QObject::connect(roots->model(), &QAbstractItemModel::rowsRemoved, &dialog,
                     [schedule_dialog_fit] { schedule_dialog_fit(); });
    QObject::connect(applications->model(), &QAbstractItemModel::rowsInserted, &dialog,
                     [schedule_dialog_fit] { schedule_dialog_fit(); });
    QObject::connect(applications->model(), &QAbstractItemModel::rowsRemoved, &dialog,
                     [schedule_dialog_fit] { schedule_dialog_fit(); });
    fit_dialog_to_content();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;

    SettingsDialogResult result;
    for (int index = 0; index < roots->count(); ++index) {
        result.search_roots.push_back(roots->item(index)->data(Qt::UserRole).toString());
    }
    for (int index = 0; index < applications->count(); ++index) {
        result.retained_application_paths.push_back(applications->item(index)->data(Qt::UserRole).toString());
    }
    result.cache_maximum_bytes = cache_limit->currentData().toULongLong();
    result.trash_maximum_bytes = trash_limit->currentData().toULongLong();
    return result;
}

} // namespace vove::ui
