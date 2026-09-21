#pragma once

#include <QColor>
#include <QString>

namespace vove::ui {

[[nodiscard]] QColor format_color(const QString &suffix, const QColor &background);

} // namespace vove::ui
