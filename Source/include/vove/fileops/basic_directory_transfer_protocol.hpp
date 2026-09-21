#pragma once

#include "vove/fileops/basic_directory_transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kBasicDirectoryTransferProtocolVersion = 5U;
inline constexpr std::size_t kMaximumBasicDirectoryTransferProtocolBytes = 64U * 1024U;

enum class BasicDirectoryTransferCommand : std::uint8_t {
    copy,
    move,
    resume,
    discover_and_resume,
    discover_and_discard,
    discard,
};

enum class BasicDirectoryTransferMessageKind : std::uint8_t {
    progress,
    complete,
};

struct BasicDirectoryTransferStreamRequest {
    std::uint64_t request_id{};
    std::uint64_t operation_id{};
    BasicDirectoryTransferCommand command{BasicDirectoryTransferCommand::copy};
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path manifest_path;
    std::string expected_source_revision_utf8;
};

struct BasicDirectoryTransferCompletionAck {
    std::uint64_t request_id{};
};

struct BasicDirectoryTransferProgressFrame {
    std::uint64_t request_id{};
    BasicDirectoryTransferProgress progress;
};

struct BasicDirectoryTransferResultFrame {
    std::uint64_t request_id{};
    BasicDirectoryTransferResult result;
};

[[nodiscard]] bool
is_basic_directory_transfer_heartbeat(const BasicDirectoryTransferProgress &previous,
                                      const BasicDirectoryTransferProgress &current) noexcept;

[[nodiscard]] bool
valid_basic_directory_transfer_stream_request(const BasicDirectoryTransferStreamRequest &request,
                                              std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_basic_directory_transfer_stream_request(const BasicDirectoryTransferStreamRequest &request);
[[nodiscard]] bool
decode_basic_directory_transfer_stream_request(std::span<const std::byte> payload,
                                               BasicDirectoryTransferStreamRequest &request,
                                               std::string &detail_utf8);

[[nodiscard]] std::vector<std::byte>
encode_basic_directory_transfer_progress_frame(const BasicDirectoryTransferProgressFrame &frame);
[[nodiscard]] bool
decode_basic_directory_transfer_progress_frame(std::span<const std::byte> payload,
                                               BasicDirectoryTransferProgressFrame &frame,
                                               std::string &detail_utf8);

[[nodiscard]] std::vector<std::byte>
encode_basic_directory_transfer_completion_ack(const BasicDirectoryTransferCompletionAck &ack);
[[nodiscard]] bool
decode_basic_directory_transfer_completion_ack(std::span<const std::byte> payload,
                                               BasicDirectoryTransferCompletionAck &ack,
                                               std::string &detail_utf8);

[[nodiscard]] std::vector<std::byte>
encode_basic_directory_transfer_result_frame(const BasicDirectoryTransferResultFrame &frame);
[[nodiscard]] bool
decode_basic_directory_transfer_result_frame(std::span<const std::byte> payload,
                                             BasicDirectoryTransferResultFrame &frame,
                                             std::string &detail_utf8);

[[nodiscard]] bool
decode_basic_directory_transfer_message_kind(std::span<const std::byte> payload,
                                             BasicDirectoryTransferMessageKind &kind,
                                             std::string &detail_utf8);

} // namespace vove::fileops
