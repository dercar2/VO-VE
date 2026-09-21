#include "../catalog_process.hpp"
#include "../catalog_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <string>
#include <thread>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <utility>

extern "C" char **environ;

namespace vove::platform::detail {

namespace {

std::string posix_error(const char *operation, const int code = errno) {
    return std::string(operation) + " failed: " + std::strerror(code);
}

void close_fd(int &descriptor) noexcept {
    if (descriptor >= 0) {
        static_cast<void>(close(descriptor));
        descriptor = -1;
    }
}

constexpr std::size_t maximumTrackedProcesses = catalog::kCatalogMaxConcurrentEnumerations;

struct ProcessRegistry {
    std::mutex mutex;
    std::vector<pid_t> orphaned_processes;
    std::size_t active_processes{};
};

ProcessRegistry &process_registry() {
    static auto *registry = new ProcessRegistry;
    return *registry;
}

void reap_orphaned_processes_locked(ProcessRegistry &registry) {
    std::erase_if(registry.orphaned_processes, [](const pid_t process) {
        int status{};
        const auto result = waitpid(process, &status, WNOHANG);
        if (result == process || (result < 0 && errno == ECHILD)) {
            return true;
        }
        static_cast<void>(kill(process, SIGKILL));
        return false;
    });
}

bool reserve_process_slot() {
    auto &registry = process_registry();
    std::scoped_lock lock(registry.mutex);
    reap_orphaned_processes_locked(registry);
    if (registry.active_processes + registry.orphaned_processes.size() >= maximumTrackedProcesses) {
        return false;
    }
    ++registry.active_processes;
    return true;
}

void release_process_slot() {
    auto &registry = process_registry();
    std::scoped_lock lock(registry.mutex);
    if (registry.active_processes != 0) {
        --registry.active_processes;
    }
}

void remember_orphaned_process(const pid_t process) {
    auto &registry = process_registry();
    std::scoped_lock lock(registry.mutex);
    if (registry.active_processes != 0) {
        --registry.active_processes;
    }
    if (std::find(registry.orphaned_processes.begin(), registry.orphaned_processes.end(),
                  process) == registry.orphaned_processes.end()) {
        registry.orphaned_processes.push_back(process);
    }
}

class PosixCatalogProcess final : public CatalogProcess {
  public:
    PosixCatalogProcess(const pid_t process, const int input, const int output)
        : process_(process), input_(input), output_(output) {}

    ~PosixCatalogProcess() override {
        terminate();
        std::string ignored;
        static_cast<void>(wait(ignored));
        close_fd(input_);
        close_fd(output_);
    }

    [[nodiscard]] bool write_frame(const std::span<const std::byte> payload,
                                   std::string &error) override {
        try {
            const auto frame = frame_payload(payload);
            std::size_t offset{};
            while (offset < frame.size()) {
                const auto written =
                    send(input_, frame.data() + offset, frame.size() - offset, MSG_NOSIGNAL);
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                if (written <= 0) {
                    error = posix_error("write");
                    static_cast<void>(shutdown(input_, SHUT_WR));
                    return false;
                }
                offset += static_cast<std::size_t>(written);
            }
            return true;
        } catch (const std::exception &exception) {
            error = exception.what();
            static_cast<void>(shutdown(input_, SHUT_WR));
            return false;
        }
    }

    void close_input() noexcept override {
        if (input_ >= 0) {
            static_cast<void>(shutdown(input_, SHUT_WR));
        }
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
        if (terminated_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (input_ >= 0) {
            static_cast<void>(shutdown(input_, SHUT_RDWR));
        }
        if (output_ >= 0) {
            static_cast<void>(shutdown(output_, SHUT_RDWR));
        }
        if (process_ > 0) {
            static_cast<void>(kill(process_, SIGKILL));
        }
    }

    [[nodiscard]] int wait(std::string &error) noexcept override {
        std::scoped_lock lock(wait_mutex_);
        if (waited_) {
            return exit_code_;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        for (;;) {
            int status{};
            const auto result = waitpid(process_, &status, WNOHANG);
            if (result == process_) {
                waited_ = true;
                release_process_slot();
                if (WIFEXITED(status)) {
                    exit_code_ = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    exit_code_ = 128 + WTERMSIG(status);
                } else {
                    exit_code_ = -1;
                }
                return exit_code_;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0) {
                waited_ = true;
                exit_code_ = -1;
                error = posix_error("waitpid");
                if (errno == ECHILD) {
                    release_process_slot();
                } else {
                    remember_orphaned_process(process_);
                }
                return exit_code_;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                remember_orphaned_process(process_);
                waited_ = true;
                exit_code_ = 124;
                error = "catalog helper did not exit within 500 milliseconds";
                return exit_code_;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

  private:
    [[nodiscard]] bool read_exact(const std::span<std::byte> output, std::string &error) {
        std::size_t offset{};
        while (offset < output.size()) {
            const auto amount = read(output_, output.data() + offset, output.size() - offset);
            if (amount < 0 && errno == EINTR) {
                continue;
            }
            if (amount < 0) {
                error = posix_error("read");
                return false;
            }
            if (amount == 0) {
                error = "catalog helper reached end of output";
                return false;
            }
            offset += static_cast<std::size_t>(amount);
        }
        return true;
    }

    pid_t process_{};
    int input_{-1};
    int output_{-1};
    std::atomic_bool terminated_{false};
    std::mutex wait_mutex_;
    bool waited_{false};
    int exit_code_{};
};

std::filesystem::path executable_path() {
    std::array<char, 32'768> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (length <= 0) {
        return {};
    }
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
}

} // namespace

std::filesystem::path default_catalog_helper_path() {
    if (const auto *configured = std::getenv("VOVE_CATALOG_HELPER");
        configured != nullptr && *configured != '\0') {
        const std::string_view text(configured);
        const auto *first = reinterpret_cast<const char8_t *>(text.data());
        return std::filesystem::path(std::u8string(first, first + text.size()));
    }
    const auto executable = executable_path();
    return executable.empty() ? std::filesystem::path{}
                              : executable.parent_path() / "vove-catalog-helper";
}

std::shared_ptr<CatalogProcess> start_catalog_process(const std::filesystem::path &helper_path,
                                                      catalog::CatalogError &error) {
    if (!reserve_process_slot()) {
        error = {.kind = catalog::CatalogErrorKind::io_error,
                 .message_utf8 = "catalog helper process limit reached while cleanup is pending"};
        return {};
    }

    int input_pipe[2]{-1, -1};
    int output_pipe[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, input_pipe) != 0) {
        release_process_slot();
        error = {.kind = catalog::CatalogErrorKind::io_error,
                 .message_utf8 = posix_error("socketpair(stdin)")};
        return {};
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, output_pipe) != 0) {
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        release_process_slot();
        error = {.kind = catalog::CatalogErrorKind::io_error,
                 .message_utf8 = posix_error("socketpair(stdout)")};
        return {};
    }

    const auto mark_close_on_exec = [](const int descriptor) {
        const auto flags = fcntl(descriptor, F_GETFD);
        return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
    };
    if (!mark_close_on_exec(input_pipe[0]) || !mark_close_on_exec(input_pipe[1]) ||
        !mark_close_on_exec(output_pipe[0]) || !mark_close_on_exec(output_pipe[1])) {
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        release_process_slot();
        error = {.kind = catalog::CatalogErrorKind::io_error,
                 .message_utf8 = posix_error("fcntl(FD_CLOEXEC)")};
        return {};
    }

    posix_spawn_file_actions_t actions{};
    auto spawn_error = posix_spawn_file_actions_init(&actions);
    const auto actions_initialized = spawn_error == 0;
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    }
    if (spawn_error == 0) {
        spawn_error =
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    for (const auto descriptor : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1]}) {
        if (spawn_error == 0) {
            spawn_error = posix_spawn_file_actions_addclose(&actions, descriptor);
        }
    }

    pid_t process{};
    const auto native = helper_path.native();
    char *arguments[]{const_cast<char *>(native.c_str()), nullptr};
    if (spawn_error == 0) {
        spawn_error = posix_spawn(&process, native.c_str(), &actions, nullptr, arguments, environ);
    }
    if (actions_initialized) {
        static_cast<void>(posix_spawn_file_actions_destroy(&actions));
    }
    if (spawn_error != 0) {
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        release_process_slot();
        error = {.kind = catalog::CatalogErrorKind::io_error,
                 .message_utf8 = "posix_spawn failed: " + std::string(std::strerror(spawn_error)),
                 .platform_code = spawn_error};
        return {};
    }

    close_fd(input_pipe[0]);
    close_fd(output_pipe[1]);
    try {
        return std::make_shared<PosixCatalogProcess>(process, input_pipe[1], output_pipe[0]);
    } catch (...) {
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        static_cast<void>(kill(process, SIGKILL));
        remember_orphaned_process(process);
        throw;
    }
}

} // namespace vove::platform::detail
