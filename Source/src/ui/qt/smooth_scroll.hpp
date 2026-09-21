#pragma once

class QAbstractItemView;

namespace vove::ui {

void install_smooth_scroll(QAbstractItemView &view, int duration_ms = 120);

} // namespace vove::ui
