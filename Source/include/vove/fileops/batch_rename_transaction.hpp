#pragma once

#include "vove/fileops/batch_rename.hpp"
#include "vove/fileops/file_operation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kBatchRenameTransactionVersion = 3;
inline constexpr std::size_t kMaximumBatchRenameItems = 10'000;

enum class BatchTransactionPhase : std::uint8_t {
    prepared,
    evacuating,
    rollback,
    commit_intent,
    publishing,
};

enum class BatchItemLocation : std::uint8_t {
    source,
    temporary,
    destination,
};

enum class BatchStepKind : std::uint8_t {
    none,
    evacuate,
    rollback,
    publish,
};

struct BatchTransactionItem {
    std::filesystem::path source;
    std::filesystem::path temporary;
    std::filesystem::path destination;
    std::filesystem::path current;
    SourceSnapshot current_snapshot;
    BatchItemLocation location{BatchItemLocation::source};
    OperationObjectKind object_kind{OperationObjectKind::regular_file};
};

struct BatchRenameTransaction {
    std::uint32_t version{kBatchRenameTransactionVersion};
    std::uint64_t operation_id{};
    BatchTransactionPhase phase{BatchTransactionPhase::prepared};
    BatchStepKind active_step{BatchStepKind::none};
    std::uint32_t active_index{};
    OperationEvidence active_evidence{OperationEvidence::none};
    OperationStatus failure_status{OperationStatus::success};
    std::string failure_detail_utf8;
    std::vector<BatchTransactionItem> items;
};

[[nodiscard]] bool valid_batch_transaction(const BatchRenameTransaction &transaction,
                                           std::string &detail_utf8);
// Unchanged rows are validated but omitted; an empty result is a no-op, not a journal.
[[nodiscard]] BatchRenameTransaction
prepare_batch_transaction(const BatchRenamePlan &plan,
                          const std::vector<BatchRenameSource> &sources,
                          std::uint64_t operation_id);
[[nodiscard]] std::vector<std::byte>
encode_batch_transaction(const BatchRenameTransaction &transaction);
[[nodiscard]] bool decode_batch_transaction(std::span<const std::byte> payload,
                                            BatchRenameTransaction &transaction,
                                            std::string &detail_utf8);

} // namespace vove::fileops
