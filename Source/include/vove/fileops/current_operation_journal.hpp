#pragma once

#include "vove/fileops/durable_journal.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kCurrentOperationJournalSchemaVersion = 1U;
inline constexpr std::size_t kCurrentOperationJournalHeaderBytes = 24U;
inline constexpr std::size_t kMaximumCurrentOperationPayloadBytes =
    kMaximumDurableJournalPayloadBytes - kCurrentOperationJournalHeaderBytes;
// Keep the Stage 7B physical name so an interrupted rename remains recoverable after upgrading.
// The typed envelope, not the filename, now distinguishes all current-operation kinds.
inline constexpr auto kCurrentOperationJournalFilename = "batch-rename.journal";

enum class CurrentOperationKind : std::uint32_t {
    batch_rename = 1U,
    permanent_delete = 2U,
    trash_move = 3U,
    trash_restore = 4U,
    trash_purge = 5U,
    file_transfer = 6U,
    directory_transfer = 7U,
    directory_leaf_transfer = 8U,
    object_transfer = 9U,
};

enum class CurrentOperationJournalEncoding : std::uint8_t {
    none,
    typed,
    legacy_untyped,
};

struct CurrentOperationJournalReadResult : DurableJournalResult {
    DurableJournalReadSource source{DurableJournalReadSource::none};
    CurrentOperationJournalEncoding encoding{CurrentOperationJournalEncoding::none};
    CurrentOperationKind kind{};
    std::vector<std::byte> payload;
};

// Adds an operation type to the crash-safe physical record. Untyped records are returned only for
// strict legacy decoding by the operation that owned the pre-envelope format.
class CurrentOperationJournalStore final {
  public:
    explicit CurrentOperationJournalStore(std::filesystem::path primary_path);

    [[nodiscard]] DurableJournalResult write(CurrentOperationKind kind,
                                             std::span<const std::byte> payload) const;
    [[nodiscard]] CurrentOperationJournalReadResult read() const;
    [[nodiscard]] DurableJournalResult remove() const;

    [[nodiscard]] const std::filesystem::path &primary_path() const noexcept;
    [[nodiscard]] const std::filesystem::path &previous_path() const noexcept;
    [[nodiscard]] const std::filesystem::path &temporary_path() const noexcept;

  private:
    DurableJournalStore durable_;
};

} // namespace vove::fileops
