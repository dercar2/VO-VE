#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../catalog_process.hpp"
#include "../catalog_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

#include <thread>
namespace vove::platform::detail {

namespace {

std::string windows_error(const char *operation, const DWORD code = GetLastError()) {
    return std::string(operation) + " failed with Windows error " + std::to_string(code);
}

void close_handle(HANDLE &handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

class WindowsCatalogProcess final : public CatalogProcess {
  public:
    WindowsCatalogProcess(HANDLE process, HANDLE thread, HANDLE job, HANDLE input, HANDLE output)
        : process_(process), thread_(thread), job_(job), input_(input), output_(output) {}

    ~WindowsCatalogProcess() override {
        terminate();
        std::string ignored;
        static_cast<void>(wait(ignored));
        close_handle(input_);
        close_handle(job_);
        close_handle(output_);
        close_handle(thread_);
        close_handle(process_);
    }

    [[nodiscard]] bool write_frame(const std::span<const std::byte> payload,
                                   std::string &error) override {
        try {
            const auto frame = frame_payload(payload);
            std::size_t offset{};
            while (offset < frame.size()) {
                const auto remaining = frame.size() - offset;
                const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                    remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
                DWORD written{};
                if (WriteFile(input_, frame.data() + offset, chunk, &written, nullptr) == FALSE ||
                    written == 0) {
                    error = windows_error("WriteFile");
                    close_handle(input_);
                    return false;
                }
                offset += written;
            }
            return true;
        } catch (const std::exception &exception) {
            error = exception.what();
            close_handle(input_);
            return false;
        }
    }

    void close_input() noexcept override {
        close_handle(input_);
    }

    [[nodiscard]] bool read_frame(std::vector<std::byte> &payload, std::string &error,
                                  const std::size_t maximum_bytes) override {
        std::array<std::byte, kCatalogFrameHeaderBytes> header{};
        if (!read_exact(header, error)) {
            return false;
        }
        std::size_t size{};
        if (!decode_frame_size(header, size, error)) {
            return false;
        }
        if (size > maximum_bytes) {
            error = "helper frame exceeds the caller size limit";
            return false;
        }
        payload.resize(size);
        return read_exact(payload, error);
    }

    void terminate() noexcept override {
        if (!terminated_.exchange(true, std::memory_order_acq_rel) && job_ != nullptr) {
            static_cast<void>(TerminateJobObject(job_, 124));
        }
    }

    [[nodiscard]] int wait(std::string &error) noexcept override {
        std::scoped_lock lock(wait_mutex_);
        if (waited_) {
            return exit_code_;
        }
        const auto wait_result =
            process_ == nullptr ? WAIT_FAILED : WaitForSingleObject(process_, 500);
        if (wait_result == WAIT_TIMEOUT) {
            waited_ = true;
            exit_code_ = 124;
            error = "catalog helper did not exit within 500 milliseconds";
            return exit_code_;
        }
        if (wait_result != WAIT_OBJECT_0) {
            error = windows_error("WaitForSingleObject");
            return -1;
        }
        DWORD code{};
        if (GetExitCodeProcess(process_, &code) == FALSE) {
            error = windows_error("GetExitCodeProcess");
            return -1;
        }
        waited_ = true;
        exit_code_ = static_cast<int>(code);
        return exit_code_;
    }

  private:
    [[nodiscard]] bool read_exact(const std::span<std::byte> output, std::string &error) {
        std::size_t offset{};
        while (offset < output.size()) {
            if (terminated_.load(std::memory_order_acquire)) {
                error = "catalog helper transport was cancelled";
                return false;
            }

            DWORD available{};
            if (PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
                const auto code = GetLastError();
                error = code == ERROR_BROKEN_PIPE ? "catalog helper closed its output"
                                                  : windows_error("PeekNamedPipe", code);
                return false;
            }
            if (available == 0) {
                if (process_ == nullptr || WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                    error = "catalog helper reached end of output";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const auto remaining = output.size() - offset;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, available));
            DWORD read{};
            if (ReadFile(output_, output.data() + offset, chunk, &read, nullptr) == FALSE) {
                const auto code = GetLastError();
                error = code == ERROR_BROKEN_PIPE ? "catalog helper closed its output"
                                                  : windows_error("ReadFile", code);
                return false;
            }
            if (read == 0) {
                error = "catalog helper reached end of output";
                return false;
            }
            offset += read;
        }
        return true;
    }

    HANDLE process_{};
    HANDLE thread_{};
    HANDLE job_{};
    HANDLE input_{};
    HANDLE output_{};
    std::atomic_bool terminated_{false};
    std::mutex wait_mutex_;
    bool waited_{false};
    int exit_code_{};
};

std::filesystem::path executable_path() {
    std::wstring buffer(32'768, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::filesystem::path helper_from_environment() {
    std::wstring buffer(32'768, L'\0');
    const auto length = GetEnvironmentVariableW(L"VOVE_CATALOG_HELPER", buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

} // namespace

std::filesystem::path default_catalog_helper_path() {
    if (auto configured = helper_from_environment(); !configured.empty()) {
        return configured;
    }
    const auto executable = executable_path();
    return executable.empty() ? std::filesystem::path{}
                              : executable.parent_path() / L"vove-catalog-helper.exe";
}

std::shared_ptr<CatalogProcess> start_catalog_process(const std::filesystem::path &helper_path,
                                                      catalog::CatalogError &error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_input{};
    HANDLE parent_input{};
    HANDLE parent_output{};
    HANDLE child_output{};
    HANDLE null_output{};
    HANDLE job{};

    const auto fail = [&](const std::string &message) {
        close_handle(child_input);
        close_handle(parent_input);
        close_handle(parent_output);
        close_handle(child_output);
        close_handle(null_output);
        close_handle(job);
        error = {.kind = catalog::CatalogErrorKind::io_error, .message_utf8 = message};
        return std::shared_ptr<CatalogProcess>{};
    };

    if (CreatePipe(&child_input, &parent_input, &attributes, 0) == FALSE ||
        SetHandleInformation(parent_input, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        return fail(windows_error("CreatePipe(stdin)"));
    }
    if (CreatePipe(&parent_output, &child_output, &attributes, 0) == FALSE ||
        SetHandleInformation(parent_output, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        return fail(windows_error("CreatePipe(stdout)"));
    }
    null_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_output == INVALID_HANDLE_VALUE) {
        return fail(windows_error("CreateFile(NUL)"));
    }

    job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return fail(windows_error("CreateJobObject"));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_information{};
    job_information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_information,
                                sizeof(job_information)) == FALSE) {
        return fail(windows_error("SetInformationJobObject"));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input;
    startup.StartupInfo.hStdOutput = child_output;
    startup.StartupInfo.hStdError = null_output;

    SIZE_T attribute_bytes{};
    static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes));
    if (attribute_bytes == 0) {
        return fail(windows_error("InitializeProcThreadAttributeList(size)"));
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    startup.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes) ==
        FALSE) {
        return fail(windows_error("InitializeProcThreadAttributeList"));
    }
    const std::array inherited_handles{child_input, child_output, null_output};
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  const_cast<HANDLE *>(inherited_handles.data()),
                                  sizeof(inherited_handles), nullptr, nullptr) == FALSE) {
        const auto code = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return fail(windows_error("UpdateProcThreadAttribute", code));
    }

    PROCESS_INFORMATION process{};
    auto command = L"\"" + helper_path.native() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const auto created =
        CreateProcessW(helper_path.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       nullptr, &startup.StartupInfo, &process);
    const auto create_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    close_handle(child_input);
    close_handle(child_output);
    close_handle(null_output);
    if (created == FALSE) {
        return fail(windows_error("CreateProcess", create_error));
    }
    if (AssignProcessToJobObject(job, process.hProcess) == FALSE) {
        const auto code = GetLastError();
        static_cast<void>(TerminateProcess(process.hProcess, 124));
        static_cast<void>(WaitForSingleObject(process.hProcess, 500));
        close_handle(process.hThread);
        close_handle(process.hProcess);
        return fail(windows_error("AssignProcessToJobObject", code));
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const auto code = GetLastError();
        static_cast<void>(TerminateJobObject(job, 124));
        static_cast<void>(WaitForSingleObject(process.hProcess, 500));
        close_handle(process.hThread);
        close_handle(process.hProcess);
        return fail(windows_error("ResumeThread", code));
    }

    return std::make_shared<WindowsCatalogProcess>(process.hProcess, process.hThread, job,
                                                   parent_input, parent_output);
}

} // namespace vove::platform::detail
