#pragma once

#include "vove/fileops/file_transfer_transaction.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

inline constexpr std::uint32_t kFileTransferProtocolVersion = 7;
inline constexpr std::size_t kMaximumFileTransferProtocolBytes = 64U * 1024U;
inline constexpr std::uint64_t kFileTransferProgressChunkBytes = 1024U * 1024U;
inline constexpr std::size_t kFileTransferReservationMarkerBytes = 40;

enum class FileTransferStreamMode : std::uint8_t {
    reserve_new,
    resume_existing,
    audit_existing,
    probe_reservation,
};

enum class FileTransferMessageKind : std::uint8_t {
    reservation,
    progress,
    complete,
};

struct FileTransferStreamRequest {
    std::uint64_t operation_id{};
    std::uint32_t item_index{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
    FileTransferStreamMode mode{FileTransferStreamMode::reserve_new};
    std::filesystem::path source;
    std::filesystem::path temp_destination;
    SourceSnapshot expected_source;
    SourceSnapshot expected_temp;
    std::string source_parent_identity_utf8;
    std::string destination_parent_identity_utf8;
    std::filesystem::path destination_anchor_path;
    std::string destination_anchor_identity_utf8;
};

struct FileTransferReservation {
    std::uint64_t operation_id{};
    std::uint32_t item_index{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
    SourceSnapshot temp_snapshot;
    std::string destination_parent_identity_utf8;
    std::string destination_parent_revision_utf8;
};

struct FileTransferReservationAck {
    std::uint64_t operation_id{};
    std::uint32_t item_index{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
    bool accepted{};
};

struct FileTransferProgress {
    std::uint64_t operation_id{};
    std::uint32_t item_index{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
    std::uint64_t bytes_written{};
};

struct FileTransferStreamResult {
    std::uint64_t operation_id{};
    std::uint32_t item_index{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
    OperationStatus status{OperationStatus::io_error};
    OperationEvidence evidence{OperationEvidence::none};
    std::int64_t platform_code{};
    SourceSnapshot source_snapshot;
    SourceSnapshot temp_snapshot;
    std::array<std::uint8_t, kFileTransferDigestBytes> content_sha256{};
    std::uint64_t bytes_written{};
    std::string detail_utf8;

    [[nodiscard]] bool ok() const noexcept {
        return status == OperationStatus::success;
    }
};

struct FileTransferReservationMarkerAddress {
    std::uint64_t operation_id{};
    std::uint32_t item_index{};
    std::array<std::uint8_t, kFileTransferRequestTokenBytes> request_token{};
};

[[nodiscard]] std::array<std::byte, kFileTransferReservationMarkerBytes>
make_file_transfer_reservation_marker(const FileTransferReservationMarkerAddress &address) noexcept;
[[nodiscard]] bool matches_file_transfer_reservation_marker(
    std::span<const std::byte> marker,
    const FileTransferReservationMarkerAddress &address) noexcept;

[[nodiscard]] bool valid_file_transfer_stream_request(const FileTransferStreamRequest &request,
                                                      std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_file_transfer_stream_request(const FileTransferStreamRequest &request);
[[nodiscard]] bool decode_file_transfer_stream_request(std::span<const std::byte> payload,
                                                       FileTransferStreamRequest &request,
                                                       std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_file_transfer_reservation(const FileTransferReservation &reservation);
[[nodiscard]] bool decode_file_transfer_reservation(std::span<const std::byte> payload,
                                                    FileTransferReservation &reservation,
                                                    std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_file_transfer_reservation_ack(const FileTransferReservationAck &ack);
[[nodiscard]] bool decode_file_transfer_reservation_ack(std::span<const std::byte> payload,
                                                        FileTransferReservationAck &ack,
                                                        std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_file_transfer_progress(const FileTransferProgress &progress);
[[nodiscard]] bool decode_file_transfer_progress(std::span<const std::byte> payload,
                                                 FileTransferProgress &progress,
                                                 std::string &detail_utf8);
[[nodiscard]] std::vector<std::byte>
encode_file_transfer_stream_result(const FileTransferStreamResult &result);
[[nodiscard]] bool decode_file_transfer_stream_result(std::span<const std::byte> payload,
                                                      FileTransferStreamResult &result,
                                                      std::string &detail_utf8);
[[nodiscard]] bool decode_file_transfer_message_kind(std::span<const std::byte> payload,
                                                     FileTransferMessageKind &kind,
                                                     std::string &detail_utf8);

} // namespace vove::fileops
