#include "update_manifest.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vove::ui::updates {
namespace {

bool has_exact_keys(const QJsonObject &object, const QSet<QString> &expected) {
    if (object.size() != expected.size()) {
        return false;
    }
    return std::ranges::all_of(object.keys(),
                               [&expected](const QString &key) { return expected.contains(key); });
}

bool clean_short_text(const QString &text, const qsizetype maximum) {
    if (text.isEmpty() || text.size() > maximum) {
        return false;
    }
    return std::ranges::none_of(text, [](const QChar character) {
        return character.unicode() == 0U ||
               (character.category() == QChar::Other_Control && character != QLatin1Char('\n'));
    });
}

std::optional<std::uint32_t> parse_version_part(const QStringView part) noexcept {
    if (part.isEmpty() || part.size() > 10 ||
        (part.size() > 1 && part.front() == QLatin1Char('0'))) {
        return std::nullopt;
    }
    std::uint64_t value{};
    for (const auto character : part) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint64_t>(character.unicode() - u'0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

bool valid_sha256(const QByteArray &value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f') ||
                      (character >= 'A' && character <= 'F');
           });
}

ManifestParseResult failure(const ManifestError error) {
    return {.manifest = std::nullopt, .error = error};
}

} // namespace

bool is_secure_https_url(const QUrl &url) noexcept {
    return url.isValid() &&
           url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
}

std::optional<SemanticVersion> parse_semantic_version(const QString &text) noexcept {
    const auto parts = QStringView(text).split(QLatin1Char('.'));
    if (parts.size() != 3) {
        return std::nullopt;
    }
    const auto major = parse_version_part(parts[0]);
    const auto minor = parse_version_part(parts[1]);
    const auto patch = parse_version_part(parts[2]);
    if (!major || !minor || !patch) {
        return std::nullopt;
    }
    return SemanticVersion{*major, *minor, *patch};
}

ManifestParseResult parse_update_manifest(const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > kMaximumManifestBytes) {
        return failure(ManifestError::too_large);
    }

    QJsonParseError json_error;
    const auto document = QJsonDocument::fromJson(bytes, &json_error);
    if (json_error.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(ManifestError::malformed_json);
    }

    const auto root = document.object();
    if (!has_exact_keys(root, {QStringLiteral("schema"), QStringLiteral("version"),
                               QStringLiteral("platform"), QStringLiteral("release_url"),
                               QStringLiteral("artifact"), QStringLiteral("notes")})) {
        return failure(ManifestError::invalid_schema);
    }
    const auto schema = root.value(QStringLiteral("schema"));
    if (!schema.isDouble() || schema.toDouble() != 1.0) {
        return failure(ManifestError::invalid_schema);
    }

    const auto version_text = root.value(QStringLiteral("version")).toString();
    const auto version = parse_semantic_version(version_text);
    if (!version) {
        return failure(ManifestError::invalid_version);
    }

    const auto platform = root.value(QStringLiteral("platform")).toString();
    if (!clean_short_text(platform, 48) || platform.contains(QLatin1Char('/')) ||
        platform.contains(QLatin1Char('\\'))) {
        return failure(ManifestError::invalid_platform);
    }

    const QUrl release_url(root.value(QStringLiteral("release_url")).toString(), QUrl::StrictMode);
    if (!is_secure_https_url(release_url)) {
        return failure(ManifestError::invalid_artifact);
    }

    const auto artifact_value = root.value(QStringLiteral("artifact"));
    if (!artifact_value.isObject()) {
        return failure(ManifestError::invalid_artifact);
    }
    const auto artifact = artifact_value.toObject();
    if (!has_exact_keys(
            artifact, {QStringLiteral("url"), QStringLiteral("sha256"), QStringLiteral("bytes")})) {
        return failure(ManifestError::invalid_artifact);
    }
    const QUrl artifact_url(artifact.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
    const auto sha256 = artifact.value(QStringLiteral("sha256")).toString().toLatin1();
    const auto artifact_bytes = artifact.value(QStringLiteral("bytes"));
    const auto byte_value = artifact_bytes.toDouble(-1.0);
    constexpr double maximum_artifact_bytes = 16.0 * 1024.0 * 1024.0 * 1024.0;
    if (!is_secure_https_url(artifact_url) || !valid_sha256(sha256) || !artifact_bytes.isDouble() ||
        !std::isfinite(byte_value) || byte_value < 1.0 || byte_value > maximum_artifact_bytes ||
        std::floor(byte_value) != byte_value) {
        return failure(ManifestError::invalid_artifact);
    }

    const auto notes_value = root.value(QStringLiteral("notes"));
    if (!notes_value.isArray()) {
        return failure(ManifestError::invalid_notes);
    }
    const auto notes_array = notes_value.toArray();
    if (notes_array.size() > 12) {
        return failure(ManifestError::invalid_notes);
    }
    QStringList notes;
    qsizetype total_note_characters{};
    for (const auto &note_value : notes_array) {
        if (!note_value.isString()) {
            return failure(ManifestError::invalid_notes);
        }
        const auto note = note_value.toString();
        total_note_characters += note.size();
        if (!clean_short_text(note, 240) || total_note_characters > 2'048) {
            return failure(ManifestError::invalid_notes);
        }
        notes.push_back(note);
    }

    UpdateManifest result;
    result.schema = 1;
    result.version_text = version_text;
    result.version = *version;
    result.platform = platform;
    result.release_url = release_url;
    result.artifact = {.url = artifact_url,
                       .sha256_hex = sha256.toLower(),
                       .bytes = static_cast<std::uint64_t>(byte_value)};
    result.notes = notes;
    return {.manifest = std::move(result), .error = ManifestError::none};
}

} // namespace vove::ui::updates
