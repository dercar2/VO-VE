#pragma once

#include <QProcess>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

namespace vove::ui {

enum class ExternalOpenResult {
    opened,
    failed,
    timed_out,
    helper_unavailable,
    busy,
};

class ExternalOpenCoordinator final {
  public:
    using Completion = std::function<void(ExternalOpenResult)>;

    explicit ExternalOpenCoordinator(QString helper_override = {}, int timeout_ms = 30'000);
    ~ExternalOpenCoordinator();

    ExternalOpenCoordinator(const ExternalOpenCoordinator &) = delete;
    ExternalOpenCoordinator &operator=(const ExternalOpenCoordinator &) = delete;

    void open(const QString &path, Completion completion);
    void reveal(const QString &path, Completion completion);
    [[nodiscard]] bool busy() const noexcept;

  private:
    enum class Action {
        open,
        reveal,
    };

    void start(Action action, const QString &path, Completion completion);
    void complete(ExternalOpenResult result);
    [[nodiscard]] QString helper_path() const;

    QProcess process_;
    QTimer timeout_;
    QString helperOverride_;
    Completion completion_;
    std::optional<ExternalOpenResult> pendingResult_;
    int timeoutMs_{};
    bool completionDelivered_{};
};

} // namespace vove::ui
