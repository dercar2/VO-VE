#include "catalog_tool_button.hpp"
#include "catalog_tool_artwork.hpp"

#include <QPainter>

namespace vove::ui {

CatalogToolButton::CatalogToolButton(const CatalogToolKind kind, QWidget *parent)
    : QToolButton(parent), kind_(kind) {
    setFixedSize(28, 28);
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
}

void CatalogToolButton::set_theme(const AppTheme theme) {
    theme_ = theme;
    update();
}

void CatalogToolButton::paintEvent(QPaintEvent *) {
    const auto interaction =
        isEnabled() && (underMouse() || isDown() ||
                        (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)));
    const auto active = interaction || (kind_ == CatalogToolKind::recursive_view && isChecked());
    const auto state = static_cast<std::size_t>(theme_) * 2 + (active ? 1 : 0);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.scale(1, -1);
    if (!isEnabled()) {
        painter.setOpacity(0.4);
    }
    const auto draw = [&painter, state](const auto &paths, const auto &inks) {
        const auto &colors = inks.at(state);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            painter.fillPath(paths[i], QColor(colors[i]));
        }
    };
    if (kind_ == CatalogToolKind::recursive_view) {
        draw(artwork::catalog_eye_paths(), artwork::catalog_eye_inks);
    } else {
        draw(artwork::catalog_refresh_paths(), artwork::catalog_refresh_inks);
    }
}

} // namespace vove::ui
