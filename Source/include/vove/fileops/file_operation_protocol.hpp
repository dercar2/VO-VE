#pragma once

#include "vove/fileops/file_operation.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace vove::fileops {

enum class OperationRequestKind : std::uint8_t {
    rename,
    permanent_delete,
    create_directory,
};

[[nodiscard]] std::vector<std::byte> encode_rename_request(const RenameRequest &request);
[[nodiscard]] bool decode_rename_request(std::span<const std::byte> payload, RenameRequest &request,
                                         std::string &error);
[[nodiscard]] std::vector<std::byte> encode_delete_request(const DeleteRequest &request);
[[nodiscard]] bool decode_delete_request(std::span<const std::byte> payload, DeleteRequest &request,
                                         std::string &error);
[[nodiscard]] std::vector<std::byte>
encode_create_directory_request(const CreateDirectoryRequest &request);
[[nodiscard]] bool decode_create_directory_request(std::span<const std::byte> payload,
                                                   CreateDirectoryRequest &request,
                                                   std::string &error);
[[nodiscard]] bool decode_operation_request_kind(std::span<const std::byte> payload,
                                                 OperationRequestKind &kind, std::string &error);
[[nodiscard]] std::vector<std::byte> encode_operation_result(const OperationResult &result);
[[nodiscard]] bool decode_operation_result(std::span<const std::byte> payload,
                                           OperationResult &result, std::string &error);

} // namespace vove::fileops
