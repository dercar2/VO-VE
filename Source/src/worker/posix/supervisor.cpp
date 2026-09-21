#include "vove/worker/supervisor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

// Undefined in production. The POSIX supervisor test provides this weak hook to keep a
// terminated child pending long enough to exercise the bounded background retry path.
extern "C" bool vove_test_force_waitpid_pending(bool background_reaper) noexcept
    __attribute__((weak));

namespace vove::worker {
namespace {

constexpr int kWorkerSourceFd = 3;
constexpr int kWorkerOutputFd = 4;
constexpr int kWorkerProfileFd = 5;
constexpr int kChildStatusFd = 6;
constexpr int kFirstTemporaryFd = 16;
constexpr std::uint32_t kChildFailureMagic = 0x5245564FU;
constexpr std::chrono::milliseconds kBubblewrapProbeTimeout{2'000};
constexpr std::chrono::milliseconds kReaperRetryDelay{10};
constexpr std::size_t kMaximumOutstandingChildren = 16;

class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(const int descriptor) noexcept : descriptor_(descriptor) {}
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    ~UniqueFd() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor_ >= 0;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(const int replacement = -1) noexcept {
        if (descriptor_ >= 0) {
            while (::close(descriptor_) != 0 && errno == EINTR) {
            }
        }
        descriptor_ = replacement;
    }

  private:
    int descriptor_{-1};
};

struct SocketPair {
    UniqueFd parent;
    UniqueFd child;
};

struct PipePair {
    UniqueFd read_end;
    UniqueFd write_end;
};

enum class ChildStage : std::int32_t {
    process_group = 1,
    parent_death = 2,
    no_new_privileges = 3,
    resource_limits = 4,
    duplicate_descriptors = 5,
    install_transport = 6,
    install_standard_error = 7,
    install_status = 8,
    close_descriptors = 9,
    execute = 10,
};

struct ChildFailure {
    std::uint32_t magic{kChildFailureMagic};
    std::int32_t stage{};
    std::int32_t error{};
};

struct SandboxChoice {
    std::optional<std::filesystem::path> bubblewrap;
    SandboxReport report;
};

struct LaunchedChild {
    pid_t pid{-1};
    UniqueFd transport;
    SandboxReport sandbox;
};

enum class IoStatus { success, timed_out, closed, failed };

struct FrameIoResult {
    IoStatus status{IoStatus::failed};
    std::vector<std::byte> bytes;
    std::string detail;
};

[[nodiscard]] std::string errno_message(const std::string_view operation, const int error = errno) {
    return std::string{operation} + ": " + std::strerror(error) + " (" + std::to_string(error) +
           ")";
}

[[nodiscard]] bool set_close_on_exec(const int descriptor, const bool enabled) noexcept {
    const auto flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0) {
        return false;
    }
    const auto replacement = enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
    return ::fcntl(descriptor, F_SETFD, replacement) == 0;
}

[[nodiscard]] std::optional<SocketPair> make_socket_pair(std::string &detail) {
    std::array<int, 2> descriptors{-1, -1};
#if defined(SOCK_CLOEXEC)
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors.data()) != 0) {
        detail = errno_message("socketpair failed");
        return std::nullopt;
    }
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
        detail = errno_message("socketpair failed");
        return std::nullopt;
    }
    if (!set_close_on_exec(descriptors[0], true) || !set_close_on_exec(descriptors[1], true)) {
        const auto error = errno;
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        detail = errno_message("setting socket close-on-exec failed", error);
        return std::nullopt;
    }
#endif
    return SocketPair{.parent = UniqueFd{descriptors[0]}, .child = UniqueFd{descriptors[1]}};
}

[[nodiscard]] std::optional<PipePair> make_pipe(std::string &detail) {
    std::array<int, 2> descriptors{-1, -1};
#if defined(__linux__)
    if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) {
        detail = errno_message("pipe2 failed");
        return std::nullopt;
    }
#else
    if (::pipe(descriptors.data()) != 0) {
        detail = errno_message("pipe failed");
        return std::nullopt;
    }
    if (!set_close_on_exec(descriptors[0], true) || !set_close_on_exec(descriptors[1], true)) {
        const auto error = errno;
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        detail = errno_message("setting pipe close-on-exec failed", error);
        return std::nullopt;
    }
#endif
    return PipePair{.read_end = UniqueFd{descriptors[0]}, .write_end = UniqueFd{descriptors[1]}};
}

[[nodiscard]] bool is_open_descriptor(const int descriptor) noexcept {
    if (descriptor < 0) {
        return false;
    }
    if (::fcntl(descriptor, F_GETFD) >= 0) {
        return true;
    }
    return errno != EBADF;
}

[[nodiscard]] bool descriptor_is_read_only(const int descriptor) noexcept {
    const auto flags = ::fcntl(descriptor, F_GETFL);
    return flags >= 0 && (flags & O_ACCMODE) == O_RDONLY;
}

[[nodiscard]] bool descriptor_is_write_only(const int descriptor) noexcept {
    const auto flags = ::fcntl(descriptor, F_GETFL);
    return flags >= 0 && (flags & O_ACCMODE) == O_WRONLY;
}

[[nodiscard]] bool empty_regular_output(const int descriptor, std::string &detail) noexcept {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        detail = errno_message("querying output descriptor failed");
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size != 0) {
        detail = "output descriptor must refer to an empty regular file";
        return false;
    }
    return true;
}

[[nodiscard]] bool output_matches_result(const int descriptor, const WorkerResult &result,
                                         const std::uint64_t maximum_bytes,
                                         std::string &detail) noexcept {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        detail = errno_message("validating worker output failed");
        return false;
    }
    const auto actual_bytes = static_cast<std::uint64_t>(status.st_size);
    if (actual_bytes > maximum_bytes || result.bytes_written > maximum_bytes ||
        actual_bytes != result.bytes_written) {
        detail = "worker output size does not match its bounded result metadata";
        return false;
    }
    return true;
}

void discard_external_output(const int descriptor) noexcept {
    static_cast<void>(::ftruncate(descriptor, 0));
    static_cast<void>(::lseek(descriptor, 0, SEEK_SET));
}

[[nodiscard]] bool external_output_size(const int descriptor, std::uint64_t &bytes,
                                        std::string &detail) noexcept {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        detail = errno_message("validating external renderer output failed");
        return false;
    }
    bytes = static_cast<std::uint64_t>(status.st_size);
    return true;
}

[[nodiscard]] std::optional<std::filesystem::path>
executable_path(const std::filesystem::path &candidate) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(candidate, error);
    if (error || canonical.empty()) {
        return std::nullopt;
    }
    const auto status = std::filesystem::status(canonical, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        ::access(canonical.c_str(), X_OK) != 0) {
        return std::nullopt;
    }
    return canonical;
}

[[nodiscard]] std::optional<std::filesystem::path> find_bubblewrap() {
    constexpr std::array<std::string_view, 3> candidates{"/usr/bin/bwrap", "/bin/bwrap",
                                                         "/usr/local/bin/bwrap"};
    for (const auto candidate : candidates) {
        const std::filesystem::path path{candidate};
        if (const auto executable = executable_path(path); executable.has_value()) {
            return executable;
        }
    }
    return std::nullopt;
}

void append_read_only_bind_if_present(std::vector<std::string> &arguments,
                                      const std::string_view path) {
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::path{path}, error) && !error) {
        arguments.emplace_back("--ro-bind");
        arguments.emplace_back(path);
        arguments.emplace_back(path);
    }
}

[[nodiscard]] std::vector<std::string>
bubblewrap_arguments(const std::filesystem::path &bubblewrap, const std::filesystem::path &program,
                     const std::span<const std::string> child_arguments = {}) {
    std::vector<std::string> arguments;
    arguments.reserve(48);
    arguments.push_back(bubblewrap.string());
    arguments.emplace_back("--unshare-all");
    arguments.emplace_back("--die-with-parent");
    arguments.emplace_back("--new-session");
    arguments.emplace_back("--clearenv");
    arguments.emplace_back("--setenv");
    arguments.emplace_back("PATH");
    arguments.emplace_back("/usr/bin:/bin");
    arguments.emplace_back("--setenv");
    arguments.emplace_back("LANG");
    arguments.emplace_back("C");
    arguments.emplace_back("--dir");
    arguments.emplace_back("/proc");
    arguments.emplace_back("--dev");
    arguments.emplace_back("/dev");
    arguments.emplace_back("--tmpfs");
    arguments.emplace_back("/tmp");
    arguments.emplace_back("--dir");
    arguments.emplace_back("/opt");
    arguments.emplace_back("--dir");
    arguments.emplace_back("/opt/vove");
    append_read_only_bind_if_present(arguments, "/usr");
    append_read_only_bind_if_present(arguments, "/bin");
    append_read_only_bind_if_present(arguments, "/lib");
    append_read_only_bind_if_present(arguments, "/lib64");
    append_read_only_bind_if_present(arguments, "/etc/ld.so.cache");
    arguments.emplace_back("--ro-bind");
    arguments.push_back(program.string());
    arguments.emplace_back("/opt/vove/vove-worker");
    arguments.emplace_back("--chdir");
    arguments.emplace_back("/tmp");
    arguments.emplace_back("--");
    arguments.emplace_back("/opt/vove/vove-worker");
    arguments.insert(arguments.end(), child_arguments.begin(), child_arguments.end());
    return arguments;
}

[[nodiscard]] std::vector<char *> argument_pointers(std::vector<std::string> &arguments) {
    std::vector<char *> pointers;
    pointers.reserve(arguments.size() + 1U);
    for (auto &argument : arguments) {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

[[nodiscard]] std::array<char *, 4> minimal_environment() {
    static char path[] = "PATH=/usr/bin:/bin";
    static char language[] = "LANG=C";
    static char locale[] = "LC_ALL=C";
    return {path, language, locale, nullptr};
}

[[nodiscard]] bool wait_until(const int descriptor, const short events,
                              const std::chrono::steady_clock::time_point deadline,
                              std::string &detail, bool &timed_out) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            detail = "worker IPC exceeded the wall-time limit";
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto timeout = static_cast<int>(
            std::clamp<std::int64_t>(remaining.count() + 1, 1, std::numeric_limits<int>::max()));
        pollfd descriptor_state{.fd = descriptor, .events = events, .revents = 0};
        const auto polled = ::poll(&descriptor_state, 1, timeout);
        if (polled > 0) {
            if ((descriptor_state.revents & events) != 0) {
                return true;
            }
            if ((descriptor_state.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                detail = "worker IPC closed unexpectedly";
                return false;
            }
            continue;
        }
        if (polled == 0) {
            timed_out = true;
            detail = "worker IPC exceeded the wall-time limit";
            return false;
        }
        if (errno != EINTR) {
            detail = errno_message("poll failed");
            return false;
        }
    }
}

[[nodiscard]] IoStatus send_all(const int descriptor, const std::span<const std::byte> bytes,
                                const std::chrono::steady_clock::time_point deadline,
                                std::string &detail) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        bool timed_out{};
        if (!wait_until(descriptor, POLLOUT, deadline, detail, timed_out)) {
            return timed_out ? IoStatus::timed_out : IoStatus::closed;
        }
        const auto sent =
            ::send(descriptor, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        detail = sent == 0 ? "worker IPC write made no progress"
                           : errno_message("worker IPC write failed");
        return IoStatus::failed;
    }
    return IoStatus::success;
}

[[nodiscard]] bool send_worker_objects(const int transport, const int source, const int output,
                                       const std::optional<int> profile, std::string &detail) {
    const std::array objects{source, output, profile.value_or(-1)};
    const auto object_count = profile.has_value() ? std::size_t{3} : std::size_t{2};
    std::array<std::byte, CMSG_SPACE(sizeof(objects))> control{};
    std::byte marker{0x56};
    iovec vector{.iov_base = &marker, .iov_len = 1};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * object_count);
    std::memcpy(CMSG_DATA(header), objects.data(), sizeof(int) * object_count);
    message.msg_controllen = CMSG_SPACE(sizeof(int) * object_count);

    ssize_t sent{};
    do {
        sent = ::sendmsg(transport, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent != 1) {
        detail = sent < 0 ? errno_message("sending worker objects failed")
                          : "sending worker objects made no progress";
        return false;
    }
    return true;
}

[[nodiscard]] IoStatus receive_exact(const int descriptor, const std::span<std::byte> bytes,
                                     const std::chrono::steady_clock::time_point deadline,
                                     std::string &detail) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        bool timed_out{};
        if (!wait_until(descriptor, POLLIN, deadline, detail, timed_out)) {
            return timed_out ? IoStatus::timed_out : IoStatus::closed;
        }
        const auto received = ::recv(descriptor, bytes.data() + offset, bytes.size() - offset, 0);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        detail = received == 0 ? "worker IPC reached end of stream"
                               : errno_message("worker IPC read failed");
        return received == 0 ? IoStatus::closed : IoStatus::failed;
    }
    return IoStatus::success;
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

[[nodiscard]] FrameIoResult receive_frame(const int descriptor,
                                          const std::chrono::steady_clock::time_point deadline) {
    FrameIoResult result;
    result.bytes.resize(kProtocolHeaderBytes);
    result.status = receive_exact(descriptor, result.bytes, deadline, result.detail);
    if (result.status != IoStatus::success) {
        result.bytes.clear();
        return result;
    }
    constexpr std::size_t frame_size_offset = 12;
    const auto frame_size = decode_u32_le(result.bytes, frame_size_offset);
    if (frame_size < kProtocolHeaderBytes || frame_size > kMaximumFrameBytes) {
        result.status = IoStatus::failed;
        result.detail = "worker returned a frame with an invalid size";
        result.bytes.clear();
        return result;
    }
    result.bytes.resize(frame_size);
    const auto payload = std::span<std::byte>{result.bytes}.subspan(kProtocolHeaderBytes);
    result.status = receive_exact(descriptor, payload, deadline, result.detail);
    if (result.status != IoStatus::success) {
        result.bytes.clear();
        return result;
    }
    DecodedFrame decoded;
    DecodeError error;
    if (!decode_frame(result.bytes, decoded, error)) {
        result.status = IoStatus::failed;
        result.detail = "worker returned an invalid frame: " + error.message;
        result.bytes.clear();
    }
    return result;
}

[[nodiscard]] bool set_limit_at_most(const int resource, const rlim_t desired_soft,
                                     const rlim_t desired_hard,
                                     const rlim_t required_minimum) noexcept {
    rlimit current{};
    if (::getrlimit(resource, &current) != 0) {
        return false;
    }
    const auto target_hard =
        current.rlim_max == RLIM_INFINITY ? desired_hard : std::min(desired_hard, current.rlim_max);
    const auto target_soft = std::min(desired_soft, target_hard);
    if (target_soft < required_minimum) {
        errno = EPERM;
        return false;
    }
    const rlimit limit{.rlim_cur = target_soft, .rlim_max = target_hard};
    return ::setrlimit(resource, &limit) == 0;
}

[[nodiscard]] bool set_child_limits(const WorkerJob &job, const bool using_bubblewrap) noexcept {
    const auto memory = static_cast<rlim_t>(std::min<std::uint64_t>(
        job.limits.memory_limit_bytes, static_cast<std::uint64_t>(RLIM_INFINITY)));
    const auto output = static_cast<rlim_t>(std::min<std::uint64_t>(
        job.limits.maximum_output_bytes, static_cast<std::uint64_t>(RLIM_INFINITY)));
    const auto cpu_seconds = std::max<rlim_t>(
        1, static_cast<rlim_t>((static_cast<std::uint64_t>(job.limits.wall_timeout_ms) + 999U) /
                               1'000U));
    const auto cpu_hard = cpu_seconds == RLIM_INFINITY ? cpu_seconds : cpu_seconds + 1U;
    // RLIMIT_NPROC counts every thread of the real user, not just this job. Preserve the
    // inherited account limit while bubblewrap starts; a desktop/browser can already exceed 64.
    const auto required_descriptors = static_cast<rlim_t>(using_bubblewrap ? 32 : 6);
    return set_limit_at_most(RLIMIT_AS, memory, memory, 1) &&
           set_limit_at_most(RLIMIT_CPU, cpu_seconds, cpu_hard, 1) &&
           set_limit_at_most(RLIMIT_FSIZE, output, output, 1) &&
           set_limit_at_most(RLIMIT_NOFILE, 64, 64, required_descriptors) &&
           (using_bubblewrap || set_limit_at_most(RLIMIT_NPROC, 1, 1, 1));
}

[[nodiscard]] int duplicate_temporary(const int descriptor) noexcept {
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, kFirstTemporaryFd);
}

void report_child_failure(const int status_descriptor, const ChildStage stage,
                          const int error) noexcept {
    const ChildFailure failure{
        .magic = kChildFailureMagic, .stage = static_cast<std::int32_t>(stage), .error = error};
    const auto bytes = std::as_bytes(std::span<const ChildFailure>{&failure, 1});
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto written =
            ::write(status_descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

[[noreturn]] void child_fail(const int status_descriptor, const ChildStage stage,
                             const int error) noexcept {
    report_child_failure(status_descriptor, stage, error);
    _exit(126);
}

[[nodiscard]] bool close_unrelated_descriptors() noexcept {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, static_cast<unsigned int>(kChildStatusFd + 1), UINT_MAX, 0U) ==
        0) {
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return false;
    }
#endif
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return false;
    }
    const auto maximum = limit.rlim_max == RLIM_INFINITY
                             ? static_cast<rlim_t>(1'048'576)
                             : std::min<rlim_t>(limit.rlim_max, 1'048'576);
    for (auto descriptor = kChildStatusFd + 1; descriptor < static_cast<int>(maximum);
         ++descriptor) {
        if (::close(descriptor) != 0 && errno != EBADF && errno != EINTR) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void execute_child(const int transport, const int status_descriptor,
                                const WorkerJob &job, std::vector<std::string> arguments,
                                const bool using_bubblewrap) noexcept {
    const auto transport_copy = duplicate_temporary(transport);
    const auto status_copy = duplicate_temporary(status_descriptor);
    static_cast<void>(::close(kWorkerSourceFd));
    static_cast<void>(::close(kWorkerOutputFd));
    static_cast<void>(::close(kWorkerProfileFd));
    const auto null_descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (transport_copy < 0 || status_copy < 0 || null_descriptor < 0) {
        child_fail(status_descriptor, ChildStage::duplicate_descriptors, errno);
    }

    if (::setpgid(0, 0) != 0) {
        child_fail(status_copy, ChildStage::process_group, errno);
    }
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() == 1) {
        child_fail(status_copy, ChildStage::parent_death, errno == 0 ? ESRCH : errno);
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        child_fail(status_copy, ChildStage::no_new_privileges, errno);
    }
    if (!set_child_limits(job, using_bubblewrap)) {
        child_fail(status_copy, ChildStage::resource_limits, errno);
    }

    if (::dup2(transport_copy, STDIN_FILENO) < 0 || ::dup2(transport_copy, STDOUT_FILENO) < 0) {
        child_fail(status_copy, ChildStage::install_transport, errno);
    }
    if (::dup2(null_descriptor, STDERR_FILENO) < 0) {
        child_fail(status_copy, ChildStage::install_standard_error, errno);
    }
    if (::dup2(status_copy, kChildStatusFd) < 0 || !set_close_on_exec(kChildStatusFd, true)) {
        child_fail(status_copy, ChildStage::install_status, errno);
    }
    if (!close_unrelated_descriptors()) {
        child_fail(kChildStatusFd, ChildStage::close_descriptors, errno);
    }

    auto pointers = argument_pointers(arguments);
    auto environment = minimal_environment();
    ::execve(pointers.front(), pointers.data(), environment.data());
    child_fail(kChildStatusFd, ChildStage::execute, errno);
}

[[noreturn]] void execute_external_child(const int source, const int output,
                                         const int status_descriptor, const WorkerJob &job,
                                         std::vector<std::string> arguments,
                                         const bool using_bubblewrap) noexcept {
    const auto source_copy = duplicate_temporary(source);
    const auto output_copy = duplicate_temporary(output);
    const auto status_copy = duplicate_temporary(status_descriptor);
    const auto null_descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (source_copy < 0 || output_copy < 0 || status_copy < 0 || null_descriptor < 0) {
        child_fail(status_descriptor, ChildStage::duplicate_descriptors, errno);
    }

    if (::setpgid(0, 0) != 0) {
        child_fail(status_copy, ChildStage::process_group, errno);
    }
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() == 1) {
        child_fail(status_copy, ChildStage::parent_death, errno == 0 ? ESRCH : errno);
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        child_fail(status_copy, ChildStage::no_new_privileges, errno);
    }
    if (!set_child_limits(job, using_bubblewrap)) {
        child_fail(status_copy, ChildStage::resource_limits, errno);
    }
    if (::dup2(source_copy, STDIN_FILENO) < 0 || ::dup2(output_copy, STDOUT_FILENO) < 0 ||
        ::dup2(null_descriptor, STDERR_FILENO) < 0) {
        child_fail(status_copy, ChildStage::install_transport, errno);
    }
    if (::dup2(status_copy, kChildStatusFd) < 0 || !set_close_on_exec(kChildStatusFd, true)) {
        child_fail(status_copy, ChildStage::install_status, errno);
    }
    if (!close_unrelated_descriptors()) {
        child_fail(kChildStatusFd, ChildStage::close_descriptors, errno);
    }

    auto pointers = argument_pointers(arguments);
    auto environment = minimal_environment();
    ::execve(pointers.front(), pointers.data(), environment.data());
    child_fail(kChildStatusFd, ChildStage::execute, errno);
}

[[nodiscard]] int decoded_exit_code(const int status) noexcept {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}

class BoundedProcessReaper {
  public:
    class Reservation {
      public:
        Reservation() = default;
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;

        Reservation(Reservation &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              active_(std::exchange(other.active_, false)) {}

        Reservation &operator=(Reservation &&other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
                active_ = std::exchange(other.active_, false);
            }
            return *this;
        }

        ~Reservation() {
            release();
        }

        [[nodiscard]] bool active() const noexcept {
            return active_;
        }

        void defer(const pid_t pid) noexcept {
            if (owner_ == nullptr || !active_) {
                return;
            }
            owner_->defer_reserved(pid);
            active_ = false;
        }

      private:
        friend class BoundedProcessReaper;

        explicit Reservation(BoundedProcessReaper &owner) noexcept
            : owner_(&owner), active_(true) {}

        void release() noexcept {
            if (owner_ != nullptr && active_) {
                owner_->release_reserved();
                active_ = false;
            }
        }

        BoundedProcessReaper *owner_{};
        bool active_{};
    };

    [[nodiscard]] static BoundedProcessReaper &instance() {
        // The process owns this single detached service for its entire lifetime. Deliberately
        // leaving the tiny controller allocated avoids a shutdown join on a kernel-stuck child.
        static auto *reaper = new BoundedProcessReaper;
        return *reaper;
    }

    [[nodiscard]] std::optional<Reservation> try_reserve() noexcept {
        std::lock_guard lock{mutex_};
        if (outstanding_ >= kMaximumOutstandingChildren) {
            return std::nullopt;
        }
        ++outstanding_;
        return Reservation{*this};
    }

  private:
    BoundedProcessReaper() {
        std::thread{[this] { reap_loop(); }}.detach();
    }

    void defer_reserved(const pid_t pid) noexcept {
        std::lock_guard lock{mutex_};
        if (pending_count_ < pending_.size()) {
            pending_[pending_count_++] = pid;
        } else {
            // A reservation accounts for every active or deferred PID, so this is defensive only.
            if (outstanding_ > 0) {
                --outstanding_;
            }
        }
        changed_.notify_one();
    }

    void release_reserved() noexcept {
        std::lock_guard lock{mutex_};
        if (outstanding_ > 0) {
            --outstanding_;
        }
        changed_.notify_all();
    }

    void reap_loop() noexcept {
        for (;;) {
            std::unique_lock lock{mutex_};
            changed_.wait(lock, [this] { return pending_count_ != 0; });
            for (std::size_t index = 0; index < pending_count_;) {
                int status{};
                pid_t waited{};
                do {
                    waited = vove_test_force_waitpid_pending != nullptr &&
                                     vove_test_force_waitpid_pending(true)
                                 ? 0
                                 : ::waitpid(pending_[index], &status, WNOHANG);
                } while (waited < 0 && errno == EINTR);

                if (waited == 0) {
                    ++index;
                    continue;
                }

                pending_[index] = pending_[pending_count_ - 1U];
                --pending_count_;
                if (outstanding_ > 0) {
                    --outstanding_;
                }
                changed_.notify_all();
            }
            if (pending_count_ != 0) {
                lock.unlock();
                std::this_thread::sleep_for(kReaperRetryDelay);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<pid_t, kMaximumOutstandingChildren> pending_{};
    std::size_t pending_count_{};
    std::size_t outstanding_{};
};

void terminate_process_group(const pid_t pid,
                             BoundedProcessReaper::Reservation &reservation) noexcept {
    if (pid <= 0) {
        return;
    }
    static_cast<void>(::kill(-pid, SIGKILL));
    static_cast<void>(::kill(pid, SIGKILL));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
    while (std::chrono::steady_clock::now() < deadline) {
        int status{};
        const auto waited =
            vove_test_force_waitpid_pending != nullptr && vove_test_force_waitpid_pending(false)
                ? 0
                : ::waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) {
            return;
        }
        if (waited < 0 && errno != EINTR) {
            return;
        }
        timespec pause{.tv_sec = 0, .tv_nsec = 1'000'000};
        while (::nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
    reservation.defer(pid);
}

[[nodiscard]] std::optional<int>
wait_for_process(const pid_t pid, const std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        int status{};
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return decoded_exit_code(status);
        }
        if (waited < 0 && errno != EINTR) {
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        timespec pause{.tv_sec = 0, .tv_nsec = 1'000'000};
        while (::nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
}

[[nodiscard]] bool probe_bubblewrap(const std::filesystem::path &bubblewrap,
                                    BoundedProcessReaper::Reservation &reservation) {
    const auto true_path = executable_path("/bin/true");
    if (!true_path.has_value()) {
        return false;
    }
    auto arguments = bubblewrap_arguments(bubblewrap, *true_path);
    const auto pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        static_cast<void>(::setpgid(0, 0));
        static_cast<void>(::prctl(PR_SET_PDEATHSIG, SIGKILL));
        const auto null_descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_descriptor >= 0) {
            static_cast<void>(::dup2(null_descriptor, STDIN_FILENO));
            static_cast<void>(::dup2(null_descriptor, STDOUT_FILENO));
            static_cast<void>(::dup2(null_descriptor, STDERR_FILENO));
            static_cast<void>(::dup2(null_descriptor, kWorkerSourceFd));
            static_cast<void>(::dup2(null_descriptor, kWorkerOutputFd));
            static_cast<void>(::dup2(null_descriptor, kWorkerProfileFd));
        }
        static_cast<void>(::close(kChildStatusFd));
        if (!close_unrelated_descriptors()) {
            _exit(126);
        }
        auto pointers = argument_pointers(arguments);
        auto environment = minimal_environment();
        ::execve(pointers.front(), pointers.data(), environment.data());
        _exit(127);
    }
    static_cast<void>(::setpgid(pid, pid));
    const auto deadline = std::chrono::steady_clock::now() + kBubblewrapProbeTimeout;
    const auto exit = wait_for_process(pid, deadline);
    if (!exit.has_value()) {
        terminate_process_group(pid, reservation);
        return false;
    }
    return *exit == 0;
}

[[nodiscard]] SandboxChoice choose_sandbox(BoundedProcessReaper::Reservation &reservation) {
    SandboxChoice choice;
    choice.report.process_contained = false;
    choice.report.no_new_privileges = true;
    choice.report.memory_limited = true;
    choice.report.cpu_limited = true;
    choice.report.level = SandboxLevel::degraded;
    choice.report.detail =
        "degraded POSIX controls: process group, parent-death signal, no_new_privs and rlimits";

    const auto bubblewrap = find_bubblewrap();
    if (bubblewrap.has_value() && probe_bubblewrap(*bubblewrap, reservation)) {
        choice.bubblewrap = bubblewrap;
        choice.report.level = SandboxLevel::strict;
        choice.report.process_contained = true;
        choice.report.identity_restricted = true;
        choice.report.network_isolated = true;
        choice.report.filesystem_isolated = true;
        choice.report.detail =
            "strict bubblewrap sandbox: user/network/mount namespaces plus process and resource "
            "limits";
    } else if (bubblewrap.has_value()) {
        choice.report.detail += "; bubblewrap was installed but its namespace probe failed";
    } else {
        choice.report.detail += "; bubblewrap was not installed";
    }
    return choice;
}

[[nodiscard]] std::optional<ChildFailure>
read_child_status(const int descriptor, const std::chrono::steady_clock::time_point deadline,
                  bool &timed_out, std::string &detail) {
    std::array<std::byte, sizeof(ChildFailure)> bytes{};
    std::size_t offset{};
    for (;;) {
        bool wait_timed_out{};
        if (!wait_until(descriptor, POLLIN, deadline, detail, wait_timed_out)) {
            if (wait_timed_out) {
                timed_out = true;
            } else if (detail == "worker IPC closed unexpectedly") {
                detail.clear();
                return std::nullopt;
            }
            return std::nullopt;
        }
        const auto received = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            if (offset == bytes.size()) {
                ChildFailure failure;
                std::memcpy(&failure, bytes.data(), sizeof(failure));
                if (failure.magic != kChildFailureMagic) {
                    detail = "child setup returned an invalid status record";
                    return ChildFailure{.magic = 0,
                                        .stage = static_cast<std::int32_t>(ChildStage::execute),
                                        .error = EPROTO};
                }
                return failure;
            }
            continue;
        }
        if (received == 0) {
            if (offset == 0) {
                return std::nullopt;
            }
            detail = "child setup status was truncated";
            return ChildFailure{.magic = 0,
                                .stage = static_cast<std::int32_t>(ChildStage::execute),
                                .error = EPROTO};
        }
        if (errno != EINTR) {
            detail = errno_message("reading child setup status failed");
            return ChildFailure{.magic = 0,
                                .stage = static_cast<std::int32_t>(ChildStage::execute),
                                .error = errno};
        }
    }
}

[[nodiscard]] std::optional<LaunchedChild>
launch_worker(const SupervisorRequest &request, const std::filesystem::path &worker,
              const SandboxChoice &sandbox, const std::chrono::steady_clock::time_point deadline,
              SupervisorResult &result, BoundedProcessReaper::Reservation &reservation) {
    std::string detail;
    auto transport = make_socket_pair(detail);
    if (!transport.has_value()) {
        result.error = SupervisorError::launch_failed;
        result.detail = std::move(detail);
        return std::nullopt;
    }
    auto status = make_pipe(detail);
    if (!status.has_value()) {
        result.error = SupervisorError::launch_failed;
        result.detail = std::move(detail);
        return std::nullopt;
    }

    std::vector<std::string> arguments;
    if (sandbox.bubblewrap.has_value()) {
        arguments = bubblewrap_arguments(*sandbox.bubblewrap, worker);
    } else {
        arguments.push_back(worker.string());
    }

    const auto pid = ::fork();
    if (pid < 0) {
        result.error = SupervisorError::launch_failed;
        result.detail = errno_message("fork failed");
        return std::nullopt;
    }
    if (pid == 0) {
        status->read_end.reset();
        transport->parent.reset();
        execute_child(transport->child.get(), status->write_end.get(), request.job,
                      std::move(arguments), sandbox.bubblewrap.has_value());
    }

    transport->child.reset();
    status->write_end.reset();
    static_cast<void>(::setpgid(pid, pid));
    bool timed_out{};
    auto failure = read_child_status(status->read_end.get(), deadline, timed_out, detail);
    status->read_end.reset();
    if (timed_out) {
        terminate_process_group(pid, reservation);
        result.error = SupervisorError::timed_out;
        result.detail = "worker launch exceeded the wall-time limit";
        return std::nullopt;
    }
    if (failure.has_value()) {
        terminate_process_group(pid, reservation);
        result.error =
            failure->stage == static_cast<std::int32_t>(ChildStage::resource_limits) ||
                    failure->stage == static_cast<std::int32_t>(ChildStage::no_new_privileges)
                ? SupervisorError::sandbox_unavailable
                : SupervisorError::launch_failed;
        result.detail = "worker child setup failed at stage " + std::to_string(failure->stage) +
                        ": " + std::strerror(failure->error) + " (" +
                        std::to_string(failure->error) + ")";
        return std::nullopt;
    }
    if (!detail.empty()) {
        terminate_process_group(pid, reservation);
        result.error = SupervisorError::launch_failed;
        result.detail = std::move(detail);
        return std::nullopt;
    }
    return LaunchedChild{
        .pid = pid, .transport = std::move(transport->parent), .sandbox = sandbox.report};
}

void fail_for_io(SupervisorResult &result, const IoStatus status, std::string detail,
                 const SupervisorError ordinary_error) {
    result.error = status == IoStatus::timed_out ? SupervisorError::timed_out : ordinary_error;
    result.detail = std::move(detail);
}

[[nodiscard]] bool valid_request(const SupervisorRequest &request, std::string &detail) {
    if (request.source_object < 0 || request.source_object > std::numeric_limits<int>::max() ||
        request.output_object < 0 || request.output_object > std::numeric_limits<int>::max() ||
        (request.profile_object != kInvalidNativeObject &&
         (request.profile_object < 0 ||
          request.profile_object > std::numeric_limits<int>::max()))) {
        detail = "source and output objects must be valid POSIX file descriptors";
        return false;
    }
    const auto source = static_cast<int>(request.source_object);
    const auto output = static_cast<int>(request.output_object);
    const auto has_profile = request.profile_object != kInvalidNativeObject;
    const auto profile = has_profile ? static_cast<int>(request.profile_object) : -1;
    if (source == output) {
        detail = "source and output descriptors must be distinct";
        return false;
    }
    if (!is_open_descriptor(source) || !descriptor_is_read_only(source)) {
        detail = "source descriptor must be open read-only";
        return false;
    }
    if (!is_open_descriptor(output) || !descriptor_is_write_only(output)) {
        detail = "output descriptor must be open write-only";
        return false;
    }
    if (has_profile && (profile == source || profile == output || !is_open_descriptor(profile) ||
                        !descriptor_is_read_only(profile))) {
        detail = "profile descriptor must be distinct, open, and read-only";
        return false;
    }
    if (!empty_regular_output(output, detail)) {
        return false;
    }
    if (request.timeout <= std::chrono::milliseconds::zero() ||
        request.timeout > std::chrono::milliseconds{kMaximumWallTimeoutMs}) {
        detail = "supervisor timeout is outside the protocol limits";
        return false;
    }
    if (request.expected_build_id.empty() ||
        request.expected_build_id.size() > kMaximumBuildIdBytes ||
        std::any_of(request.expected_build_id.begin(), request.expected_build_id.end(),
                    [](const char character) {
                        const auto value = static_cast<unsigned char>(character);
                        return value < 0x21U || value > 0x7EU;
                    })) {
        detail = "expected worker build id is invalid";
        return false;
    }
    if (request.job.job_id == 0 || request.job.generation == 0 ||
        request.job.limits.maximum_input_bytes == 0 ||
        request.job.limits.maximum_input_bytes > kMaximumInputBytes ||
        request.job.limits.wall_timeout_ms == 0 ||
        request.job.limits.wall_timeout_ms > kMaximumWallTimeoutMs ||
        request.job.limits.memory_limit_bytes == 0 ||
        request.job.limits.memory_limit_bytes > kMaximumWorkerMemoryBytes ||
        request.job.limits.maximum_output_bytes == 0 ||
        request.job.limits.maximum_output_bytes > kMaximumOutputBytes ||
        request.job.limits.canonical_edge == 0 ||
        request.job.limits.canonical_edge > kMaximumThumbnailEdge) {
        detail = "worker job limits are invalid";
        return false;
    }
    return true;
}

[[nodiscard]] bool valid_external_request(const ExternalRendererRequest &request,
                                          std::string &detail, SupervisorError &error) {
    error = SupervisorError::invalid_request;
    if (request.source_object < 0 || request.source_object > std::numeric_limits<int>::max() ||
        request.output_object < 0 || request.output_object > std::numeric_limits<int>::max()) {
        detail = "external renderer objects must be valid POSIX file descriptors";
        return false;
    }
    const auto source = static_cast<int>(request.source_object);
    const auto output = static_cast<int>(request.output_object);
    if (source == output || !is_open_descriptor(source) || !descriptor_is_read_only(source) ||
        !is_open_descriptor(output) || !descriptor_is_write_only(output)) {
        detail = "external renderer requires distinct read-only source and write-only output";
        return false;
    }
    struct stat source_status{};
    if (::fstat(source, &source_status) != 0 || !S_ISREG(source_status.st_mode) ||
        source_status.st_size < 0) {
        detail = "external renderer source must be a regular file";
        return false;
    }
    if (!empty_regular_output(output, detail)) {
        return false;
    }
    if (request.timeout <= std::chrono::milliseconds::zero() ||
        request.timeout > std::chrono::milliseconds{kMaximumWallTimeoutMs}) {
        detail = "external renderer timeout is outside the protocol limits";
        return false;
    }
    if (request.arguments_utf8.size() > 32U ||
        std::any_of(request.arguments_utf8.cbegin(), request.arguments_utf8.cend(),
                    [](const std::string &argument) {
                        return argument.size() > 1'024U || argument.find('\0') != std::string::npos;
                    })) {
        detail = "external renderer arguments exceed the fixed-command limit";
        return false;
    }
    if (request.limits.maximum_input_bytes != 0 &&
        request.limits.maximum_input_bytes <= kMaximumInputBytes &&
        static_cast<std::uint64_t>(source_status.st_size) > request.limits.maximum_input_bytes) {
        detail = "external renderer source exceeds the configured input limit";
        error = SupervisorError::resource_limit;
        return false;
    }
    if (request.limits.maximum_input_bytes == 0 ||
        request.limits.maximum_input_bytes > kMaximumInputBytes ||
        request.limits.wall_timeout_ms == 0 ||
        request.limits.wall_timeout_ms > kMaximumWallTimeoutMs ||
        request.limits.memory_limit_bytes == 0 ||
        request.limits.memory_limit_bytes > kMaximumWorkerMemoryBytes ||
        request.limits.maximum_output_bytes == 0 ||
        request.limits.maximum_output_bytes > kMaximumOutputBytes ||
        request.limits.canonical_edge == 0 ||
        request.limits.canonical_edge > kMaximumThumbnailEdge) {
        detail = "external renderer limits are invalid";
        return false;
    }
    return true;
}

} // namespace

SupervisorResult run_worker_once(const SupervisorRequest &request) {
    SupervisorResult result;
    if (!valid_request(request, result.detail)) {
        result.error = SupervisorError::invalid_request;
        return result;
    }
    const auto worker = executable_path(request.worker_executable);
    if (!worker.has_value()) {
        result.error = SupervisorError::invalid_request;
        result.detail = "worker executable does not name an executable regular file";
        return result;
    }

    std::optional<BoundedProcessReaper::Reservation> reservation;
    try {
        reservation = BoundedProcessReaper::instance().try_reserve();
    } catch (const std::exception &error) {
        result.error = SupervisorError::launch_failed;
        result.detail = "starting the bounded process reaper failed: " + std::string{error.what()};
        return result;
    }
    if (!reservation.has_value()) {
        result.error = SupervisorError::launch_failed;
        result.detail = "worker process capacity is exhausted while prior children await cleanup";
        return result;
    }

    const auto sandbox = choose_sandbox(*reservation);
    result.sandbox = sandbox.report;
    if (!reservation->active()) {
        result.error = SupervisorError::launch_failed;
        result.detail = "bubblewrap probe did not terminate promptly and was deferred for cleanup";
        return result;
    }
    if (request.require_minimum_sandbox && sandbox.report.level < SandboxLevel::minimum) {
        result.error = SupervisorError::sandbox_unavailable;
        result.detail = sandbox.report.detail;
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    auto child = launch_worker(request, *worker, sandbox, deadline, result, *reservation);
    if (!child.has_value()) {
        return result;
    }
    result.sandbox = child->sandbox;

    const auto handshake =
        encode_handshake({.build_id = request.expected_build_id,
                          .capabilities = capability_bit(Capability::read_only_source_token) |
                                          capability_bit(Capability::write_only_output_token) |
                                          capability_bit(Capability::sandbox_active)});
    auto io_status = send_all(child->transport.get(), handshake, deadline, result.detail);
    if (io_status != IoStatus::success) {
        fail_for_io(result, io_status, std::move(result.detail), SupervisorError::transport_error);
        terminate_process_group(child->pid, *reservation);
        return result;
    }

    auto frame = receive_frame(child->transport.get(), deadline);
    if (frame.status != IoStatus::success) {
        fail_for_io(result, frame.status, std::move(frame.detail),
                    SupervisorError::handshake_failed);
        const auto reap_deadline =
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{100});
        if (const auto exit = wait_for_process(child->pid, reap_deadline); exit.has_value()) {
            result.exit_code = *exit;
            constexpr auto handshake_rejected_exit = 3;
            if (*exit != 0 && *exit != handshake_rejected_exit) {
                result.error = SupervisorError::worker_crashed;
                result.detail = "worker exited before completing the handshake";
            }
        } else {
            terminate_process_group(child->pid, *reservation);
        }
        return result;
    }
    Handshake acknowledgement;
    DecodeError decode_error;
    if (!decode_handshake(frame.bytes, acknowledgement, decode_error,
                          MessageKind::handshake_acknowledgement)) {
        result.error = SupervisorError::handshake_failed;
        result.detail = "worker handshake acknowledgement is invalid: " + decode_error.message;
        terminate_process_group(child->pid, *reservation);
        return result;
    }
    auto required_capabilities = capability_bit(Capability::read_only_source_token) |
                                 capability_bit(Capability::write_only_output_token) |
                                 capability_bit(Capability::sandbox_active);
    if (request.profile_object != kInvalidNativeObject) {
        required_capabilities |= capability_bit(Capability::read_only_profile_token);
    }
    if (acknowledgement.build_id != request.expected_build_id ||
        (acknowledgement.capabilities & required_capabilities) != required_capabilities) {
        result.error = SupervisorError::incompatible_worker;
        result.detail = "worker build id or required handle capabilities are incompatible";
        terminate_process_group(child->pid, *reservation);
        return result;
    }

    result.handshake_completed = true;
    const auto profile = request.profile_object == kInvalidNativeObject
                             ? std::optional<int>{}
                             : std::optional<int>{static_cast<int>(request.profile_object)};
    if (!send_worker_objects(child->transport.get(), static_cast<int>(request.source_object),
                             static_cast<int>(request.output_object), profile, result.detail)) {
        result.error = SupervisorError::transport_error;
        terminate_process_group(child->pid, *reservation);
        return result;
    }

    auto child_job = request.job;
    child_job.source_token = static_cast<OpaqueToken>(kWorkerSourceFd);
    child_job.output_token = static_cast<OpaqueToken>(kWorkerOutputFd);
    child_job.profile_token = profile ? static_cast<OpaqueToken>(kWorkerProfileFd) : 0;
    const auto encoded_job = encode_worker_job(child_job);
    io_status = send_all(child->transport.get(), encoded_job, deadline, result.detail);
    if (io_status != IoStatus::success) {
        fail_for_io(result, io_status, std::move(result.detail), SupervisorError::transport_error);
        terminate_process_group(child->pid, *reservation);
        return result;
    }
    if (::shutdown(child->transport.get(), SHUT_WR) != 0) {
        result.error = SupervisorError::transport_error;
        result.detail = errno_message("closing worker command stream failed");
        terminate_process_group(child->pid, *reservation);
        return result;
    }

    frame = receive_frame(child->transport.get(), deadline);
    if (frame.status != IoStatus::success) {
        fail_for_io(result, frame.status, std::move(frame.detail),
                    SupervisorError::transport_error);
        terminate_process_group(child->pid, *reservation);
        return result;
    }
    if (!decode_worker_result(frame.bytes, result.worker_result, decode_error)) {
        result.error = SupervisorError::transport_error;
        result.detail = "worker result is invalid: " + decode_error.message;
        terminate_process_group(child->pid, *reservation);
        return result;
    }
    if (result.worker_result.job_id != child_job.job_id) {
        result.error = SupervisorError::transport_error;
        result.detail = "worker result job id does not match the request";
        terminate_process_group(child->pid, *reservation);
        return result;
    }
    if (!output_matches_result(static_cast<int>(request.output_object), result.worker_result,
                               request.job.limits.maximum_output_bytes, result.detail)) {
        result.error = SupervisorError::transport_error;
        terminate_process_group(child->pid, *reservation);
        return result;
    }

    const auto exit = wait_for_process(child->pid, deadline);
    if (!exit.has_value()) {
        terminate_process_group(child->pid, *reservation);
        result.error = SupervisorError::timed_out;
        result.detail = "worker did not exit before the wall-time limit";
        return result;
    }
    result.exit_code = *exit;
    if (*exit != 0) {
        result.error = SupervisorError::worker_crashed;
        result.detail = "worker exited abnormally after returning a result";
        return result;
    }
    result.error = SupervisorError::none;
    result.detail.clear();
    return result;
}

ExternalRendererResult run_external_renderer_once(const ExternalRendererRequest &request) {
    ExternalRendererResult result;
    SupervisorError validation_error{};
    if (!valid_external_request(request, result.detail, validation_error)) {
        result.error = validation_error;
        return result;
    }
    const auto executable = executable_path(request.executable);
    if (!executable.has_value()) {
        result.error = SupervisorError::invalid_request;
        result.detail = "external renderer executable does not name an executable regular file";
        return result;
    }
    const auto source = static_cast<int>(request.source_object);
    const auto output = static_cast<int>(request.output_object);
    if (::lseek(source, 0, SEEK_SET) < 0 || ::ftruncate(output, 0) != 0 ||
        ::lseek(output, 0, SEEK_SET) < 0) {
        result.error = SupervisorError::invalid_request;
        result.detail = errno_message("rewinding external renderer files failed");
        return result;
    }

    std::optional<BoundedProcessReaper::Reservation> reservation;
    try {
        reservation = BoundedProcessReaper::instance().try_reserve();
    } catch (const std::exception &error) {
        result.error = SupervisorError::launch_failed;
        result.detail = "starting the bounded process reaper failed: " + std::string{error.what()};
        return result;
    }
    if (!reservation.has_value()) {
        result.error = SupervisorError::launch_failed;
        result.detail = "renderer process capacity is exhausted while prior children await cleanup";
        return result;
    }

    const auto sandbox = choose_sandbox(*reservation);
    result.sandbox = sandbox.report;
    if (!reservation->active()) {
        result.error = SupervisorError::launch_failed;
        result.detail = "bubblewrap probe did not terminate promptly and was deferred for cleanup";
        return result;
    }
    if (request.require_minimum_sandbox && sandbox.report.level < SandboxLevel::minimum) {
        result.error = SupervisorError::sandbox_unavailable;
        result.detail = sandbox.report.detail;
        return result;
    }

    std::vector<std::string> arguments;
    if (sandbox.bubblewrap.has_value()) {
        arguments = bubblewrap_arguments(*sandbox.bubblewrap, *executable,
                                         std::span<const std::string>{request.arguments_utf8});
    } else {
        arguments.reserve(request.arguments_utf8.size() + 1U);
        arguments.push_back(executable->string());
        arguments.insert(arguments.end(), request.arguments_utf8.begin(),
                         request.arguments_utf8.end());
    }

    std::string detail;
    auto status = make_pipe(detail);
    if (!status.has_value()) {
        result.error = SupervisorError::launch_failed;
        result.detail = std::move(detail);
        return result;
    }
    const auto effective_timeout =
        std::min(request.timeout, std::chrono::milliseconds{request.limits.wall_timeout_ms});
    const auto deadline = std::chrono::steady_clock::now() + effective_timeout;
    const WorkerJob limits_job{.job_id = 1,
                               .generation = 1,
                               .source_token = 1,
                               .output_token = 2,
                               .limits = request.limits,
                               .page_index = 0,
                               .password_utf8 = {}};
    const auto pid = ::fork();
    if (pid < 0) {
        result.error = SupervisorError::launch_failed;
        result.detail = errno_message("forking external renderer failed");
        return result;
    }
    if (pid == 0) {
        status->read_end.reset();
        execute_external_child(source, output, status->write_end.get(), limits_job,
                               std::move(arguments), sandbox.bubblewrap.has_value());
    }
    status->write_end.reset();
    static_cast<void>(::setpgid(pid, pid));

    bool launch_timed_out{};
    auto failure = read_child_status(status->read_end.get(), deadline, launch_timed_out, detail);
    status->read_end.reset();
    if (launch_timed_out) {
        terminate_process_group(pid, *reservation);
        discard_external_output(output);
        result.error = SupervisorError::timed_out;
        result.detail = "external renderer launch exceeded the wall-time limit";
        return result;
    }
    if (failure.has_value()) {
        terminate_process_group(pid, *reservation);
        discard_external_output(output);
        result.error =
            failure->stage == static_cast<std::int32_t>(ChildStage::resource_limits) ||
                    failure->stage == static_cast<std::int32_t>(ChildStage::no_new_privileges)
                ? SupervisorError::sandbox_unavailable
                : SupervisorError::launch_failed;
        result.detail = "external renderer setup failed at stage " +
                        std::to_string(failure->stage) + ": " + std::strerror(failure->error) +
                        " (" + std::to_string(failure->error) + ")";
        return result;
    }
    if (!detail.empty()) {
        terminate_process_group(pid, *reservation);
        discard_external_output(output);
        result.error = SupervisorError::launch_failed;
        result.detail = std::move(detail);
        return result;
    }

    const auto exit = wait_for_process(pid, deadline);
    if (!exit.has_value()) {
        terminate_process_group(pid, *reservation);
        discard_external_output(output);
        result.error = SupervisorError::timed_out;
        result.detail = "external renderer did not exit before the wall-time limit";
        return result;
    }
    result.exit_code = *exit;
    if (*exit != 0) {
        discard_external_output(output);
        result.error = *exit == 128 + SIGXFSZ ? SupervisorError::resource_limit
                                              : SupervisorError::worker_crashed;
        result.detail = *exit == 128 + SIGXFSZ ? "external renderer exceeded the output limit"
                                               : "external renderer exited unsuccessfully";
        return result;
    }

    if (!external_output_size(output, result.bytes_written, result.detail)) {
        discard_external_output(output);
        result.error = SupervisorError::transport_error;
        return result;
    }
    if (result.bytes_written == 0) {
        discard_external_output(output);
        result.error = SupervisorError::transport_error;
        result.detail = "external renderer output boundary violated: actual=0, maximum=" +
                        std::to_string(request.limits.maximum_output_bytes);
        return result;
    }
    if (result.bytes_written > request.limits.maximum_output_bytes) {
        const auto bytes = result.bytes_written;
        discard_external_output(output);
        result.bytes_written = 0;
        result.error = SupervisorError::resource_limit;
        result.detail =
            "external renderer output boundary violated: actual=" + std::to_string(bytes) +
            ", maximum=" + std::to_string(request.limits.maximum_output_bytes);
        return result;
    }
    result.error = SupervisorError::none;
    result.detail.clear();
    return result;
}

} // namespace vove::worker
