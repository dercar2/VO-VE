#pragma once

#include <QtTypes>

namespace vove::ui {

// Presentation only. Never a source of paths, revisions, MIME payloads or operation approval.
struct SelectionActionSummary {
    qsizetype count{}; // Includes the current-row fallback only when nothing is selected.
    qsizetype directories{};
    bool has_selection{};
    bool all_ordinary{true};
    bool same_parent{true};
};

} // namespace vove::ui
