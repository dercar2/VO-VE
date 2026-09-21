#include "about_dialog.hpp"
#include "release_configuration.hpp"
#include "ui_text.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSysInfo>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace vove::ui {
namespace {

QString about_text(const char *source) {
    return QCoreApplication::translate("AboutDialog", source).toHtmlEscaped();
}

QString link(const QString &url, const QString &label) {
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), label.toHtmlEscaped());
}

class WrappingLabel final : public QLabel {
  public:
    using QLabel::QLabel;

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        if (updatingMinimum_) {
            return;
        }
        QScopedValueRollback<bool> updating(updatingMinimum_, true);
        // QLabel's heightForWidth includes minimumHeight; discard the previous width's limit.
        setMinimumHeight(0);
        setMinimumHeight(heightForWidth(width()));
    }

  private:
    bool updatingMinimum_{};
};

} // namespace

AboutDialog::AboutDialog(const QString &version, QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aboutDialog"));
    setProperty("mainMenuDialog", true);
    setWindowTitle(ui_text(UiTextId::about_title));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *details = new QTextBrowser(this);
    details->setObjectName(QStringLiteral("aboutDetails"));
    details->setAccessibleName(ui_text(UiTextId::about_title));
    details->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    details->setOpenLinks(false);
    details->setOpenExternalLinks(false);
    details->document()->setDefaultFont(font());
    details->document()->setDocumentMargin(12);
    auto heading_font = font();
    heading_font.setBold(true);
    if (heading_font.pointSizeF() > 0) {
        heading_font.setPointSizeF(heading_font.pointSizeF() * 1.2);
    } else if (heading_font.pixelSize() > 0) {
        heading_font.setPixelSize(std::max(1, qRound(heading_font.pixelSize() * 1.2)));
    }
    auto detail_palette = details->palette();
    detail_palette.setColor(QPalette::Base, palette().color(QPalette::Window));
    detail_palette.setColor(QPalette::Text, palette().color(QPalette::WindowText));
    details->setPalette(detail_palette);
    details->document()->setDefaultStyleSheet(
        QStringLiteral("p { margin-top: 0; margin-bottom: 10px; }"
                       "h3 { margin: 12px 0 4px; }"
                       "li { margin-bottom: 6px; }"
                       "a { color: %1; }")
            .arg(palette().color(QPalette::WindowText).name()));

    auto html = QStringLiteral("<p>%1</p><h3>%2</h3><p>%3</p><h3>%4</h3><ol>")
                    .arg(ui_text(UiTextId::about_summary).toHtmlEscaped(),
                         about_text(QT_TRANSLATE_NOOP("AboutDialog", "Who it is for")),
                         about_text(QT_TRANSLATE_NOOP(
                             "AboutDialog", "For designers, print shops, publishers and everyone "
                                            "who needs fast, convenient browsing of large graphics "
                                            "collections, including network locations.")),
                         about_text(QT_TRANSLATE_NOOP("AboutDialog", "Key features")));
    for (const auto *feature :
         {QT_TRANSLATE_NOOP("AboutDialog",
                            "Preview a wide range of everyday graphics formats, including "
                            "embedded previews from crappy CDR files."),
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Responsive browsing of graphics on servers, NAS devices and other "
                            "network locations."),
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Copy, move, rename and delete files, including over the network. "
                            "Interrupted transfers can be continued after reconnecting when "
                            "recovery is available."),
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Instant local name filtering in the current folder as you type."),
#ifdef Q_OS_WIN
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Instant global name search as you type, inside VO-VE, with the "
                            "outstanding Everything (installed separately)."),
#else
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Global name search inside VO-VE with plocate (installed "
                            "separately)."),
#endif
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Fast viewing in a dedicated preview pane, including multi-page "
                            "documents."),
          QT_TRANSLATE_NOOP("AboutDialog", "Color-coded file format labels."),
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Folder thumbnails with previews of up to four documents inside."),
          QT_TRANSLATE_NOOP("AboutDialog",
                            "A vintage interface inspired by electronic circuit boards.")}) {
        html += QStringLiteral("<li>%1</li>").arg(about_text(feature));
    }
    html += QStringLiteral("<li>%1 %2</li><li>%3 %4</li><li>%5</li>")
                .arg(about_text(QT_TRANSLATE_NOOP("AboutDialog", "Persistent preview cache:")),
                     about_text(QT_TRANSLATE_NOOP(
                         "AboutDialog", "View again without reprocessing heavy documents.")),
                     about_text(QT_TRANSLATE_NOOP("AboutDialog", "Color management:")),
                     about_text(
                         QT_TRANSLATE_NOOP("AboutDialog", "Display using embedded ICC profiles.")),
                     about_text(QT_TRANSLATE_NOOP(
                         "AboutDialog", "Page-count markers on multi-page document thumbnails.")));
    const auto repository_url = updates::compiled_release_configuration().repository_url;
    const auto repository_link = link(repository_url.toString(QUrl::FullyEncoded),
                                      QStringLiteral("github.com/dercar2/VO-VE"));
    html += QStringLiteral(
                "</ol><p>%1: %2<br>GitHub: %3</p><p>%4: %5<br>%6: release Alfa<br>%7: %8</p>")
                .arg(about_text(QT_TRANSLATE_NOOP("AboutDialog", "Website")),
                     link(QStringLiteral("https://vo-ve.ru"), QStringLiteral("vo-ve.ru")),
                     repository_link, ui_text(UiTextId::version).toHtmlEscaped(),
                     version.toHtmlEscaped(), ui_text(UiTextId::build).toHtmlEscaped(),
                     ui_text(UiTextId::platform).toHtmlEscaped(),
                     QSysInfo::prettyProductName().toHtmlEscaped());
    html += QStringLiteral("<h3>%1</h3><p>%2: %3<br>%4: %5</p><p>%6</p>")
                .arg(ui_text(UiTextId::contributors_title).toHtmlEscaped(),
                     about_text(QT_TRANSLATE_NOOP("AboutDialog", "Design")),
                     link(QStringLiteral("mailto:dercar@ya.com"), QStringLiteral("dercar")),
                     about_text(QT_TRANSLATE_NOOP("AboutDialog", "Programming")),
                     link(QStringLiteral("mailto:Rubilaxik@ya.com"), QStringLiteral("Rubilaks")),
                     ui_text(UiTextId::contributors_summary).toHtmlEscaped());
    html +=
        QStringLiteral("<h3>%1</h3><p><b>%2</b></p><ul>")
            .arg(about_text(QT_TRANSLATE_NOOP("AboutDialog", "Components")),
                 about_text(QT_TRANSLATE_NOOP("AboutDialog", "Embedded on Windows and Linux:")));
    for (const auto *component :
         {QT_TRANSLATE_NOOP("AboutDialog", "Qt 6 Widgets: interface."),
          QT_TRANSLATE_NOOP("AboutDialog", "MuPDF: PDF and PDF-compatible AI pages."),
          QT_TRANSLATE_NOOP("AboutDialog", "Little CMS: ICC color conversion."),
          QT_TRANSLATE_NOOP("AboutDialog", "resvg: SVG and SVGZ."),
          QT_TRANSLATE_NOOP("AboutDialog", "GNU hp2xx: PLT/HPGL."),
          QT_TRANSLATE_NOOP("AboutDialog", "libheif / libde265 / dav1d: HEIC and AVIF."),
          QT_TRANSLATE_NOOP("AboutDialog", "libjxl: JPEG XL."),
          QT_TRANSLATE_NOOP("AboutDialog", "libwebp: WebP."),
          QT_TRANSLATE_NOOP("AboutDialog", "PIEX: embedded RAW previews."),
          QT_TRANSLATE_NOOP("AboutDialog", "SQLite / QOI: thumbnail cache."),
          QT_TRANSLATE_NOOP("AboutDialog", "utf8proc: Unicode file names."),
          QT_TRANSLATE_NOOP("AboutDialog", "miniz: embedded CDR, KRA and ORA previews."),
          QT_TRANSLATE_NOOP("AboutDialog", "KImageFormats XCF reader: embedded XCF previews.")}) {
        html += QStringLiteral("<li>%1</li>").arg(about_text(component));
    }
    html += QStringLiteral("</ul><p><b>%1</b></p><ul>")
                .arg(about_text(QT_TRANSLATE_NOOP("AboutDialog",
                                                  "Platform-specific and separately installed:")));
    for (const auto *component :
         {QT_TRANSLATE_NOOP(
              "AboutDialog",
              "Windows and Linux - Ghostscript (installed separately): PS, EPS and legacy AI."),
          QT_TRANSLATE_NOOP(
              "AboutDialog",
              "Windows and Linux - LINE Seed JP (installed separately): interface typeface."),
          QT_TRANSLATE_NOOP("AboutDialog",
                            "Linux - plocate (installed separately): global search."),
          QT_TRANSLATE_NOOP("AboutDialog", "Linux - OpenSSL: update-signature verification.")}) {
        html += QStringLiteral("<li>%1</li>").arg(about_text(component));
    }
    html += QStringLiteral("<li>%1</li></ul>")
                .arg(about_text(
                         QT_TRANSLATE_NOOP(
                             "AboutDialog",
                             "Windows - With boundless gratitude and respect, we use %1 (installed "
                             "separately) for instant global search."))
                         .arg(link(QStringLiteral("https://www.voidtools.com/"),
                                   QStringLiteral("Everything"))));
    details->setHtml(html);
    QTextCharFormat heading_format;
    heading_format.setFont(heading_font);
    // Use one font for native labels and rich-text headings, without HTML size adjustments.
    for (auto block = details->document()->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().headingLevel() != 0) {
            QTextCursor heading(block);
            heading.select(QTextCursor::BlockUnderCursor);
            heading.setCharFormat(heading_format);
        }
    }
    auto brand = details->document()->find(QStringLiteral("VO-VE"));
    if (!brand.isNull() && brand.blockNumber() == 0) {
        brand.mergeCharFormat(heading_format);
    }
    connect(details, &QTextBrowser::anchorClicked, this, [repository_url](const QUrl &url) {
        if (url == QUrl(QStringLiteral("https://vo-ve.ru")) || url == repository_url ||
            url == QUrl(QStringLiteral("mailto:dercar@ya.com")) ||
            url == QUrl(QStringLiteral("mailto:Rubilaxik@ya.com")) ||
            url == QUrl(QStringLiteral("https://www.voidtools.com/"))) {
            QDesktopServices::openUrl(url);
        }
    });
    layout->addWidget(details, 3);

    auto *license_title = new QLabel(ui_text(UiTextId::license_title), this);
    license_title->setObjectName(QStringLiteral("aboutLicenseTitle"));
    license_title->setFont(heading_font);
    license_title->setContentsMargins(12, 0, 12, 0);
    layout->addWidget(license_title);
    auto *license_summary = new WrappingLabel(
        ui_text(UiTextId::license_summary) + QStringLiteral("\n\n") +
            QCoreApplication::translate("AboutDialog",
                                        "In short, it is free: you may copy, modify and even "
                                        "sell it. If you distribute a derivative version, "
                                        "you must preserve the GPL freedoms and provide its "
                                        "corresponding source code under the license terms."),
        this);
    license_summary->setObjectName(QStringLiteral("aboutLicenseSummary"));
    license_summary->setTextFormat(Qt::PlainText);
    license_summary->setWordWrap(true);
    license_summary->setContentsMargins(12, 0, 12, 0);
    license_summary->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                             Qt::TextSelectableByKeyboard);
    layout->addWidget(license_summary);

    auto *license = new QPlainTextEdit(this);
    license->setObjectName(QStringLiteral("aboutLicenseText"));
    license->setAccessibleName(QStringLiteral("GNU General Public License v3"));
    license->setReadOnly(true);
    license->document()->setDocumentMargin(12);
    license->setLayoutDirection(Qt::LeftToRight);
    license->setMinimumHeight(2 * license->fontMetrics().height());
    QFile license_file(QStringLiteral(":/licenses/LICENSE"));
    if (license_file.open(QIODevice::ReadOnly)) {
        license->setPlainText(QString::fromUtf8(license_file.readAll()));
    } else {
        license->setPlainText(
            QCoreApplication::translate("AboutDialog", "The bundled license text is unavailable."));
    }
    license_title->setBuddy(license);
    layout->addWidget(license, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setContentsMargins(12, 0, 12, 0);
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    auto *inspiration = new WrappingLabel(
        QCoreApplication::translate(
            "AboutDialog", "Inspired by the fine ideas and shortcomings of XnView, Geeqie, "
                           "TotalCommander and the outstanding Everything."),
        this);
    inspiration->setObjectName(QStringLiteral("aboutInspiration"));
    inspiration->setTextFormat(Qt::PlainText);
    inspiration->setWordWrap(true);
    inspiration->setContentsMargins(12, 0, 12, 0);
    inspiration->setAlignment(Qt::AlignRight | Qt::AlignAbsolute);
    auto inspiration_font = font();
    if (inspiration_font.pointSizeF() > 0) {
        inspiration_font.setPointSizeF(inspiration_font.pointSizeF() * 0.8);
    } else if (inspiration_font.pixelSize() > 0) {
        inspiration_font.setPixelSize(std::max(1, qRound(inspiration_font.pixelSize() * 0.8)));
    }
    inspiration->setFont(inspiration_font);
    layout->addWidget(inspiration);

    const auto available = screen()->availableGeometry().size();
    resize(std::min(820, available.width() - 40), std::min(840, available.height() - 80));
}

} // namespace vove::ui
