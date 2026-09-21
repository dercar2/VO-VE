#include "diagnostic_sink.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace vove::ui {
namespace {

#ifndef VOVE_VERSION
#define VOVE_VERSION "unknown"
#endif
#ifndef VOVE_BUILD_LABEL
#define VOVE_BUILD_LABEL "development"
#endif

constexpr std::size_t kMaximumQueuedLines = 256;
constexpr std::size_t kMaximumLineBytes = 2U * 1024U;
constexpr std::size_t kMaximumValueBytes = 512;
constexpr std::string_view kProtocolMarker{"VOVEDIAG/1"};

constexpr std::string_view platform_name() noexcept {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

constexpr std::string_view architecture_name() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

bool valid_token(const std::string_view token) noexcept {
    return token.size() == 32U && std::all_of(token.begin(), token.end(), [](const char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                      (value >= 'A' && value <= 'F');
           });
}

bool valid_identifier(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64U &&
           std::all_of(value.begin(), value.end(), [](const char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
                      (value >= 'A' && value <= 'Z') || value == '-' || value == '_' ||
                      value == '.';
           });
}

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation{};
        std::uint32_t codepoint{};
        if ((lead & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = lead & 0x1fU;
            if (codepoint < 2U)
                return false;
        } else if ((lead & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = lead & 0x0fU;
        } else if ((lead & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (continuation > value.size() - index - 1U)
            return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2U && codepoint < 0x800U) ||
            (continuation == 3U && codepoint < 0x10000U) || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += continuation + 1U;
    }
    return true;
}

std::string bounded_utf8(const std::string_view value) {
    if (value.size() <= kMaximumValueBytes) {
        return std::string(value);
    }
    std::size_t end = kMaximumValueBytes;
    while (end != 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
        --end;
    }
    return std::string(value.substr(0, end));
}

std::string percent_encode(const std::string_view value) {
    static constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const char raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        const auto unreserved = (character >= 'a' && character <= 'z') ||
                                (character >= 'A' && character <= 'Z') ||
                                (character >= '0' && character <= '9') || character == '-' ||
                                character == '_' || character == '.';
        if (unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hexadecimal[character >> 4U]);
            encoded.push_back(hexadecimal[character & 0x0fU]);
        }
    }
    return encoded;
}

std::uint64_t unix_milliseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::string make_line(const std::string_view token, const std::uint64_t sequence,
                      const std::string_view event,
                      const std::initializer_list<DiagnosticField> fields) {
    if (!valid_identifier(event)) {
        return {};
    }
    if (fields.size() > 32U)
        return {};
    std::string line{kProtocolMarker};
    line.append("\t").append(token);
    line.append("\t").append(std::to_string(sequence));
    line.append("\t").append(std::to_string(unix_milliseconds()));
    line.append("\tui\t").append(event);
    std::unordered_set<std::string_view> keys;
    for (const auto &[key, value] : fields) {
        if (!valid_identifier(key) || !valid_utf8(value) || !keys.insert(key).second) {
            return {};
        }
        line.append("\t").append(key).append("=").append(percent_encode(bounded_utf8(value)));
    }
    line.push_back('\n');
    return line.size() <= kMaximumLineBytes ? line : std::string{};
}

#ifdef _WIN32
bool write_all(const HANDLE endpoint, const std::string_view bytes,
               const std::atomic_bool &) noexcept {
    std::size_t offset{};
    while (offset < bytes.size()) {
        DWORD written{};
        const auto remaining =
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max());
        if (WriteFile(endpoint, bytes.data() + offset, static_cast<DWORD>(remaining), &written,
                      nullptr) == FALSE ||
            written == 0U) {
            return false;
        }
        offset += written;
    }
    return true;
}
#else
bool write_all(const int endpoint, const std::string_view bytes,
               const std::atomic_bool &stopping) noexcept {
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto written = ::write(endpoint, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stopping.load(std::memory_order_relaxed))
                return false;
            pollfd descriptor{endpoint, POLLOUT, 0};
            static_cast<void>(::poll(&descriptor, 1, 100));
            continue;
        }
        return false;
    }
    return true;
}
#endif

} // namespace

struct DiagnosticSink::State {
#ifdef _WIN32
    HANDLE endpoint{INVALID_HANDLE_VALUE};
#else
    int endpoint{-1};
#endif
    std::string token;
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable finished_ready;
    std::deque<std::string> lines;
    std::thread writer;
    std::atomic_bool stopping{};
    std::uint64_t sequence{};
    std::uint64_t dropped{};
    std::string last_event{"activated"};
    bool finished{};
};

DiagnosticSink &DiagnosticSink::instance() noexcept {
    static DiagnosticSink sink;
    return sink;
}

bool DiagnosticSink::activate_from_environment() noexcept {
    if (state_) {
        return true;
    }
    const auto *schema = std::getenv("VOVE_DIAGNOSTIC_SCHEMA");
    const auto *token_text = std::getenv("VOVE_DIAGNOSTIC_TOKEN");
#ifdef _WIN32
    const auto *endpoint_text = std::getenv("VOVE_DIAGNOSTIC_HANDLE");
#else
    const auto *endpoint_text = std::getenv("VOVE_DIAGNOSTIC_FD");
#endif
    if (schema == nullptr || std::string_view(schema) != "1" || token_text == nullptr ||
        endpoint_text == nullptr || !valid_token(token_text)) {
        return false;
    }

    try {
        auto candidate = std::make_unique<State>();
        candidate->token = token_text;
#ifdef _WIN32
        std::uint64_t raw{};
        const auto parsed = std::from_chars(
            endpoint_text, endpoint_text + std::char_traits<char>::length(endpoint_text), raw);
        if (parsed.ec != std::errc{} || *parsed.ptr != '\0' || raw == 0U) {
            return false;
        }
        candidate->endpoint = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
        DWORD flags{};
        if (GetHandleInformation(candidate->endpoint, &flags) == FALSE ||
            SetHandleInformation(candidate->endpoint, HANDLE_FLAG_INHERIT, 0U) == FALSE) {
            return false;
        }
#else
        int raw{};
        const auto end = endpoint_text + std::char_traits<char>::length(endpoint_text);
        const auto parsed = std::from_chars(endpoint_text, end, raw);
        if (parsed.ec != std::errc{} || parsed.ptr != end || raw < 3 ||
            ::fcntl(raw, F_GETFD) == -1) {
            return false;
        }
        candidate->endpoint = raw;
        const auto descriptor_flags = ::fcntl(raw, F_GETFD);
        const auto status_flags = ::fcntl(raw, F_GETFL);
        if (descriptor_flags == -1 || status_flags == -1 ||
            ::fcntl(raw, F_SETFD, descriptor_flags | FD_CLOEXEC) == -1 ||
            ::fcntl(raw, F_SETFL, status_flags | O_NONBLOCK) == -1) {
            return false;
        }
#endif
        state_ = std::move(candidate);
        auto *state = state_.get();
        state->writer = std::thread([state] {
#ifndef _WIN32
            sigset_t blocked;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            static_cast<void>(pthread_sigmask(SIG_BLOCK, &blocked, nullptr));
#endif
            const auto mark_finished = [state] {
                std::lock_guard lock(state->mutex);
                state->finished = true;
                state->finished_ready.notify_all();
            };
            for (;;) {
                std::string line;
                {
                    std::unique_lock lock(state->mutex);
                    state->ready.wait(lock, [state] {
                        return state->stopping.load(std::memory_order_relaxed) ||
                               !state->lines.empty();
                    });
                    if (state->stopping.load(std::memory_order_relaxed) && state->lines.empty()) {
                        lock.unlock();
                        mark_finished();
                        return;
                    }
                    line = std::move(state->lines.front());
                    state->lines.pop_front();
                }
                if (!write_all(state->endpoint, line, state->stopping)) {
                    state->stopping.store(true, std::memory_order_relaxed);
                    std::lock_guard lock(state->mutex);
                    state->lines.clear();
                    state->finished = true;
                    state->finished_ready.notify_all();
                    return;
                }
            }
        });
        record("hello", {{"schema", "1"},
                         {"version", VOVE_VERSION},
                         {"build", VOVE_BUILD_LABEL},
                         {"platform", platform_name()},
                         {"arch", architecture_name()}});
        return true;
    } catch (...) {
        state_.reset();
        return false;
    }
}

bool DiagnosticSink::active() const noexcept {
    return state_ && !state_->stopping.load(std::memory_order_relaxed);
}

void DiagnosticSink::record(const std::string_view event,
                            const std::initializer_list<DiagnosticField> fields) noexcept {
    if (!active()) {
        return;
    }
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping.load(std::memory_order_relaxed)) {
            return;
        }
        if (state_->lines.size() >= kMaximumQueuedLines) {
            ++state_->dropped;
            return;
        }
        if (state_->dropped != 0U && state_->lines.size() + 1U < kMaximumQueuedLines) {
            const auto count = std::to_string(state_->dropped);
            auto overflow =
                make_line(state_->token, ++state_->sequence, "sink.overflow", {{"dropped", count}});
            if (!overflow.empty()) {
                state_->lines.push_back(std::move(overflow));
                state_->dropped = 0;
            }
        }
        auto line = make_line(state_->token, ++state_->sequence, event, fields);
        if (!line.empty()) {
            state_->lines.push_back(std::move(line));
            if (event != "heartbeat") {
                state_->last_event.assign(event);
            }
            state_->ready.notify_one();
        }
    } catch (...) {
        ++state_->dropped;
    }
}

void DiagnosticSink::heartbeat() noexcept {
    if (!active())
        return;
    std::string last_event;
    {
        std::lock_guard lock(state_->mutex);
        last_event = state_->last_event;
    }
    record("heartbeat", {{"state", "event_loop"}, {"last_event", last_event}});
}

void DiagnosticSink::shutdown() noexcept {
    if (!state_) {
        return;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (state_->dropped != 0U) {
            if (state_->lines.size() >= kMaximumQueuedLines) {
                state_->lines.pop_back();
                ++state_->dropped;
            }
            const auto count = std::to_string(state_->dropped);
            auto overflow =
                make_line(state_->token, ++state_->sequence, "sink.overflow", {{"dropped", count}});
            if (!overflow.empty())
                state_->lines.push_back(std::move(overflow));
            state_->dropped = 0;
        }
    }
    state_->stopping.store(true, std::memory_order_relaxed);
    state_->ready.notify_all();
    if (state_->writer.joinable()) {
#ifdef _WIN32
        {
            std::unique_lock lock(state_->mutex);
            if (!state_->finished_ready.wait_for(lock, std::chrono::milliseconds(250),
                                                 [this] { return state_->finished; })) {
                static_cast<void>(CancelSynchronousIo(state_->writer.native_handle()));
            }
        }
#endif
        state_->writer.join();
    }
#ifdef _WIN32
    if (state_->endpoint != INVALID_HANDLE_VALUE) {
        CloseHandle(state_->endpoint);
    }
#else
    if (state_->endpoint >= 0) {
        ::close(state_->endpoint);
    }
#endif
    state_.reset();
}

DiagnosticSink::~DiagnosticSink() {
    shutdown();
}

} // namespace vove::ui
