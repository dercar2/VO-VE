#pragma once

#include "vove/preview/thumbnail_types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vove::preview::helper_protocol {

using RequestId = std::uint64_t;

inline constexpr std::uint32_t kMagic = 0x48564F56U; // "VOVH" in little-endian order.
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 19;
inline constexpr std::size_t kFrameHeaderBytes = 24;
inline constexpr std::size_t kMaximumFrameBytes = std::size_t{17} * 1024U * 1024U;
inline constexpr std::size_t kMaximumFramePayloadBytes = kMaximumFrameBytes - kFrameHeaderBytes;
inline constexpr std::size_t kMaximumPathBytes = std::size_t{32} * 1024U;
inline constexpr std::size_t kMaximumBuildIdBytes = 96;
inline constexpr std::size_t kMaximumAuthTokenBytes = 256;
inline constexpr std::size_t kMaximumColorModelBytes = 32;
inline constexpr std::size_t kMaximumColorProfileBytes = 160;
inline constexpr std::size_t kMaximumSourceRevisionBytes = 160;
inline constexpr std::size_t kMaximumPasswordBytes = 512;
inline constexpr std::size_t kProfileFingerprintBytes = 64;
inline constexpr std::uint16_t kMaximumCanonicalEdge = 2048;
inline constexpr std::size_t kMaximumRgbaBytes = std::size_t{16} * 1024U * 1024U;
inline constexpr std::uint32_t kMaximumPageCount = 1'000'000;

enum class MessageType : std::uint16_t { // NOLINT(performance-enum-size)
    hello = 1,
    hello_ack = 2,
    preview_request = 3,
    cancel_generation = 4,
    preview_response = 5,
    job_started = 6,
    cache_request = 7,
    cache_response = 8,
};

enum class CacheOperation : std::uint8_t {
    inspect = 0,
    clear = 1,
};

enum class CacheStatus : std::uint8_t {
    success = 0,
    failed = 1,
};

enum class ResponseStatus : std::uint8_t {
    success_cached = 0,
    success_decoded = 1,
    unsupported = 2,
    malformed = 3,
    disconnected = 4,
    authentication_failed = 5,
    permission_denied = 6,
    not_found = 7,
    too_large = 8,
    timed_out = 9,
    internal_error = 10,
    cancelled = 11,
    queue_busy = 12,
    source_unavailable = 13,
    color_profile_required = 14,
    password_required = 15,
    ghostscript_required = 16,
    embedded_preview_unavailable = 17,
    success_offline_cached = 18,
    source_changed = 19,
    document_password_incorrect = 20,
    pdf_input_too_large = 21,
    memory_limit = 22,
    processing_timed_out = 23,
    worker_start_failed = 24,
};

enum class PreviewProvenance : std::uint8_t {
    primary_render = 0,
    embedded_preview = 1,
};

enum class DecodeErrorCode : std::uint8_t {
    none = 0,
    truncated,
    bad_magic,
    incompatible_version,
    unknown_message_type,
    unexpected_message_type,
    frame_too_large,
    frame_size_mismatch,
    invalid_value,
    invalid_utf8,
    limit_exceeded,
    payload_size_mismatch,
    trailing_bytes,
};

struct DecodeError {
    DecodeErrorCode code{DecodeErrorCode::none};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != DecodeErrorCode::none;
    }
};

struct FrameHeader {
    std::uint32_t magic{kMagic};
    std::uint16_t major{kProtocolMajor};
    std::uint16_t minor{kProtocolMinor};
    MessageType type{MessageType::hello};
    std::uint16_t flags{};
    std::uint32_t total_size{};
    RequestId request_id{};
};

struct Hello {
    std::string build_id_utf8;
    std::string auth_token_utf8;
};

struct HelloAck {
    std::string build_id_utf8;
    std::string auth_token_utf8;
};

struct PreviewRequest {
    RequestId request_id{};
    RequestGeneration generation{};
    std::string path_utf8;
    std::uint64_t source_size{};
    std::int64_t modified_unix_ns{};
    std::string source_revision_utf8;
    std::uint16_t canonical_edge{512};
    ThumbnailPriority priority{ThumbnailPriority::rest};
    std::uint32_t page_index{};
    std::string password_utf8;
    bool offline_cache_only{};
    bool allow_offline_fallback{true};
    bool selected_view{};
    bool extended_limits{};
};

struct CancelGeneration {
    RequestGeneration generation{};
};

struct JobStarted {
    RequestId request_id{};
};

struct PreviewResponse {
    RequestId request_id{};
    ResponseStatus status{ResponseStatus::internal_error};
    std::uint16_t width{};
    std::uint16_t height{};
    std::string source_color_model_utf8;
    std::string source_color_profile_utf8;
    std::string source_profile_fingerprint;
    PreviewProvenance provenance{PreviewProvenance::primary_render};
    std::uint32_t page_index{};
    std::uint32_t page_count{};
    std::vector<std::byte> rgba8;
    WorkerStartupDiagnostic startup_diagnostic{};
};

struct CacheRequest {
    RequestId request_id{};
    CacheOperation operation{CacheOperation::inspect};
};

struct CacheResponse {
    RequestId request_id{};
    CacheStatus status{CacheStatus::failed};
    std::uint64_t entries{};
    std::uint64_t bytes{};
};

// Parses and validates the fixed header. A caller may pass exactly the first
// kFrameHeaderBytes bytes to discover the complete frame size on a stream.
[[nodiscard]] bool decode_header(std::span<const std::byte> bytes, FrameHeader &header,
                                 DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_hello(const Hello &hello);
[[nodiscard]] bool decode_hello(std::span<const std::byte> bytes, Hello &hello, DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_hello_ack(const HelloAck &ack);
[[nodiscard]] bool decode_hello_ack(std::span<const std::byte> bytes, HelloAck &ack,
                                    DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_preview_request(const PreviewRequest &request);
[[nodiscard]] bool decode_preview_request(std::span<const std::byte> bytes, PreviewRequest &request,
                                          DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_cancel_generation(CancelGeneration cancel);
[[nodiscard]] bool decode_cancel_generation(std::span<const std::byte> bytes,
                                            CancelGeneration &cancel, DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_job_started(JobStarted started);
[[nodiscard]] bool decode_job_started(std::span<const std::byte> bytes, JobStarted &started,
                                      DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_preview_response(const PreviewResponse &response);
[[nodiscard]] bool decode_preview_response(std::span<const std::byte> bytes,
                                           PreviewResponse &response, DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_cache_request(CacheRequest request);
[[nodiscard]] bool decode_cache_request(std::span<const std::byte> bytes, CacheRequest &request,
                                        DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_cache_response(CacheResponse response);
[[nodiscard]] bool decode_cache_response(std::span<const std::byte> bytes, CacheResponse &response,
                                         DecodeError &error);

} // namespace vove::preview::helper_protocol
