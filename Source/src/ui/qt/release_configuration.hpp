#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace vove::ui::updates {

struct ReleaseConfiguration {
    QUrl manifest_url;
    QByteArray public_key;
    QUrl repository_url;
    QUrl releases_api_url;
    QUrl donate_url;
    QUrl issue_url;
    QString platform;

    [[nodiscard]] bool update_configured() const noexcept;
    [[nodiscard]] bool repository_check_configured() const noexcept;
};

[[nodiscard]] ReleaseConfiguration compiled_release_configuration();
[[nodiscard]] QString compiled_application_version();

} // namespace vove::ui::updates
