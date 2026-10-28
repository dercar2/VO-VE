#include "project_dialogs.hpp"
#include "ui_text.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>

namespace vove::ui {

void show_project_contact_dialog(QWidget *owner, const bool support) {
    QDialog dialog(owner);
    dialog.setObjectName(support ? QStringLiteral("donateDialog")
                                 : QStringLiteral("reportIssueDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(ui_text(support ? UiTextId::donate : UiTextId::report_issue));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    const auto make_text = [&dialog](const QString &name) {
        auto *text = new QTextBrowser(&dialog);
        text->setObjectName(name);
        text->setFrameShape(QFrame::NoFrame);
        text->setOpenLinks(false);
        text->setOpenExternalLinks(false);
        text->document()->setDefaultFont(dialog.font());
        auto text_palette = text->palette();
        text_palette.setColor(QPalette::Base, dialog.palette().color(QPalette::Window));
        text_palette.setColor(QPalette::Text, dialog.palette().color(QPalette::WindowText));
        text->setPalette(text_palette);
        return text;
    };
    QTextBrowser *message{};
    if (support) {
        message = make_text(QStringLiteral("contactMessage"));
        message->setPlainText(
            QCoreApplication::translate("ProjectContactDialog",
                                        "Send us a few words of thanks - that would mean a lot!") +
            QStringLiteral("\n\n") +
            QCoreApplication::translate(
                "ProjectContactDialog",
                "And if you would like to support us financially, cash only. And only in rubles.") +
            QStringLiteral("\n\n") +
            QCoreApplication::translate(
                "ProjectContactDialog",
                "We can also accept a good bottle of vodka, such as: Chistye Rosy (rye), "
                "Belvedere and Belvedere Single Estate Rye (Lake Bartężek or Smogóry Forest), "
                "Chopin Potato Vodka, Polba, Black Cow, Hangar 1, Luksusowa, Beluga "
                "(Noble / Transatlantic Racing), Spelta, Tito’s Handmade Vodka, Ocean Organic "
                "Vodka, Boyd & Blair, Potocki, Koval Millet or Koval Rye, Reyka, Chopin Rye, "
                "and perhaps others we have yet to discover?"));
        message->setMinimumHeight(4 * message->fontMetrics().height());
        layout->addWidget(message);
    }

    QStringList emails{QStringLiteral("dercar@ya.com")};
    if (support) emails.append(QStringLiteral("Rubilaxik@ya.com"));
    QStringList links;
    for (const auto &email : emails) {
        links.append(QStringLiteral("<a href=\"mailto:%1\" style=\"color: %2;\">%1</a>")
                         .arg(email.toHtmlEscaped(),
                              dialog.palette().color(QPalette::WindowText).name()));
    }
    auto *contact = new QLabel(&dialog);
    contact->setObjectName(QStringLiteral("contactEmail"));
    contact->setTextFormat(Qt::RichText);
    contact->setText(links.join(QStringLiteral(" | ")));
    contact->setLayoutDirection(Qt::LeftToRight);
    contact->setTextInteractionFlags(Qt::TextBrowserInteraction);
    contact->setOpenExternalLinks(false);
    layout->addWidget(contact);

    auto *error = make_text(QStringLiteral("contactError"));
    error->setMinimumHeight(2 * error->fontMetrics().height());
    error->setMaximumHeight(4 * error->fontMetrics().height());
    error->hide();
    layout->addWidget(error);
    QObject::connect(contact, &QLabel::linkActivated, &dialog, [emails, error](const QString &href) {
        const QUrl mailto(href);
        const auto email = std::find_if(emails.cbegin(), emails.cend(), [&](const QString &address) {
            return mailto == QUrl(QStringLiteral("mailto:") + address);
        });
        if (email == emails.cend()) return;
        const auto opened = QDesktopServices::openUrl(mailto);
        error->setPlainText(opened
                                ? QString{}
                                : QCoreApplication::translate(
                                      "ProjectContactDialog",
                                      "Could not open the mail application. You can write to %1.")
                                      .arg(*email));
        error->setVisible(!opened);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.ensurePolished();
    const auto available = owner->screen()->availableGeometry();
    const auto width = std::min(600, available.width() - 40);
    auto height = dialog.sizeHint().height();
    if (message != nullptr) {
        const auto margins = layout->contentsMargins();
        QTextDocument measurement;
        measurement.setDefaultFont(message->document()->defaultFont());
        measurement.setPlainText(message->toPlainText());
        measurement.setTextWidth(width - margins.left() - margins.right());
        const auto *message_item = layout->itemAt(layout->indexOf(message));
        const auto layout_height = layout->hasHeightForWidth()
                                       ? layout->totalHeightForWidth(width)
                                       : layout->totalSizeHint().height();
        // Preserve the polished title row; replace only QTextBrowser's generic size hint.
        height = layout_height - message_item->sizeHint().height() +
                 std::max(message_item->minimumSize().height(),
                          qCeil(measurement.size().height()) +
                              2 * message->fontMetrics().height());
    }
    dialog.resize(width, std::min(height, available.height() - 40));
    dialog.exec();
}

void show_project_components_dialog(QWidget *owner, ComponentSpecsProvider specs,
                                    RefreshComponents refresh) {
    QDialog dialog(owner);
    dialog.setObjectName(QStringLiteral("componentsDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(ui_text(UiTextId::components_title));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);
    auto *components = new ComponentsPanel(&dialog);
    layout->addWidget(components, 1);

    const QPointer<QDialog> dialog_pointer{&dialog};
    const QPointer<ComponentsPanel> components_pointer{components};
    const auto populate = [components_pointer, dialog_pointer, specs](const bool refresh) {
        if (components_pointer.isNull() || dialog_pointer.isNull()) return;
        components_pointer->set_components(specs(refresh));
    };
    populate(false);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, &dialog);
    auto *check_again = new QPushButton(ui_text(UiTextId::check_again), &dialog);
    check_again->setObjectName(QStringLiteral("checkComponentsAgain"));
    buttons->addButton(check_again, QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    const QPointer<QPushButton> check_pointer{check_again};
    QObject::connect(check_again, &QPushButton::clicked, &dialog,
            [dialog_pointer, check_pointer, populate, refresh] {
                if (!check_pointer.isNull()) check_pointer->setEnabled(false);
                refresh([dialog_pointer, check_pointer, populate] {
                    if (dialog_pointer.isNull()) return;
                    populate(true);
                    if (!check_pointer.isNull()) check_pointer->setEnabled(true);
                });
            });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.ensurePolished();
    layout->activate();
    const auto available = owner->screen()->availableGeometry();
    const auto width = std::min(800, available.width() - 40);
    const auto margins = layout->contentsMargins();
    const auto content_width = width - margins.left() - margins.right();
    auto natural_height = margins.top() + margins.bottom() +
                          layout->spacing() * (layout->count() - 1);
    for (int index = 0; index < layout->count(); ++index) {
        const auto *item = layout->itemAt(index);
        // The inner scroll area hides the panel's natural height from QWidgetItem.
        natural_height += item->widget() == components
                              ? components->heightForWidth(content_width)
                              : (item->hasHeightForWidth() ? item->heightForWidth(content_width)
                                                           : item->sizeHint().height());
    }
    dialog.resize(width, std::min(available.height() - 60, natural_height));
    check_again->click();
    dialog.exec();
}

} // namespace vove::ui
