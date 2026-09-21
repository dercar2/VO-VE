#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vove::worker {

using JobId = std::uint64_t;
using RequestGeneration = std::uint64_t;
using OpaqueToken = std::uint64_t;
using CapabilitySet = std::uint64_t;

inline constexpr std::uint32_t kProtocolMagic = 0x45564F56U;
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 18;
inline constexpr std::size_t kProtocolHeaderBytes = 24;
inline constexpr std::size_t kMaximumPayloadBytes = std::size_t{2} * 1024U * 1024U;
inline constexpr std::size_t kMaximumFrameBytes = kProtocolHeaderBytes + kMaximumPayloadBytes;
inline constexpr std::size_t kMaximumBuildIdBytes = 64;
inline constexpr std::size_t kMaximumProfileNameBytes = 96;
inline constexpr std::size_t kProfileFingerprintBytes = 64;
inline constexpr std::size_t kMaximumDiagnosticBytes = 512;
inline constexpr std::size_t kMaximumPasswordBytes = 512;
inline constexpr std::uint32_t kMaximumThumbnailEdge = 4'096;
inline constexpr std::uint32_t kMaximumPageCount = 1'000'000;
inline constexpr std::uint64_t kMaximumInputBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumOutputBytes = 24ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumWorkerMemoryBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumWallTimeoutMs = 120'000;

enum class MessageKind : std::uint16_t { // NOLINT(performance-enum-size)
    handshake = 1,
    handshake_acknowledgement = 2,
    worker_job = 3,
    worker_result = 4,
    cancel_generation = 5,
};

enum class Capability : std::uint64_t { // NOLINT(performance-enum-size)
    read_only_source_token = 1ULL << 0U,
    write_only_output_token = 1ULL << 1U,
    sandbox_active = 1ULL << 2U,
    color_management = 1ULL << 3U,
    synthetic_handler = 1ULL << 4U,
    raster_handler = 1ULL << 5U,
    document_handler = 1ULL << 6U,
    cdr_handler = 1ULL << 7U,
    read_only_profile_token = 1ULL << 8U,
};

[[nodiscard]] constexpr CapabilitySet capability_bit(const Capability capability) noexcept {
    return static_cast<CapabilitySet>(capability);
}

enum class ResultStatus : std::uint8_t {
    success = 0,
    unsupported = 1,
    malformed_source = 2,
    timed_out = 3,
    cancelled = 4,
    resource_limit = 5,
    internal_error = 6,
    source_unavailable = 7,
    disconnected = 8,
    color_profile_required = 9,
    password_required = 10,
    reserved_source_authentication_failure = 11,
    ghostscript_required = 12,
    embedded_preview_unavailable = 13,
    source_changed = 14,
    document_password_incorrect = 15,
    memory_limit = 16,
};

enum class ColorModel : std::uint8_t {
    unknown = 0,
    grayscale = 1,
    rgb = 2,
    cmyk = 3,
    lab = 4,
    mixed = 5,
};

enum class PreviewProvenance : std::uint8_t {
    primary_render = 0,
    embedded_preview = 1,
};

enum class SourceFormatHint : std::uint8_t {
    auto_detect = 0,
    camera_raw = 1,
    // The caller routes KRA/ORA ZIPs explicitly; the worker verifies their internal identity.
    layered_archive = 2,
    hpgl = 3,
    // ASCII DXF is routed explicitly; the worker verifies its code/value structure.
    dxf = 4,
    // IDML ZIP packages are routed explicitly; the worker verifies the package mimetype.
    idml = 5,
    // XCF is decoded by the embedded KImageFormats reader; no external editor is launched.
    xcf = 6,
};

enum class DecodeErrorCode : std::uint8_t {
    none,
    truncated,
    bad_magic,
    incompatible_version,
    unknown_message_kind,
    frame_too_large,
    frame_size_mismatch,
    unexpected_message_kind,
    invalid_value,
    limit_exceeded,
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
    std::uint32_t magic{kProtocolMagic};
    std::uint16_t major{kProtocolMajor};
    std::uint16_t minor{kProtocolMinor};
    MessageKind kind{MessageKind::handshake};
    std::uint16_t flags{};
    std::uint32_t frame_bytes{};
    JobId job_id{};
};

struct DecodedFrame {
    FrameHeader header;
    std::vector<std::byte> payload;
};

struct Handshake {
    std::string build_id;
    CapabilitySet capabilities{};
};

struct JobLimits {
    std::uint64_t maximum_input_bytes{kMaximumInputBytes};
    std::uint64_t maximum_output_bytes{kMaximumOutputBytes};
    std::uint64_t memory_limit_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint32_t wall_timeout_ms{30'000};
    std::uint32_t canonical_edge{512};
};

struct WorkerJob {
    JobId job_id{};
    RequestGeneration generation{};
    OpaqueToken source_token{};
    OpaqueToken output_token{};
    OpaqueToken profile_token{};
    JobLimits limits;
    std::uint32_t page_index{};
    SourceFormatHint source_format_hint{SourceFormatHint::auto_detect};
    std::string password_utf8;
};

struct WorkerResult {
    JobId job_id{};
    ResultStatus status{ResultStatus::internal_error};
    std::uint32_t width{};
    std::uint32_t height{};
    ColorModel color_model{ColorModel::unknown};
    PreviewProvenance provenance{PreviewProvenance::primary_render};
    std::string color_profile_utf8;
    std::string source_profile_fingerprint;
    std::uint32_t page_index{};
    std::uint32_t page_count{};
    std::uint64_t bytes_written{};
    std::string diagnostic_utf8;
};

struct CancelGeneration {
    RequestGeneration generation{};
};

[[nodiscard]] bool decode_frame(std::span<const std::byte> bytes, DecodedFrame &frame,
                                DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_handshake(const Handshake &handshake,
                                                      MessageKind kind = MessageKind::handshake);
[[nodiscard]] bool decode_handshake(std::span<const std::byte> bytes, Handshake &handshake,
                                    DecodeError &error,
                                    MessageKind expected_kind = MessageKind::handshake);

[[nodiscard]] std::vector<std::byte> encode_worker_job(const WorkerJob &job);
[[nodiscard]] bool decode_worker_job(std::span<const std::byte> bytes, WorkerJob &job,
                                     DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_worker_result(const WorkerResult &result);
[[nodiscard]] bool decode_worker_result(std::span<const std::byte> bytes, WorkerResult &result,
                                        DecodeError &error);

[[nodiscard]] std::vector<std::byte> encode_cancel_generation(CancelGeneration cancel);
[[nodiscard]] bool decode_cancel_generation(std::span<const std::byte> bytes,
                                            CancelGeneration &cancel, DecodeError &error);

} // namespace vove::worker
