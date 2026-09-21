#pragma once

#include <windows.h>

namespace vove::platform::detail {

enum class WindowsFilesystemErrorKind {
    not_found,
    permission_denied,
    authentication_required,
    timed_out,
    disconnected,
    io_error,
};

[[nodiscard]] constexpr WindowsFilesystemErrorKind
classify_windows_filesystem_error(const DWORD code) noexcept {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return WindowsFilesystemErrorKind::not_found;
    case ERROR_ACCESS_DENIED:
    case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return WindowsFilesystemErrorKind::permission_denied;
    case ERROR_INVALID_PASSWORD:
    case ERROR_LOGON_FAILURE:
    case ERROR_ACCOUNT_RESTRICTION:
    case ERROR_INVALID_LOGON_HOURS:
    case ERROR_INVALID_WORKSTATION:
    case ERROR_PASSWORD_EXPIRED:
    case ERROR_ACCOUNT_DISABLED:
    case ERROR_NOT_LOGGED_ON:
    case ERROR_NO_SUCH_LOGON_SESSION:
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
    case ERROR_NOT_AUTHENTICATED:
    case ERROR_LOGON_TYPE_NOT_GRANTED:
    case ERROR_ACCOUNT_EXPIRED:
    case ERROR_BAD_USERNAME:
    case ERROR_PASSWORD_MUST_CHANGE:
    case ERROR_ACCOUNT_LOCKED_OUT:
    case ERROR_INVALID_ACCOUNT_NAME:
    case ERROR_NO_SUCH_USER:
    case ERROR_DOWNGRADE_DETECTED:
    case ERROR_MUTUAL_AUTH_FAILED:
    case ERROR_NO_TRUST_LSA_SECRET:
    case ERROR_NO_TRUST_SAM_ACCOUNT:
    case ERROR_TRUSTED_DOMAIN_FAILURE:
    case ERROR_TRUSTED_RELATIONSHIP_FAILURE:
    case ERROR_NOLOGON_INTERDOMAIN_TRUST_ACCOUNT:
    case ERROR_NOLOGON_WORKSTATION_TRUST_ACCOUNT:
    case ERROR_NOLOGON_SERVER_TRUST_ACCOUNT:
    case ERROR_DOMAIN_TRUST_INCONSISTENT:
    case ERROR_AUTHENTICATION_FIREWALL_FAILED:
    case ERROR_CANT_ACCESS_DOMAIN_INFO:
    case ERROR_NO_SUCH_DOMAIN:
    case ERROR_TRUST_FAILURE:
        return WindowsFilesystemErrorKind::authentication_required;
    case ERROR_SEM_TIMEOUT:
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:
        return WindowsFilesystemErrorKind::timed_out;
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_UNAVAIL:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_NO_LOGON_SERVERS:
    case ERROR_NETLOGON_NOT_STARTED:
        return WindowsFilesystemErrorKind::disconnected;
    default:
        return WindowsFilesystemErrorKind::io_error;
    }
}

} // namespace vove::platform::detail
