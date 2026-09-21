#include "ui_text.hpp"

#include <QCoreApplication>

#include <array>

namespace vove::ui {
namespace {

using TextTable = std::array<const char *, 90>;

static_assert(static_cast<int>(UiTextId::confirm_external_link) + 1 == 90);

constexpr TextTable english{
    QT_TRANSLATE_NOOP("UiText", "Main menu"), QT_TRANSLATE_NOOP("UiText", "Language"),
    QT_TRANSLATE_NOOP("UiText", "Settings…"), QT_TRANSLATE_NOOP("UiText", "Check for updates…"),
    QT_TRANSLATE_NOOP("UiText", "About VO-VE"), QT_TRANSLATE_NOOP("UiText", "Components…"),
    QT_TRANSLATE_NOOP("UiText", "Support the project…"),
    QT_TRANSLATE_NOOP("UiText", "Report a problem…"), QT_TRANSLATE_NOOP("UiText", "Filter by name"),
    QT_TRANSLATE_NOOP("UiText", "Global search"), QT_TRANSLATE_NOOP("UiText", "Sort field"),
    QT_TRANSLATE_NOOP("UiText", "Sort direction"), QT_TRANSLATE_NOOP("UiText", "Name"),
    QT_TRANSLATE_NOOP("UiText", "Extension"), QT_TRANSLATE_NOOP("UiText", "Date"),
    QT_TRANSLATE_NOOP("UiText", "Size"), QT_TRANSLATE_NOOP("UiText", "Ascending"),
    QT_TRANSLATE_NOOP("UiText", "Descending"), QT_TRANSLATE_NOOP("UiText", "Thumbnail size"),
    QT_TRANSLATE_NOOP("UiText", "Smaller thumbnails"),
    QT_TRANSLATE_NOOP("UiText", "Larger thumbnails"),
    QT_TRANSLATE_NOOP("UiText", "Local or network path"), QT_TRANSLATE_NOOP("UiText", "North"),
    QT_TRANSLATE_NOOP("UiText", "Vanilla"), QT_TRANSLATE_NOOP("UiText", "Breeze"),
    QT_TRANSLATE_NOOP("UiText", "Twilight"), QT_TRANSLATE_NOOP("UiText", "Save"),
    QT_TRANSLATE_NOOP("UiText", "Cancel"), QT_TRANSLATE_NOOP("UiText", "Close"),
    QT_TRANSLATE_NOOP("UiText", "Settings"), QT_TRANSLATE_NOOP("UiText", "Global search settings"),
    QT_TRANSLATE_NOOP("UiText",
                      "Global search is limited to these local or mounted network folders."),
    QT_TRANSLATE_NOOP("UiText", "Local folder or UNC path"), QT_TRANSLATE_NOOP("UiText", "Browse…"),
    QT_TRANSLATE_NOOP("UiText", "Add"), QT_TRANSLATE_NOOP("UiText", "Remove"),
    QT_TRANSLATE_NOOP("UiText", "External applications"),
    QT_TRANSLATE_NOOP("UiText", "Applications saved for the Open with command."),
    QT_TRANSLATE_NOOP("UiText", "Preview cache"), QT_TRANSLATE_NOOP("UiText", "Occupied"),
    QT_TRANSLATE_NOOP("UiText", "Calculating…"), QT_TRANSLATE_NOOP("UiText", "Limit"),
    QT_TRANSLATE_NOOP("UiText", "Clear cache"),
    QT_TRANSLATE_NOOP("UiText", "Preview cache is unavailable."),
    QT_TRANSLATE_NOOP("UiText", "Startup"), QT_TRANSLATE_NOOP("UiText", "Start with the system"),
    QT_TRANSLATE_NOOP("UiText", "Could not update the startup setting."),
    QT_TRANSLATE_NOOP("UiText", "Updates"),
    // NOLINTNEXTLINE(bugprone-suspicious-missing-comma)
    QT_TRANSLATE_NOOP("UiText", "Update sources are not configured. The network was not used."),
    QT_TRANSLATE_NOOP("UiText", "About VO-VE"),
    QT_TRANSLATE_NOOP(
        "UiText",
        "VO-VE is an ultra-lightweight and fast viewer. The concept is based on two core "
        "principles: view only and view everything. No editing of graphics, tags or other "
        "unnecessary features."),
    QT_TRANSLATE_NOOP("UiText", "Version"), QT_TRANSLATE_NOOP("UiText", "Build"),
    QT_TRANSLATE_NOOP("UiText", "Platform"), QT_TRANSLATE_NOOP("UiText", "License"),
    // NOLINTNEXTLINE(bugprone-suspicious-missing-comma)
    QT_TRANSLATE_NOOP("UiText", "VO-VE is licensed under GPL-3.0-or-later. Third-party components "
                                "retain their own licenses."),
    QT_TRANSLATE_NOOP("UiText", "Authors"),
    QT_TRANSLATE_NOOP("UiText", "Contributors: there are no external contributors yet"),
    QT_TRANSLATE_NOOP("UiText", "Additional components"), QT_TRANSLATE_NOOP("UiText", "Component"),
    QT_TRANSLATE_NOOP("UiText", "State"), QT_TRANSLATE_NOOP("UiText", "Purpose"),
    QT_TRANSLATE_NOOP("UiText", "Download"), QT_TRANSLATE_NOOP("UiText", "Installed"),
    QT_TRANSLATE_NOOP("UiText", "Not found"), QT_TRANSLATE_NOOP("UiText", "Recommended"),
    QT_TRANSLATE_NOOP("UiText", "Required"), QT_TRANSLATE_NOOP("UiText", "Check again"),
    QT_TRANSLATE_NOOP(
        "UiText", "Without this font the interface works, but differs from the authored design."),
    QT_TRANSLATE_NOOP("UiText", "Required for full PS, EPS and legacy AI preview support."),
    QT_TRANSLATE_NOOP("UiText",
                      "Required on Linux to run preview handlers with minimum permissions."),
    QT_TRANSLATE_NOOP("UiText", "Recommended for instant global search on Windows."),
    QT_TRANSLATE_NOOP("UiText", "Recommended for indexed global search on Linux."),
    QT_TRANSLATE_NOOP("UiText", "The project link is not configured yet."),
    QT_TRANSLATE_NOOP("UiText", "Installed version"),
    QT_TRANSLATE_NOOP("UiText", "Available version"),
    QT_TRANSLATE_NOOP("UiText", "Checking for updates…"),
    QT_TRANSLATE_NOOP("UiText", "No newer version is available."),
    QT_TRANSLATE_NOOP("UiText", "A new version is available."),
    QT_TRANSLATE_NOOP("UiText", "Could not check for updates. Try again later."),
    QT_TRANSLATE_NOOP("UiText",
                      "The update information could not be verified. Nothing was downloaded."),
    QT_TRANSLATE_NOOP("UiText", "This update is intended for another platform."),
    QT_TRANSLATE_NOOP("UiText", "Update verification is not available on this platform."),
    QT_TRANSLATE_NOOP("UiText", "Changes"), QT_TRANSLATE_NOOP("UiText", "Open release page"),
    QT_TRANSLATE_NOOP("UiText", "No newer published version is available on GitHub."),
    QT_TRANSLATE_NOOP("UiText", "A newer GitHub release is available."),
    QT_TRANSLATE_NOOP("UiText", "No published GitHub release is available yet."),
    QT_TRANSLATE_NOOP("UiText", "Open repository"),
    QT_TRANSLATE_NOOP("UiText", "Open this page in the default browser?")};

} // namespace

QString ui_text(const UiTextId id) {
    const auto index = static_cast<std::size_t>(id);
    const auto *value = index < english.size() ? english[index] : "";
    return QCoreApplication::translate("UiText", value);
}

QString language_native_name(const AppLanguage language) {
    switch (language) {
    case AppLanguage::russian:
        return QString::fromUtf8("Русский");
    case AppLanguage::english:
        return QStringLiteral("English");
    case AppLanguage::chinese_simplified:
        return QString::fromUtf8("简体中文");
    case AppLanguage::arabic:
        return QString::fromUtf8("العربية");
    }
    return QStringLiteral("English");
}

QString language_code(const AppLanguage language) {
    switch (language) {
    case AppLanguage::russian:
        return QStringLiteral("ru");
    case AppLanguage::english:
        return QStringLiteral("en");
    case AppLanguage::chinese_simplified:
        return QStringLiteral("zh-CN");
    case AppLanguage::arabic:
        return QStringLiteral("ar");
    }
    return QStringLiteral("en");
}

AppLanguage app_language_from_code(const QString &code) {
    const auto normalized = code.trimmed().toLower();
    if (normalized.startsWith(QStringLiteral("ru"))) {
        return AppLanguage::russian;
    }
    if (normalized.startsWith(QStringLiteral("zh"))) {
        return AppLanguage::chinese_simplified;
    }
    if (normalized.startsWith(QStringLiteral("ar"))) {
        return AppLanguage::arabic;
    }
    return AppLanguage::english;
}

AppLanguage app_language_from_locale(const QLocale &locale) {
    switch (locale.language()) {
    case QLocale::Russian:
        return AppLanguage::russian;
    case QLocale::Chinese:
        return AppLanguage::chinese_simplified;
    case QLocale::Arabic:
        return AppLanguage::arabic;
    default:
        return AppLanguage::english;
    }
}

QLocale app_language_locale(const AppLanguage language) {
    switch (language) {
    case AppLanguage::russian:
        return {QLocale::Russian, QLocale::Russia};
    case AppLanguage::english:
        return {QLocale::English, QLocale::UnitedStates};
    case AppLanguage::chinese_simplified:
        return {QLocale::Chinese, QLocale::SimplifiedChineseScript, QLocale::China};
    case AppLanguage::arabic:
        return {QLocale::Arabic, QLocale::Egypt};
    }
    return {QLocale::English, QLocale::UnitedStates};
}

Qt::LayoutDirection app_language_direction(const AppLanguage language) noexcept {
    return language == AppLanguage::arabic ? Qt::RightToLeft : Qt::LeftToRight;
}

} // namespace vove::ui
