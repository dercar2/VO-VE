#include "preview_modified_label.hpp"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QPalette>

#include <algorithm>
#include <limits>
#include <utility>

namespace vove::ui {

PreviewModifiedLabel::PreviewModifiedLabel(QWidget *parent, Clock clock)
    : QLabel(parent), clock_(clock ? std::move(clock) : Clock{QDateTime::currentMSecsSinceEpoch}),
      timer_(this) {
    setObjectName(QStringLiteral("previewModified"));
    setTextFormat(Qt::RichText);
    setAlignment(Qt::AlignTrailing | Qt::AlignVCenter);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
    timer_.setObjectName(QStringLiteral("previewModifiedTimer"));
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState) { refresh(); });
    refresh();
}

void PreviewModifiedLabel::set_modified_time(const std::optional<std::int64_t> modified_unix_ns) {
    modifiedUnixNs_ = modified_unix_ns;
    refresh();
}

void PreviewModifiedLabel::refresh() {
    timer_.stop();
    const auto prefix = QCoreApplication::translate("PreviewModifiedLabel", "Modified:");
    const auto unknown = QCoreApplication::translate("PreviewModifiedLabel", "unknown");
    const auto metrics = fontMetrics();
    int digit_width = 0;
    for (char digit = '0'; digit <= '9'; ++digit) {
        digit_width = std::max(digit_width, metrics.horizontalAdvance(QLatin1Char(digit)));
    }
    const auto value_width = std::max(digit_width * 10 + metrics.horizontalAdvance(QStringLiteral(".. | :")),
                                      metrics.horizontalAdvance(unknown));
    setMinimumWidth(std::max(value_width, metrics.horizontalAdvance(prefix)) + 4);
    const auto two_lines = contentsRect().width() <
                           metrics.horizontalAdvance(prefix + QLatin1Char(' ')) + value_width + 4;
    setFixedHeight(metrics.lineSpacing() * (two_lines ? 2 : 1) + 4);
    if (!modifiedUnixNs_) {
        clear();
        setAccessibleName({});
        return;
    }

    // Round upward so sub-millisecond timestamps never become fresh before their actual time.
    const auto modified_ms = *modifiedUnixNs_ / 1'000'000 + (*modifiedUnixNs_ % 1'000'000 > 0);
    const auto modified = QDateTime::fromMSecsSinceEpoch(*modifiedUnixNs_ / 1'000'000);
    QString value = unknown.toHtmlEscaped();
    QString plain_value = unknown;
    if (*modifiedUnixNs_ > 0 && modified.isValid()) {
        const auto now = clock_();
        const auto fresh = now >= modified_ms && now - modified_ms < 300'000;
        const auto date = modified.toString(QStringLiteral("dd.MM.yy"));
        const auto time = modified.toString(QStringLiteral("HH:mm"));
        plain_value = date + QStringLiteral(" | ") + time;
        const auto green = palette().color(QPalette::Window).lightness() < 128
                               ? QStringLiteral("#286038")
                               : QStringLiteral("#A8DEB3");
        // Highlight only the time's background; retain the theme foreground and font throughout.
        value = QStringLiteral("\u202A%1 | %2\u202C")
                    .arg(date, fresh ? QStringLiteral("<span style=\"background-color:%1\">%2</span>")
                                          .arg(green, time)
                                    : time);
        if (isVisible() && !window()->isMinimized()) {
            qint64 delay = 0;
            if (fresh) {
                delay = 300'000 - (now - modified_ms);
            } else if (now >= 0 && now < modified_ms &&
                       modified_ms - now <= std::numeric_limits<int>::max()) {
                delay = modified_ms - now;
            }
            if (delay > 0) {
                timer_.start(static_cast<int>(delay));
            }
        }
    }
    setText(prefix.toHtmlEscaped() + (two_lines ? QStringLiteral("<br>") : QStringLiteral(" ")) +
            QStringLiteral("<span style=\"white-space:nowrap\">%1</span>").arg(value));
    setAccessibleName(prefix + QLatin1Char(' ') + plain_value);
}

void PreviewModifiedLabel::resizeEvent(QResizeEvent *event) {
    QLabel::resizeEvent(event);
    refresh();
}

void PreviewModifiedLabel::changeEvent(QEvent *event) {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::LanguageChange || event->type() == QEvent::LayoutDirectionChange) {
        refresh();
    }
}

void PreviewModifiedLabel::showEvent(QShowEvent *event) {
    QLabel::showEvent(event);
    refresh();
}

void PreviewModifiedLabel::hideEvent(QHideEvent *event) {
    timer_.stop();
    QLabel::hideEvent(event);
}

} // namespace vove::ui
