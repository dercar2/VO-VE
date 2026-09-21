#include "components_panel.hpp"

#include "ui_text.hpp"

#include <QDesktopServices>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace vove::ui {
namespace {

constexpr int preferred_width = 700;
constexpr int preferred_height_limit = 480;

QLabel *wrapped_label(const QString &text, const char *object_name, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(object_name));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    return label;
}

void update_icons(QWidget *root) {
    for (auto *label : root->findChildren<QLabel *>()) {
        auto colors = label->palette();
        const auto role = label->foregroundRole();
        const auto required_color = root->palette().color(QPalette::Window).lightness() < 128
                                        ? QColor(255, 102, 102)
                                        : QColor(198, 40, 40);
        colors.setColor(role, label->property("componentRequired").toBool()
                                  ? required_color
                                  : root->palette().color(role));
        label->setPalette(colors);
    }
    for (auto *label : root->findChildren<QLabel *>(QStringLiteral("componentStatusIcon"))) {
        const auto extent = label->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, label);
        const auto icon = label->property("componentInstalled").toBool()
                              ? QStyle::SP_DialogApplyButton
                              : QStyle::SP_MessageBoxWarning;
        label->setPixmap(label->style()->standardIcon(icon, nullptr, label).pixmap(extent, extent));
        label->setFixedSize(extent, extent);
    }
    for (auto *button : root->findChildren<QPushButton *>()) {
        button->setIcon(button->style()->standardIcon(QStyle::SP_ArrowDown, nullptr, button));
    }
}

class ComponentRow final : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void changeEvent(QEvent *event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange) {
            if (auto *name = findChild<QLabel *>(QStringLiteral("componentName"))) {
                auto name_font = font();
                name_font.setBold(true);
                name->setFont(name_font);
            }
        }
    }
};

QWidget *component_row(const ComponentSpec &spec, QWidget *parent) {
    auto *row = new ComponentRow(parent);
    row->setObjectName(QStringLiteral("componentRow"));
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->setAccessibleName(spec.name);

    auto *layout = new QGridLayout(row);
    layout->setContentsMargins(0, 10, 0, 10);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(6);
    layout->setColumnStretch(0, 1);

    auto *name = wrapped_label(spec.name, "componentName", row);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto name_font = row->font();
    name_font.setBold(true);
    name->setFont(name_font);
    layout->addWidget(name, 0, 0);

    const auto download_text = ui_text(UiTextId::download);
    auto *download = new QPushButton(download_text, row);
    download->setObjectName(QStringLiteral("download_%1").arg(spec.name));
    download->setAccessibleName(download_text + QLatin1Char(' ') + spec.name);
    download->setAccessibleDescription(spec.url.toDisplayString());
    download->setToolTip(spec.url.toDisplayString());
    download->setAutoDefault(false);
    download->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (spec.url.isEmpty() || !spec.url.isValid()) {
        download->setEnabled(false);
        download->setToolTip(ui_text(UiTextId::unavailable_link));
    }
    QObject::connect(download, &QPushButton::clicked, download,
                     [url = spec.url] { static_cast<void>(QDesktopServices::openUrl(url)); });
    layout->addWidget(download, 0, 1, Qt::AlignTrailing | Qt::AlignTop);

    const auto state_text = ui_text(spec.installed ? UiTextId::installed : UiTextId::not_found);
    const auto classification_text =
        ui_text(spec.required ? UiTextId::required : UiTextId::recommended);
    auto *metadata = new QHBoxLayout;
    metadata->setContentsMargins(0, 0, 0, 0);
    metadata->setSpacing(8);
    auto *icon = new QLabel(row);
    icon->setObjectName(QStringLiteral("componentStatusIcon"));
    icon->setProperty("componentInstalled", spec.installed);
    metadata->addWidget(icon, 0, Qt::AlignTop);
    metadata->addWidget(wrapped_label(state_text, "componentState", row));
    auto *classification = wrapped_label(classification_text, "componentClassification", row);
    classification->setProperty("componentRequired", spec.required);
    classification->setForegroundRole(QPalette::PlaceholderText);
    metadata->addWidget(classification);
    metadata->addStretch();
    layout->addLayout(metadata, 1, 0, 1, 2);

    layout->addWidget(wrapped_label(spec.purpose, "componentPurpose", row), 2, 0, 1, 2);
    row->setAccessibleDescription(state_text + QStringLiteral(", ") + classification_text +
                                  QLatin1Char('\n') + spec.purpose);
    return row;
}

} // namespace

ComponentsPanel::ComponentsPanel(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("componentsList"));
    setAccessibleName(ui_text(UiTextId::components_title));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    scrollArea_ = new QScrollArea(this);
    scrollArea_->setObjectName(QStringLiteral("componentsScrollArea"));
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->viewport()->setAutoFillBackground(false);
    layout->addWidget(scrollArea_);
    set_components({});
}

void ComponentsPanel::set_components(const std::vector<ComponentSpec> &components) {
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("componentsContent"));
    content->setPalette(palette());
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // QScrollArea uses the layout's height-for-width to keep wrapped rows fully scrollable.
    layout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    for (const auto &spec : components) {
        if (layout->count() != 0) {
            auto *separator = new QFrame(content);
            separator->setObjectName(QStringLiteral("componentSeparator"));
            separator->setFixedHeight(1);
            separator->setStyleSheet(QStringLiteral(
                "QFrame#componentSeparator { border: 0; background: palette(mid); }"));
            layout->addWidget(separator);
        }
        layout->addWidget(component_row(spec, content));
    }
    layout->addStretch();
    delete scrollArea_->takeWidget();
    scrollArea_->setWidget(content);
    // setWidget enables this by default; rows should retain their inherited dialog background.
    content->setAutoFillBackground(false);
    update_icons(this);
    updateGeometry();
}

QSize ComponentsPanel::sizeHint() const {
    return {preferred_width, std::min(preferred_height_limit, heightForWidth(preferred_width))};
}

QSize ComponentsPanel::minimumSizeHint() const {
    return {180, std::min(fontMetrics().lineSpacing() * 3, heightForWidth(180))};
}

bool ComponentsPanel::hasHeightForWidth() const {
    return true;
}

int ComponentsPanel::heightForWidth(const int width) const {
    if (scrollArea_ == nullptr || scrollArea_->widget() == nullptr) {
        return 0;
    }
    const auto *layout = scrollArea_->widget()->layout();
    const auto content_width = std::max(width, layout->minimumSize().width());
    const auto height = layout->hasHeightForWidth() ? layout->totalHeightForWidth(content_width)
                                                    : layout->sizeHint().height();
    const auto scrollbar_extent =
        scrollArea_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scrollArea_);
    const auto horizontal_scroll = content_width > width ? scrollbar_extent : 0;
    return std::max(0, height) + horizontal_scroll;
}

void ComponentsPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (scrollArea_ != nullptr &&
        (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::FontChange)) {
        if (auto *content = scrollArea_->widget()) {
            content->setPalette(palette());
        }
        update_icons(this);
        updateGeometry();
    }
}

} // namespace vove::ui
