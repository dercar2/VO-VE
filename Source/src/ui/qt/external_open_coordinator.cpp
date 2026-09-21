#include "external_open_coordinator.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdlib>
#include <utility>

namespace vove::ui {

ExternalOpenCoordinator::ExternalOpenCoordinator(QString helper_override, const int timeout_ms)
    : helperOverride_(std::move(helper_override)), timeoutMs_(timeout_ms) {
    timeout_.setSingleShot(true);
    QObject::connect(&timeout_, &QTimer::timeout, &timeout_, [this] {
        pendingResult_ = ExternalOpenResult::timed_out;
        if (process_.state() != QProcess::NotRunning) {
            process_.kill();
            return;
        }
        complete(*pendingResult_);
    });
    QObject::connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &process_, [this](const int exit_code, const QProcess::ExitStatus status) {
                         const auto result = pendingResult_.value_or(
                             status == QProcess::NormalExit && exit_code == EXIT_SUCCESS
                                 ? ExternalOpenResult::opened
                                 : ExternalOpenResult::failed);
                         pendingResult_.reset();
                         complete(result);
                     });
    QObject::connect(&process_, &QProcess::errorOccurred, &process_,
                     [this](const QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart) {
                             complete(ExternalOpenResult::helper_unavailable);
                         }
                     });
}

ExternalOpenCoordinator::~ExternalOpenCoordinator() {
    timeout_.stop();
    completion_ = {};
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1'000);
    }
}

void ExternalOpenCoordinator::open(const QString &path, Completion completion) {
    start(Action::open, path, std::move(completion));
}

void ExternalOpenCoordinator::reveal(const QString &path, Completion completion) {
    start(Action::reveal, path, std::move(completion));
}

void ExternalOpenCoordinator::start(const Action action, const QString &path,
                                    Completion completion) {
    if (busy()) {
        if (completion) {
            completion(ExternalOpenResult::busy);
        }
        return;
    }
    const auto source = QDir::cleanPath(path);
    const auto helper = helper_path();
    if (!QDir::isAbsolutePath(source) || helper.isEmpty()) {
        if (completion) {
            completion(ExternalOpenResult::helper_unavailable);
        }
        return;
    }
    completion_ = std::move(completion);
    completionDelivered_ = false;
    pendingResult_.reset();
    process_.setProgram(helper);
    process_.setArguments(
        {action == Action::open ? QStringLiteral("open") : QStringLiteral("reveal"), source});
    process_.start(QIODevice::ReadOnly);
    timeout_.start(timeoutMs_);
}

bool ExternalOpenCoordinator::busy() const noexcept {
    return process_.state() != QProcess::NotRunning;
}

void ExternalOpenCoordinator::complete(const ExternalOpenResult result) {
    if (completionDelivered_) {
        return;
    }
    completionDelivered_ = true;
    timeout_.stop();
    auto completion = std::move(completion_);
    completion_ = {};
    if (completion) {
        completion(result);
    }
}

QString ExternalOpenCoordinator::helper_path() const {
    const auto candidate = helperOverride_.isEmpty()
                               ? QDir(QCoreApplication::applicationDirPath())
                                     .filePath(QStringLiteral("vove-open-helper")
#ifdef _WIN32
                                               + QStringLiteral(".exe")
#endif
                                                   )
                               : helperOverride_;
    const QFileInfo info(candidate);
    if (!info.isAbsolute() || !info.exists() || !info.isFile()) {
        return {};
    }
    return info.canonicalFilePath();
}

} // namespace vove::ui
