#include "preview_name_label.hpp"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QTextDocument>

#include <algorithm>
#include <cmath>

namespace vove::ui {

PreviewNameLabel::PreviewNameLabel(QWidget *parent) : QTextEdit(parent) {
    setObjectName(QStringLiteral("previewName"));
    setReadOnly(true);
    setAcceptDrops(false);
    viewport()->setAcceptDrops(false);
    setAcceptRichText(false);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setStyleSheet(QStringLiteral("background: transparent; border: none; padding: 0px;"));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document()->setDocumentMargin(0);
    auto policy = QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("MMMM")));
    connect(this, &QTextEdit::textChanged, this, [this] {
        update_scroll_policy();
        updateGeometry();
    });
}

void PreviewNameLabel::set_full_text(const QString &text) {
    setPlainText(text);
    setAlignment(Qt::AlignRight | Qt::AlignAbsolute);
    updateGeometry();
}

int PreviewNameLabel::heightForWidth(const int width) const {
    if (toPlainText().isEmpty()) {
        return 0;
    }
    // Measure a separate document: layout queries must not mutate the displayed text or selection.
    QTextDocument measured;
    measured.setDocumentMargin(0);
    measured.setDefaultFont(font());
    auto option = measured.defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    measured.setDefaultTextOption(option);
    measured.setPlainText(toPlainText());
    measured.setTextWidth(std::max(1, width));
    return static_cast<int>(std::ceil(measured.documentLayout()->documentSize().height()));
}

QSize PreviewNameLabel::sizeHint() const {
    return {0, heightForWidth(std::max(1, width()))};
}

QSize PreviewNameLabel::minimumSizeHint() const {
    return {0, toPlainText().isEmpty() ? 0 : fontMetrics().height()};
}

void PreviewNameLabel::changeEvent(QEvent *event) {
    QTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("MMMM")));
        update_scroll_policy();
        updateGeometry();
    }
}

void PreviewNameLabel::resizeEvent(QResizeEvent *event) {
    QTextEdit::resizeEvent(event);
    update_scroll_policy();
}

void PreviewNameLabel::update_scroll_policy() {
    // A transient scrollbar must not narrow the text and keep itself alive after the label grows.
    const auto fits = heightForWidth(contentsRect().width()) <= contentsRect().height();
    setVerticalScrollBarPolicy(fits ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
}

} // namespace vove::ui
