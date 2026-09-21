#include "default_application_name.hpp"

#include <QFileInfo>

#ifdef _WIN32
#include <shlwapi.h>

#include <string>
#endif

namespace vove::ui {

QString default_application_name(const QString &file_path) {
#ifdef _WIN32
    const auto suffix = QFileInfo(file_path).suffix();
    if (suffix.isEmpty()) {
        return {};
    }
    const auto association = QStringLiteral(".") + suffix;
    DWORD length{};
    const auto first =
        AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, association.toStdWString().c_str(),
                          nullptr, nullptr, &length);
    if (first != S_FALSE || length < 2) {
        return {};
    }
    std::wstring buffer(length, L'\0');
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME,
                                 association.toStdWString().c_str(), nullptr, buffer.data(),
                                 &length))) {
        return {};
    }
    return QString::fromWCharArray(buffer.c_str()).trimmed();
#else
    Q_UNUSED(file_path);
    return {};
#endif
}

} // namespace vove::ui
