#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

namespace vove::fileops {

inline constexpr std::size_t kMaximumDurableJournalPayloadBytes = std::size_t{4} * 1024U * 1024U;
inline constexpr std::size_t kAbsoluteMaximumDurableJournalPayloadBytes =
    std::size_t{64} * 1024U * 1024U;
inline constexpr std::uint32_t kDurableJournalSchemaVersion = 1U;

enum class DurableJournalStatus : std::uint8_t {
    success,
    not_found,
    corrupt,
    payload_mismatch,
    payload_too_large,
    io_error,
};

enum class DurableJournalReadSource : std::uint8_t {
    none,
    primary,
    previous,
};

struct DurableJournalResult {
    DurableJournalStatus status{DurableJournalStatus::io_error};
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == DurableJournalStatus::success;
    }
};

struct DurableJournalReadResult : DurableJournalResult {
    DurableJournalReadSource source{DurableJournalReadSource::none};
    std::vector<std::byte> payload;
};

class DurableJournalStore final {
  public:
    explicit DurableJournalStore(
        std::filesystem::path primary_path,
        std::size_t maximum_payload_bytes = kMaximumDurableJournalPayloadBytes);

    [[nodiscard]] DurableJournalResult write(std::span<const std::byte> payload) const;
    [[nodiscard]] DurableJournalReadResult read() const;
    [[nodiscard]] DurableJournalReadResult read_primary_generation() const;
    [[nodiscard]] DurableJournalReadResult read_previous_generation() const;
    [[nodiscard]] DurableJournalResult remove() const;
    [[nodiscard]] DurableJournalResult
    remove_if_payload_matches(std::span<const std::byte> expected_payload) const;

    [[nodiscard]] const std::filesystem::path &primary_path() const noexcept;
    [[nodiscard]] const std::filesystem::path &previous_path() const noexcept;
    [[nodiscard]] const std::filesystem::path &temporary_path() const noexcept;
    [[nodiscard]] std::size_t maximum_payload_bytes() const noexcept;

  private:
    std::filesystem::path primary_path_;
    std::filesystem::path previous_path_;
    std::filesystem::path temporary_path_;
    std::size_t maximum_payload_bytes_{};
};

} // namespace vove::fileops
