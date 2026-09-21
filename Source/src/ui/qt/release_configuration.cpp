#include "release_configuration.hpp"

#include "release_configuration_data.hpp"
#include "update_manifest.hpp"

namespace vove::ui::updates {
namespace {

QByteArray decode_canonical_base64(const QByteArray &encoded) {
    if (encoded.isEmpty() || encoded.contains(' ') || encoded.contains('\t') ||
        encoded.contains('\r') || encoded.contains('\n')) {
        return {};
    }
    const auto decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    return decoded.toBase64() == encoded ? decoded : QByteArray{};
}

QUrl configured_https_url(const char *value) {
    const QUrl url(QString::fromUtf8(value), QUrl::StrictMode);
    return is_secure_https_url(url) ? url : QUrl{};
}

} // namespace

bool ReleaseConfiguration::update_configured() const noexcept {
    return is_secure_https_url(manifest_url) && public_key.size() == 65 &&
           public_key.front() == '\x04' && !platform.isEmpty();
}

bool ReleaseConfiguration::repository_check_configured() const noexcept {
    return is_secure_https_url(repository_url) && is_secure_https_url(releases_api_url);
}

ReleaseConfiguration compiled_release_configuration() {
    ReleaseConfiguration result;
    result.manifest_url = configured_https_url(build_config::manifest_url);
    result.public_key = decode_canonical_base64(QByteArray(build_config::public_key_base64));
    result.repository_url = configured_https_url(build_config::repository_url);
    result.releases_api_url = configured_https_url(build_config::releases_api_url);
    result.donate_url = configured_https_url(build_config::donate_url);
    result.issue_url = configured_https_url(build_config::issue_url);
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    result.platform = QStringLiteral("windows-x64");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    result.platform = QStringLiteral("linux-x64");
#else
    result.platform = QStringLiteral("unsupported");
#endif
    return result;
}

QString compiled_application_version() {
    return QString::fromLatin1(build_config::application_version);
}

} // namespace vove::ui::updates
