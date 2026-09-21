#pragma once

#include "vove/search/global_search.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class QObject;
class QProcess;
class QTimer;

namespace vove::ui {

class GlobalSearchClient final {
  public:
    using ReplyHandler = std::function<void(search::SearchBatch)>;

    explicit GlobalSearchClient(
        QObject *owner, QString helper_override = {},
        std::chrono::milliseconds inactivity_timeout = std::chrono::seconds(5));
    ~GlobalSearchClient();

    GlobalSearchClient(const GlobalSearchClient &) = delete;
    GlobalSearchClient &operator=(const GlobalSearchClient &) = delete;

    void set_reply_handler(ReplyHandler handler);
    [[nodiscard]] std::uint64_t start(const QString &query, const QStringList &allowed_roots,
                                      std::size_t maximum_results = search::kMaximumSearchResults);
    void cancel();
    [[nodiscard]] bool active() const noexcept;

  private:
    void consume_output(QProcess *process, std::uint64_t generation);
    void process_finished(QProcess *process, std::uint64_t generation);
    void process_failed(QProcess *process, std::uint64_t generation);
    void fail_current(search::SearchStatus status, const QString &message);
    void deliver(search::SearchBatch batch);
    [[nodiscard]] QString helper_path() const;

    QObject *owner_{};
    QProcess *process_{};
    QTimer *timeout_{};
    ReplyHandler replyHandler_;
    QString helperOverride_;
    QByteArray incoming_;
    QByteArray requestFrame_;
    std::vector<std::string> allowedRoots_;
    std::uint64_t nextGeneration_{};
    std::uint64_t activeGeneration_{};
    bool finalSeen_{};
};

} // namespace vove::ui
