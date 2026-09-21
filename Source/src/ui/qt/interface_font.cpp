#include "interface_font.hpp"

#include "external_component_detection.hpp"

#include <QApplication>
#include <QStringList>
#include <QToolTip>

namespace vove::ui {

QFont interface_font(const AppLanguage language, const QFont &base_font) {
    QStringList candidates;
    const auto line_seed =
        line_seed_jp_available() ? external_component_detail::line_seed_jp_family() : QString{};
    // LINE Seed JP covers Russian, including Yo; CJK and Arabic retain regional preferences.
    if ((language == AppLanguage::english || language == AppLanguage::russian) &&
        !line_seed.isEmpty()) {
        candidates.push_back(line_seed);
    }
    switch (language) {
    case AppLanguage::chinese_simplified:
        candidates << QStringLiteral("Microsoft YaHei UI") << QStringLiteral("Noto Sans CJK SC");
        break;
    case AppLanguage::arabic:
        candidates << QStringLiteral("Noto Sans Arabic") << QStringLiteral("Segoe UI");
        break;
    case AppLanguage::russian:
    case AppLanguage::english:
        candidates << QStringLiteral("Segoe UI") << QStringLiteral("Noto Sans");
        break;
    }
    candidates << QStringLiteral("DejaVu Sans");
    auto font = base_font;
    font.setFamilies(candidates);
    font.setStyleHint(QFont::SansSerif, QFont::PreferAntialias);
    font.setPointSize(11);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

void apply_interface_font(const AppLanguage language) {
    const auto font = interface_font(language, qApp->font());
    qApp->setFont(font);
    // Popups have platform-specific defaults separate from the application font.
    qApp->setFont(font, "QMenu");
    qApp->setFont(font, "QMenuBar");
    QToolTip::setFont(font);
}

} // namespace vove::ui
