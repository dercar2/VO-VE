#pragma once

#include "vove/fileops/file_transfer_protocol.hpp"

#include <functional>
#include <memory>

namespace vove::fileops::detail {

class FileTransferStreamSession {
  public:
    using Progress = std::function<bool(const FileTransferProgress &)>;

    virtual ~FileTransferStreamSession() = default;

    FileTransferStreamSession(const FileTransferStreamSession &) = delete;
    FileTransferStreamSession &operator=(const FileTransferStreamSession &) = delete;

    [[nodiscard]] virtual const FileTransferReservation &reservation() const noexcept = 0;
    [[nodiscard]] virtual FileTransferStreamResult stream(const Progress &progress) = 0;

  protected:
    FileTransferStreamSession() = default;
};

struct FileTransferBeginResult {
    std::unique_ptr<FileTransferStreamSession> session;
    FileTransferStreamResult failure;
};

[[nodiscard]] FileTransferBeginResult
begin_file_transfer_stream(const FileTransferStreamRequest &request);

} // namespace vove::fileops::detail
