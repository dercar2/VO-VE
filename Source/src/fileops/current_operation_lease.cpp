#include "vove/fileops/current_operation_lease.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <utility>

namespace vove::fileops {
namespace {

std::filesystem::path lock_path(const std::filesystem::path &journal_path) {
    auto result = journal_path;
    result += ".lock";
    return result;
}

} // namespace

CurrentOperationLease::~CurrentOperationLease() {
    release();
}

CurrentOperationLease::CurrentOperationLease(CurrentOperationLease &&other) noexcept
    : native_(std::exchange(other.native_, -1)), journal_path_(std::move(other.journal_path_)) {}

CurrentOperationLease &CurrentOperationLease::operator=(CurrentOperationLease &&other) noexcept {
    if (this != &other) {
        release();
        native_ = std::exchange(other.native_, -1);
        journal_path_ = std::move(other.journal_path_);
    }
    return *this;
}

CurrentOperationLease CurrentOperationLease::try_acquire(const std::filesystem::path &journal_path,
                                                         std::error_code &error) noexcept {
    error.clear();
    if (journal_path.empty()) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }
    try {
        auto protected_path = journal_path;
        const auto parent = journal_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) {
                return {};
            }
        }
#ifdef _WIN32
        const auto handle =
            CreateFileW(lock_path(journal_path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            return {};
        }
        return CurrentOperationLease(reinterpret_cast<std::intptr_t>(handle),
                                     std::move(protected_path));
#else
        const auto path = lock_path(journal_path);
        const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            error = std::error_code(errno, std::generic_category());
            return {};
        }
        if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const auto code = errno;
            static_cast<void>(close(descriptor));
            error = std::error_code(code, std::generic_category());
            return {};
        }
        return CurrentOperationLease(static_cast<std::intptr_t>(descriptor),
                                     std::move(protected_path));
#endif
    } catch (const std::filesystem::filesystem_error &failure) {
        error = failure.code();
    } catch (...) {
        error = std::make_error_code(std::errc::io_error);
    }
    return {};
}

bool CurrentOperationLease::owns_lock() const noexcept {
    return native_ != -1;
}

bool CurrentOperationLease::protects(const std::filesystem::path &journal_path) const {
    return owns_lock() && journal_path_ == journal_path;
}

void CurrentOperationLease::release() noexcept {
    if (!owns_lock()) {
        return;
    }
#ifdef _WIN32
    static_cast<void>(CloseHandle(reinterpret_cast<HANDLE>(native_)));
#else
    static_cast<void>(flock(static_cast<int>(native_), LOCK_UN));
    static_cast<void>(close(static_cast<int>(native_)));
#endif
    native_ = -1;
}

} // namespace vove::fileops
