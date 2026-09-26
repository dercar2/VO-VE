#pragma once

#include <QString>

namespace vove::preview::helper {

// Called only after establishing the helper's parent-process lifetime guard.
[[nodiscard]] int run_gif_stream(const QString &path, quint64 source_size, qint64 source_modified,
                                 const QString &source_revision);

} // namespace vove::preview::helper
