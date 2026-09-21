#pragma once

#include "release_configuration.hpp"
#include "update_manifest.hpp"

#include <QByteArray>
#include <QObject>
#include <QPointer>

#include <cstdint>
#include <functional>

class QNetworkReply;
class QNetworkRequest;
class QNetworkAccessManager;
class QTimer;

namespace vove::ui::updates {

enum class UpdateCheckStatus : std::uint8_t {
    current,
    update_available,
    not_configured,
    network_error,
    invalid_manifest,
    invalid_signature,
    wrong_platform,
    unsupported_verifier,
    repository_current,
    repository_update_available,
    no_published_release,
};

struct UpdateCheckResult {
    UpdateCheckStatus status{UpdateCheckStatus::not_configured};
    UpdateManifest manifest;
};

struct SignedUpdatePayload {
    QByteArray manifest;
    QByteArray signature_base64;
};

struct UpdateNetworkLimits {
    int inactivity_timeout_ms{10'000};
    int operation_timeout_ms{25'000};
};

[[nodiscard]] QUrl detached_signature_url(const QUrl &manifest_url);
[[nodiscard]] QNetworkRequest secure_update_request(const QUrl &url,
                                                    int inactivity_timeout_ms = 10'000);
[[nodiscard]] QByteArray decode_update_signature(const QByteArray &encoded);
[[nodiscard]] UpdateCheckResult evaluate_signed_update(const SignedUpdatePayload &payload,
                                                       const ReleaseConfiguration &configuration,
                                                       const QString &current_version);
[[nodiscard]] UpdateCheckResult
evaluate_repository_release(const QByteArray &payload, const ReleaseConfiguration &configuration,
                            const QString &current_version);

class UpdateChecker final : public QObject {
  public:
    explicit UpdateChecker(ReleaseConfiguration configuration, QObject *parent = nullptr);
    UpdateChecker(ReleaseConfiguration configuration, QNetworkAccessManager *manager,
                  UpdateNetworkLimits limits, QObject *parent = nullptr);
    ~UpdateChecker() override;

    void start(const QString &current_version, std::function<void(UpdateCheckResult)> completion);

  private:
    enum class PayloadKind : std::uint8_t {
        manifest,
        signature,
        repository_release,
    };

    void start_request(const QUrl &url, PayloadKind kind, qsizetype maximum_bytes);
    void finish_request(QNetworkReply *reply, const QUrl &requested_url, PayloadKind kind,
                        qsizetype maximum_bytes);
    void consume_reply(QNetworkReply *reply, qsizetype maximum_bytes);
    void fail_network();
    void finish(UpdateCheckResult result);

    ReleaseConfiguration configuration_;
    QNetworkAccessManager *manager_{};
    UpdateNetworkLimits limits_;
    QTimer *inactivityTimer_{};
    QTimer *operationTimer_{};
    QPointer<QNetworkReply> active_reply_;
    std::function<void(UpdateCheckResult)> completion_;
    QByteArray manifest_bytes_;
    QByteArray payload_;
    QString current_version_;
    QUrl final_manifest_url_;
    bool oversized_{};
    bool running_{};
};

} // namespace vove::ui::updates
