#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace vove::handlers::raster {

#ifdef _WIN32
using NativeSourceHandle = void *;
#else
using NativeSourceHandle = int;
#endif

enum class NativeSourceErrorCode : std::uint8_t {
    none,
    invalid_handle,
    not_regular_file,
    size_limit_exceeded,
    offset_out_of_range,
    offset_overflow,
    io_error,
    disconnected,
    source_changed,
};

struct NativeSourceError {
    NativeSourceErrorCode code{NativeSourceErrorCode::none};
    std::uint32_t system_code{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != NativeSourceErrorCode::none;
    }
};

struct NativeSourceReadResult {
    std::size_t bytes_read{};
    NativeSourceError error;

    [[nodiscard]] bool ok() const noexcept {
        return !error;
    }
};

struct NativeSourceCreateResult;

struct NativeSourceSnapshot {
    std::uint64_t identity_high{};
    std::uint64_t identity_low{};
    std::uint64_t size{};
    std::int64_t modified{};
    std::int64_t changed{};

    friend bool operator==(const NativeSourceSnapshot &, const NativeSourceSnapshot &) = default;
};

struct NativeSourceValidationResult {
    bool unchanged{};
    NativeSourceError error;
};

class NativeSource final {
  public:
    ~NativeSource();
    NativeSource(NativeSource &&other) noexcept;
    NativeSource &operator=(NativeSource &&other) noexcept;

    NativeSource(const NativeSource &) = delete;
    NativeSource &operator=(const NativeSource &) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] NativeSourceReadResult read_at(std::uint64_t offset,
                                                 std::span<std::byte> destination) const noexcept;
    [[nodiscard]] NativeSourceValidationResult validate_unchanged() const noexcept;

  private:
    struct State {
        NativeSourceHandle supplied_handle;
        NativeSourceHandle read_handle;
        std::uint64_t size;
        NativeSourceSnapshot snapshot;
    };

    explicit NativeSource(State state) noexcept;
    void release() noexcept;

    [[nodiscard]] static constexpr NativeSourceHandle empty_handle() noexcept {
#ifdef _WIN32
        return nullptr;
#else
        return -1;
#endif
    }

    friend NativeSourceCreateResult make_native_source(NativeSourceHandle, std::uint64_t) noexcept;

    State state_;
    mutable std::mutex read_mutex_;
};

struct NativeSourceCreateResult {
    std::optional<NativeSource> source;
    NativeSourceError error;

    [[nodiscard]] bool ok() const noexcept {
        return source.has_value() && !error;
    }
};

// The returned source never closes supplied_handle. The caller must keep it valid for the source.
[[nodiscard]] NativeSourceCreateResult make_native_source(NativeSourceHandle supplied_handle,
                                                          std::uint64_t maximum_size) noexcept;

} // namespace vove::handlers::raster
