#include "external_application_registry.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace vove::ui {
namespace {

constexpr qsizetype maximumApplications = 32;
constexpr qsizetype maximumIconBytes = static_cast<qsizetype>(256) * 1024;

[[nodiscard]] QByteArray normalized_icon(QByteArray icon) {
    if (icon.size() > maximumIconBytes) {
        icon.clear();
    }
    return icon;
}

[[nodiscard]] QString desktop_entry_name(const QString &path) {
#ifdef _WIN32
    Q_UNUSED(path);
    return {};
#else
    QSettings desktop(path, QSettings::IniFormat);
    desktop.beginGroup(QStringLiteral("Desktop Entry"));
    const auto valid =
        desktop.value(QStringLiteral("Type")).toString() == QStringLiteral("Application") &&
        !desktop.value(QStringLiteral("Exec")).toString().trimmed().isEmpty();
    const auto name = valid ? desktop.value(QStringLiteral("Name")).toString() : QString{};
    desktop.endGroup();
    return name;
#endif
}

[[nodiscard]] QString normalized_name(QString name, const QString &path,
                                      const ExternalApplicationKind kind) {
    name.replace(QLatin1Char('\n'), QLatin1Char(' '));
    name.replace(QLatin1Char('\r'), QLatin1Char(' '));
    name.replace(QLatin1Char('\t'), QLatin1Char(' '));
    name = name.simplified().left(128);
    if (name.isEmpty() && kind == ExternalApplicationKind::desktop_entry) {
        name = desktop_entry_name(path).simplified().left(128);
    }
    if (name.isEmpty()) {
        name = QFileInfo(path).completeBaseName().simplified().left(128);
    }
    return name;
}

[[nodiscard]] QString structural_application_path(const QString &path,
                                                  const ExternalApplicationKind kind) {
    auto cleaned = QDir::cleanPath(path.trimmed());
    if (!QDir::isAbsolutePath(cleaned)) {
        return {};
    }
#ifdef _WIN32
    if (kind != ExternalApplicationKind::executable) {
        return {};
    }
    const auto suffix = QFileInfo(cleaned).suffix().toCaseFolded();
    if (suffix != QStringLiteral("exe") && suffix != QStringLiteral("com")) {
        return {};
    }
#else
    if (kind == ExternalApplicationKind::desktop_entry &&
        QFileInfo(cleaned).suffix().compare(QStringLiteral("desktop"), Qt::CaseInsensitive) != 0) {
        return {};
    }
#endif
    return cleaned;
}

[[nodiscard]] QString validated_application_path(const ExternalApplication &application) {
    const auto structural = structural_application_path(application.path, application.kind);
    const QFileInfo info(structural);
    if (structural.isEmpty() || !info.exists() || !info.isFile()) {
        return {};
    }
#ifndef _WIN32
    if (application.kind == ExternalApplicationKind::desktop_entry) {
        if (desktop_entry_name(structural).isEmpty()) {
            return {};
        }
    } else if (!info.isExecutable()) {
        return {};
    }
#endif
    return info.canonicalFilePath();
}

[[nodiscard]] QString comparison_key(const QString &path) {
    const auto cleaned = QDir::cleanPath(path);
#ifdef _WIN32
    return cleaned.toCaseFolded();
#else
    return cleaned;
#endif
}

} // namespace

void ExternalApplicationRegistry::load(QSettings &settings) {
    applications_.clear();
    const auto count = std::min(settings.beginReadArray(QStringLiteral("desktop/openWith")),
                                static_cast<int>(maximumApplications));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        const auto kind =
            settings.value(QStringLiteral("kind")).toString() == QStringLiteral("desktop")
                ? ExternalApplicationKind::desktop_entry
                : ExternalApplicationKind::executable;
        const auto path =
            structural_application_path(settings.value(QStringLiteral("path")).toString(), kind);
        if (path.isEmpty()) {
            continue;
        }
        const auto key = comparison_key(path);
        if (std::ranges::none_of(applications_, [&](const auto &application) {
                return comparison_key(application.path) == key;
            })) {
            applications_.push_back(
                {.kind = kind,
                 .name =
                     normalized_name(settings.value(QStringLiteral("name")).toString(), path, kind),
                 .path = path,
                 .icon_png =
                     normalized_icon(settings.value(QStringLiteral("iconPng")).toByteArray())});
        }
    }
    settings.endArray();
}

void ExternalApplicationRegistry::save(QSettings &settings) const {
    settings.beginWriteArray(QStringLiteral("desktop/openWith"),
                             static_cast<int>(applications_.size()));
    for (qsizetype index = 0; index < applications_.size(); ++index) {
        settings.setArrayIndex(static_cast<int>(index));
        settings.setValue(QStringLiteral("kind"),
                          applications_.at(index).kind == ExternalApplicationKind::desktop_entry
                              ? QStringLiteral("desktop")
                              : QStringLiteral("executable"));
        settings.setValue(QStringLiteral("name"), applications_.at(index).name);
        settings.setValue(QStringLiteral("path"), applications_.at(index).path);
        settings.setValue(QStringLiteral("iconPng"), applications_.at(index).icon_png);
    }
    settings.endArray();
}

AddExternalApplicationResult
ExternalApplicationRegistry::add(const ExternalApplication &application) {
    auto candidate = application;
#ifndef _WIN32
    if (QFileInfo(candidate.path)
            .suffix()
            .compare(QStringLiteral("desktop"), Qt::CaseInsensitive) == 0) {
        candidate.kind = ExternalApplicationKind::desktop_entry;
    }
#endif
    const auto validated = validated_application_path(candidate);
    if (validated.isEmpty()) {
        return AddExternalApplicationResult::invalid;
    }
    const auto key = comparison_key(validated);
    const auto duplicate = std::ranges::find_if(applications_, [&](const auto &application) {
        return comparison_key(application.path) == key;
    });
    if (duplicate != applications_.end()) {
        return AddExternalApplicationResult::duplicate;
    }
    if (applications_.size() >= maximumApplications) {
        return AddExternalApplicationResult::invalid;
    }
    applications_.push_back({.kind = candidate.kind,
                             .name = normalized_name(candidate.name, validated, candidate.kind),
                             .path = validated,
                             .icon_png = normalized_icon(std::move(candidate.icon_png))});
    return AddExternalApplicationResult::added;
}

bool ExternalApplicationRegistry::remove(const QString &path) {
    const auto key = comparison_key(path);
    const auto found = std::ranges::find_if(applications_, [&](const auto &application) {
        return comparison_key(application.path) == key;
    });
    if (found == applications_.end()) {
        return false;
    }
    applications_.erase(found);
    return true;
}

bool ExternalApplicationRegistry::launch(const ExternalApplication &application,
                                         const QString &file_path, QString *error) const {
    const auto executable = validated_application_path(application);
    const auto source = QDir::fromNativeSeparators(file_path);
    if (executable.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("application_unavailable");
        }
        return false;
    }
    if (source.isEmpty() || !QDir::isAbsolutePath(source)) {
        if (error != nullptr) {
            *error = QStringLiteral("file_unavailable");
        }
        return false;
    }
    // The catalog already owns this path. Avoid a synchronous stat here: a disconnected SMB
    // resource must not freeze the UI before the selected application gets a chance to report it.
    const auto absolute_source = QDir::cleanPath(source);
    bool started{};
    if (application.kind == ExternalApplicationKind::desktop_entry) {
        const auto gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
        if (gio.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("launcher_unavailable");
            }
            return false;
        }
        const auto source_uri = QUrl::fromLocalFile(absolute_source).toString(QUrl::FullyEncoded);
        started = QProcess::startDetached(gio, {QStringLiteral("launch"), executable, source_uri});
    } else {
        started = QProcess::startDetached(executable, {QDir::toNativeSeparators(absolute_source)});
    }
    if (!started) {
        if (error != nullptr) {
            *error = QStringLiteral("launch_failed");
        }
        return false;
    }
    return true;
}

const QList<ExternalApplication> &ExternalApplicationRegistry::applications() const noexcept {
    return applications_;
}

} // namespace vove::ui
