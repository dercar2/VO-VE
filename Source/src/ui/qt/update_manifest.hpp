#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <compare>
#include <optional>

namespace vove::ui::updates {

struct SemanticVersion {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};

    auto operator<=>(const SemanticVersion &) const = default;
};

struct UpdateArtifact {
    QUrl url;
    QByteArray sha256_hex;
    std::uint64_t bytes{};
};

struct UpdateManifest {
    int schema{};
    QString version_text;
    SemanticVersion version;
    QString platform;
    QUrl release_url;
    UpdateArtifact artifact;
    QStringList notes;
};

enum class ManifestError : std::uint8_t {
    none,
    too_large,
    malformed_json,
    invalid_schema,
    invalid_version,
    invalid_platform,
    invalid_artifact,
    invalid_notes,
};

struct ManifestParseResult {
    std::optional<UpdateManifest> manifest;
    ManifestError error{ManifestError::none};
};

inline constexpr qsizetype kMaximumManifestBytes = 64 * 1024;
inline constexpr qsizetype kMaximumSignatureBytes = 512;

[[nodiscard]] bool is_secure_https_url(const QUrl &url) noexcept;
[[nodiscard]] std::optional<SemanticVersion> parse_semantic_version(const QString &text) noexcept;
[[nodiscard]] ManifestParseResult parse_update_manifest(const QByteArray &bytes);

} // namespace vove::ui::updates
