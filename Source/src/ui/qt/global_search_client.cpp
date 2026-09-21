#include "global_search_client.hpp"

#include "search_protocol.hpp"
#include "vove/core/directory_model.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace vove::ui {
namespace {

std::string utf8(const QString &text) {
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QByteArray bytes(const std::vector<std::byte> &value) {
    return {reinterpret_cast<const char *>(value.data()), static_cast<qsizetype>(value.size())};
}

std::span<const std::byte> byte_span(const QByteArray &value, const qsizetype offset = 0,
                                     const qsizetype size = -1) {
    const auto actual_size = size < 0 ? value.size() - offset : size;
    return {reinterpret_cast<const std::byte *>(value.constData() + offset),
            static_cast<std::size_t>(actual_size)};
}

} // namespace

GlobalSearchClient::GlobalSearchClient(QObject *owner, QString helper_override,
                                       const std::chrono::milliseconds inactivity_timeout)
    : owner_(owner), helperOverride_(std::move(helper_override)) {
    timeout_ = new QTimer(owner_);
    timeout_->setSingleShot(true);
    timeout_->setInterval(static_cast<int>(std::clamp<std::int64_t>(
        inactivity_timeout.count(), 100, std::numeric_limits<int>::max())));
    QObject::connect(timeout_, &QTimer::timeout, owner_, [this] {
        if (active()) {
            fail_current(search::SearchStatus::timed_out,
                         QStringLiteral("search helper timed out"));
        }
    });
}

GlobalSearchClient::~GlobalSearchClient() {
    cancel();
    delete timeout_;
    timeout_ = nullptr;
}

void GlobalSearchClient::set_reply_handler(ReplyHandler handler) {
    replyHandler_ = std::move(handler);
}

std::uint64_t GlobalSearchClient::start(const QString &query, const QStringList &allowed_roots,
                                        const std::size_t maximum_results) {
    cancel();
    const auto generation = ++nextGeneration_;
    activeGeneration_ = generation;

    std::vector<std::string> roots;
    roots.reserve(static_cast<std::size_t>(allowed_roots.size()));
    for (const auto &root : allowed_roots) {
        roots.push_back(utf8(root));
    }
    allowedRoots_ = search::normalize_allowed_roots(roots);
    const auto query_utf8 = utf8(query.trimmed());
    if (query_utf8.empty() || allowedRoots_.empty() || maximum_results == 0U) {
        fail_current(search::SearchStatus::invalid_request,
                     QStringLiteral("query and allowed roots are required"));
        return generation;
    }

    search::SearchRequest request;
    request.generation = generation;
    request.query_utf8 = query_utf8;
    request.allowed_roots_utf8 = allowedRoots_;
    request.maximum_results = std::min(maximum_results, search::kMaximumSearchResults);
    try {
        requestFrame_ = bytes(
            search::protocol::frame_payload(search::protocol::encode_request_payload(request)));
    } catch (...) {
        fail_current(search::SearchStatus::invalid_request,
                     QStringLiteral("search request is too large"));
        return generation;
    }

    auto *process = new QProcess(owner_);
    process_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(process, &QProcess::started, owner_, [this, process, generation] {
        if (process_ != process || activeGeneration_ != generation) {
            return;
        }
        if (process->write(requestFrame_) != requestFrame_.size()) {
            fail_current(search::SearchStatus::io_error,
                         QStringLiteral("search request could not be written"));
            return;
        }
        process->closeWriteChannel();
        timeout_->start();
    });
    QObject::connect(process, &QProcess::readyReadStandardOutput, owner_,
                     [this, process, generation] { consume_output(process, generation); });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), owner_,
                     [this, process, generation](int, QProcess::ExitStatus) {
                         process_finished(process, generation);
                     });
    QObject::connect(process, &QProcess::errorOccurred, owner_,
                     [this, process, generation](QProcess::ProcessError) {
                         process_failed(process, generation);
                     });
    process->start(helper_path(), {});
    return generation;
}

void GlobalSearchClient::cancel() {
    timeout_->stop();
    activeGeneration_ = 0;
    finalSeen_ = false;
    incoming_.clear();
    requestFrame_.clear();
    allowedRoots_.clear();
    if (process_ == nullptr) {
        return;
    }
    auto *process = std::exchange(process_, nullptr);
    QObject::disconnect(process, nullptr, owner_, nullptr);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
    }
    process->deleteLater();
}

bool GlobalSearchClient::active() const noexcept {
    return process_ != nullptr && activeGeneration_ != 0 && !finalSeen_;
}

void GlobalSearchClient::consume_output(QProcess *process, const std::uint64_t generation) {
    if (process_ != process || activeGeneration_ != generation) {
        return;
    }
    incoming_.append(process->readAllStandardOutput());
    timeout_->start();
    while (incoming_.size() >= static_cast<qsizetype>(search::protocol::kSearchFrameHeaderBytes)) {
        std::size_t payload_size{};
        std::string error;
        if (!search::protocol::decode_frame_size(
                byte_span(incoming_, 0,
                          static_cast<qsizetype>(search::protocol::kSearchFrameHeaderBytes)),
                payload_size, error)) {
            fail_current(search::SearchStatus::io_error, QString::fromStdString(error));
            return;
        }
        const auto frame_size = search::protocol::kSearchFrameHeaderBytes + payload_size;
        if (static_cast<std::size_t>(incoming_.size()) < frame_size) {
            return;
        }
        search::SearchBatch batch;
        if (!search::protocol::decode_batch_payload(
                byte_span(incoming_,
                          static_cast<qsizetype>(search::protocol::kSearchFrameHeaderBytes),
                          static_cast<qsizetype>(payload_size)),
                batch, error)) {
            fail_current(search::SearchStatus::io_error, QString::fromStdString(error));
            return;
        }
        incoming_.remove(0, static_cast<qsizetype>(frame_size));
        if (batch.generation != generation) {
            fail_current(search::SearchStatus::io_error,
                         QStringLiteral("search helper returned a wrong generation"));
            return;
        }
        const auto escaped = std::ranges::find_if(batch.entries, [this](const auto &entry) {
            return !search::path_is_within_allowed_roots(entry.path_utf8, allowedRoots_);
        });
        if (escaped != batch.entries.end()) {
            fail_current(search::SearchStatus::io_error,
                         QStringLiteral("search helper returned a path outside allowed roots"));
            return;
        }
        for (auto &entry : batch.entries) {
            entry.search_key_utf8 = core::make_search_key(entry.name_utf8);
        }
        if (batch.is_final) {
            finalSeen_ = true;
            timeout_->stop();
        }
        deliver(std::move(batch));
    }
}

void GlobalSearchClient::process_finished(QProcess *process, const std::uint64_t generation) {
    if (process_ != process || activeGeneration_ != generation) {
        process->deleteLater();
        return;
    }
    consume_output(process, generation);
    if (process_ != process || activeGeneration_ != generation) {
        process->deleteLater();
        return;
    }
    if (!finalSeen_) {
        fail_current(search::SearchStatus::io_error,
                     incoming_.isEmpty()
                         ? QStringLiteral("search helper stopped without a result")
                         : QStringLiteral("search helper returned a partial frame"));
        return;
    }
    timeout_->stop();
    process_ = nullptr;
    activeGeneration_ = 0;
    process->deleteLater();
}

void GlobalSearchClient::process_failed(QProcess *process, const std::uint64_t generation) {
    if (process_ != process || activeGeneration_ != generation || finalSeen_) {
        return;
    }
    const auto status = process->error() == QProcess::FailedToStart
                            ? search::SearchStatus::provider_unavailable
                            : search::SearchStatus::io_error;
    fail_current(status, process->errorString());
}

void GlobalSearchClient::fail_current(const search::SearchStatus status, const QString &message) {
    if (activeGeneration_ == 0) {
        return;
    }
    const auto generation = activeGeneration_;
    auto *process = std::exchange(process_, nullptr);
    activeGeneration_ = 0;
    finalSeen_ = true;
    timeout_->stop();
    incoming_.clear();
    if (process != nullptr) {
        QObject::disconnect(process, nullptr, owner_, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
        process->deleteLater();
    }
    search::SearchBatch batch;
    batch.generation = generation;
    batch.status = status;
    batch.message_utf8 = utf8(message);
    batch.is_final = true;
    deliver(std::move(batch));
}

void GlobalSearchClient::deliver(search::SearchBatch batch) {
    if (replyHandler_) {
        replyHandler_(std::move(batch));
    }
}

QString GlobalSearchClient::helper_path() const {
    if (!helperOverride_.isEmpty()) {
        return helperOverride_;
    }
#ifdef _WIN32
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("vove-everything-helper.exe"));
#else
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("vove-plocate-helper"));
#endif
}

} // namespace vove::ui
