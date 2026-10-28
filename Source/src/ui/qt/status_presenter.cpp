#include "status_presenter.hpp"

#include "operation_notice.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QLocale>
#include <QPalette>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vove::ui {
namespace {

QColor technical_status_accent() {
    return QColor(QStringLiteral("#FF7900"));
}

QColor blend_colors(const QColor &base, const QColor &accent, const qreal amount) {
    const auto bounded = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    return QColor::fromRgbF(base.redF() + (accent.redF() - base.redF()) * bounded,
                            base.greenF() + (accent.greenF() - base.greenF()) * bounded,
                            base.blueF() + (accent.blueF() - base.blueF()) * bounded);
}

QString format_drag_status(const DragStatusMessage &message) {
    if (!message.transfer) {
        return message.message.toHtmlEscaped();
    }
    const auto red = QStringLiteral("<span style=\"color:#ff414c\">%1</span>");
    return message.move
               ? red.arg(QCoreApplication::translate("MainWindow", "Move: %n item(s)", nullptr,
                                                     message.count).toHtmlEscaped())
               : QStringLiteral("<span style=\"color:#00b7ff\">%1</span> %2")
                     .arg(QCoreApplication::translate("MainWindow", "Copy %n item(s)", nullptr,
                                                       message.count).toHtmlEscaped(),
                          red.arg(QCoreApplication::translate(
                                      "MainWindow", "(hold Shift to move instead)")
                                      .toHtmlEscaped()));
}

} // namespace

StatusPresenter::StatusPresenter(QLabel &status, OperationNotice *notice, QObject *parent)
    : QObject(parent), status_(&status), notice_(notice), operationStatusPulseTimer_(this) {
    operationStatusPulseTimer_.setInterval(80);
    operationStatusPulseTimer_.setTimerType(Qt::CoarseTimer);
    connect(&operationStatusPulseTimer_, &QTimer::timeout, this, &StatusPresenter::advancePulse);
    setInterfaceFont(QApplication::font());
    render();
}

StatusPresenter::~StatusPresenter() = default;

void StatusPresenter::setCatalogSnapshot(CatalogStatusSnapshot snapshot) {
    catalog_ = std::move(snapshot);
    render();
}

void StatusPresenter::setSelectionSummary(QString summary) {
    selectionSummary_ = std::move(summary);
    render();
}

void StatusPresenter::setSelectedFile(QString name, QString color_summary) {
    selectedName_ = std::move(name);
    selectedColorSummary_ = std::move(color_summary);
    render();
}

void StatusPresenter::setOperationMessage(QString message) {
    operationStatus_ = std::move(message);
    render();
}

void StatusPresenter::setOperationActivity(const OperationStatusActivity activity) {
    activity_ = activity;
    render();
}

void StatusPresenter::setDragStatus(std::optional<DragStatusMessage> message) {
    dragMessage_ = std::move(message);
    dragStatus_ = dragMessage_ ? format_drag_status(*dragMessage_) : QString{};
    render();
}

void StatusPresenter::showDragStatus(const DragStatusMessage &message) {
    setDragStatus(message);
}

void StatusPresenter::clearDragStatus() {
    setDragStatus(std::nullopt);
}

bool StatusPresenter::dragStatusActive() const noexcept {
    return !dragStatus_.isEmpty();
}

const QString &StatusPresenter::operationMessage() const noexcept {
    return operationStatus_;
}

void StatusPresenter::setThemeColors(QColor muted, QColor operation_accent) {
    muted_ = std::move(muted);
    operationAccent_ = std::move(operation_accent);
    updatePulse();
}

void StatusPresenter::setInterfaceFont(const QFont &base_font) {
    if (status_) {
        auto font = base_font;
        font.setPointSizeF(base_font.pointSizeF() * 0.9);
        status_->setFont(font);
    }
}

void StatusPresenter::retranslate() {
    if (dragMessage_) {
        dragStatus_ = format_drag_status(*dragMessage_);
    }
    render();
}

void StatusPresenter::notifyFileInUse() {
    if (notice_) {
        notice_->show_file_in_use();
    }
}

QString StatusPresenter::withSelectionAndOperation(QString text, const bool include_color) const {
    if (!selectionSummary_.isEmpty()) {
        text = selectionSummary_;
    } else if (!selectedName_.isEmpty()) {
        text += QStringLiteral(" · %1").arg(selectedName_);
    }
    if (selectionSummary_.isEmpty() && include_color && !selectedColorSummary_.isEmpty()) {
        text += QStringLiteral(" · %1").arg(selectedColorSummary_);
    }
    if (!operationStatus_.isEmpty()) {
        text += QStringLiteral(" · %1").arg(operationStatus_);
    }
    return text;
}

QString StatusPresenter::catalogText() const {
    const auto &update = catalog_.update;
    const auto visible = static_cast<qulonglong>(catalog_.visible_items);
    const auto total = static_cast<qulonglong>(catalog_.total_items);
    const auto directories = static_cast<qulonglong>(catalog_.visible_directories);
    const auto files = visible - directories;
    const auto counts = QCoreApplication::translate("MainWindow", "Folders: %1 · files: %2")
                            .arg(directories).arg(files);
    QString text;
    if (catalog_.trash_view) {
        if (!catalog_.trash) {
            text = QCoreApplication::translate("MainWindow", "VO-VE Trash: loading…");
        } else if (!catalog_.trash->available) {
            text = QCoreApplication::translate("MainWindow", "VO-VE Trash is unavailable");
        } else {
            text = QCoreApplication::translate("MainWindow", "VO-VE Trash: %1 objects · %2")
                       .arg(static_cast<qulonglong>(catalog_.trash->total_items))
                       .arg(QLocale().formattedDataSize(
                           static_cast<qint64>(catalog_.trash->total_bytes)));
            if (visible != total) {
                text += QCoreApplication::translate("MainWindow", " · showing %1 of %2")
                            .arg(visible).arg(total);
            }
        }
        return withSelectionAndOperation(std::move(text), false);
    }
    if (catalog_.global_search) {
        text = catalog_.search_status.isEmpty()
                   ? QCoreApplication::translate("MainWindow", "Global search…")
                   : catalog_.search_status;
        return withSelectionAndOperation(std::move(text), true);
    }
    switch (update.state) {
    case catalog::CatalogSessionState::loading:
        text = total == 0
                   ? QCoreApplication::translate("MainWindow", "Loading folder…")
                   : QCoreApplication::translate("MainWindow", "Refreshing… · %1").arg(counts);
        break;
    case catalog::CatalogSessionState::waiting_retry: {
        const auto milliseconds = update.retry_in.value_or(std::chrono::milliseconds(0)).count();
        const auto seconds = std::max<std::int64_t>(1, (milliseconds + 999) / 1000);
        text = QCoreApplication::translate("MainWindow", "Connection lost. Retrying in %1 s · %2")
                   .arg(seconds).arg(counts);
        break;
    }
    case catalog::CatalogSessionState::network_disconnected:
        text = QCoreApplication::translate("MainWindow",
                                           "Network folder unavailable. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::timed_out:
        text = QCoreApplication::translate(
                   "MainWindow", "Network folder is not responding. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::authentication_required:
        text = QCoreApplication::translate(
                   "MainWindow", "Network folder sign-in required. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::permission_denied:
        text = QCoreApplication::translate("MainWindow",
                                           "Folder access denied. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::unavailable:
        text = QCoreApplication::translate("MainWindow", "Folder unavailable. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::ready:
        if (total == 0) {
            text = QCoreApplication::translate("MainWindow", "Folder is empty");
        } else if (visible == 0) {
            text = QCoreApplication::translate("MainWindow", "No items match the filter");
        } else {
            text = counts;
            if (visible != total) {
                text += QCoreApplication::translate("MainWindow", " · showing %1 of %2")
                            .arg(visible).arg(total);
            }
            if (update.truncated) {
                text += QCoreApplication::translate("MainWindow", " · results limited");
            }
        }
        break;
    }
    text = withSelectionAndOperation(std::move(text), true);
    if (catalog_.recursive_view) {
        const auto mode = update.state == catalog::CatalogSessionState::loading
                              ? QCoreApplication::translate("MainWindow", "Reading subfolders…")
                              : QCoreApplication::translate("MainWindow", "All subfolders");
        text = mode + QStringLiteral(" · ") + text;
        if (update.truncated || (update.state != catalog::CatalogSessionState::ready &&
                                 update.state != catalog::CatalogSessionState::loading)) {
            text += QCoreApplication::translate("MainWindow", " · View incomplete");
        }
    }
    return text;
}

void StatusPresenter::render() {
    if (!status_) {
        operationStatusPulseTimer_.stop();
        operationStatusPulsePhase_ = 0;
        return;
    }
    // Legacy drag overrides text/tooltip only; technical/catalog diagnostics stay latched
    // until drag ends. In particular an existing technical accent must not start pulsing.
    if (!dragStatus_.isEmpty()) {
        status_->setTextFormat(Qt::RichText);
        status_->setToolTip({});
        status_->setText(dragStatus_);
        updatePulse();
        return;
    }
    const auto &update = catalog_.update;
    status_->setTextFormat(Qt::PlainText);
    status_->setProperty("catalogErrorKind", static_cast<int>(update.error.kind));
    status_->setProperty("catalogPlatformCode",
                         QVariant::fromValue<qlonglong>(update.error.platform_code));
    const auto catalog_technical = !catalog_.trash_view && !catalog_.global_search &&
        ((update.state != catalog::CatalogSessionState::loading &&
          update.state != catalog::CatalogSessionState::ready) ||
         (catalog_.recursive_view && update.truncated));
    technicalStatusActive_ = activity_.recovery_pending || activity_.journal_blocked ||
                             catalog_technical;
    status_->setProperty("technicalStatusActive", technicalStatusActive_);
    status_->setProperty("technicalStatusAccent", technical_status_accent().name(QColor::HexRgb));
    status_->setToolTip(QString::fromUtf8(update.error.message_utf8));
    if (catalog_.trash_view || catalog_.global_search) {
        status_->setProperty("catalogErrorKind", 0);
        status_->setProperty("catalogPlatformCode", QVariant::fromValue<qlonglong>(0));
        status_->setToolTip({});
        if (!catalog_.trash_view) {
            QStringList native_roots;
            native_roots.reserve(catalog_.search_roots.size());
            for (const auto &root : catalog_.search_roots) {
                native_roots.push_back(QDir::toNativeSeparators(root));
            }
            status_->setToolTip(
                native_roots.isEmpty()
                    ? QCoreApplication::translate("MainWindow", "Global search scope is not configured")
                    : QCoreApplication::translate("MainWindow", "Global search roots:\n%1")
                          .arg(native_roots.join(QLatin1Char('\n'))));
        }
    }
    status_->setText(catalogText());
    updatePulse();
}

void StatusPresenter::updatePulse() {
    if (!status_) {
        operationStatusPulseTimer_.stop();
        operationStatusPulsePhase_ = 0;
        return;
    }
    const auto active = dragStatus_.isEmpty() && !technicalStatusActive_ &&
                        !operationStatus_.isEmpty() && activity_.active;
    status_->setProperty("operationPulseActive", active);
    status_->setProperty("operationPulseAccent", operationAccent_.name(QColor::HexRgb));
    if (active) {
        if (!operationStatusPulseTimer_.isActive()) {
            operationStatusPulsePhase_ = 0;
            operationStatusPulseTimer_.start();
        }
    } else {
        operationStatusPulseTimer_.stop();
        operationStatusPulsePhase_ = 0;
    }
    applyColor();
}

void StatusPresenter::advancePulse() {
    if (!status_) {
        operationStatusPulseTimer_.stop();
        operationStatusPulsePhase_ = 0;
        return;
    }
    if (operationStatusPulseTimer_.isActive()) {
        operationStatusPulsePhase_ = (operationStatusPulsePhase_ + 1) % 30;
        applyColor();
    }
}

void StatusPresenter::applyColor() {
    if (!status_) {
        return;
    }
    auto color = muted_;
    if (technicalStatusActive_) {
        color = technical_status_accent();
    } else if (operationStatusPulseTimer_.isActive()) {
        constexpr auto pi = 3.14159265358979323846;
        const auto phase = static_cast<qreal>(operationStatusPulsePhase_) / 30.0;
        const auto wave = 0.5 - 0.5 * std::cos(phase * 2.0 * pi);
        color = blend_colors(color, operationAccent_, 0.72 + 0.28 * wave);
    }
    auto palette = status_->palette();
    palette.setColor(QPalette::WindowText, color);
    status_->setPalette(palette);
    status_->setProperty("operationPulseColor", color.name(QColor::HexRgb));
}

} // namespace vove::ui
