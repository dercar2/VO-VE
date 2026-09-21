#pragma once

#include "vove/preview/thumbnail_types.hpp"

#include <QCoreApplication>
#include <QString>

namespace vove::ui {

[[nodiscard]] inline QString worker_startup_message() {
    return QCoreApplication::translate("PreviewDiagnostics", "Cannot start the preview handler");
}

[[nodiscard]] inline QString worker_startup_tooltip(const preview::WorkerStartupDiagnostic &value) {
    using Stage = preview::WorkerStartupStage;
    using Worker = preview::PreviewWorker;
    if (!value.valid() || !value.present())
        return {};
    QString name;
    switch (value.worker) {
    case Worker::raster:
        name = QStringLiteral("vove-raster-worker");
        break;
    case Worker::document:
        name = QStringLiteral("vove-document-worker");
        break;
    case Worker::cdr:
        name = QStringLiteral("vove-cdr-worker");
        break;
    case Worker::svg:
        name = QStringLiteral("vove-svg-worker");
        break;
    case Worker::xcf:
        name = QStringLiteral("vove-xcf-worker");
        break;
    case Worker::none:
        return {};
    }
#ifdef Q_OS_WIN
    if (!name.endsWith(QStringLiteral(".exe"))) {
        name += QStringLiteral(".exe");
    }
#endif
    QString stage;
    switch (value.stage) {
    case Stage::configuration:
        stage = QCoreApplication::translate("PreviewDiagnostics", "Configuration");
        break;
    case Stage::launch:
        stage = QCoreApplication::translate("PreviewDiagnostics", "Process launch");
        break;
    case Stage::handshake:
        stage = QCoreApplication::translate("PreviewDiagnostics", "Handshake");
        break;
    case Stage::compatibility:
        stage = QCoreApplication::translate("PreviewDiagnostics", "Worker version");
        break;
    case Stage::containment:
        stage = QCoreApplication::translate("PreviewDiagnostics", "Process restrictions");
        break;
    case Stage::none:
        return {};
    }
    const auto hex = [](const std::uint32_t code) {
        return QStringLiteral("0x") +
               QString::number(code, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    };
    auto text = QCoreApplication::translate("PreviewDiagnostics", "Handler: %1\nStage: %2")
                    .arg(name, stage);
    if (value.system_error != 0) {
        text += QLatin1Char('\n') +
                QCoreApplication::translate("PreviewDiagnostics", "System error: %1 (%2)")
                    .arg(value.system_error)
                    .arg(hex(value.system_error));
    }
    if (value.exit_code != 0) {
        text +=
            QLatin1Char('\n') + QCoreApplication::translate("PreviewDiagnostics", "Exit code: %1")
                                    .arg(hex(value.exit_code));
    }
    return text;
}

} // namespace vove::ui
