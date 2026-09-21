#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vove::platform {

using NativeFileObject = std::intptr_t;
inline constexpr NativeFileObject kInvalidNativeFileObject = static_cast<NativeFileObject>(-1);

enum class SourceOpenErrorKind : std::uint8_t {
    none,
    invalid_path,
    not_found,
    access_denied,
    authentication_failed,
    timed_out,
    disconnected,
    not_regular_file,
    too_large,
    io_error,
};

struct SourceOpenError {
    SourceOpenErrorKind kind{SourceOpenErrorKind::none};
    std::int64_t platform_code{};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return kind != SourceOpenErrorKind::none;
    }
};

struct SourceOpenResult;

enum class SourceIdentityStatus : std::uint8_t {
    unchanged,
    changed,
    unavailable,
};

struct SourceIdentityResult {
    SourceIdentityStatus status{SourceIdentityStatus::unavailable};
    SourceOpenErrorKind error_kind{SourceOpenErrorKind::none};
    std::int64_t platform_code{};
};

class ReadOnlySource {
  public:
    ReadOnlySource() = default;
    ~ReadOnlySource();
    ReadOnlySource(ReadOnlySource &&other) noexcept;
    ReadOnlySource &operator=(ReadOnlySource &&other) noexcept;

    ReadOnlySource(const ReadOnlySource &) = delete;
    ReadOnlySource &operator=(const ReadOnlySource &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] NativeFileObject native_object() const noexcept;
    [[nodiscard]] std::uint64_t size_bytes() const noexcept;
    [[nodiscard]] std::int64_t modified_unix_ns() const noexcept;
    [[nodiscard]] const std::string &source_revision_utf8() const noexcept;
    [[nodiscard]] SourceIdentityResult identity_result() const noexcept;
    [[nodiscard]] SourceIdentityStatus identity_status() const noexcept;
    [[nodiscard]] bool identity_unchanged() const noexcept;

  private:
    friend struct SourceOpenResult;
    friend SourceOpenResult open_read_only_source(const std::filesystem::path &, std::uint64_t);

    struct Snapshot {
        NativeFileObject object{kInvalidNativeFileObject};
        std::uint64_t size_bytes{};
        std::int64_t modified_unix_ns{};
        std::string source_revision_utf8;
    };

    explicit ReadOnlySource(Snapshot snapshot);
    void close() noexcept;

    NativeFileObject object_{kInvalidNativeFileObject};
    std::uint64_t size_bytes_{};
    std::int64_t modified_unix_ns_{};
    std::string source_revision_utf8_;
};

struct SourceOpenResult {
    std::optional<ReadOnlySource> source;
    SourceOpenError error;
};

struct DirectoryRevisionResult {
    std::string revision_utf8;
    SourceOpenError error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return !revision_utf8.empty() && !error;
    }
};

[[nodiscard]] SourceOpenResult open_read_only_source(const std::filesystem::path &path,
                                                     std::uint64_t maximum_bytes);
[[nodiscard]] DirectoryRevisionResult query_directory_revision(const std::filesystem::path &path);
[[nodiscard]] std::string storage_identity(NativeFileObject object) noexcept;
[[nodiscard]] std::string storage_identity(const std::filesystem::path &path) noexcept;

} // namespace vove::platform
