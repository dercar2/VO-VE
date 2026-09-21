#pragma once

#include <QList>
#include <QRect>

namespace vove::ui {

[[nodiscard]] QRect visible_window_geometry(const QRect &requested,
                                            const QList<QRect> &available_screens) noexcept;

} // namespace vove::ui
