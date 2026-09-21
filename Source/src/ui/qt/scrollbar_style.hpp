#pragma once

class QStyle;

namespace vove::ui {

enum class AppTheme : int;

QStyle *create_application_style();
void apply_scrollbar_theme(AppTheme theme);

} // namespace vove::ui
