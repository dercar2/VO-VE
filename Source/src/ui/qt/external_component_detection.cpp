#include "external_component_detection.hpp"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace vove::ui::external_component_detail {

bool everything_ipc_available() {
#ifdef Q_OS_WIN
    return ::FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr) != nullptr;
#else
    return false;
#endif
}

} // namespace vove::ui::external_component_detail
