#pragma once

#include "vove/fileops/file_operation.hpp"

namespace vove::fileops::detail {

[[nodiscard]] OperationResult execute_rename(const RenameRequest &request);
[[nodiscard]] OperationResult reconcile_rename(const RenameRequest &request);
[[nodiscard]] OperationResult execute_delete(const DeleteRequest &request);
[[nodiscard]] OperationResult reconcile_delete(const DeleteRequest &request);
[[nodiscard]] OperationResult execute_create_directory(const CreateDirectoryRequest &request);
[[nodiscard]] OperationResult reconcile_create_directory(const CreateDirectoryRequest &request);

} // namespace vove::fileops::detail
