#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>

namespace vove::fileops {

// A cross-process owner token for the single durable current-operation journal. The lock file is
// intentionally persistent: removing it would allow two processes to lock different inodes.
class CurrentOperationLease final {
  public:
    CurrentOperationLease() = default;
    ~CurrentOperationLease();

    CurrentOperationLease(const CurrentOperationLease &) = delete;
    CurrentOperationLease &operator=(const CurrentOperationLease &) = delete;
    CurrentOperationLease(CurrentOperationLease &&other) noexcept;
    CurrentOperationLease &operator=(CurrentOperationLease &&other) noexcept;

    [[nodiscard]] static CurrentOperationLease
    try_acquire(const std::filesystem::path &journal_path, std::error_code &error) noexcept;

    [[nodiscard]] bool owns_lock() const noexcept;
    [[nodiscard]] bool protects(const std::filesystem::path &journal_path) const;
    void release() noexcept;

  private:
    CurrentOperationLease(std::intptr_t native, std::filesystem::path journal_path) noexcept
        : native_(native), journal_path_(std::move(journal_path)) {}

    std::intptr_t native_{-1};
    std::filesystem::path journal_path_;
};

} // namespace vove::fileops
