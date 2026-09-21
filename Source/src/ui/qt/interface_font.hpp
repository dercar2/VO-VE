#pragma once

#include "ui_text.hpp"

#include <QFont>

namespace vove::ui {

[[nodiscard]] QFont interface_font(AppLanguage language, const QFont &base_font);
void apply_interface_font(AppLanguage language);

} // namespace vove::ui
