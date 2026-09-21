#pragma once

#include <QLocale>
#include <QString>
#include <Qt>

namespace vove::ui {

enum class AppLanguage : int {
    russian,
    english,
    chinese_simplified,
    arabic,
};

enum class UiTextId : int {
    main_menu,
    language,
    settings,
    check_updates,
    about,
    components,
    donate,
    report_issue,
    local_filter,
    global_search,
    sort_field,
    sort_direction,
    sort_name,
    sort_extension,
    sort_date,
    sort_size,
    ascending,
    descending,
    thumbnail_size,
    smaller_thumbnails,
    larger_thumbnails,
    local_or_network_path,
    theme_north,
    theme_vanilla,
    theme_breeze,
    theme_twilight,
    save,
    cancel,
    close,
    settings_title,
    search_roots_title,
    search_roots_description,
    local_or_unc_path,
    browse,
    add,
    remove,
    external_applications_title,
    external_applications_description,
    preview_cache,
    cache_occupied,
    calculating,
    cache_limit,
    clear_cache,
    cache_clear_failed,
    startup_title,
    start_with_system,
    startup_change_failed,
    update_title,
    update_not_configured,
    about_title,
    about_summary,
    version,
    build,
    platform,
    license_title,
    license_summary,
    contributors_title,
    contributors_summary,
    components_title,
    component,
    state,
    purpose,
    download,
    installed,
    not_found,
    recommended,
    required,
    check_again,
    line_seed_purpose,
    ghostscript_purpose,
    bubblewrap_purpose,
    everything_purpose,
    plocate_purpose,
    unavailable_link,
    update_installed_version,
    update_available_version,
    update_checking,
    update_current,
    update_available,
    update_network_failed,
    update_security_failed,
    update_platform_mismatch,
    update_verifier_unavailable,
    update_changes,
    update_open_release,
    update_repository_current,
    update_repository_available,
    update_no_published_release,
    update_open_repository,
    confirm_external_link,
};

[[nodiscard]] QString ui_text(UiTextId id);
[[nodiscard]] QString language_native_name(AppLanguage language);
[[nodiscard]] QString language_code(AppLanguage language);
[[nodiscard]] AppLanguage app_language_from_code(const QString &code);
[[nodiscard]] AppLanguage app_language_from_locale(const QLocale &locale);
[[nodiscard]] QLocale app_language_locale(AppLanguage language);
[[nodiscard]] Qt::LayoutDirection app_language_direction(AppLanguage language) noexcept;

} // namespace vove::ui
