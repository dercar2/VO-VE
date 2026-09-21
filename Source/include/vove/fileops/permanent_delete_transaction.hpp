#pragma once

#include "vove/fileops/file_operation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kPermanentDeleteTransactionVersion = 1;
inline constexpr std::size_t kMaximumPermanentDeleteItems = 10'000;

enum class PermanentDeletePhase : std::uint8_t {
    prepared,
    evacuating,
    rollback,
    permanent_delete_intent,
    deleting,
};

enum class PermanentDeleteItemLocation : std::uint8_t {
    source,
    temporary,
    deleted,
};

enum class PermanentDeleteStep : std::uint8_t {
    none,
    evacuate,
    rollback,
    erase,
};

struct PermanentDeleteSource {
    std::filesystem::path path;
    SourceSnapshot snapshot;
};

struct PermanentDeleteItem {
    std::filesystem::path source;
    std::filesystem::path temporary;
    std::filesystem::path current;
    SourceSnapshot current_snapshot;
    PermanentDeleteItemLocation location{PermanentDeleteItemLocation::source};
};

struct PermanentDeleteTransaction {
    std::uint32_t version{kPermanentDeleteTransactionVersion};
    std::uint64_t operation_id{};
    PermanentDeletePhase phase{PermanentDeletePhase::prepared};
    PermanentDeleteStep active_step{PermanentDeleteStep::none};
    std::uint32_t active_index{};
    OperationEvidence active_evidence{OperationEvidence::none};
    OperationStatus failure_status{OperationStatus::success};
    std::string failure_detail_utf8;
    std::vector<PermanentDeleteItem> items;
};

[[nodiscard]] bool valid_permanent_delete_transaction(const PermanentDeleteTransaction &transaction,
                                                      std::string &detail_utf8);
[[nodiscard]] PermanentDeleteTransaction
prepare_permanent_delete_transaction(const std::vector<PermanentDeleteSource> &sources,
                                     std::uint64_t operation_id);
[[nodiscard]] std::vector<std::byte>
encode_permanent_delete_transaction(const PermanentDeleteTransaction &transaction);
[[nodiscard]] bool decode_permanent_delete_transaction(std::span<const std::byte> payload,
                                                       PermanentDeleteTransaction &transaction,
                                                       std::string &detail_utf8);

} // namespace vove::fileops
