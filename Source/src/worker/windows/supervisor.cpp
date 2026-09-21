#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <userenv.h>

#include "vove/worker/supervisor.hpp"
#include "vove/worker/windows_runtime_access.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vove::worker {
namespace {

constexpr DWORD kSupervisorTimeoutExitCode = 124;
constexpr DWORD kSupervisorFailureExitCode = 125;
constexpr std::chrono::milliseconds kPollInterval{5};

class UniqueHandle {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(UniqueHandle &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (*this) {
            static_cast<void>(CloseHandle(handle_));
        }
        handle_ = replacement;
    }

  private:
    HANDLE handle_{};
};

class SidOwner {
  public:
    SidOwner() = default;
    ~SidOwner() {
        if (sid_ != nullptr) {
            FreeSid(sid_);
        }
    }
    SidOwner(const SidOwner &) = delete;
    SidOwner &operator=(const SidOwner &) = delete;
    SidOwner(SidOwner &&other) noexcept : sid_(std::exchange(other.sid_, nullptr)) {}
    SidOwner &operator=(SidOwner &&other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.sid_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] PSID *out() noexcept {
        return &sid_;
    }
    [[nodiscard]] PSID get() const noexcept {
        return sid_;
    }
    void reset(PSID replacement = nullptr) noexcept {
        if (sid_ != nullptr) {
            FreeSid(sid_);
        }
        sid_ = replacement;
    }

  private:
    PSID sid_{};
};

[[nodiscard]] std::string windows_error(const char *operation, const DWORD code = GetLastError()) {
    return std::string{operation} + " failed with Windows error " + std::to_string(code);
}

[[nodiscard]] HANDLE as_handle(const NativeObject object) noexcept {
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(object));
}

[[nodiscard]] OpaqueToken as_token(const HANDLE handle) noexcept {
    return static_cast<OpaqueToken>(reinterpret_cast<std::uintptr_t>(handle));
}

[[nodiscard]] bool valid_handle(const HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

#if defined(VOVE_SUPERVISOR_TEST_HOOKS)
void pause_after_child_creation_for_test() noexcept {
    std::array<wchar_t, 32'768> marker{};
    const auto length = GetEnvironmentVariableW(L"VOVE_TEST_SUPERVISOR_AFTER_CREATE_MARKER",
                                                marker.data(), static_cast<DWORD>(marker.size()));
    if (length == 0 || length >= marker.size()) {
        return;
    }
    UniqueHandle signal{CreateFileW(marker.data(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    if (signal) {
        static_cast<void>(FlushFileBuffers(signal.get()));
        Sleep(10'000);
    }
}
#else
void pause_after_child_creation_for_test() noexcept {}
#endif

[[nodiscard]] bool output_size(const HANDLE output, std::uint64_t &bytes, std::string &detail) {
    LARGE_INTEGER size{};
    if (GetFileSizeEx(output, &size) == FALSE || size.QuadPart < 0) {
        detail = windows_error("GetFileSizeEx(worker output)");
        return false;
    }
    bytes = static_cast<std::uint64_t>(size.QuadPart);
    return true;
}

void append_detail(std::string &detail, const std::string &addition) {
    if (!detail.empty()) {
        detail += "; ";
    }
    detail += addition;
}

[[nodiscard]] SupervisorResult failure(const SupervisorError error, std::string detail,
                                       SandboxReport sandbox = {}, const int exit_code = 0,
                                       const DWORD system_error = 0) {
    return {.error = error,
            .detail = std::move(detail),
            .worker_result = {},
            .sandbox = std::move(sandbox),
            .exit_code = exit_code,
            .system_error = system_error};
}

[[nodiscard]] ExternalRendererResult external_failure(const SupervisorError error,
                                                      std::string detail,
                                                      SandboxReport sandbox = {},
                                                      const int exit_code = 0) {
    return {.error = error,
            .detail = std::move(detail),
            .sandbox = std::move(sandbox),
            .exit_code = exit_code,
            .bytes_written = 0};
}

[[nodiscard]] bool path_is_executable_file(const std::filesystem::path &path) {
    if (path.empty()) {
        return false;
    }
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] bool request_is_valid(const SupervisorRequest &request, std::string &detail) {
    if (!path_is_executable_file(request.worker_executable)) {
        detail = "worker executable is missing or is not a file";
        return false;
    }
    const auto source = as_handle(request.source_object);
    const auto output = as_handle(request.output_object);
    const auto has_profile = request.profile_object != kInvalidNativeObject;
    const auto profile = has_profile ? as_handle(request.profile_object) : nullptr;
    if (!valid_handle(source) || !valid_handle(output) || source == output) {
        detail = "source and output objects must be distinct valid handles";
        return false;
    }
    if (has_profile && (!valid_handle(profile) || profile == source || profile == output)) {
        detail = "profile object must be a distinct valid handle";
        return false;
    }
    if (GetFileType(source) != FILE_TYPE_DISK || GetFileType(output) != FILE_TYPE_DISK) {
        detail = "source and output objects must be disk-file handles";
        return false;
    }
    if (has_profile && GetFileType(profile) != FILE_TYPE_DISK) {
        detail = "profile object must be a disk-file handle";
        return false;
    }
    std::uint64_t existing_output_bytes{};
    if (!output_size(output, existing_output_bytes, detail)) {
        return false;
    }
    if (existing_output_bytes != 0) {
        detail = "worker output object must be initially empty";
        return false;
    }
    if (request.timeout.count() <= 0 ||
        request.timeout > std::chrono::milliseconds{kMaximumWallTimeoutMs}) {
        detail = "supervisor timeout is outside the protocol limit";
        return false;
    }

    try {
        auto job = request.job;
        job.source_token = 1;
        job.output_token = 2;
        job.profile_token = has_profile ? 3 : 0;
        static_cast<void>(encode_worker_job(job));
        static_cast<void>(
            encode_handshake({.build_id = request.expected_build_id, .capabilities = 0}));
    } catch (const std::exception &exception) {
        detail = std::string{"invalid worker request: "} + exception.what();
        return false;
    }
    return true;
}

[[nodiscard]] bool external_request_is_valid(const ExternalRendererRequest &request,
                                             std::string &detail, SupervisorError &error) {
    error = SupervisorError::invalid_request;
    if (!path_is_executable_file(request.executable)) {
        detail = "external renderer executable is missing or is not a file";
        return false;
    }
    const auto source = as_handle(request.source_object);
    const auto output = as_handle(request.output_object);
    if (!valid_handle(source) || !valid_handle(output) || source == output ||
        GetFileType(source) != FILE_TYPE_DISK || GetFileType(output) != FILE_TYPE_DISK) {
        detail = "external renderer requires distinct disk-file source and output handles";
        return false;
    }
    std::uint64_t existing_output_bytes{};
    if (!output_size(output, existing_output_bytes, detail)) {
        return false;
    }
    if (existing_output_bytes != 0) {
        detail = "external renderer output object must be initially empty";
        return false;
    }
    LARGE_INTEGER source_size{};
    if (GetFileSizeEx(source, &source_size) == FALSE || source_size.QuadPart < 0) {
        detail = windows_error("GetFileSizeEx(external renderer source)");
        return false;
    }
    if (static_cast<std::uint64_t>(source_size.QuadPart) > request.limits.maximum_input_bytes) {
        detail = "external renderer source exceeds the configured input limit";
        error = SupervisorError::resource_limit;
        return false;
    }
    if (request.timeout.count() <= 0 ||
        request.timeout > std::chrono::milliseconds{kMaximumWallTimeoutMs}) {
        detail = "external renderer timeout is outside the protocol limit";
        return false;
    }
    if (request.arguments_utf8.size() > 32U) {
        detail = "external renderer argument count exceeds the fixed-command limit";
        return false;
    }
    for (const auto &argument : request.arguments_utf8) {
        if (argument.size() > 1'024U || argument.find('\0') != std::string::npos) {
            detail = "external renderer argument exceeds the fixed-command limit";
            return false;
        }
    }
    try {
        const WorkerJob validation{.job_id = 1,
                                   .generation = 1,
                                   .source_token = 1,
                                   .output_token = 2,
                                   .limits = request.limits,
                                   .page_index = 0,
                                   .password_utf8 = {}};
        static_cast<void>(encode_worker_job(validation));
    } catch (const std::exception &exception) {
        detail = std::string{"invalid external renderer limits: "} + exception.what();
        return false;
    }
    return true;
}

[[nodiscard]] bool duplicate_inheritable_handle(const HANDLE source, const DWORD access,
                                                UniqueHandle &duplicate, std::string &detail) {
    HANDLE raw{};
    if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &raw, access, TRUE, 0) ==
        FALSE) {
        detail = windows_error("DuplicateHandle(external renderer)");
        return false;
    }
    duplicate.reset(raw);
    return true;
}

[[nodiscard]] bool utf8_to_wide(const std::string_view utf8, std::wstring &wide) {
    if (utf8.empty()) {
        wide.clear();
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const std::string utf8_buffer{utf8};
    const auto bytes = static_cast<int>(utf8_buffer.size());
    const auto characters =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_buffer.c_str(), bytes, nullptr, 0);
    if (characters <= 0) {
        return false;
    }
    wide.resize(static_cast<std::size_t>(characters));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_buffer.c_str(), bytes,
                               wide.data(), characters) == characters;
}

void append_quoted_argument(std::wstring &command, const std::wstring_view argument) {
    if (!command.empty()) {
        command.push_back(L' ');
    }
    const auto requires_quotes =
        argument.empty() || argument.find_first_of(L" \t\"") != std::wstring_view::npos;
    if (!requires_quotes) {
        command.append(argument);
        return;
    }
    command.push_back(L'\"');
    std::size_t backslashes{};
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            command.append(backslashes * 2U + 1U, L'\\');
            command.push_back(character);
            backslashes = 0;
            continue;
        }
        command.append(backslashes, L'\\');
        backslashes = 0;
        command.push_back(character);
    }
    command.append(backslashes * 2U, L'\\');
    command.push_back(L'\"');
}

[[nodiscard]] bool external_command_line(const ExternalRendererRequest &request,
                                         std::vector<wchar_t> &command, std::string &detail) {
    std::wstring assembled;
    append_quoted_argument(assembled, request.executable.native());
    for (const auto &argument_utf8 : request.arguments_utf8) {
        std::wstring argument;
        if (!utf8_to_wide(argument_utf8, argument)) {
            detail = "external renderer argument is not valid UTF-8";
            return false;
        }
        append_quoted_argument(assembled, argument);
    }
    command.assign(assembled.begin(), assembled.end());
    command.push_back(L'\0');
    return true;
}

[[nodiscard]] bool duplicate_reduced_handle_into_process(const HANDLE source,
                                                         const HANDLE target_process,
                                                         const DWORD access, HANDLE &remote_handle,
                                                         std::string &detail) {
    remote_handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), source, target_process, &remote_handle, access, FALSE,
                        0) == FALSE) {
        detail = windows_error("DuplicateHandle(worker authority)");
        return false;
    }
    return true;
}

struct PipePair {
    UniqueHandle read;
    UniqueHandle write;
};

[[nodiscard]] bool create_pipe(PipePair &pipe, const bool child_reads, std::string &detail) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read{};
    HANDLE write{};
    if (CreatePipe(&read, &write, &attributes, 0) == FALSE) {
        detail = windows_error("CreatePipe");
        return false;
    }
    pipe.read.reset(read);
    pipe.write.reset(write);
    const auto parent = child_reads ? pipe.write.get() : pipe.read.get();
    if (SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        detail = windows_error("SetHandleInformation(parent pipe)");
        return false;
    }
    return true;
}

struct RestrictedIdentity {
    UniqueHandle token;
    bool low_integrity{};
    std::string detail;
};

struct AppContainerIdentity {
    SidOwner sid;
    std::wstring folder;
    std::string detail;
};

[[nodiscard]] bool load_app_container_folder(AppContainerIdentity &identity) {
    LPWSTR sid_text{};
    if (ConvertSidToStringSidW(identity.sid.get(), &sid_text) == FALSE) {
        identity.detail = windows_error("ConvertSidToStringSid(AppContainer)");
        return false;
    }
    PWSTR folder{};
    const auto result = GetAppContainerFolderPath(sid_text, &folder);
    static_cast<void>(LocalFree(sid_text));
    if (FAILED(result) || folder == nullptr) {
        identity.detail = "GetAppContainerFolderPath failed with HRESULT " +
                          std::to_string(static_cast<unsigned long>(result));
        return false;
    }
    identity.folder = folder;
    CoTaskMemFree(folder);
    return true;
}

[[nodiscard]] AppContainerIdentity create_app_container_identity() {
    constexpr auto profile_name = kWorkerProfileName;
    AppContainerIdentity identity;
    PSID sid{};
    const auto created =
        CreateAppContainerProfile(profile_name, L"VO-VE Worker",
                                  L"Capability-free thumbnail decoder sandbox", nullptr, 0, &sid);
    if (SUCCEEDED(created)) {
        identity.sid.reset(sid);
        identity.detail = "dedicated capability-free AppContainer profile created";
        if (!load_app_container_folder(identity)) {
            identity.sid.reset();
        }
        return identity;
    }
    if (created != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        identity.detail = "CreateAppContainerProfile failed with HRESULT " +
                          std::to_string(static_cast<unsigned long>(created));
        return identity;
    }
    const auto derived = DeriveAppContainerSidFromAppContainerName(profile_name, &sid);
    if (FAILED(derived)) {
        identity.detail = "DeriveAppContainerSidFromAppContainerName failed with HRESULT " +
                          std::to_string(static_cast<unsigned long>(derived));
        return identity;
    }
    identity.sid.reset(sid);
    identity.detail = "dedicated capability-free AppContainer profile reused";
    if (!load_app_container_folder(identity)) {
        identity.sid.reset();
    }
    return identity;
}

[[nodiscard]] std::vector<wchar_t>
clean_environment_block(const std::wstring_view app_container_folder) {
    std::array<wchar_t, MAX_PATH + 1> windows_directory{};
    const auto length =
        GetWindowsDirectoryW(windows_directory.data(), static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return {L'\0', L'\0'};
    }
    const std::wstring root{windows_directory.data(), static_cast<std::size_t>(length)};
    const std::wstring local_app_data = L"LOCALAPPDATA=" + std::wstring{app_container_folder};
    const std::wstring system_root = L"SystemRoot=" + root;
    const std::wstring temp = L"TEMP=" + std::wstring{app_container_folder} + L"\\Temp";
    const std::wstring tmp = L"TMP=" + std::wstring{app_container_folder} + L"\\Temp";
    std::wstring sandbox_temp;
    if (app_container_folder.size() >= 3 && app_container_folder[1] == L':') {
        const auto drive = std::wstring{app_container_folder.substr(0, 2)};
        std::array<wchar_t, 32'768> device{};
        const auto device_length =
            QueryDosDeviceW(drive.c_str(), device.data(), static_cast<DWORD>(device.size()));
        if (device_length != 0) {
            sandbox_temp = L"VOVE_SANDBOX_TEMP=\\\\?\\GLOBALROOT" + std::wstring{device.data()} +
                           std::wstring{app_container_folder.substr(2)} + L"\\Temp";
        }
    }
    std::vector<wchar_t> environment;
    for (const auto &entry : {local_app_data, system_root, temp, tmp}) {
        environment.insert(environment.end(), entry.begin(), entry.end());
        environment.push_back(L'\0');
    }
    if (!sandbox_temp.empty()) {
        environment.insert(environment.end(), sandbox_temp.begin(), sandbox_temp.end());
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    return environment;
}

[[nodiscard]] bool child_is_app_container(const HANDLE process, std::string &detail) {
    UniqueHandle token;
    HANDLE raw_token{};
    if (OpenProcessToken(process, TOKEN_QUERY, &raw_token) == FALSE) {
        detail = windows_error("OpenProcessToken(child)");
        return false;
    }
    token.reset(raw_token);
    DWORD is_app_container{};
    DWORD returned{};
    if (GetTokenInformation(token.get(), TokenIsAppContainer, &is_app_container,
                            sizeof(is_app_container), &returned) == FALSE) {
        detail = windows_error("GetTokenInformation(TokenIsAppContainer)");
        return false;
    }
    if (returned != sizeof(is_app_container) || is_app_container == 0) {
        detail = "child token is not an AppContainer token";
        return false;
    }

    DWORD integrity_bytes{};
    static_cast<void>(
        GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &integrity_bytes));
    if (integrity_bytes < sizeof(TOKEN_MANDATORY_LABEL)) {
        detail = "AppContainer token integrity level is unavailable";
        return false;
    }
    std::vector<std::byte> integrity_storage(integrity_bytes);
    if (GetTokenInformation(token.get(), TokenIntegrityLevel, integrity_storage.data(),
                            integrity_bytes, &integrity_bytes) == FALSE) {
        detail = windows_error("GetTokenInformation(TokenIntegrityLevel)");
        return false;
    }
    const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(integrity_storage.data());
    const auto subauthority_count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (subauthority_count == 0 || *GetSidSubAuthority(label->Label.Sid, subauthority_count - 1U) >
                                       SECURITY_MANDATORY_LOW_RID) {
        detail = "AppContainer token is not low integrity";
        return false;
    }

    DWORD capability_bytes{};
    static_cast<void>(
        GetTokenInformation(token.get(), TokenCapabilities, nullptr, 0, &capability_bytes));
    if (capability_bytes < sizeof(DWORD)) {
        detail = "AppContainer token capability list is unavailable";
        return false;
    }
    std::vector<std::byte> capability_storage(capability_bytes);
    if (GetTokenInformation(token.get(), TokenCapabilities, capability_storage.data(),
                            capability_bytes, &capability_bytes) == FALSE) {
        detail = windows_error("GetTokenInformation(TokenCapabilities)");
        return false;
    }
    const auto *capability_groups =
        reinterpret_cast<const TOKEN_GROUPS *>(capability_storage.data());
    const auto has_capability = [capability_groups](const WELL_KNOWN_SID_TYPE type) {
        std::array<DWORD, SECURITY_MAX_SID_SIZE / sizeof(DWORD)> sid{};
        DWORD sid_bytes = static_cast<DWORD>(sid.size() * sizeof(DWORD));
        if (CreateWellKnownSid(type, nullptr, sid.data(), &sid_bytes) == FALSE) {
            return true;
        }
        for (DWORD index = 0; index < capability_groups->GroupCount; ++index) {
            if (EqualSid(capability_groups->Groups[index].Sid, sid.data()) != FALSE) {
                return true;
            }
        }
        return false;
    };
    if (has_capability(WinCapabilityInternetClientSid) ||
        has_capability(WinCapabilityInternetClientServerSid) ||
        has_capability(WinCapabilityPrivateNetworkClientServerSid)) {
        detail = "AppContainer token unexpectedly contains a network capability";
        return false;
    }
    return true;
}

[[nodiscard]] RestrictedIdentity create_restricted_identity() {
    RestrictedIdentity identity;
    UniqueHandle process_token;
    HANDLE raw_process_token{};
    constexpr DWORD token_access = TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY |
                                   TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
    if (OpenProcessToken(GetCurrentProcess(), token_access, &raw_process_token) == FALSE) {
        identity.detail = windows_error("OpenProcessToken");
        return identity;
    }
    process_token.reset(raw_process_token);

    HANDLE raw_restricted_token{};
    constexpr DWORD restriction_flags = DISABLE_MAX_PRIVILEGE | LUA_TOKEN;
    if (CreateRestrictedToken(process_token.get(), restriction_flags, 0, nullptr, 0, nullptr, 0,
                              nullptr, &raw_restricted_token) == FALSE) {
        identity.detail = windows_error("CreateRestrictedToken");
        return identity;
    }
    identity.token.reset(raw_restricted_token);

    SidOwner low_sid;
    SID_IDENTIFIER_AUTHORITY mandatory_authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    if (AllocateAndInitializeSid(&mandatory_authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0,
                                 0, 0, low_sid.out()) == FALSE) {
        identity.detail = windows_error("AllocateAndInitializeSid(low integrity)");
        identity.token.reset();
        return identity;
    }
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = low_sid.get();
    const auto label_bytes = static_cast<DWORD>(sizeof(label) + GetLengthSid(low_sid.get()));
    if (SetTokenInformation(identity.token.get(), TokenIntegrityLevel, &label, label_bytes) ==
        FALSE) {
        identity.detail = windows_error("SetTokenInformation(low integrity)");
        identity.token.reset();
        return identity;
    }
    identity.low_integrity = true;
    identity.detail = "restricted primary token with disabled privileges and low integrity";
    return identity;
}

[[nodiscard]] bool configure_job(const JobLimits &worker_limits,
                                 const std::chrono::milliseconds effective_timeout,
                                 UniqueHandle &job, SandboxReport &report, std::string &detail) {
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        detail = windows_error("CreateJobObject");
        return false;
    }
    if (worker_limits.memory_limit_bytes >
        static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max())) {
        detail = "worker memory limit cannot be represented by this process";
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
        std::max<LONGLONG>(1, effective_timeout.count()) * 10'000;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(worker_limits.memory_limit_bytes);
    if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits)) == FALSE) {
        detail = windows_error("SetInformationJobObject(limits)");
        return false;
    }
    report.memory_limited = true;
    report.cpu_limited = true;

    return true;
}

[[nodiscard]] bool configure_job_ui(const HANDLE job, std::string &detail) {
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                             JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                             JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) == FALSE) {
        detail = windows_error("SetInformationJobObject(UI restrictions)");
        return false;
    }
    return true;
}

enum class TimedIoStatus : std::uint8_t { success, timed_out, child_exited, transport_error };

struct WorkerTransport {
    HANDLE response_pipe{};
    HANDLE process{};
};

[[nodiscard]] TimedIoStatus read_exact_until(const WorkerTransport &transport,
                                             const std::span<std::byte> output,
                                             const std::chrono::steady_clock::time_point deadline,
                                             std::string &detail) {
    std::size_t offset{};
    while (offset < output.size()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            detail = "worker response exceeded the wall-time limit";
            return TimedIoStatus::timed_out;
        }
        DWORD available{};
        if (PeekNamedPipe(transport.response_pipe, nullptr, 0, nullptr, &available, nullptr) ==
            FALSE) {
            const auto code = GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
                detail = "worker closed its response pipe";
                return TimedIoStatus::child_exited;
            }
            detail = windows_error("PeekNamedPipe", code);
            return TimedIoStatus::transport_error;
        }
        if (available == 0) {
            if (WaitForSingleObject(transport.process, 0) == WAIT_OBJECT_0) {
                detail = "worker exited before completing its response";
                return TimedIoStatus::child_exited;
            }
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }
        const auto remaining = output.size() - offset;
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(available)));
        DWORD received{};
        if (ReadFile(transport.response_pipe, output.data() + offset, chunk, &received, nullptr) ==
                FALSE ||
            received == 0) {
            detail = windows_error("ReadFile(worker response)");
            return TimedIoStatus::transport_error;
        }
        offset += static_cast<std::size_t>(received);
    }
    return TimedIoStatus::success;
}

[[nodiscard]] std::uint32_t decode_u32_le(const std::span<const std::byte> bytes,
                                          const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

struct TimedFrame {
    TimedIoStatus status{TimedIoStatus::transport_error};
    std::vector<std::byte> bytes;
    std::string detail;
};

[[nodiscard]] TimedFrame read_frame_until(const WorkerTransport &transport,
                                          const std::chrono::steady_clock::time_point deadline) {
    TimedFrame frame;
    frame.bytes.resize(kProtocolHeaderBytes);
    frame.status = read_exact_until(transport, frame.bytes, deadline, frame.detail);
    if (frame.status != TimedIoStatus::success) {
        frame.bytes.clear();
        return frame;
    }
    constexpr std::size_t frame_size_offset = 12;
    const auto frame_bytes = decode_u32_le(frame.bytes, frame_size_offset);
    if (frame_bytes < kProtocolHeaderBytes || frame_bytes > kMaximumFrameBytes) {
        frame.status = TimedIoStatus::transport_error;
        frame.detail = "worker declared an invalid response frame size";
        frame.bytes.clear();
        return frame;
    }
    const auto header_bytes = frame.bytes.size();
    frame.bytes.resize(frame_bytes);
    frame.status = read_exact_until(
        transport, std::span<std::byte>{frame.bytes}.subspan(header_bytes), deadline, frame.detail);
    if (frame.status != TimedIoStatus::success) {
        frame.bytes.clear();
    }
    return frame;
}

[[nodiscard]] bool write_all(const HANDLE pipe, const std::span<const std::byte> bytes,
                             std::string &detail) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written{};
        if (WriteFile(pipe, bytes.data() + offset, chunk, &written, nullptr) == FALSE ||
            written == 0) {
            detail = windows_error("WriteFile(worker command)");
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] int process_exit_code(const HANDLE process) noexcept {
    DWORD code{};
    return GetExitCodeProcess(process, &code) != FALSE ? static_cast<int>(code) : -1;
}

void discard_rejected_output(const HANDLE output) noexcept {
    LARGE_INTEGER beginning{};
    if (SetFilePointerEx(output, beginning, nullptr, FILE_BEGIN) != FALSE) {
        static_cast<void>(SetEndOfFile(output));
    }
}

struct ControlledProcess {
    HANDLE job{};
    HANDLE process{};
};

[[nodiscard]] SupervisorResult timed_frame_failure(const TimedFrame &frame,
                                                   const ControlledProcess &controlled,
                                                   const SandboxReport &sandbox) {
    // Pipe closure can precede process signalling; retain the loader's own exit code.
    const auto exited = WaitForSingleObject(controlled.process,
        frame.status == TimedIoStatus::child_exited ? 100U : 0U) == WAIT_OBJECT_0;
    if (!exited && valid_handle(controlled.job)) {
        static_cast<void>(TerminateJobObject(
            controlled.job, frame.status == TimedIoStatus::timed_out ? kSupervisorTimeoutExitCode
                                                                     : kSupervisorFailureExitCode));
    }
    static_cast<void>(WaitForSingleObject(controlled.process, 1'000));
    const auto exit = process_exit_code(controlled.process);
    if (frame.status == TimedIoStatus::timed_out) {
        return failure(SupervisorError::timed_out, frame.detail, sandbox, exit);
    }
    if (frame.status == TimedIoStatus::child_exited) {
        return failure(SupervisorError::worker_crashed, frame.detail, sandbox, exit);
    }
    return failure(SupervisorError::transport_error, frame.detail, sandbox, exit);
}

} // namespace

SupervisorResult run_worker_once(const SupervisorRequest &request) {
    std::string detail;
    if (!request_is_valid(request, detail)) {
        return failure(SupervisorError::invalid_request, std::move(detail));
    }
    const auto effective_timeout =
        std::min(request.timeout, std::chrono::milliseconds{request.job.limits.wall_timeout_ms});
    const auto deadline = std::chrono::steady_clock::now() + effective_timeout;

    SandboxReport sandbox;
    sandbox.network_isolated = false;
    sandbox.filesystem_isolated = false;
    sandbox.no_new_privileges = false;

    PipePair commands;
    PipePair responses;
    if (!create_pipe(commands, true, detail) || !create_pipe(responses, false, detail)) {
        return failure(SupervisorError::launch_failed, std::move(detail), sandbox);
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    UniqueHandle null_error(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                        nullptr));
    if (!null_error) {
        return failure(SupervisorError::launch_failed, windows_error("CreateFile(NUL)"), sandbox);
    }

    UniqueHandle job;
    if (!configure_job(request.job.limits, effective_timeout, job, sandbox, detail)) {
        return failure(SupervisorError::sandbox_unavailable, std::move(detail), sandbox);
    }

    auto app_container = create_app_container_identity();
    append_detail(sandbox.detail, app_container.detail);
    const auto use_app_container = app_container.sid.get() != nullptr;
    if (!use_app_container && request.require_minimum_sandbox) {
        sandbox.level = SandboxLevel::degraded;
        return failure(SupervisorError::sandbox_unavailable,
                       "minimum Windows sandbox requires a capability-free AppContainer", sandbox);
    }
    auto identity = use_app_container ? RestrictedIdentity{} : create_restricted_identity();
    sandbox.identity_restricted = static_cast<bool>(identity.token);
    if (use_app_container) {
        append_detail(sandbox.detail,
                      "AppContainer launch supplies the restricted low-integrity child token");
    } else {
        append_detail(sandbox.detail, identity.detail);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = commands.read.get();
    startup.StartupInfo.hStdOutput = responses.write.get();
    startup.StartupInfo.hStdError = null_error.get();

    const DWORD attribute_count = use_app_container ? 3U : 2U;
    SIZE_T attribute_bytes{};
    static_cast<void>(
        InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &attribute_bytes));
    if (attribute_bytes == 0) {
        return failure(SupervisorError::sandbox_unavailable,
                       windows_error("InitializeProcThreadAttributeList(size)"), sandbox);
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    startup.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (InitializeProcThreadAttributeList(startup.lpAttributeList, attribute_count, 0,
                                          &attribute_bytes) == FALSE) {
        return failure(SupervisorError::sandbox_unavailable,
                       windows_error("InitializeProcThreadAttributeList"), sandbox);
    }

    const std::array inherited_handles{commands.read.get(), responses.write.get(),
                                       null_error.get()};
    const auto attribute_ok = UpdateProcThreadAttribute(
        startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        reinterpret_cast<void *>(const_cast<HANDLE *>(inherited_handles.data())),
        sizeof(inherited_handles), nullptr, nullptr);
    if (attribute_ok == FALSE) {
        const auto error = windows_error("UpdateProcThreadAttribute(handle list)");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return failure(SupervisorError::sandbox_unavailable, error, sandbox);
    }
    const std::array job_list{job.get()};
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                  reinterpret_cast<void *>(const_cast<HANDLE *>(job_list.data())),
                                  sizeof(job_list), nullptr, nullptr) == FALSE) {
        const auto error = windows_error("UpdateProcThreadAttribute(job list)");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return failure(SupervisorError::sandbox_unavailable, error, sandbox);
    }

    SECURITY_CAPABILITIES security_capabilities{};
    if (use_app_container) {
        security_capabilities.AppContainerSid = app_container.sid.get();
        security_capabilities.Capabilities = nullptr;
        security_capabilities.CapabilityCount = 0;
        security_capabilities.Reserved = 0;
        if (UpdateProcThreadAttribute(
                startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                &security_capabilities, sizeof(security_capabilities), nullptr, nullptr) == FALSE) {
            const auto error =
                windows_error("UpdateProcThreadAttribute(AppContainer security capabilities)");
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            return failure(SupervisorError::sandbox_unavailable, error, sandbox);
        }
    }

    auto command = L"\"" + request.worker_executable.native() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    PROCESS_INFORMATION process{};
    constexpr DWORD creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    auto environment = clean_environment_block(app_container.folder);
    const auto worker_directory = request.worker_executable.parent_path();
    const auto *current_directory = worker_directory.empty() ? nullptr : worker_directory.c_str();
    BOOL created{};
    // SECURITY_CAPABILITIES asks Windows to construct the AppContainer token. Composing that
    // attribute with a separately restricted CreateProcessAsUser token can terminate during
    // process initialization on Windows 10, before the worker reaches its entry point.
    if (use_app_container) {
        created = CreateProcessW(request.worker_executable.c_str(), mutable_command.data(), nullptr,
                                 nullptr, TRUE, creation_flags, environment.data(),
                                 current_directory, &startup.StartupInfo, &process);
    } else if (identity.token) {
        created = CreateProcessAsUserW(identity.token.get(), request.worker_executable.c_str(),
                                       mutable_command.data(), nullptr, nullptr, TRUE,
                                       creation_flags, environment.data(), current_directory,
                                       &startup.StartupInfo, &process);
    } else {
        created = CreateProcessW(request.worker_executable.c_str(), mutable_command.data(), nullptr,
                                 nullptr, TRUE, creation_flags, environment.data(),
                                 current_directory, &startup.StartupInfo, &process);
    }
    auto create_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    if (created == FALSE && !use_app_container && identity.token &&
        !request.require_minimum_sandbox) {
        sandbox.identity_restricted = false;
        sandbox.level = SandboxLevel::degraded;
        append_detail(sandbox.detail,
                      "restricted launch unavailable; explicit degraded launch requested");
        mutable_command.assign(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        created = CreateProcessW(request.worker_executable.c_str(), mutable_command.data(), nullptr,
                                 nullptr, TRUE, creation_flags, environment.data(),
                                 current_directory, &startup.StartupInfo, &process);
        create_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    }
    if (created != FALSE) {
        pause_after_child_creation_for_test();
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (created == FALSE) {
        const auto error = windows_error(
            use_app_container ? "CreateProcess(AppContainer)"
                              : (identity.token ? "CreateProcessAsUser" : "CreateProcess"),
            create_error);
        return failure(SupervisorError::launch_failed, error, sandbox, 0, create_error);
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);

    if (use_app_container && !child_is_app_container(process_handle.get(), detail)) {
        static_cast<void>(TerminateProcess(process_handle.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::sandbox_unavailable,
                       "created process is not an AppContainer: " + detail, sandbox,
                       process_exit_code(process_handle.get()));
    }
    if (use_app_container) {
        sandbox.identity_restricted = true;
    }
    sandbox.network_isolated = use_app_container;
    sandbox.filesystem_isolated = use_app_container;

    commands.read.reset();
    responses.write.reset();
    null_error.reset();

    // PROC_THREAD_ATTRIBUTE_JOB_LIST contains the child from the instant CreateProcess succeeds.
    // This closes the shutdown gap in which a suspended, not-yet-assigned worker could outlive its
    // supervisor.
    sandbox.process_contained = true;
    sandbox.level = sandbox.identity_restricted && sandbox.memory_limited && sandbox.cpu_limited &&
                            sandbox.network_isolated && sandbox.filesystem_isolated
                        ? SandboxLevel::strict
                        : SandboxLevel::degraded;
    append_detail(sandbox.detail, "Job Object limits process count, memory, CPU time and lifetime");
    append_detail(sandbox.detail,
                  use_app_container
                      ? "AppContainer with zero capabilities; network and arbitrary "
                        "filesystem access denied; explicit inherited-handle allow-list"
                      : "explicit degraded handle allow-list without AppContainer isolation");
    if (request.require_minimum_sandbox && sandbox.level < SandboxLevel::minimum) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::sandbox_unavailable,
                       "minimum Windows sandbox controls are incomplete", sandbox,
                       process_exit_code(process_handle.get()));
    }
    if (!configure_job_ui(job.get(), detail)) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::sandbox_unavailable, std::move(detail), sandbox,
                       process_exit_code(process_handle.get()));
    }
    append_detail(sandbox.detail, "Job Object UI restrictions active before worker start");
    if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        const auto code = GetLastError();
        const auto error = windows_error("ResumeThread", code);
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::launch_failed, error, sandbox,
                       process_exit_code(process_handle.get()), code);
    }

    CapabilitySet required_capabilities = capability_bit(Capability::read_only_source_token) |
                                          capability_bit(Capability::write_only_output_token);
    if (request.profile_object != kInvalidNativeObject) {
        required_capabilities |= capability_bit(Capability::read_only_profile_token);
    }
    if (sandbox.level >= SandboxLevel::minimum) {
        required_capabilities |= capability_bit(Capability::sandbox_active);
    }

    std::vector<std::byte> handshake;
    try {
        handshake = encode_handshake(
            {.build_id = request.expected_build_id, .capabilities = required_capabilities});
    } catch (const std::exception &exception) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::invalid_request, exception.what(), sandbox,
                       process_exit_code(process_handle.get()));
    }
    if (!write_all(commands.write.get(), handshake, detail)) {
        const auto code = GetLastError();
        if (WaitForSingleObject(process_handle.get(), 100) != WAIT_OBJECT_0)
            static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::transport_error, std::move(detail), sandbox,
                       process_exit_code(process_handle.get()), code);
    }
    const WorkerTransport transport{.response_pipe = responses.read.get(),
                                    .process = process_handle.get()};
    const ControlledProcess controlled{.job = job.get(), .process = process_handle.get()};
    auto frame = read_frame_until(transport, deadline);
    if (frame.status != TimedIoStatus::success) {
        frame.detail = "handshake: " + frame.detail;
        return timed_frame_failure(frame, controlled, sandbox);
    }
    Handshake acknowledgement;
    DecodeError decode_error;
    if (!decode_handshake(frame.bytes, acknowledgement, decode_error,
                          MessageKind::handshake_acknowledgement)) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::handshake_failed,
                       "invalid worker handshake: " + decode_error.message, sandbox,
                       process_exit_code(process_handle.get()));
    }
    if (acknowledgement.build_id != request.expected_build_id ||
        (acknowledgement.capabilities & required_capabilities) != required_capabilities) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return failure(SupervisorError::incompatible_worker,
                       "worker build or mandatory capabilities do not match: " +
                           acknowledgement.build_id,
                       sandbox, process_exit_code(process_handle.get()));
    }

    const auto after_handshake = [](SupervisorResult result) {
        result.handshake_completed = true;
        return result;
    };
    HANDLE child_source{};
    HANDLE child_output{};
    HANDLE child_profile{};
    if (!duplicate_reduced_handle_into_process(as_handle(request.source_object),
                                               process_handle.get(), GENERIC_READ, child_source,
                                               detail) ||
        !duplicate_reduced_handle_into_process(as_handle(request.output_object),
                                               process_handle.get(), GENERIC_WRITE, child_output,
                                               detail) ||
        (request.profile_object != kInvalidNativeObject &&
         !duplicate_reduced_handle_into_process(as_handle(request.profile_object),
                                                process_handle.get(), GENERIC_READ, child_profile,
                                                detail))) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(failure(SupervisorError::sandbox_unavailable, std::move(detail),
                                       sandbox, process_exit_code(process_handle.get())));
    }

    std::vector<std::byte> job_frame;
    try {
        auto child_job = request.job;
        child_job.source_token = as_token(child_source);
        child_job.output_token = as_token(child_output);
        child_job.profile_token = child_profile == nullptr ? 0 : as_token(child_profile);
        child_job.limits.wall_timeout_ms = static_cast<std::uint32_t>(effective_timeout.count());
        job_frame = encode_worker_job(child_job);
    } catch (const std::exception &exception) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(failure(SupervisorError::invalid_request, exception.what(), sandbox,
                                       process_exit_code(process_handle.get())));
    }

    if (!write_all(commands.write.get(), job_frame, detail)) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(failure(SupervisorError::transport_error, std::move(detail), sandbox,
                                       process_exit_code(process_handle.get())));
    }
    commands.write.reset();

    frame = read_frame_until(transport, deadline);
    if (frame.status != TimedIoStatus::success) {
        frame.detail = "job result: " + frame.detail;
        return after_handshake(timed_frame_failure(frame, controlled, sandbox));
    }
    WorkerResult worker_result;
    if (!decode_worker_result(frame.bytes, worker_result, decode_error)) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(failure(SupervisorError::transport_error,
                                       "invalid worker result: " + decode_error.message, sandbox,
                                       process_exit_code(process_handle.get())));
    }
    if (worker_result.job_id != request.job.job_id) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(failure(SupervisorError::transport_error,
                                       "worker returned a different job id", sandbox,
                                       process_exit_code(process_handle.get())));
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorTimeoutExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(
            failure(SupervisorError::timed_out,
                    "worker process exceeded the wall-time limit after its result", sandbox,
                    process_exit_code(process_handle.get())));
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto wait_ms = static_cast<DWORD>(std::max<std::int64_t>(1, remaining.count()));
    if (WaitForSingleObject(process_handle.get(), wait_ms) != WAIT_OBJECT_0) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorTimeoutExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return after_handshake(failure(SupervisorError::timed_out,
                                       "worker did not exit before the wall-time limit", sandbox,
                                       process_exit_code(process_handle.get())));
    }
    const auto exit = process_exit_code(process_handle.get());
    if (exit != 0) {
        return after_handshake(failure(SupervisorError::worker_crashed,
                                       "worker exited unsuccessfully after returning a result",
                                       sandbox, exit));
    }

    std::uint64_t actual_output_bytes{};
    if (!output_size(as_handle(request.output_object), actual_output_bytes, detail)) {
        discard_rejected_output(as_handle(request.output_object));
        return after_handshake(
            failure(SupervisorError::transport_error, std::move(detail), sandbox, exit));
    }
    const auto maximum_output_bytes = request.job.limits.maximum_output_bytes;
    if (worker_result.bytes_written > maximum_output_bytes ||
        actual_output_bytes > maximum_output_bytes ||
        actual_output_bytes != worker_result.bytes_written) {
        const auto mismatch =
            "worker output boundary violated: actual=" + std::to_string(actual_output_bytes) +
            ", reported=" + std::to_string(worker_result.bytes_written) +
            ", maximum=" + std::to_string(maximum_output_bytes);
        discard_rejected_output(as_handle(request.output_object));
        return after_handshake(failure(SupervisorError::transport_error, mismatch, sandbox, exit));
    }

    return after_handshake({.error = SupervisorError::none,
                            .detail = {},
                            .worker_result = std::move(worker_result),
                            .sandbox = std::move(sandbox),
                            .exit_code = exit});
}

ExternalRendererResult run_external_renderer_once(const ExternalRendererRequest &request) {
    std::string detail;
    SupervisorError validation_error{};
    if (!external_request_is_valid(request, detail, validation_error)) {
        return external_failure(validation_error, std::move(detail));
    }
    const auto effective_timeout =
        std::min(request.timeout, std::chrono::milliseconds{request.limits.wall_timeout_ms});

    LARGE_INTEGER beginning{};
    if (SetFilePointerEx(as_handle(request.source_object), beginning, nullptr, FILE_BEGIN) ==
            FALSE ||
        SetFilePointerEx(as_handle(request.output_object), beginning, nullptr, FILE_BEGIN) ==
            FALSE ||
        SetEndOfFile(as_handle(request.output_object)) == FALSE) {
        return external_failure(SupervisorError::invalid_request,
                                windows_error("rewinding external renderer files"));
    }

    UniqueHandle child_source;
    if (!duplicate_inheritable_handle(as_handle(request.source_object), GENERIC_READ, child_source,
                                      detail)) {
        return external_failure(SupervisorError::invalid_request, std::move(detail));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE raw_output_read{};
    HANDLE raw_output_write{};
    if (CreatePipe(&raw_output_read, &raw_output_write, &inheritable, 0) == FALSE) {
        return external_failure(SupervisorError::launch_failed,
                                windows_error("CreatePipe(external renderer output)"));
    }
    UniqueHandle parent_output_read(raw_output_read);
    UniqueHandle child_output_write(raw_output_write);
    if (SetHandleInformation(parent_output_read.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
        return external_failure(SupervisorError::launch_failed,
                                windows_error("SetHandleInformation(renderer output read end)"));
    }
    UniqueHandle null_error(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                        nullptr));
    if (!null_error) {
        return external_failure(SupervisorError::launch_failed, windows_error("CreateFile(NUL)"));
    }

    SandboxReport sandbox;
    UniqueHandle job;
    if (!configure_job(request.limits, effective_timeout, job, sandbox, detail)) {
        return external_failure(SupervisorError::sandbox_unavailable, std::move(detail), sandbox);
    }

    auto app_container = create_app_container_identity();
    append_detail(sandbox.detail, app_container.detail);
    const auto use_app_container = app_container.sid.get() != nullptr;
    if (!use_app_container && request.require_minimum_sandbox) {
        sandbox.level = SandboxLevel::degraded;
        return external_failure(SupervisorError::sandbox_unavailable,
                                "minimum Windows controls require a capability-free AppContainer",
                                sandbox);
    }
    auto identity = use_app_container ? RestrictedIdentity{} : create_restricted_identity();
    sandbox.identity_restricted = static_cast<bool>(identity.token);
    if (use_app_container) {
        append_detail(sandbox.detail,
                      "AppContainer launch supplies the restricted low-integrity child token");
    } else {
        append_detail(sandbox.detail, identity.detail);
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_source.get();
    startup.StartupInfo.hStdOutput = child_output_write.get();
    startup.StartupInfo.hStdError = null_error.get();

    const DWORD attribute_count = use_app_container ? 3U : 2U;
    SIZE_T attribute_bytes{};
    static_cast<void>(
        InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &attribute_bytes));
    if (attribute_bytes == 0) {
        return external_failure(SupervisorError::sandbox_unavailable,
                                windows_error("InitializeProcThreadAttributeList(size)"), sandbox);
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    startup.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (InitializeProcThreadAttributeList(startup.lpAttributeList, attribute_count, 0,
                                          &attribute_bytes) == FALSE) {
        return external_failure(SupervisorError::sandbox_unavailable,
                                windows_error("InitializeProcThreadAttributeList"), sandbox);
    }

    const std::array inherited_handles{child_source.get(), child_output_write.get(),
                                       null_error.get()};
    if (UpdateProcThreadAttribute(
            startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            reinterpret_cast<void *>(const_cast<HANDLE *>(inherited_handles.data())),
            sizeof(inherited_handles), nullptr, nullptr) == FALSE) {
        const auto error = windows_error("UpdateProcThreadAttribute(handle list)");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return external_failure(SupervisorError::sandbox_unavailable, error, sandbox);
    }
    const std::array job_list{job.get()};
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                  reinterpret_cast<void *>(const_cast<HANDLE *>(job_list.data())),
                                  sizeof(job_list), nullptr, nullptr) == FALSE) {
        const auto error = windows_error("UpdateProcThreadAttribute(job list)");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return external_failure(SupervisorError::sandbox_unavailable, error, sandbox);
    }

    SECURITY_CAPABILITIES security_capabilities{};
    if (use_app_container) {
        security_capabilities.AppContainerSid = app_container.sid.get();
        if (UpdateProcThreadAttribute(
                startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                &security_capabilities, sizeof(security_capabilities), nullptr, nullptr) == FALSE) {
            const auto error = windows_error("UpdateProcThreadAttribute(AppContainer)");
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            return external_failure(SupervisorError::sandbox_unavailable, error, sandbox);
        }
    }

    std::vector<wchar_t> command;
    if (!external_command_line(request, command, detail)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return external_failure(SupervisorError::invalid_request, std::move(detail), sandbox);
    }
    PROCESS_INFORMATION process{};
    constexpr DWORD creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    auto environment = clean_environment_block(app_container.folder);
    BOOL created{};
    if (use_app_container) {
        created = CreateProcessW(request.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                 creation_flags, environment.data(), nullptr, &startup.StartupInfo,
                                 &process);
    } else if (identity.token) {
        created = CreateProcessAsUserW(identity.token.get(), request.executable.c_str(),
                                       command.data(), nullptr, nullptr, TRUE, creation_flags,
                                       environment.data(), nullptr, &startup.StartupInfo, &process);
    } else {
        created = CreateProcessW(request.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                 creation_flags, environment.data(), nullptr, &startup.StartupInfo,
                                 &process);
    }
    const auto create_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    if (created != FALSE) {
        pause_after_child_creation_for_test();
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (created == FALSE) {
        return external_failure(
            identity.token && !use_app_container ? SupervisorError::sandbox_unavailable
                                                 : SupervisorError::launch_failed,
            windows_error(use_app_container
                              ? "CreateProcess(AppContainer external renderer)"
                              : (identity.token ? "CreateProcessAsUser(external renderer)"
                                                : "CreateProcess(external renderer)"),
                          create_error),
            sandbox);
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    child_source.reset();
    child_output_write.reset();
    null_error.reset();

    if (use_app_container && !child_is_app_container(process_handle.get(), detail)) {
        static_cast<void>(TerminateProcess(process_handle.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return external_failure(SupervisorError::sandbox_unavailable,
                                "created renderer is not an AppContainer: " + detail, sandbox,
                                process_exit_code(process_handle.get()));
    }
    if (use_app_container) {
        sandbox.identity_restricted = true;
    }
    sandbox.network_isolated = use_app_container;
    sandbox.filesystem_isolated = use_app_container;
    sandbox.process_contained = true;
    sandbox.level = sandbox.identity_restricted && sandbox.memory_limited && sandbox.cpu_limited &&
                            sandbox.network_isolated && sandbox.filesystem_isolated
                        ? SandboxLevel::strict
                        : SandboxLevel::degraded;
    append_detail(sandbox.detail,
                  "Job Object limits one renderer process, memory, CPU time and lifetime");
    append_detail(sandbox.detail,
                  use_app_container
                      ? "AppContainer with zero capabilities and inherited standard handles"
                      : "explicit degraded standard-handle allow-list");
    if (request.require_minimum_sandbox && sandbox.level < SandboxLevel::minimum) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return external_failure(SupervisorError::sandbox_unavailable,
                                "minimum Windows renderer controls are incomplete", sandbox,
                                process_exit_code(process_handle.get()));
    }
    if (!configure_job_ui(job.get(), detail)) {
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return external_failure(SupervisorError::sandbox_unavailable, std::move(detail), sandbox,
                                process_exit_code(process_handle.get()));
    }
    append_detail(sandbox.detail, "Job Object UI restrictions active before renderer start");
    if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        const auto error = windows_error("ResumeThread(external renderer)");
        static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
        return external_failure(SupervisorError::launch_failed, error, sandbox,
                                process_exit_code(process_handle.get()));
    }

    const auto deadline = std::chrono::steady_clock::now() + effective_timeout;
    std::array<std::byte, 16'384> output_buffer{};
    std::uint64_t copied_bytes{};
    bool process_exited{};
    bool output_closed{};
    for (;;) {
        while (!output_closed) {
            DWORD available{};
            if (PeekNamedPipe(parent_output_read.get(), nullptr, 0, nullptr, &available, nullptr) ==
                FALSE) {
                const auto error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    output_closed = true;
                    break;
                }
                static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
                static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
                discard_rejected_output(as_handle(request.output_object));
                return external_failure(
                    SupervisorError::transport_error,
                    windows_error("PeekNamedPipe(external renderer output)", error), sandbox,
                    process_exit_code(process_handle.get()));
            }
            if (available == 0) {
                break;
            }
            const auto requested =
                static_cast<DWORD>(std::min<std::size_t>(available, output_buffer.size()));
            DWORD received{};
            if (ReadFile(parent_output_read.get(), output_buffer.data(), requested, &received,
                         nullptr) == FALSE) {
                const auto error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    output_closed = true;
                    break;
                }
                static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
                static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
                discard_rejected_output(as_handle(request.output_object));
                return external_failure(SupervisorError::transport_error,
                                        windows_error("ReadFile(external renderer output)", error),
                                        sandbox, process_exit_code(process_handle.get()));
            }
            if (received == 0) {
                output_closed = true;
                break;
            }
            if (copied_bytes > request.limits.maximum_output_bytes ||
                received > request.limits.maximum_output_bytes - copied_bytes) {
                static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
                static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
                discard_rejected_output(as_handle(request.output_object));
                return external_failure(SupervisorError::resource_limit,
                                        "external renderer exceeded the output limit", sandbox,
                                        process_exit_code(process_handle.get()));
            }
            DWORD offset{};
            while (offset < received) {
                DWORD written{};
                const auto write_ok =
                    WriteFile(as_handle(request.output_object), output_buffer.data() + offset,
                              received - offset, &written, nullptr);
                if (write_ok == FALSE || written == 0) {
                    const auto error = write_ok == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
                    static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
                    static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
                    discard_rejected_output(as_handle(request.output_object));
                    return external_failure(
                        SupervisorError::transport_error,
                        windows_error("WriteFile(external renderer bounded output)", error),
                        sandbox, process_exit_code(process_handle.get()));
                }
                offset += written;
            }
            copied_bytes += received;
        }
        if (process_exited) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            static_cast<void>(TerminateJobObject(job.get(), kSupervisorTimeoutExitCode));
            static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
            discard_rejected_output(as_handle(request.output_object));
            return external_failure(SupervisorError::timed_out,
                                    "external renderer exceeded the wall-time limit", sandbox,
                                    process_exit_code(process_handle.get()));
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto wait_ms = static_cast<DWORD>(std::max<std::int64_t>(
            1, std::min<std::int64_t>(kPollInterval.count(), remaining.count())));
        const auto wait = WaitForSingleObject(process_handle.get(), wait_ms);
        if (wait == WAIT_OBJECT_0) {
            process_exited = true;
        } else if (wait == WAIT_FAILED) {
            const auto error = windows_error("WaitForSingleObject(external renderer)");
            static_cast<void>(TerminateJobObject(job.get(), kSupervisorFailureExitCode));
            static_cast<void>(WaitForSingleObject(process_handle.get(), 1'000));
            discard_rejected_output(as_handle(request.output_object));
            return external_failure(SupervisorError::transport_error, error, sandbox,
                                    process_exit_code(process_handle.get()));
        }
    }
    const auto exit = process_exit_code(process_handle.get());
    if (exit != 0) {
        discard_rejected_output(as_handle(request.output_object));
        return external_failure(SupervisorError::worker_crashed,
                                "external renderer exited unsuccessfully", sandbox, exit);
    }

    std::uint64_t actual_output_bytes{};
    if (!output_size(as_handle(request.output_object), actual_output_bytes, detail)) {
        discard_rejected_output(as_handle(request.output_object));
        return external_failure(SupervisorError::transport_error, std::move(detail), sandbox, exit);
    }
    if (actual_output_bytes == 0 || actual_output_bytes != copied_bytes ||
        actual_output_bytes > request.limits.maximum_output_bytes) {
        const auto mismatch = "external renderer output boundary violated: actual=" +
                              std::to_string(actual_output_bytes) +
                              ", copied=" + std::to_string(copied_bytes) +
                              ", maximum=" + std::to_string(request.limits.maximum_output_bytes);
        discard_rejected_output(as_handle(request.output_object));
        return external_failure(actual_output_bytes > request.limits.maximum_output_bytes
                                    ? SupervisorError::resource_limit
                                    : SupervisorError::transport_error,
                                mismatch, sandbox, exit);
    }
    return {.error = SupervisorError::none,
            .detail = {},
            .sandbox = std::move(sandbox),
            .exit_code = exit,
            .bytes_written = actual_output_bytes};
}

} // namespace vove::worker
