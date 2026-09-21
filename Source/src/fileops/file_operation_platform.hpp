#pragma once

#include "vove/fileops/file_operation.hpp"

#include <filesystem>
#include <cstdint>

namespace vove::fileops::detail {

[[nodiscard]] OperationResult rename_no_replace(const RenameRequest &request);
[[nodiscard]] OperationResult verify_rename_destination_anchor(const RenameRequest &request);
[[nodiscard]] OperationResult permanent_delete_remote(const DeleteRequest &request);
[[nodiscard]] OperationResult create_directory_relative(const CreateDirectoryRequest &request);
[[nodiscard]] OperationResult
verify_private_empty_directory_relative(const CreateDirectoryRequest &request);
[[nodiscard]] OperationResult
remove_empty_directory_relative(const CreateDirectoryRequest &request);

#ifdef VOVE_FILEOP_TEST_HOOKS
using BeforeDirectoryPinHook = void (*)(const std::filesystem::path &source_path);
using PinnedSourcePathHook = void (*)(std::filesystem::path &observed_path);
using BeforeRenameCommitHook = void (*)();
using BeforeTransferRenameSyscallHook = void (*)();
using AfterTransferRenameSyscallHook = void (*)();
using BeforeTransferRenameRollbackHook = void (*)();
using BeforeTransferSourceDeleteHook = void (*)();
using DeleteCloseErrorHook = std::uint32_t (*)();
using BeforeCreateDirectoryCommitHook = void (*)();
using AfterCreateDirectoryCommitHook = void (*)();
void set_before_directory_pin_hook(BeforeDirectoryPinHook hook) noexcept;
void set_pinned_source_path_hook(PinnedSourcePathHook hook) noexcept;
void set_before_rename_commit_hook(BeforeRenameCommitHook hook) noexcept;
void set_before_transfer_rename_syscall_hook(BeforeTransferRenameSyscallHook hook) noexcept;
void set_after_transfer_rename_syscall_hook(AfterTransferRenameSyscallHook hook) noexcept;
void set_before_transfer_rename_rollback_hook(BeforeTransferRenameRollbackHook hook) noexcept;
void set_before_transfer_source_delete_hook(BeforeTransferSourceDeleteHook hook) noexcept;
void set_delete_close_error_hook(DeleteCloseErrorHook hook) noexcept;
void set_before_create_directory_commit_hook(BeforeCreateDirectoryCommitHook hook) noexcept;
void set_after_create_directory_commit_hook(AfterCreateDirectoryCommitHook hook) noexcept;
#ifdef _WIN32
[[nodiscard]] std::filesystem::path stable_path_root_for_test(const std::filesystem::path &path);
[[nodiscard]] bool lexical_same_path_for_test(const std::filesystem::path &left,
                                              const std::filesystem::path &right);
#endif
#endif

} // namespace vove::fileops::detail
