#include "update_checker.hpp"

#include "update_signature.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace vove::ui::updates {

QUrl detached_signature_url(const QUrl &manifest_url) {
    if (!is_secure_https_url(manifest_url)) {
        return {};
    }
    auto signature = manifest_url;
    signature.setPath(signature.path() + QStringLiteral(".sig"));
    return signature;
}

namespace {

int effective_port(const QUrl &url) {
    return url.port(443);
}

bool same_origin(const QUrl &left, const QUrl &right) {
    return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0 &&
           left.host().compare(right.host(), Qt::CaseInsensitive) == 0 &&
           effective_port(left) == effective_port(right);
}

bool repository_release_url(const QUrl &url, const QUrl &repository_url, const QString &tag) {
    if (!is_secure_https_url(url) || !same_origin(url, repository_url) || url.hasQuery()) {
        return false;
    }
    auto repository_path = repository_url.path();
    while (repository_path.endsWith(QLatin1Char('/'))) {
        repository_path.chop(1);
    }
    return url.path() == repository_path + QStringLiteral("/releases/tag/") + tag;
}

} // namespace

QNetworkRequest secure_update_request(const QUrl &url, const int inactivity_timeout_ms) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setMaximumRedirectsAllowed(3);
    request.setTransferTimeout(inactivity_timeout_ms);
    request.setDecompressedSafetyCheckThreshold(kMaximumManifestBytes);
    request.setRawHeader("Accept", "application/json, text/plain;q=0.5");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "VO-VE-update/1");
    return request;
}

QByteArray decode_update_signature(const QByteArray &encoded) {
    const auto trimmed = encoded.trimmed();
    if (trimmed.size() != 88 || trimmed.contains(' ') || trimmed.contains('\t') ||
        trimmed.contains('\r') || trimmed.contains('\n')) {
        return {};
    }
    const auto decoded = QByteArray::fromBase64(trimmed, QByteArray::AbortOnBase64DecodingErrors);
    return decoded.size() == 64 && decoded.toBase64() == trimmed ? decoded : QByteArray{};
}

UpdateCheckResult evaluate_signed_update(const SignedUpdatePayload &payload,
                                         const ReleaseConfiguration &configuration,
                                         const QString &current_version) {
    const auto signature = decode_update_signature(payload.signature_base64);
    if (signature.isEmpty()) {
        return {.status = UpdateCheckStatus::invalid_signature, .manifest = {}};
    }
    const auto signature_status =
        verify_p256_sha256(payload.manifest, configuration.public_key, signature);
    if (signature_status == SignatureStatus::unsupported) {
        return {.status = UpdateCheckStatus::unsupported_verifier, .manifest = {}};
    }
    if (signature_status != SignatureStatus::verified) {
        return {.status = UpdateCheckStatus::invalid_signature, .manifest = {}};
    }

    auto parsed = parse_update_manifest(payload.manifest);
    if (!parsed.manifest) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }
    if (parsed.manifest->platform != configuration.platform) {
        return {.status = UpdateCheckStatus::wrong_platform,
                .manifest = std::move(*parsed.manifest)};
    }
    const auto current = parse_semantic_version(current_version);
    if (!current) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }
    const auto update_available = parsed.manifest->version > *current;
    return {.status =
                update_available ? UpdateCheckStatus::update_available : UpdateCheckStatus::current,
            .manifest = std::move(*parsed.manifest)};
}

UpdateCheckResult evaluate_repository_release(const QByteArray &payload,
                                              const ReleaseConfiguration &configuration,
                                              const QString &current_version) {
    if (payload.isEmpty() || payload.size() > kMaximumManifestBytes ||
        !configuration.repository_check_configured()) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }
    const auto current = parse_semantic_version(current_version);
    if (!current) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }
    const auto object = document.object();
    const auto draft = object.value(QStringLiteral("draft"));
    const auto prerelease = object.value(QStringLiteral("prerelease"));
    const auto tag = object.value(QStringLiteral("tag_name"));
    const auto page = object.value(QStringLiteral("html_url"));
    if (!draft.isBool() || draft.toBool() || !prerelease.isBool() || prerelease.toBool() ||
        !tag.isString() || !page.isString()) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }

    auto version_text = tag.toString().trimmed();
    if (version_text.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version_text.remove(0, 1);
    }
    const auto version = parse_semantic_version(version_text);
    const QUrl release_url(page.toString(), QUrl::StrictMode);
    if (!version || !repository_release_url(release_url, configuration.repository_url,
                                            tag.toString())) {
        return {.status = UpdateCheckStatus::invalid_manifest, .manifest = {}};
    }

    UpdateManifest manifest;
    manifest.version_text = version_text;
    manifest.version = *version;
    manifest.platform = configuration.platform;
    manifest.release_url = release_url;
    return {.status = *version > *current ? UpdateCheckStatus::repository_update_available
                                          : UpdateCheckStatus::repository_current,
            .manifest = std::move(manifest)};
}

UpdateChecker::UpdateChecker(ReleaseConfiguration configuration, QObject *parent)
    : UpdateChecker(std::move(configuration), nullptr, {}, parent) {}

UpdateChecker::UpdateChecker(ReleaseConfiguration configuration, QNetworkAccessManager *manager,
                             const UpdateNetworkLimits limits, QObject *parent)
    : QObject(parent), configuration_(std::move(configuration)),
      manager_(manager != nullptr ? manager : new QNetworkAccessManager(this)), limits_(limits),
      inactivityTimer_(new QTimer(this)), operationTimer_(new QTimer(this)) {
    inactivityTimer_->setSingleShot(true);
    operationTimer_->setSingleShot(true);
    connect(inactivityTimer_, &QTimer::timeout, this, &UpdateChecker::fail_network);
    connect(operationTimer_, &QTimer::timeout, this, &UpdateChecker::fail_network);
}

UpdateChecker::~UpdateChecker() {
    if (active_reply_ != nullptr) {
        disconnect(active_reply_, nullptr, this, nullptr);
        active_reply_->abort();
        active_reply_->deleteLater();
    }
}

void UpdateChecker::start(const QString &current_version,
                          std::function<void(UpdateCheckResult)> completion) {
    if (running_) {
        return;
    }
    completion_ = std::move(completion);
    current_version_ = current_version;
    if (!configuration_.update_configured() && !configuration_.repository_check_configured()) {
        finish({.status = UpdateCheckStatus::not_configured, .manifest = {}});
        return;
    }
    if (!parse_semantic_version(current_version_)) {
        finish({.status = UpdateCheckStatus::invalid_manifest, .manifest = {}});
        return;
    }
    if (limits_.inactivity_timeout_ms <= 0 || limits_.operation_timeout_ms <= 0) {
        finish({.status = UpdateCheckStatus::network_error, .manifest = {}});
        return;
    }
    running_ = true;
    manifest_bytes_.clear();
    final_manifest_url_.clear();
    operationTimer_->start(limits_.operation_timeout_ms);
    if (configuration_.update_configured()) {
        start_request(configuration_.manifest_url, PayloadKind::manifest, kMaximumManifestBytes);
    } else {
        start_request(configuration_.releases_api_url, PayloadKind::repository_release,
                      kMaximumManifestBytes);
    }
}

void UpdateChecker::start_request(const QUrl &url, const PayloadKind kind,
                                  const qsizetype maximum_bytes) {
    payload_.clear();
    oversized_ = false;
    auto *reply = manager_->get(secure_update_request(url, limits_.inactivity_timeout_ms));
    active_reply_ = reply;
    reply->setReadBufferSize(static_cast<qint64>(maximum_bytes) + 1);
    inactivityTimer_->start(limits_.inactivity_timeout_ms);
    connect(reply, &QNetworkReply::readyRead, this,
            [this, reply, maximum_bytes] { consume_reply(reply, maximum_bytes); });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply, maximum_bytes](const qint64, const qint64 total) {
                if (!running_ || reply != active_reply_) {
                    return;
                }
                if (total > maximum_bytes) {
                    oversized_ = true;
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, kind, maximum_bytes] {
        finish_request(reply, url, kind, maximum_bytes);
    });
}

void UpdateChecker::consume_reply(QNetworkReply *reply, const qsizetype maximum_bytes) {
    if (!running_ || reply != active_reply_) {
        return;
    }
    const auto remaining = std::max<qsizetype>(0, maximum_bytes + 1 - payload_.size());
    if (remaining > 0) {
        const auto bytes = reply->read(remaining);
        if (!bytes.isEmpty()) {
            payload_.append(bytes);
            inactivityTimer_->start(limits_.inactivity_timeout_ms);
        }
    }
    if (payload_.size() > maximum_bytes || reply->bytesAvailable() > 0) {
        oversized_ = true;
        if (!reply->isFinished()) {
            reply->abort();
        }
    }
}

void UpdateChecker::finish_request(QNetworkReply *reply, const QUrl &requested_url,
                                   const PayloadKind kind, const qsizetype maximum_bytes) {
    if (!running_ || reply != active_reply_) {
        reply->deleteLater();
        return;
    }
    consume_reply(reply, maximum_bytes);
    // abort() may synchronously finish this reply and start a retry in the completion callback.
    if (!running_ || reply != active_reply_) {
        return;
    }
    active_reply_.clear();
    inactivityTimer_->stop();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto encrypted = reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool();
    const auto final_url = reply->url();
    const auto response_origin_ok =
        encrypted && is_secure_https_url(final_url) && same_origin(requested_url, final_url);
    if (kind == PayloadKind::repository_release && !oversized_ && status == 404 &&
        response_origin_ok) {
        reply->deleteLater();
        finish({.status = UpdateCheckStatus::no_published_release, .manifest = {}});
        return;
    }
    const auto network_ok = !oversized_ && payload_.size() <= maximum_bytes &&
                            reply->error() == QNetworkReply::NoError && status == 200 &&
                            response_origin_ok;
    reply->deleteLater();
    if (!network_ok) {
        finish({.status = UpdateCheckStatus::network_error, .manifest = {}});
        return;
    }

    if (kind == PayloadKind::manifest) {
        manifest_bytes_ = payload_;
        final_manifest_url_ = final_url;
        start_request(detached_signature_url(final_manifest_url_), PayloadKind::signature,
                      kMaximumSignatureBytes);
        return;
    }

    if (kind == PayloadKind::repository_release) {
        finish(evaluate_repository_release(payload_, configuration_, current_version_));
        return;
    }

    finish(evaluate_signed_update({.manifest = manifest_bytes_, .signature_base64 = payload_},
                                  configuration_, current_version_));
}

void UpdateChecker::fail_network() {
    if (!running_) {
        return;
    }
    auto reply = active_reply_;
    active_reply_.clear();
    inactivityTimer_->stop();
    operationTimer_->stop();
    if (reply != nullptr) {
        reply->abort();
        reply->deleteLater();
    }
    finish({.status = UpdateCheckStatus::network_error, .manifest = {}});
}

void UpdateChecker::finish(UpdateCheckResult result) {
    inactivityTimer_->stop();
    operationTimer_->stop();
    active_reply_.clear();
    running_ = false;
    auto completion = std::move(completion_);
    completion_ = {};
    if (completion) {
        completion(std::move(result));
    }
}

} // namespace vove::ui::updates
