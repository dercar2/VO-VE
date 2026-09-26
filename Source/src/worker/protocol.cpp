#include "vove/worker/protocol.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vove::worker {
namespace {

template <typename Integer>
void append_integer(std::vector<std::byte> &bytes, const Integer value) {
    static_assert(std::is_integral_v<Integer>);
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto encoded = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.push_back(static_cast<std::byte>((encoded >> (index * 8U)) & 0xFFU));
    }
}

void append_string(std::vector<std::byte> &bytes, const std::string_view value) {
    append_integer(bytes, static_cast<std::uint16_t>(value.size()));
    for (const char character : value) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

template <typename Integer>
void write_integer(const std::span<std::byte> bytes, std::size_t &offset, const Integer value) {
    static_assert(std::is_integral_v<Integer>);
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto encoded = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((encoded >> (index * 8U)) & static_cast<Unsigned>(0xFFU));
    }
    offset += sizeof(Integer);
}

void write_string(const std::span<std::byte> bytes, std::size_t &offset,
                  const std::string_view value) {
    write_integer(bytes, offset, static_cast<std::uint16_t>(value.size()));
    for (const char character : value) {
        bytes[offset] = static_cast<std::byte>(static_cast<unsigned char>(character));
        ++offset;
    }
}

class Cursor {
  public:
    explicit Cursor(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename Integer> [[nodiscard]] bool read(Integer &value) {
        static_assert(std::is_integral_v<Integer>);
        if (remaining() < sizeof(Integer)) {
            return false;
        }
        using Unsigned = std::make_unsigned_t<Integer>;
        std::uint64_t decoded{};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            decoded |=
                static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes_[offset_ + index]))
                << (index * 8U);
        }
        value = static_cast<Integer>(static_cast<Unsigned>(decoded));
        offset_ += sizeof(Integer);
        return true;
    }

    [[nodiscard]] bool read_string(std::string &value, const std::size_t maximum_bytes,
                                   DecodeError &error, const std::string_view field) {
        std::uint16_t size{};
        if (!read(size)) {
            set_error(error, DecodeErrorCode::truncated,
                      std::string(field) + " length is truncated");
            return false;
        }
        if (size > maximum_bytes) {
            set_error(error, DecodeErrorCode::limit_exceeded,
                      std::string(field) + " exceeds its byte limit");
            return false;
        }
        if (remaining() < size) {
            set_error(error, DecodeErrorCode::truncated,
                      std::string(field) + " bytes are truncated");
            return false;
        }
        value.assign(reinterpret_cast<const char *>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

  private:
    static void set_error(DecodeError &error, const DecodeErrorCode code, std::string message) {
        error = {.code = code, .message = std::move(message)};
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

void fail(DecodeError &error, const DecodeErrorCode code, std::string message) {
    error = {.code = code, .message = std::move(message)};
}

[[nodiscard]] bool valid_message_kind(const MessageKind kind) noexcept {
    switch (kind) {
    case MessageKind::handshake:
    case MessageKind::handshake_acknowledgement:
    case MessageKind::worker_job:
    case MessageKind::worker_result:
    case MessageKind::cancel_generation:
    case MessageKind::animation_frame:
    case MessageKind::animation_advance:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_result_status(const ResultStatus status) noexcept {
    return status <= ResultStatus::memory_limit &&
           status != ResultStatus::reserved_source_authentication_failure;
}

[[nodiscard]] bool valid_color_model(const ColorModel model) noexcept {
    return model <= ColorModel::mixed;
}

[[nodiscard]] bool valid_provenance(const PreviewProvenance provenance) noexcept {
    return provenance <= PreviewProvenance::embedded_preview;
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1;
            code_point = first & 0x1FU;
            if (code_point < 2U) {
                return false;
            }
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

[[nodiscard]] bool valid_fingerprint(const std::string_view fingerprint) noexcept {
    if (fingerprint.empty()) {
        return true;
    }
    if (fingerprint.size() != kProfileFingerprintBytes) {
        return false;
    }
    return std::ranges::all_of(fingerprint, [](const char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool valid_build_id(const std::string_view build_id) noexcept {
    if (build_id.empty() || build_id.size() > kMaximumBuildIdBytes) {
        return false;
    }
    for (const char character : build_id) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x21U || value > 0x7EU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_limits(const JobLimits &limits) noexcept {
    return limits.maximum_input_bytes != 0 && limits.maximum_input_bytes <= kMaximumInputBytes &&
           limits.maximum_output_bytes != 0 && limits.maximum_output_bytes <= kMaximumOutputBytes &&
           limits.memory_limit_bytes != 0 &&
           limits.memory_limit_bytes <= kMaximumWorkerMemoryBytes && limits.wall_timeout_ms != 0 &&
           limits.wall_timeout_ms <= kMaximumWallTimeoutMs && limits.canonical_edge != 0 &&
           limits.canonical_edge <= kMaximumThumbnailEdge;
}

[[nodiscard]] bool valid_job(const WorkerJob &job) noexcept {
    return job.job_id != 0 && job.generation != 0 && job.source_token != 0 &&
           job.output_token != 0 && job.source_token != job.output_token &&
           (job.profile_token == 0 ||
            (job.profile_token != job.source_token && job.profile_token != job.output_token)) &&
           job.page_index < kMaximumPageCount &&
           static_cast<std::uint8_t>(job.source_format_hint) <=
               static_cast<std::uint8_t>(SourceFormatHint::gif_animation) &&
           job.password_utf8.size() <= kMaximumPasswordBytes &&
           job.password_utf8.find('\0') == std::string::npos && valid_utf8(job.password_utf8) &&
           valid_limits(job.limits);
}

[[nodiscard]] bool valid_result(const WorkerResult &result) noexcept {
    if (result.job_id == 0 || !valid_result_status(result.status) ||
        !valid_color_model(result.color_model) || !valid_provenance(result.provenance) ||
        result.width > kMaximumThumbnailEdge || result.height > kMaximumThumbnailEdge ||
        result.bytes_written > kMaximumOutputBytes || result.page_count > kMaximumPageCount ||
        result.color_profile_utf8.size() > kMaximumProfileNameBytes ||
        !valid_fingerprint(result.source_profile_fingerprint) ||
        result.diagnostic_utf8.size() > kMaximumDiagnosticBytes ||
        !valid_utf8(result.color_profile_utf8) || !valid_utf8(result.diagnostic_utf8)) {
        return false;
    }
    if ((result.page_count == 0 && result.page_index != 0) ||
        (result.page_count != 0 && result.page_index >= result.page_count)) {
        return false;
    }
    if (result.status == ResultStatus::success) {
        return result.width != 0 && result.height != 0 && result.bytes_written != 0 &&
               result.page_count != 0;
    }
    return result.provenance == PreviewProvenance::primary_render;
}

[[nodiscard]] std::vector<std::byte> make_frame(const MessageKind kind, const JobId job_id,
                                                const std::span<const std::byte> payload) {
    if (!valid_message_kind(kind)) {
        throw std::invalid_argument("worker protocol message kind is invalid");
    }
    if (payload.size() > kMaximumPayloadBytes) {
        throw std::length_error("worker protocol payload exceeds the size limit");
    }
    const auto frame_size = kProtocolHeaderBytes + payload.size();
    if (frame_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("worker protocol frame cannot be represented");
    }

    std::vector<std::byte> frame;
    frame.reserve(frame_size);
    append_integer(frame, kProtocolMagic);
    append_integer(frame, kProtocolMajor);
    append_integer(frame, kProtocolMinor);
    append_integer(frame, static_cast<std::uint16_t>(kind));
    append_integer(frame, static_cast<std::uint16_t>(0));
    append_integer(frame, static_cast<std::uint32_t>(frame_size));
    append_integer(frame, job_id);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

[[nodiscard]] bool decode_typed_frame(const std::span<const std::byte> bytes,
                                      const MessageKind expected_kind, DecodedFrame &frame,
                                      DecodeError &error) {
    if (!decode_frame(bytes, frame, error)) {
        return false;
    }
    if (frame.header.kind != expected_kind) {
        fail(error, DecodeErrorCode::unexpected_message_kind,
             "worker protocol message kind does not match the decoder");
        return false;
    }
    return true;
}

[[nodiscard]] bool finish_payload(const Cursor &cursor, DecodeError &error) {
    if (cursor.remaining() != 0) {
        fail(error, DecodeErrorCode::trailing_bytes, "worker protocol payload has trailing bytes");
        return false;
    }
    return true;
}

} // namespace

bool decode_frame(const std::span<const std::byte> bytes, DecodedFrame &frame, DecodeError &error) {
    error = {};
    if (bytes.size() < kProtocolHeaderBytes) {
        fail(error, DecodeErrorCode::truncated, "worker protocol header is truncated");
        return false;
    }

    Cursor cursor(bytes.first(kProtocolHeaderBytes));
    FrameHeader decoded;
    std::uint16_t encoded_kind{};
    if (!cursor.read(decoded.magic) || !cursor.read(decoded.major) || !cursor.read(decoded.minor) ||
        !cursor.read(encoded_kind) || !cursor.read(decoded.flags) ||
        !cursor.read(decoded.frame_bytes) || !cursor.read(decoded.job_id)) {
        fail(error, DecodeErrorCode::truncated, "worker protocol header is truncated");
        return false;
    }
    decoded.kind = static_cast<MessageKind>(encoded_kind);
    if (decoded.magic != kProtocolMagic) {
        fail(error, DecodeErrorCode::bad_magic, "worker protocol magic is invalid");
        return false;
    }
    if (decoded.major != kProtocolMajor || decoded.minor != kProtocolMinor) {
        fail(error, DecodeErrorCode::incompatible_version,
             "worker protocol version is incompatible");
        return false;
    }
    if (!valid_message_kind(decoded.kind)) {
        fail(error, DecodeErrorCode::unknown_message_kind,
             "worker protocol message kind is unknown");
        return false;
    }
    if (decoded.flags != 0) {
        fail(error, DecodeErrorCode::invalid_value, "worker protocol header flags are not zero");
        return false;
    }
    if (decoded.frame_bytes > kMaximumFrameBytes) {
        fail(error, DecodeErrorCode::frame_too_large,
             "worker protocol frame exceeds the size limit");
        return false;
    }
    if (decoded.frame_bytes < kProtocolHeaderBytes || decoded.frame_bytes != bytes.size()) {
        fail(error, DecodeErrorCode::frame_size_mismatch,
             "worker protocol frame size does not match its bytes");
        return false;
    }

    DecodedFrame output;
    output.header = decoded;
    output.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kProtocolHeaderBytes),
                          bytes.end());
    frame = std::move(output);
    return true;
}

std::vector<std::byte> encode_handshake(const Handshake &handshake, const MessageKind kind) {
    if (kind != MessageKind::handshake && kind != MessageKind::handshake_acknowledgement) {
        throw std::invalid_argument("handshake requires a handshake message kind");
    }
    if (!valid_build_id(handshake.build_id)) {
        throw std::invalid_argument("worker build id is invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(10U + handshake.build_id.size());
    append_integer(payload, handshake.capabilities);
    append_string(payload, handshake.build_id);
    return make_frame(kind, 0, payload);
}

bool decode_handshake(const std::span<const std::byte> bytes, Handshake &handshake,
                      DecodeError &error, const MessageKind expected_kind) {
    DecodedFrame frame;
    if (!decode_typed_frame(bytes, expected_kind, frame, error)) {
        return false;
    }
    if (frame.header.job_id != 0) {
        fail(error, DecodeErrorCode::invalid_value, "handshake job id must be zero");
        return false;
    }

    Cursor cursor(frame.payload);
    Handshake decoded;
    if (!cursor.read(decoded.capabilities) ||
        !cursor.read_string(decoded.build_id, kMaximumBuildIdBytes, error, "build id")) {
        if (!error) {
            fail(error, DecodeErrorCode::truncated, "handshake payload is truncated");
        }
        return false;
    }
    if (!finish_payload(cursor, error)) {
        return false;
    }
    if (!valid_build_id(decoded.build_id)) {
        fail(error, DecodeErrorCode::invalid_value, "worker build id is invalid");
        return false;
    }
    handshake = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_worker_job(const WorkerJob &job) {
    if (!valid_job(job)) {
        throw std::invalid_argument("worker job is invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(70U + kMaximumPasswordBytes);
    append_integer(payload, job.generation);
    append_integer(payload, job.source_token);
    append_integer(payload, job.output_token);
    append_integer(payload, job.profile_token);
    append_integer(payload, job.page_index);
    append_integer(payload, static_cast<std::uint8_t>(job.source_format_hint));
    append_string(payload, job.password_utf8);
    append_integer(payload, job.limits.maximum_input_bytes);
    append_integer(payload, job.limits.maximum_output_bytes);
    append_integer(payload, job.limits.memory_limit_bytes);
    append_integer(payload, job.limits.wall_timeout_ms);
    append_integer(payload, job.limits.canonical_edge);
    return make_frame(MessageKind::worker_job, job.job_id, payload);
}

bool decode_worker_job(const std::span<const std::byte> bytes, WorkerJob &job, DecodeError &error) {
    DecodedFrame frame;
    if (!decode_typed_frame(bytes, MessageKind::worker_job, frame, error)) {
        return false;
    }
    Cursor cursor(frame.payload);
    WorkerJob decoded;
    decoded.job_id = frame.header.job_id;
    std::uint8_t source_format_hint{};
    if (!cursor.read(decoded.generation) || !cursor.read(decoded.source_token) ||
        !cursor.read(decoded.output_token) || !cursor.read(decoded.profile_token) ||
        !cursor.read(decoded.page_index) || !cursor.read(source_format_hint) ||
        !cursor.read_string(decoded.password_utf8, kMaximumPasswordBytes, error, "password") ||
        !cursor.read(decoded.limits.maximum_input_bytes) ||
        !cursor.read(decoded.limits.maximum_output_bytes) ||
        !cursor.read(decoded.limits.memory_limit_bytes) ||
        !cursor.read(decoded.limits.wall_timeout_ms) ||
        !cursor.read(decoded.limits.canonical_edge)) {
        if (!error) {
            fail(error, DecodeErrorCode::truncated, "worker job payload is truncated");
        }
        return false;
    }
    decoded.source_format_hint = static_cast<SourceFormatHint>(source_format_hint);
    if (!finish_payload(cursor, error)) {
        return false;
    }
    if (!valid_job(decoded)) {
        fail(error, DecodeErrorCode::limit_exceeded,
             "worker job contains an invalid token or limit");
        return false;
    }
    job = decoded;
    return true;
}

std::vector<std::byte> encode_worker_result(const WorkerResult &result) {
    if (!valid_result(result)) {
        throw std::invalid_argument("worker result is invalid");
    }
    std::vector<std::byte> payload(34U + result.color_profile_utf8.size() +
                                   result.source_profile_fingerprint.size() +
                                   result.diagnostic_utf8.size());
    std::size_t offset{};
    write_integer(payload, offset, static_cast<std::uint8_t>(result.status));
    write_integer(payload, offset, static_cast<std::uint8_t>(result.color_model));
    write_integer(payload, offset, static_cast<std::uint8_t>(result.provenance));
    write_integer(payload, offset, static_cast<std::uint8_t>(0));
    write_integer(payload, offset, result.width);
    write_integer(payload, offset, result.height);
    write_integer(payload, offset, result.page_index);
    write_integer(payload, offset, result.page_count);
    write_integer(payload, offset, result.bytes_written);
    write_string(payload, offset, result.color_profile_utf8);
    write_string(payload, offset, result.source_profile_fingerprint);
    write_string(payload, offset, result.diagnostic_utf8);
    return make_frame(MessageKind::worker_result, result.job_id, payload);
}

bool decode_worker_result(const std::span<const std::byte> bytes, WorkerResult &result,
                          DecodeError &error) {
    DecodedFrame frame;
    if (!decode_typed_frame(bytes, MessageKind::worker_result, frame, error)) {
        return false;
    }
    Cursor cursor(frame.payload);
    WorkerResult decoded;
    decoded.job_id = frame.header.job_id;
    std::uint8_t encoded_status{};
    std::uint8_t encoded_color_model{};
    std::uint8_t encoded_provenance{};
    std::uint8_t reserved{};
    if (!cursor.read(encoded_status) || !cursor.read(encoded_color_model) ||
        !cursor.read(encoded_provenance) || !cursor.read(reserved) || !cursor.read(decoded.width) ||
        !cursor.read(decoded.height) || !cursor.read(decoded.page_index) ||
        !cursor.read(decoded.page_count) || !cursor.read(decoded.bytes_written) ||
        !cursor.read_string(decoded.color_profile_utf8, kMaximumProfileNameBytes, error,
                            "color profile") ||
        !cursor.read_string(decoded.source_profile_fingerprint, kProfileFingerprintBytes, error,
                            "source profile fingerprint") ||
        !cursor.read_string(decoded.diagnostic_utf8, kMaximumDiagnosticBytes, error,
                            "diagnostic")) {
        if (!error) {
            fail(error, DecodeErrorCode::truncated, "worker result payload is truncated");
        }
        return false;
    }
    if (!finish_payload(cursor, error)) {
        return false;
    }
    decoded.status = static_cast<ResultStatus>(encoded_status);
    decoded.color_model = static_cast<ColorModel>(encoded_color_model);
    decoded.provenance = static_cast<PreviewProvenance>(encoded_provenance);
    if (reserved != 0 || !valid_result(decoded)) {
        fail(error, DecodeErrorCode::invalid_value,
             "worker result metadata is invalid or exceeds a limit");
        return false;
    }
    result = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_animation_frame(const AnimationFrame &frame) {
    if (frame.image.status != ResultStatus::success || frame.delay_ms == 0 ||
        frame.delay_ms > 655'350 || frame.image.width == 0 || frame.image.height == 0 ||
        frame.image.bytes_written == 0) {
        throw std::invalid_argument("invalid animation frame");
    }
    std::vector<std::byte> payload;
    append_integer(payload, frame.delay_ms);
    append_integer(payload, frame.sequence);
    append_integer(payload, static_cast<std::uint8_t>(frame.animated));
    const auto result = encode_worker_result(frame.image);
    payload.insert(payload.end(), result.begin(), result.end());
    return make_frame(MessageKind::animation_frame, frame.image.job_id, payload);
}

bool decode_animation_frame(const std::span<const std::byte> bytes, AnimationFrame &result,
                            DecodeError &error) {
    DecodedFrame frame;
    if (!decode_typed_frame(bytes, MessageKind::animation_frame, frame, error))
        return false;
    Cursor cursor(frame.payload);
    AnimationFrame decoded;
    std::uint8_t animated{};
    if (!cursor.read(decoded.delay_ms) || !cursor.read(decoded.sequence) ||
        !cursor.read(animated)) {
        fail(error, DecodeErrorCode::truncated, "animation metadata is truncated");
        return false;
    }
    if (animated > 1 || decoded.delay_ms == 0 || decoded.delay_ms > 655'350 ||
        !decode_worker_result(std::span<const std::byte>{frame.payload}.subspan(13),
                              decoded.image, error) ||
        decoded.image.job_id != frame.header.job_id ||
        decoded.image.status != ResultStatus::success || decoded.image.width == 0 ||
        decoded.image.height == 0 || decoded.image.bytes_written == 0) {
        if (!error)
            fail(error, DecodeErrorCode::invalid_value, "invalid animation metadata");
        return false;
    }
    decoded.animated = animated != 0;
    result = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_animation_advance(const AnimationAdvance &advance) {
    if (advance.job_id == 0)
        throw std::invalid_argument("animation acknowledgement needs a job id");
    const std::array payload{static_cast<std::byte>(advance.proceed ? 1 : 0)};
    return make_frame(MessageKind::animation_advance, advance.job_id, payload);
}

bool decode_animation_advance(const std::span<const std::byte> bytes, AnimationAdvance &advance,
                              DecodeError &error) {
    DecodedFrame frame;
    if (!decode_typed_frame(bytes, MessageKind::animation_advance, frame, error))
        return false;
    if (frame.header.job_id == 0 || frame.payload.size() != 1 ||
        std::to_integer<unsigned>(frame.payload[0]) > 1) {
        fail(error, DecodeErrorCode::invalid_value, "invalid animation acknowledgement");
        return false;
    }
    advance = {frame.header.job_id, frame.payload[0] != std::byte{0}};
    return true;
}

std::vector<std::byte> encode_cancel_generation(const CancelGeneration cancel) {
    if (cancel.generation == 0) {
        throw std::invalid_argument("cancelled generation must be non-zero");
    }
    std::vector<std::byte> payload;
    payload.reserve(sizeof(cancel.generation));
    append_integer(payload, cancel.generation);
    return make_frame(MessageKind::cancel_generation, 0, payload);
}

bool decode_cancel_generation(const std::span<const std::byte> bytes, CancelGeneration &cancel,
                              DecodeError &error) {
    DecodedFrame frame;
    if (!decode_typed_frame(bytes, MessageKind::cancel_generation, frame, error)) {
        return false;
    }
    if (frame.header.job_id != 0) {
        fail(error, DecodeErrorCode::invalid_value, "generation cancellation job id must be zero");
        return false;
    }
    Cursor cursor(frame.payload);
    CancelGeneration decoded;
    if (!cursor.read(decoded.generation)) {
        fail(error, DecodeErrorCode::truncated, "generation cancellation payload is truncated");
        return false;
    }
    if (!finish_payload(cursor, error)) {
        return false;
    }
    if (decoded.generation == 0) {
        fail(error, DecodeErrorCode::invalid_value, "cancelled generation must be non-zero");
        return false;
    }
    cancel = decoded;
    return true;
}

} // namespace vove::worker
