#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../file_transfer_executor.hpp"
#include "../file_in_use_error.hpp"
#include "../sha256.hpp"
#include "windows/file_identity.hpp"
#include "windows/file_data_streams.hpp"
#include "windows/file_time.hpp"
#include "windows/filesystem_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vove::fileops::detail {
namespace {

constexpr std::size_t transferBufferBytes =
    static_cast<std::size_t>(kFileTransferProgressChunkBytes);

class ScopedHandle final {
  public:
    explicit ScopedHandle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        close();
    }

    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    ScopedHandle(ScopedHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    ScopedHandle &operator=(ScopedHandle &&other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    void close() noexcept {
        if (valid()) {
            static_cast<void>(CloseHandle(std::exchange(handle_, INVALID_HANDLE_VALUE)));
        }
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

OperationStatus status_from_error(const DWORD code) noexcept {
    if (file_in_use_error(code)) {
        return OperationStatus::file_in_use;
    }
    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
        return OperationStatus::conflict;
    }
    if (code == ERROR_NOT_SAME_DEVICE || code == ERROR_NOT_SUPPORTED ||
        code == ERROR_INVALID_FUNCTION) {
        return OperationStatus::unsupported;
    }
    using Error = platform::detail::WindowsFilesystemErrorKind;
    switch (platform::detail::classify_windows_filesystem_error(code)) {
    case Error::not_found:
        return OperationStatus::not_found;
    case Error::permission_denied:
        return OperationStatus::permission_denied;
    case Error::authentication_required:
        return OperationStatus::authentication_required;
    case Error::timed_out:
        return OperationStatus::timed_out;
    case Error::disconnected:
        return OperationStatus::disconnected;
    case Error::io_error:
        return OperationStatus::io_error;
    }
    return OperationStatus::io_error;
}

FileTransferStreamResult failure(const FileTransferStreamRequest &request, const DWORD code,
                                 const OperationEvidence evidence, std::string detail) {
    if (detail.empty()) {
        detail = "Windows error " + std::to_string(code);
    }
    FileTransferStreamResult result;
    result.operation_id = request.operation_id;
    result.item_index = request.item_index;
    result.request_token = request.request_token;
    result.status = status_from_error(code);
    result.evidence = evidence;
    result.platform_code = static_cast<std::int64_t>(code);
    result.detail_utf8 = std::move(detail);
    return result;
}

FileTransferStreamResult rejected(const FileTransferStreamRequest &request,
                                  const OperationStatus status, std::string detail) {
    FileTransferStreamResult result;
    result.operation_id = request.operation_id;
    result.item_index = request.item_index;
    result.request_token = request.request_token;
    result.status = status;
    result.evidence = OperationEvidence::no_commit;
    result.detail_utf8 = std::move(detail);
    return result;
}

std::optional<std::filesystem::path> final_path(const HANDLE handle) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const auto written = GetFinalPathNameByHandleW(handle, buffer.data(), required, flags);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    buffer.resize(written);
    return std::filesystem::path(std::move(buffer));
}

std::wstring comparable_path(const std::filesystem::path &path) {
    auto text = path.lexically_normal().native();
    std::ranges::replace(text, L'/', L'\\');
    constexpr std::wstring_view extendedUncPrefix = LR"(\\?\UNC\)";
    constexpr std::wstring_view extendedPrefix = LR"(\\?\)";
    if (std::wstring_view(text).starts_with(extendedUncPrefix)) {
        text = LR"(\\)" + text.substr(extendedUncPrefix.size());
    } else if (std::wstring_view(text).starts_with(extendedPrefix)) {
        text.erase(0, extendedPrefix.size());
    }
    while (text.size() > 1U && text.back() == L'\\') {
        text.pop_back();
    }
    return text;
}

bool same_path(const std::filesystem::path &left, const std::filesystem::path &right) {
    const auto volume_guid_path =
        [](const std::filesystem::path &path) -> std::optional<std::wstring> {
        std::array<wchar_t, 32'768> full{};
        const auto written =
            GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (written == 0 || static_cast<std::size_t>(written) >= full.size()) {
            return std::nullopt;
        }
        std::array<wchar_t, 32'768> volumeRoot{};
        if (GetVolumePathNameW(full.data(), volumeRoot.data(),
                               static_cast<DWORD>(volumeRoot.size())) == FALSE) {
            return std::nullopt;
        }
        std::array<wchar_t, MAX_PATH + 1U> volumeGuid{};
        if (GetVolumeNameForVolumeMountPointW(volumeRoot.data(), volumeGuid.data(),
                                              static_cast<DWORD>(volumeGuid.size())) == FALSE) {
            return std::nullopt;
        }
        const std::wstring_view absolute(full.data(), written);
        const std::wstring_view root(volumeRoot.data());
        if (absolute.size() < root.size() ||
            CompareStringOrdinal(full.data(), static_cast<int>(root.size()), volumeRoot.data(),
                                 static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) {
            return std::nullopt;
        }
        std::wstring canonical(volumeGuid.data());
        canonical.append(absolute.substr(root.size()));
        return canonical;
    };
    const auto leftGuid = volume_guid_path(left);
    const auto rightGuid = volume_guid_path(right);
    if (leftGuid && rightGuid) {
        return CompareStringOrdinal(leftGuid->data(), static_cast<int>(leftGuid->size()),
                                    rightGuid->data(), static_cast<int>(rightGuid->size()),
                                    TRUE) == CSTR_EQUAL;
    }
    const auto leftText = comparable_path(left);
    const auto rightText = comparable_path(right);
    return CompareStringOrdinal(leftText.data(), static_cast<int>(leftText.size()),
                                rightText.data(), static_cast<int>(rightText.size()),
                                TRUE) == CSTR_EQUAL;
}

std::filesystem::path stable_path_root(const std::filesystem::path &path) {
    const auto &text = path.native();
    constexpr std::wstring_view extendedUncPrefix = LR"(\\?\UNC\)";
    if (text.starts_with(extendedUncPrefix)) {
        const auto serverEnd = text.find(L'\\', extendedUncPrefix.size());
        if (serverEnd != std::wstring::npos) {
            const auto shareEnd = text.find(L'\\', serverEnd + 1U);
            return std::filesystem::path(text.substr(0, shareEnd));
        }
    }
    constexpr std::wstring_view extendedPrefix = LR"(\\?\)";
    if (text.starts_with(extendedPrefix)) {
        const auto componentEnd = text.find(L'\\', extendedPrefix.size());
        if (componentEnd != std::wstring::npos) {
            return std::filesystem::path(text.substr(0, componentEnd + 1U));
        }
    }
    return path.root_path();
}

struct SnapshotQuery {
    SourceSnapshot snapshot;
    FILE_BASIC_INFO basic{};
    DWORD error{ERROR_SUCCESS};

    [[nodiscard]] bool ok() const noexcept {
        return !snapshot.source_revision_utf8.empty();
    }
};

SnapshotQuery snapshot_for_handle(const HANDLE handle, const std::string_view expectedRevision) {
    SnapshotQuery result;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0 ||
        GetFileInformationByHandleEx(handle, FileBasicInfo, &result.basic, sizeof(result.basic)) ==
            FALSE) {
        result.error = GetLastError();
        return result;
    }
    const auto identity =
        platform::windows_detail::query_file_identity(handle, result.basic.ChangeTime);
    const auto &revision =
        expectedRevision.empty()
            ? platform::windows_detail::preferred_revision(identity)
            : platform::windows_detail::matching_revision(identity, expectedRevision);
    if (revision.empty()) {
        result.error = identity.error;
        return result;
    }
    result.snapshot = {
        .size_bytes = static_cast<std::uint64_t>(size.QuadPart),
        .modified_unix_ns =
            platform::windows_detail::filetime_to_unix_ns(result.basic.LastWriteTime),
        .source_revision_utf8 = revision,
    };
    return result;
}

std::optional<std::string>
directory_revision(const HANDLE handle, const std::string_view expectedRevision, DWORD &error) {
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        error = GetLastError();
        return std::nullopt;
    }
    const auto identity = platform::windows_detail::query_file_identity(handle, basic.ChangeTime);
    const auto &revision =
        expectedRevision.empty()
            ? platform::windows_detail::preferred_revision(identity)
            : platform::windows_detail::matching_revision(identity, expectedRevision);
    if (revision.empty()) {
        error = identity.error;
        return std::nullopt;
    }
    error = ERROR_SUCCESS;
    return revision;
}

struct PinnedDirectory {
    ScopedHandle handle;
    std::filesystem::path path;
};

struct PinnedChainResult {
    std::vector<PinnedDirectory> chain;
    FileTransferStreamResult failure;
};

PinnedChainResult pin_directory_chain(const FileTransferStreamRequest &request,
                                      const std::filesystem::path &leafDirectory,
                                      const std::string &expectedLeafIdentity,
                                      const std::string_view purpose,
                                      const bool require_destination_anchor) {
    PinnedChainResult result;
    bool destination_anchor_seen = !require_destination_anchor;
    auto directory = leafDirectory;
    const auto root = stable_path_root(directory);
    if (directory.empty() || root.empty() || same_path(directory, root)) {
        result.failure = rejected(request, OperationStatus::unsupported,
                                  std::string(purpose) + " directory has no pinnable parent");
        return result;
    }
    while (!same_path(directory, root)) {
        ScopedHandle handle(
            CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid()) {
            result.failure = failure(request, GetLastError(), OperationEvidence::no_commit,
                                     std::string(purpose) + " directory could not be pinned");
            result.chain.clear();
            return result;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                         sizeof(attributes)) == FALSE) {
            result.failure = failure(request, GetLastError(), OperationEvidence::no_commit,
                                     std::string(purpose) + " directory metadata is unavailable");
            result.chain.clear();
            return result;
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            result.failure =
                rejected(request, OperationStatus::unsupported,
                         std::string(purpose) + " directory chain contains a reparse point");
            result.chain.clear();
            return result;
        }
        const auto openedPath = final_path(handle.get());
        if (!openedPath || !same_path(*openedPath, directory)) {
            result.failure = rejected(request, OperationStatus::source_changed,
                                      std::string(purpose) + " directory path changed while open");
            result.chain.clear();
            return result;
        }
        if (result.chain.empty()) {
            DWORD revisionError{};
            const auto revision = directory_revision(handle.get(), {}, revisionError);
            if (!revision) {
                result.failure =
                    failure(request, revisionError, OperationEvidence::no_commit,
                            std::string(purpose) + " directory identity is unavailable");
                result.chain.clear();
                return result;
            }
            if (stable_object_identity(*revision) != expectedLeafIdentity) {
                result.failure = rejected(request, OperationStatus::source_changed,
                                          std::string(purpose) + " directory identity changed");
                result.chain.clear();
                return result;
            }
        }
        if (require_destination_anchor && same_path(*openedPath, request.destination_anchor_path)) {
            DWORD revisionError{};
            const auto revision = directory_revision(handle.get(), {}, revisionError);
            if (!revision) {
                result.failure = failure(request, revisionError, OperationEvidence::no_commit,
                                         "destination staging-root identity is unavailable");
                result.chain.clear();
                return result;
            }
            if (stable_object_identity(*revision) != request.destination_anchor_identity_utf8) {
                result.failure = rejected(request, OperationStatus::source_changed,
                                          "destination staging-root identity changed");
                result.chain.clear();
                return result;
            }
            destination_anchor_seen = true;
        }
        result.chain.push_back({.handle = std::move(handle), .path = *openedPath});
        const auto parent = directory.parent_path();
        if (parent.empty() || same_path(parent, directory)) {
            result.failure = rejected(request, OperationStatus::unsupported,
                                      std::string(purpose) + " directory chain has no stable root");
            result.chain.clear();
            return result;
        }
        directory = parent;
    }
    if (!destination_anchor_seen) {
        result.failure = rejected(request, OperationStatus::invalid_request,
                                  "destination staging root is not an ancestor of the transfer");
        result.chain.clear();
    }
    return result;
}

bool chain_unchanged(const std::vector<PinnedDirectory> &chain) {
    return std::ranges::all_of(chain, [](const PinnedDirectory &directory) {
        const auto path = final_path(directory.handle.get());
        return path && same_path(*path, directory.path);
    });
}

bool regular_file(const HANDLE handle, DWORD &error,
                  platform::windows_detail::WindowsTransferDataStreams *streams = nullptr) {
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE) {
        error = GetLastError();
        return false;
    }
    if ((attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
          FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0 ||
        standard.NumberOfLinks != 1) {
        error = ERROR_NOT_SUPPORTED;
        return false;
    }
    auto observed_streams = platform::windows_detail::query_supported_transfer_data_streams(handle);
    error = observed_streams.error;
    if (!observed_streams.ok()) {
        return false;
    }
    if (streams != nullptr) {
        *streams = observed_streams;
    }
    return true;
}

bool mark_delete_on_close(const HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != FALSE;
}

class ReservationCleanup final {
  public:
    ReservationCleanup(const HANDLE handle, const bool armed) noexcept
        : handle_(handle), armed_(armed) {}

    ~ReservationCleanup() {
        if (armed_) {
            static_cast<void>(mark_delete_on_close(handle_));
        }
    }

    ReservationCleanup(const ReservationCleanup &) = delete;
    ReservationCleanup &operator=(const ReservationCleanup &) = delete;

    void release() noexcept {
        armed_ = false;
    }

  private:
    HANDLE handle_{};
    bool armed_{};
};

#ifdef VOVE_FILE_TRANSFER_TEST_HOOKS
void pause_after_temp_create_for_test() {
    std::array<wchar_t, 32'768> marker{};
    const auto length = GetEnvironmentVariableW(L"VOVE_TEST_TRANSFER_AFTER_CREATE_MARKER",
                                                marker.data(), static_cast<DWORD>(marker.size()));
    if (length == 0 || static_cast<std::size_t>(length) >= marker.size()) {
        return;
    }
    ScopedHandle signal(CreateFileW(marker.data(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Sleep(5'000);
}

#endif

class WindowsFileAuditSession final : public FileTransferStreamSession {
  public:
    WindowsFileAuditSession(FileTransferStreamRequest request, ScopedHandle source,
                            std::vector<PinnedDirectory> source_chain,
                            SnapshotQuery source_snapshot, FileTransferReservation reservation)
        : request_(std::move(request)), source_(std::move(source)),
          source_chain_(std::move(source_chain)), source_snapshot_(std::move(source_snapshot)),
          reservation_(std::move(reservation)) {}

    [[nodiscard]] const FileTransferReservation &reservation() const noexcept override {
        return reservation_;
    }

    [[nodiscard]] FileTransferStreamResult stream(const Progress &progress) override {
        LARGE_INTEGER zero{};
        if (SetFilePointerEx(source_.get(), zero, nullptr, FILE_BEGIN) == FALSE) {
            return audit_failure(GetLastError(), "published file could not be rewound for audit");
        }

        Sha256 digest;
        std::vector<std::byte> buffer(transferBufferBytes);
        std::uint64_t total{};
        std::uint64_t last_progress{};
        while (true) {
            DWORD read{};
            if (ReadFile(source_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                         nullptr) == FALSE) {
                return audit_failure(GetLastError(), "published file audit read failed");
            }
            if (read == 0U) {
                break;
            }
            digest.update(std::span<const std::byte>(buffer.data(), read));
            total += read;
            if (total > request_.expected_source.size_bytes) {
                return audit_rejected("published file grew during audit");
            }
            if (total - last_progress >= kFileTransferProgressChunkBytes ||
                total == request_.expected_source.size_bytes) {
                FileTransferProgress update{
                    .operation_id = request_.operation_id,
                    .item_index = request_.item_index,
                    .request_token = request_.request_token,
                    .bytes_written = total,
                };
                if (progress && !progress(update)) {
                    return audit_rejected("published file audit progress channel closed");
                }
                last_progress = total;
            }
        }

        const auto final_snapshot =
            snapshot_for_handle(source_.get(), request_.expected_source.source_revision_utf8);
        const auto observed_path = final_path(source_.get());
        if (total != request_.expected_source.size_bytes || !final_snapshot.ok() ||
            final_snapshot.snapshot.size_bytes != request_.expected_source.size_bytes ||
            final_snapshot.snapshot.modified_unix_ns != request_.expected_source.modified_unix_ns ||
            !same_source_revision(final_snapshot.snapshot.source_revision_utf8,
                                  request_.expected_source.source_revision_utf8) ||
            !observed_path || !same_path(*observed_path, request_.source) ||
            !chain_unchanged(source_chain_)) {
            return audit_rejected("published file identity changed during audit");
        }

        return {.operation_id = request_.operation_id,
                .item_index = request_.item_index,
                .request_token = request_.request_token,
                .status = OperationStatus::success,
                .evidence = OperationEvidence::committed,
                .source_snapshot = final_snapshot.snapshot,
                .temp_snapshot = final_snapshot.snapshot,
                .content_sha256 = digest.digest(),
                .bytes_written = total,
                .detail_utf8 = {}};
    }

  private:
    [[nodiscard]] FileTransferStreamResult audit_failure(const DWORD code,
                                                         std::string detail) const {
        auto result = failure(request_, code, OperationEvidence::no_commit, std::move(detail));
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = source_snapshot_.snapshot;
        return result;
    }

    [[nodiscard]] FileTransferStreamResult audit_rejected(std::string detail) const {
        auto result = rejected(request_, OperationStatus::source_changed, std::move(detail));
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = source_snapshot_.snapshot;
        return result;
    }

    FileTransferStreamRequest request_;
    ScopedHandle source_;
    std::vector<PinnedDirectory> source_chain_;
    SnapshotQuery source_snapshot_;
    FileTransferReservation reservation_;
};

class WindowsFileTransferSession final : public FileTransferStreamSession {
  public:
    WindowsFileTransferSession(FileTransferStreamRequest request, ScopedHandle source,
                               ScopedHandle temp, std::vector<PinnedDirectory> sourceChain,
                               std::vector<PinnedDirectory> destinationChain,
                               SnapshotQuery sourceSnapshot, FileTransferReservation reservation,
                               platform::windows_detail::WindowsTransferDataStreams sourceStreams,
                               platform::windows_detail::WindowsTransferDataStreams tempStreams,
                               const bool reservationPending)
        : request_(std::move(request)), source_(std::move(source)), temp_(std::move(temp)),
          source_chain_(std::move(sourceChain)), destination_chain_(std::move(destinationChain)),
          source_snapshot_(std::move(sourceSnapshot)), reservation_(std::move(reservation)),
          source_streams_(sourceStreams), temp_streams_(tempStreams),
          reservation_pending_(reservationPending) {}

    ~WindowsFileTransferSession() override {
        if (reservation_pending_) {
            static_cast<void>(mark_delete_on_close(temp_.get()));
        }
    }

    [[nodiscard]] const FileTransferReservation &reservation() const noexcept override {
        return reservation_;
    }

    [[nodiscard]] FileTransferStreamResult stream(const Progress &progress) override {
        reservation_pending_ = false;
        LARGE_INTEGER zero{};
        if (SetFilePointerEx(source_.get(), zero, nullptr, FILE_BEGIN) == FALSE ||
            SetFilePointerEx(temp_.get(), zero, nullptr, FILE_BEGIN) == FALSE ||
            SetEndOfFile(temp_.get()) == FALSE) {
            return stream_failure(GetLastError(), "transfer streams could not be reset");
        }

        Sha256 digest;
        std::vector<std::byte> buffer(transferBufferBytes);
        std::uint64_t total{};
        std::uint64_t lastProgress{};
        while (true) {
            DWORD read{};
            if (ReadFile(source_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                         nullptr) == FALSE) {
                return stream_failure(GetLastError(), "source read failed");
            }
            if (read == 0) {
                break;
            }
            DWORD offset{};
            while (offset < read) {
                DWORD written{};
                if (WriteFile(temp_.get(), buffer.data() + offset, read - offset, &written,
                              nullptr) == FALSE ||
                    written == 0) {
                    return stream_failure(GetLastError(), "temporary destination write failed");
                }
                offset += written;
            }
            digest.update(std::span<const std::byte>(buffer.data(), read));
            total += read;
            if (total > request_.expected_source.size_bytes) {
                return rejected_after_ack(OperationStatus::source_changed,
                                          "source grew during transfer");
            }
            if (total - lastProgress >= kFileTransferProgressChunkBytes ||
                total == request_.expected_source.size_bytes) {
                FileTransferProgress update;
                update.operation_id = request_.operation_id;
                update.item_index = request_.item_index;
                update.request_token = request_.request_token;
                update.bytes_written = total;
                if (progress && !progress(update)) {
                    return rejected_after_ack(OperationStatus::unknown_outcome,
                                              "transfer progress channel closed");
                }
                lastProgress = total;
            }
        }
        if (total != request_.expected_source.size_bytes || !source_matches_expected()) {
            return rejected_after_ack(OperationStatus::source_changed,
                                      "source identity changed during transfer");
        }
        if (const auto stream_result = synchronize_data_streams()) {
            return *stream_result;
        }
        if (!source_matches_expected()) {
            return rejected_after_ack(OperationStatus::source_changed,
                                      "source identity changed while copying file metadata");
        }

        FILETIME modified{.dwLowDateTime = source_snapshot_.basic.LastWriteTime.LowPart,
                          .dwHighDateTime =
                              static_cast<DWORD>(source_snapshot_.basic.LastWriteTime.HighPart)};
        if (SetFileTime(temp_.get(), nullptr, nullptr, &modified) == FALSE ||
            FlushFileBuffers(temp_.get()) == FALSE) {
            return stream_failure(GetLastError(), "temporary destination flush failed");
        }
        const auto tempSnapshot =
            snapshot_for_handle(temp_.get(), reservation_.temp_snapshot.source_revision_utf8);
        const auto tempPath = final_path(temp_.get());
        if (!tempSnapshot.ok() ||
            !same_object_identity(reservation_.temp_snapshot.source_revision_utf8,
                                  tempSnapshot.snapshot.source_revision_utf8) ||
            tempSnapshot.snapshot.size_bytes != total || !tempPath ||
            !same_path(*tempPath, request_.temp_destination) || !chain_unchanged(source_chain_) ||
            !chain_unchanged(destination_chain_)) {
            return rejected_after_ack(OperationStatus::source_changed,
                                      "transfer identity changed before content became ready");
        }
        FileTransferStreamResult result;
        result.operation_id = request_.operation_id;
        result.item_index = request_.item_index;
        result.request_token = request_.request_token;
        result.status = OperationStatus::success;
        result.evidence = OperationEvidence::committed;
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = tempSnapshot.snapshot;
        result.content_sha256 = digest.digest();
        result.bytes_written = total;
        return result;
    }

  private:
    [[nodiscard]] std::optional<FileTransferStreamResult> synchronize_data_streams() {
        const auto result = platform::windows_detail::copy_supported_transfer_data_streams(
            request_.source, source_.get(), temp_.get(), source_streams_, temp_streams_);
        if (result.ok()) return std::nullopt;
        if (result.source_changed)
            return rejected_after_ack(OperationStatus::source_changed, result.detail);
        return stream_failure(result.error, result.detail);
    }

    [[nodiscard]] bool source_matches_expected() const {
        const auto current =
            snapshot_for_handle(source_.get(), request_.expected_source.source_revision_utf8);
        const auto path = final_path(source_.get());
        return current.ok() && current.snapshot.size_bytes == request_.expected_source.size_bytes &&
               current.snapshot.modified_unix_ns == request_.expected_source.modified_unix_ns &&
               same_source_revision(current.snapshot.source_revision_utf8,
                                    request_.expected_source.source_revision_utf8) &&
               path && same_path(*path, request_.source);
    }

    FileTransferStreamResult stream_failure(const DWORD code, std::string detail) const {
        auto result = failure(request_, code, OperationEvidence::none, std::move(detail));
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = reservation_.temp_snapshot;
        return result;
    }

    FileTransferStreamResult rejected_after_ack(const OperationStatus status,
                                                std::string detail) const {
        auto result = rejected(request_, status, std::move(detail));
        result.evidence = OperationEvidence::none;
        result.source_snapshot = source_snapshot_.snapshot;
        result.temp_snapshot = reservation_.temp_snapshot;
        return result;
    }

    FileTransferStreamRequest request_;
    ScopedHandle source_;
    ScopedHandle temp_;
    std::vector<PinnedDirectory> source_chain_;
    std::vector<PinnedDirectory> destination_chain_;
    SnapshotQuery source_snapshot_;
    FileTransferReservation reservation_;
    platform::windows_detail::WindowsTransferDataStreams source_streams_;
    platform::windows_detail::WindowsTransferDataStreams temp_streams_;
    bool reservation_pending_{};
};

} // namespace

FileTransferBeginResult begin_file_transfer_stream(const FileTransferStreamRequest &request) {
    std::string validation;
    if (!valid_file_transfer_stream_request(request, validation)) {
        return {.session = nullptr,
                .failure =
                    rejected(request, OperationStatus::invalid_request, std::move(validation))};
    }

    const auto audit = request.mode == FileTransferStreamMode::audit_existing;
    const auto probe = request.mode == FileTransferStreamMode::probe_reservation;
    auto sourceChain = pin_directory_chain(request, request.source.parent_path(),
                                           request.source_parent_identity_utf8, "source", audit);
    if (sourceChain.chain.empty()) {
        return {.session = nullptr, .failure = std::move(sourceChain.failure)};
    }
    ScopedHandle source(CreateFileW(
        request.source.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!source.valid()) {
        return {.session = nullptr,
                .failure = failure(request, GetLastError(), OperationEvidence::no_commit,
                                   "source could not be opened for stable transfer")};
    }
    DWORD metadataError{};
    platform::windows_detail::WindowsTransferDataStreams sourceStreams;
    if (!regular_file(source.get(), metadataError, &sourceStreams)) {
        return {.session = nullptr,
                .failure = failure(request, metadataError, OperationEvidence::no_commit,
                                   "source is not a supported ordinary file")};
    }
    const auto sourcePath = final_path(source.get());
    const auto sourceSnapshot =
        snapshot_for_handle(source.get(), request.expected_source.source_revision_utf8);
    const auto sourceMatches =
        probe ||
        (sourceSnapshot.snapshot.size_bytes == request.expected_source.size_bytes &&
         sourceSnapshot.snapshot.modified_unix_ns == request.expected_source.modified_unix_ns &&
         same_source_revision(sourceSnapshot.snapshot.source_revision_utf8,
                              request.expected_source.source_revision_utf8));
    if (!sourcePath || !same_path(*sourcePath, request.source) || !sourceSnapshot.ok() ||
        !sourceMatches) {
        return {.session = nullptr,
                .failure = rejected(request, OperationStatus::source_changed,
                                    "source identity changed before transfer reservation")};
    }

    auto effectiveRequest = request;
    if (probe) {
        effectiveRequest.expected_source = sourceSnapshot.snapshot;
    }
    if (audit || probe) {
        if (!chain_unchanged(sourceChain.chain)) {
            return {.session = nullptr,
                    .failure = rejected(request, OperationStatus::source_changed,
                                        "directory chain changed before file audit")};
        }
        DWORD parent_error{};
        const auto parent_revision =
            directory_revision(sourceChain.chain.front().handle.get(), {}, parent_error);
        if (!parent_revision) {
            return {.session = nullptr,
                    .failure = failure(request, parent_error, OperationEvidence::no_commit,
                                       "published file parent identity is unavailable")};
        }
        FileTransferReservation reservation{
            .operation_id = request.operation_id,
            .item_index = request.item_index,
            .request_token = request.request_token,
            .temp_snapshot = sourceSnapshot.snapshot,
            .destination_parent_identity_utf8 = stable_object_identity(*parent_revision),
            .destination_parent_revision_utf8 = *parent_revision,
        };
        return {.session = std::make_unique<WindowsFileAuditSession>(
                    std::move(effectiveRequest), std::move(source), std::move(sourceChain.chain),
                    sourceSnapshot, std::move(reservation)),
                .failure = {}};
    }

    auto destinationChain = pin_directory_chain(
        request, request.temp_destination.parent_path(), request.destination_parent_identity_utf8,
        "destination", !request.destination_anchor_path.empty());
    if (destinationChain.chain.empty()) {
        return {.session = nullptr, .failure = std::move(destinationChain.failure)};
    }

    const auto reserveNew = request.mode == FileTransferStreamMode::reserve_new;
    const DWORD disposition = reserveNew ? CREATE_NEW : OPEN_EXISTING;
    const DWORD tempFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT;
    ScopedHandle temp(CreateFileW(request.temp_destination.c_str(),
                                  GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, disposition,
                                  tempFlags, nullptr));
    if (!temp.valid()) {
        return {.session = nullptr,
                .failure = failure(request, GetLastError(), OperationEvidence::no_commit,
                                   "temporary destination could not be reserved")};
    }
    ReservationCleanup reservationCleanup(temp.get(), reserveNew);
    if (reserveNew) {
        const auto marker =
            make_file_transfer_reservation_marker({.operation_id = request.operation_id,
                                                   .item_index = request.item_index,
                                                   .request_token = request.request_token});
        DWORD written{};
        if (WriteFile(temp.get(), marker.data(), static_cast<DWORD>(marker.size()), &written,
                      nullptr) == FALSE ||
            written != marker.size() || FlushFileBuffers(temp.get()) == FALSE) {
            return {.session = nullptr,
                    .failure = failure(request, GetLastError(), OperationEvidence::none,
                                       "temporary reservation marker could not be committed")};
        }
    }
#ifdef VOVE_FILE_TRANSFER_TEST_HOOKS
    pause_after_temp_create_for_test();
#endif
    platform::windows_detail::WindowsTransferDataStreams tempStreams;
    if (!regular_file(temp.get(), metadataError, &tempStreams)) {
        return {.session = nullptr,
                .failure =
                    failure(request, metadataError,
                            reserveNew ? OperationEvidence::none : OperationEvidence::no_commit,
                            "temporary destination is not an ordinary file")};
    }
    if (tempStreams.has_empty_encryptable && !sourceStreams.has_empty_encryptable) {
        return {.session = nullptr,
                .failure = rejected(request, OperationStatus::source_changed,
                    "temporary encryptable stream is absent from the source")};
    }
    const auto tempPath = final_path(temp.get());
    const auto expectedTempRevision =
        request.mode == FileTransferStreamMode::resume_existing
            ? std::string_view(request.expected_temp.source_revision_utf8)
            : std::string_view{};
    const auto tempSnapshot = snapshot_for_handle(temp.get(), expectedTempRevision);
    if (!tempPath || !same_path(*tempPath, request.temp_destination) || !tempSnapshot.ok() ||
        (request.mode == FileTransferStreamMode::reserve_new &&
         tempSnapshot.snapshot.size_bytes != kFileTransferReservationMarkerBytes) ||
        (request.mode == FileTransferStreamMode::resume_existing &&
         (tempSnapshot.snapshot.size_bytes != request.expected_temp.size_bytes ||
          tempSnapshot.snapshot.modified_unix_ns != request.expected_temp.modified_unix_ns ||
          !same_source_revision(tempSnapshot.snapshot.source_revision_utf8,
                                request.expected_temp.source_revision_utf8)))) {
        auto result = rejected(request, OperationStatus::source_changed,
                               "temporary destination identity is not the reserved object");
        if (reserveNew) {
            result.evidence = OperationEvidence::none;
        }
        return {.session = nullptr, .failure = std::move(result)};
    }
    if (!chain_unchanged(sourceChain.chain) || !chain_unchanged(destinationChain.chain)) {
        auto result = rejected(request, OperationStatus::source_changed,
                               "directory chain changed during transfer reservation");
        if (reserveNew) {
            result.evidence = OperationEvidence::none;
        }
        return {.session = nullptr, .failure = std::move(result)};
    }
    DWORD parentError{};
    const auto destinationParentRevision =
        directory_revision(destinationChain.chain.front().handle.get(), {}, parentError);
    if (!destinationParentRevision) {
        return {.session = nullptr,
                .failure =
                    failure(request, parentError,
                            reserveNew ? OperationEvidence::none : OperationEvidence::no_commit,
                            "destination directory identity changed after reservation")};
    }

    FileTransferReservation reservation{
        .operation_id = request.operation_id,
        .item_index = request.item_index,
        .request_token = request.request_token,
        .temp_snapshot = tempSnapshot.snapshot,
        .destination_parent_identity_utf8 = stable_object_identity(*destinationParentRevision),
        .destination_parent_revision_utf8 = *destinationParentRevision,
    };
    auto session = std::make_unique<WindowsFileTransferSession>(
        request, std::move(source), std::move(temp), std::move(sourceChain.chain),
        std::move(destinationChain.chain), sourceSnapshot, std::move(reservation), sourceStreams,
        tempStreams, reserveNew);
    reservationCleanup.release();
    return {.session = std::move(session), .failure = {}};
}

} // namespace vove::fileops::detail
