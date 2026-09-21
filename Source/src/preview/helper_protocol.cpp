#include "vove/preview/helper_protocol.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vove::preview::helper_protocol {
namespace {

template <typename Integer>
void append_integer(std::vector<std::byte> &bytes, const Integer value) {
    static_assert(std::is_integral_v<Integer>);
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto encoded = static_cast<Unsigned>(value);
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(Integer));
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((encoded >> (index * 8U)) & static_cast<Unsigned>(0xFFU));
    }
}

void append_text(std::vector<std::byte> &bytes, const std::string_view value) {
    const auto offset = bytes.size();
    bytes.resize(offset + value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
    }
}

void append_string16(std::vector<std::byte> &bytes, const std::string_view value) {
    append_integer(bytes, static_cast<std::uint16_t>(value.size()));
    append_text(bytes, value);
}

void append_string32(std::vector<std::byte> &bytes, const std::string_view value) {
    append_integer(bytes, static_cast<std::uint32_t>(value.size()));
    append_text(bytes, value);
}

void append_blob(std::vector<std::byte> &bytes, const std::span<const std::byte> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void fail(DecodeError &error, const DecodeErrorCode code, std::string message) {
    error = {.code = code, .message = std::move(message)};
}

class Reader {
  public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename Integer> [[nodiscard]] bool read(Integer &value) {
        static_assert(std::is_integral_v<Integer>);
        if (remaining() < sizeof(Integer)) {
            return false;
        }

        using Unsigned = std::make_unsigned_t<Integer>;
        Unsigned decoded{};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            const auto octet =
                static_cast<Unsigned>(std::to_integer<unsigned char>(bytes_[offset_ + index]));
            decoded |= static_cast<Unsigned>(octet << (index * 8U));
        }
        if constexpr (std::is_signed_v<Integer>) {
            value = std::bit_cast<Integer>(decoded);
        } else {
            value = decoded;
        }
        offset_ += sizeof(Integer);
        return true;
    }

    [[nodiscard]] bool read_string(std::string &value, const std::size_t size) {
        if (remaining() < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool read_blob(std::vector<std::byte> &value, const std::size_t size) {
        if (remaining() < size) {
            return false;
        }
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

  private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

struct ParsedFrame {
    FrameHeader header;
    std::span<const std::byte> payload;
};

struct HelloFields {
    std::string build_id;
    std::string auth_token;
};

[[nodiscard]] bool valid_message_type(const MessageType type) noexcept {
    switch (type) {
    case MessageType::hello:
    case MessageType::hello_ack:
    case MessageType::preview_request:
    case MessageType::cancel_generation:
    case MessageType::preview_response:
    case MessageType::job_started:
    case MessageType::cache_request:
    case MessageType::cache_response:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_priority(const ThumbnailPriority priority) noexcept {
    switch (priority) {
    case ThumbnailPriority::visible_selected:
    case ThumbnailPriority::visible:
    case ThumbnailPriority::nearby:
    case ThumbnailPriority::rest:
    case ThumbnailPriority::folder:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_status(const ResponseStatus status) noexcept {
    switch (status) {
    case ResponseStatus::success_cached:
    case ResponseStatus::success_offline_cached:
    case ResponseStatus::source_changed:
    case ResponseStatus::success_decoded:
    case ResponseStatus::unsupported:
    case ResponseStatus::malformed:
    case ResponseStatus::disconnected:
    case ResponseStatus::authentication_failed:
    case ResponseStatus::permission_denied:
    case ResponseStatus::not_found:
    case ResponseStatus::too_large:
    case ResponseStatus::pdf_input_too_large:
    case ResponseStatus::memory_limit:
    case ResponseStatus::processing_timed_out:
    case ResponseStatus::worker_start_failed:
    case ResponseStatus::timed_out:
    case ResponseStatus::internal_error:
    case ResponseStatus::cancelled:
    case ResponseStatus::queue_busy:
    case ResponseStatus::source_unavailable:
    case ResponseStatus::color_profile_required:
    case ResponseStatus::password_required:
    case ResponseStatus::document_password_incorrect:
    case ResponseStatus::ghostscript_required:
    case ResponseStatus::embedded_preview_unavailable:
        return true;
    }
    return false;
}

[[nodiscard]] bool is_success(const ResponseStatus status) noexcept {
    return status == ResponseStatus::success_cached ||
           status == ResponseStatus::success_offline_cached ||
           status == ResponseStatus::success_decoded;
}

[[nodiscard]] bool valid_fingerprint(const std::string_view fingerprint) noexcept {
    if (fingerprint.empty()) {
        return true;
    }
    return fingerprint.size() == kProfileFingerprintBytes &&
           std::ranges::all_of(fingerprint, [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
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
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
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

[[nodiscard]] bool valid_text(const std::string_view text, const std::size_t maximum,
                              const bool allow_empty) noexcept {
    return (allow_empty || !text.empty()) && text.size() <= maximum &&
           text.find('\0') == std::string_view::npos && valid_utf8(text);
}

[[nodiscard]] bool valid_hello_fields(const std::string_view build_id,
                                      const std::string_view auth_token) noexcept {
    return valid_text(build_id, kMaximumBuildIdBytes, false) &&
           valid_text(auth_token, kMaximumAuthTokenBytes, false);
}

[[nodiscard]] bool valid_request(const PreviewRequest &request) noexcept {
    return request.request_id != 0 && request.generation != 0 &&
           valid_text(request.path_utf8, kMaximumPathBytes, false) && request.canonical_edge != 0 &&
           request.canonical_edge <= kMaximumCanonicalEdge && valid_priority(request.priority) &&
           valid_text(request.source_revision_utf8, kMaximumSourceRevisionBytes, true) &&
           request.page_index < kMaximumPageCount &&
           valid_text(request.password_utf8, kMaximumPasswordBytes, true);
}

[[nodiscard]] bool expected_rgba_size(const std::uint16_t width, const std::uint16_t height,
                                      std::size_t &bytes) noexcept {
    if (width == 0 || height == 0 || width > kMaximumCanonicalEdge ||
        height > kMaximumCanonicalEdge) {
        return false;
    }
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixels > kMaximumRgbaBytes / 4U) {
        return false;
    }
    bytes = pixels * 4U;
    return true;
}

[[nodiscard]] bool valid_response(const PreviewResponse &response) noexcept {
    if (!response.startup_diagnostic.valid() ||
        response.startup_diagnostic.present() !=
            (response.status == ResponseStatus::worker_start_failed)) {
        return false;
    }
    if (response.request_id == 0 || !valid_status(response.status) ||
        response.provenance > PreviewProvenance::embedded_preview ||
        response.width > kMaximumCanonicalEdge || response.height > kMaximumCanonicalEdge ||
        response.page_count > kMaximumPageCount ||
        !valid_text(response.source_color_model_utf8, kMaximumColorModelBytes, true) ||
        !valid_text(response.source_color_profile_utf8, kMaximumColorProfileBytes, true) ||
        !valid_fingerprint(response.source_profile_fingerprint) ||
        response.rgba8.size() > kMaximumRgbaBytes) {
        return false;
    }

    if (!is_success(response.status)) {
        return response.width == 0 && response.height == 0 &&
               response.page_index < kMaximumPageCount && response.page_count == 0 &&
               response.source_color_model_utf8.empty() &&
               response.source_color_profile_utf8.empty() &&
               response.source_profile_fingerprint.empty() && response.rgba8.empty() &&
               response.provenance == PreviewProvenance::primary_render;
    }

    std::size_t expected{};
    return response.page_count != 0 && response.page_index < response.page_count &&
           expected_rgba_size(response.width, response.height, expected) &&
           response.rgba8.size() == expected;
}

[[nodiscard]] std::vector<std::byte> make_frame(const MessageType type, const RequestId request_id,
                                                const std::span<const std::byte> payload) {
    if (!valid_message_type(type)) {
        throw std::invalid_argument("helper protocol message type is invalid");
    }
    if (payload.size() > kMaximumFramePayloadBytes) {
        throw std::length_error("helper protocol frame exceeds 2 MiB");
    }
    const auto total_size = kFrameHeaderBytes + payload.size();
    if (total_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("helper protocol frame size cannot be represented");
    }

    std::vector<std::byte> frame;
    frame.reserve(total_size);
    append_integer(frame, kMagic);
    append_integer(frame, kProtocolMajor);
    append_integer(frame, kProtocolMinor);
    append_integer(frame, static_cast<std::uint16_t>(type));
    append_integer(frame, static_cast<std::uint16_t>(0));
    append_integer(frame, static_cast<std::uint32_t>(total_size));
    append_integer(frame, request_id);
    append_blob(frame, payload);
    return frame;
}

[[nodiscard]] bool parse_frame(const std::span<const std::byte> bytes,
                               const MessageType expected_type, ParsedFrame &frame,
                               DecodeError &error) {
    FrameHeader header;
    if (!decode_header(bytes, header, error)) {
        return false;
    }
    if (header.type != expected_type) {
        fail(error, DecodeErrorCode::unexpected_message_type,
             "helper protocol frame has the wrong message type");
        return false;
    }
    const auto declared = static_cast<std::size_t>(header.total_size);
    if (bytes.size() < declared) {
        fail(error, DecodeErrorCode::truncated, "helper protocol frame is truncated");
        return false;
    }
    if (bytes.size() > declared) {
        fail(error, DecodeErrorCode::trailing_bytes, "helper protocol frame has trailing bytes");
        return false;
    }
    frame = {.header = header, .payload = bytes.subspan(kFrameHeaderBytes)};
    return true;
}

[[nodiscard]] bool finish_payload(const Reader &reader, DecodeError &error) {
    if (reader.remaining() != 0) {
        fail(error, DecodeErrorCode::trailing_bytes, "helper protocol payload has trailing bytes");
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::byte> encode_hello_like(const std::string_view build_id,
                                                       const std::string_view auth_token,
                                                       const MessageType type) {
    if (!valid_hello_fields(build_id, auth_token)) {
        throw std::invalid_argument("helper protocol hello fields are invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(4U + build_id.size() + auth_token.size());
    append_string16(payload, build_id);
    append_string16(payload, auth_token);
    return make_frame(type, 0, payload);
}

[[nodiscard]] bool decode_hello_like(const std::span<const std::byte> bytes, HelloFields &fields,
                                     DecodeError &error, const MessageType expected_type) {
    ParsedFrame frame;
    if (!parse_frame(bytes, expected_type, frame, error)) {
        return false;
    }
    if (frame.header.request_id != 0) {
        fail(error, DecodeErrorCode::invalid_value,
             "helper protocol hello request id must be zero");
        return false;
    }

    Reader reader(frame.payload);
    std::uint16_t build_size{};
    std::uint16_t token_size{};
    std::string decoded_build;
    std::string decoded_token;
    if (!reader.read(build_size)) {
        fail(error, DecodeErrorCode::truncated, "helper build id length is truncated");
        return false;
    }
    if (build_size == 0 || build_size > kMaximumBuildIdBytes) {
        fail(error, DecodeErrorCode::limit_exceeded, "helper build id length is invalid");
        return false;
    }
    if (!reader.read_string(decoded_build, build_size) || !reader.read(token_size)) {
        fail(error, DecodeErrorCode::truncated, "helper hello fields are truncated");
        return false;
    }
    if (token_size == 0 || token_size > kMaximumAuthTokenBytes) {
        fail(error, DecodeErrorCode::limit_exceeded, "helper auth token length is invalid");
        return false;
    }
    if (!reader.read_string(decoded_token, token_size)) {
        fail(error, DecodeErrorCode::truncated, "helper auth token is truncated");
        return false;
    }
    if (!finish_payload(reader, error)) {
        return false;
    }
    if (!valid_utf8(decoded_build) || !valid_utf8(decoded_token) ||
        decoded_build.find('\0') != std::string::npos ||
        decoded_token.find('\0') != std::string::npos) {
        fail(error, DecodeErrorCode::invalid_utf8, "helper hello text is invalid UTF-8");
        return false;
    }
    fields = {.build_id = std::move(decoded_build), .auth_token = std::move(decoded_token)};
    return true;
}

} // namespace

bool decode_header(const std::span<const std::byte> bytes, FrameHeader &header,
                   DecodeError &error) {
    error = {};
    if (bytes.size() < kFrameHeaderBytes) {
        fail(error, DecodeErrorCode::truncated, "helper protocol header is truncated");
        return false;
    }

    Reader reader(bytes.first(kFrameHeaderBytes));
    FrameHeader decoded;
    std::uint16_t encoded_type{};
    if (!reader.read(decoded.magic) || !reader.read(decoded.major) || !reader.read(decoded.minor) ||
        !reader.read(encoded_type) || !reader.read(decoded.flags) ||
        !reader.read(decoded.total_size) || !reader.read(decoded.request_id)) {
        fail(error, DecodeErrorCode::truncated, "helper protocol header is truncated");
        return false;
    }

    if (decoded.magic != kMagic) {
        fail(error, DecodeErrorCode::bad_magic, "helper protocol magic does not match");
        return false;
    }
    if (decoded.major != kProtocolMajor || decoded.minor != kProtocolMinor) {
        fail(error, DecodeErrorCode::incompatible_version,
             "helper protocol version is incompatible");
        return false;
    }
    decoded.type = static_cast<MessageType>(encoded_type);
    if (!valid_message_type(decoded.type)) {
        fail(error, DecodeErrorCode::unknown_message_type,
             "helper protocol message type is unknown");
        return false;
    }
    if (decoded.flags != 0) {
        fail(error, DecodeErrorCode::invalid_value, "helper protocol reserved flags are non-zero");
        return false;
    }
    if (decoded.total_size < kFrameHeaderBytes) {
        fail(error, DecodeErrorCode::frame_size_mismatch,
             "helper protocol frame is smaller than its header");
        return false;
    }
    if (decoded.total_size > kMaximumFrameBytes) {
        fail(error, DecodeErrorCode::frame_too_large, "helper protocol frame exceeds 2 MiB");
        return false;
    }
    header = decoded;
    return true;
}

std::vector<std::byte> encode_hello(const Hello &hello) {
    return encode_hello_like(hello.build_id_utf8, hello.auth_token_utf8, MessageType::hello);
}

bool decode_hello(const std::span<const std::byte> bytes, Hello &hello, DecodeError &error) {
    HelloFields fields;
    if (!decode_hello_like(bytes, fields, error, MessageType::hello)) {
        return false;
    }
    hello = {.build_id_utf8 = std::move(fields.build_id),
             .auth_token_utf8 = std::move(fields.auth_token)};
    return true;
}

std::vector<std::byte> encode_hello_ack(const HelloAck &ack) {
    return encode_hello_like(ack.build_id_utf8, ack.auth_token_utf8, MessageType::hello_ack);
}

bool decode_hello_ack(const std::span<const std::byte> bytes, HelloAck &ack, DecodeError &error) {
    HelloFields fields;
    if (!decode_hello_like(bytes, fields, error, MessageType::hello_ack)) {
        return false;
    }
    ack = {.build_id_utf8 = std::move(fields.build_id),
           .auth_token_utf8 = std::move(fields.auth_token)};
    return true;
}

std::vector<std::byte> encode_preview_request(const PreviewRequest &request) {
    if (!valid_request(request)) {
        throw std::invalid_argument("helper preview request is invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(40U + request.path_utf8.size() + request.source_revision_utf8.size() +
                    request.password_utf8.size());
    append_integer(payload, request.generation);
    append_integer(payload, request.source_size);
    append_integer(payload, request.modified_unix_ns);
    append_integer(payload, request.canonical_edge);
    append_integer(payload, static_cast<std::uint8_t>(request.priority));
    const auto request_options = static_cast<std::uint8_t>(
        (request.offline_cache_only ? 1U : 0U) | (request.allow_offline_fallback ? 2U : 0U) |
        (request.selected_view ? 4U : 0U) | (request.extended_limits ? 8U : 0U));
    append_integer(payload, request_options);
    append_integer(payload, request.page_index);
    append_string32(payload, request.path_utf8);
    append_string16(payload, request.source_revision_utf8);
    append_string16(payload, request.password_utf8);
    return make_frame(MessageType::preview_request, request.request_id, payload);
}

bool decode_preview_request(const std::span<const std::byte> bytes, PreviewRequest &request,
                            DecodeError &error) {
    ParsedFrame frame;
    if (!parse_frame(bytes, MessageType::preview_request, frame, error)) {
        return false;
    }

    Reader reader(frame.payload);
    PreviewRequest decoded;
    decoded.request_id = frame.header.request_id;
    std::uint8_t encoded_priority{};
    std::uint8_t request_options{};
    std::uint32_t path_size{};
    if (!reader.read(decoded.generation) || !reader.read(decoded.source_size) ||
        !reader.read(decoded.modified_unix_ns) || !reader.read(decoded.canonical_edge) ||
        !reader.read(encoded_priority) || !reader.read(request_options) ||
        !reader.read(decoded.page_index) || !reader.read(path_size)) {
        fail(error, DecodeErrorCode::truncated, "helper preview request is truncated");
        return false;
    }
    if ((request_options & ~std::uint8_t{15U}) != 0U) {
        fail(error, DecodeErrorCode::invalid_value, "helper preview request options are invalid");
        return false;
    }
    decoded.offline_cache_only = (request_options & 1U) != 0U;
    decoded.allow_offline_fallback = (request_options & 2U) != 0U;
    decoded.selected_view = (request_options & 4U) != 0U;
    decoded.extended_limits = (request_options & 8U) != 0U;
    decoded.priority = static_cast<ThumbnailPriority>(encoded_priority);
    if (!valid_priority(decoded.priority)) {
        fail(error, DecodeErrorCode::invalid_value, "helper preview priority is unknown");
        return false;
    }
    if (path_size == 0 || path_size > kMaximumPathBytes) {
        fail(error, DecodeErrorCode::limit_exceeded, "helper preview path length is invalid");
        return false;
    }
    if (!reader.read_string(decoded.path_utf8, path_size)) {
        fail(error, DecodeErrorCode::truncated, "helper preview path is truncated");
        return false;
    }
    std::uint16_t revision_size{};
    if (!reader.read(revision_size)) {
        fail(error, DecodeErrorCode::truncated,
             "helper preview source revision length is truncated");
        return false;
    }
    if (revision_size > kMaximumSourceRevisionBytes) {
        fail(error, DecodeErrorCode::limit_exceeded,
             "helper preview source revision length is invalid");
        return false;
    }
    if (!reader.read_string(decoded.source_revision_utf8, revision_size)) {
        fail(error, DecodeErrorCode::truncated, "helper preview source revision is truncated");
        return false;
    }
    std::uint16_t password_size{};
    if (!reader.read(password_size)) {
        fail(error, DecodeErrorCode::truncated, "helper preview password length is truncated");
        return false;
    }
    if (password_size > kMaximumPasswordBytes) {
        fail(error, DecodeErrorCode::limit_exceeded, "helper preview password length is invalid");
        return false;
    }
    if (!reader.read_string(decoded.password_utf8, password_size)) {
        fail(error, DecodeErrorCode::truncated, "helper preview password is truncated");
        return false;
    }
    if (!finish_payload(reader, error)) {
        return false;
    }
    if (!valid_utf8(decoded.path_utf8) || decoded.path_utf8.find('\0') != std::string::npos ||
        !valid_utf8(decoded.source_revision_utf8) ||
        decoded.source_revision_utf8.find('\0') != std::string::npos ||
        !valid_utf8(decoded.password_utf8) ||
        decoded.password_utf8.find('\0') != std::string::npos) {
        fail(error, DecodeErrorCode::invalid_utf8,
             "helper preview path is invalid UTF-8 or contains NUL");
        return false;
    }
    if (!valid_request(decoded)) {
        fail(error, DecodeErrorCode::invalid_value, "helper preview request values are invalid");
        return false;
    }
    request = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_cancel_generation(const CancelGeneration cancel) {
    if (cancel.generation == 0) {
        throw std::invalid_argument("helper cancelled generation must be non-zero");
    }
    std::vector<std::byte> payload;
    payload.reserve(sizeof(cancel.generation));
    append_integer(payload, cancel.generation);
    return make_frame(MessageType::cancel_generation, 0, payload);
}

bool decode_cancel_generation(const std::span<const std::byte> bytes, CancelGeneration &cancel,
                              DecodeError &error) {
    ParsedFrame frame;
    if (!parse_frame(bytes, MessageType::cancel_generation, frame, error)) {
        return false;
    }
    if (frame.header.request_id != 0) {
        fail(error, DecodeErrorCode::invalid_value, "helper cancellation request id must be zero");
        return false;
    }

    Reader reader(frame.payload);
    CancelGeneration decoded;
    if (!reader.read(decoded.generation)) {
        fail(error, DecodeErrorCode::truncated, "helper cancellation is truncated");
        return false;
    }
    if (!finish_payload(reader, error)) {
        return false;
    }
    if (decoded.generation == 0) {
        fail(error, DecodeErrorCode::invalid_value, "helper cancelled generation must be non-zero");
        return false;
    }
    cancel = decoded;
    return true;
}

std::vector<std::byte> encode_job_started(const JobStarted started) {
    if (started.request_id == 0) {
        throw std::invalid_argument("helper started request id must be non-zero");
    }
    return make_frame(MessageType::job_started, started.request_id, {});
}

bool decode_job_started(const std::span<const std::byte> bytes, JobStarted &started,
                        DecodeError &error) {
    ParsedFrame frame;
    if (!parse_frame(bytes, MessageType::job_started, frame, error)) {
        return false;
    }
    if (frame.header.request_id == 0 || !frame.payload.empty()) {
        fail(error, DecodeErrorCode::invalid_value,
             "helper started frame must contain only a non-zero request id");
        return false;
    }
    started = {.request_id = frame.header.request_id};
    return true;
}

std::vector<std::byte> encode_preview_response(const PreviewResponse &response) {
    if (!valid_response(response)) {
        throw std::invalid_argument("helper preview response is invalid");
    }

    std::vector<std::byte> payload;
    payload.reserve(34U + response.source_color_model_utf8.size() +
                    response.source_color_profile_utf8.size() +
                    response.source_profile_fingerprint.size() + response.rgba8.size());
    append_integer(payload, static_cast<std::uint8_t>(response.status));
    append_integer(payload, static_cast<std::uint8_t>(response.provenance));
    append_integer(payload, response.width);
    append_integer(payload, response.height);
    append_integer(payload, static_cast<std::uint16_t>(response.source_color_model_utf8.size()));
    append_integer(payload, static_cast<std::uint16_t>(response.source_color_profile_utf8.size()));
    append_integer(payload, static_cast<std::uint16_t>(response.source_profile_fingerprint.size()));
    append_integer(payload, response.page_index);
    append_integer(payload, response.page_count);
    append_integer(payload, static_cast<std::uint32_t>(response.rgba8.size()));
    append_integer(payload, static_cast<std::uint8_t>(response.startup_diagnostic.stage));
    append_integer(payload, static_cast<std::uint8_t>(response.startup_diagnostic.worker));
    append_integer(payload, response.startup_diagnostic.system_error);
    append_integer(payload, response.startup_diagnostic.exit_code);
    append_text(payload, response.source_color_model_utf8);
    append_text(payload, response.source_color_profile_utf8);
    append_text(payload, response.source_profile_fingerprint);
    append_blob(payload, response.rgba8);
    return make_frame(MessageType::preview_response, response.request_id, payload);
}

bool decode_preview_response(const std::span<const std::byte> bytes, PreviewResponse &response,
                             DecodeError &error) {
    ParsedFrame frame;
    if (!parse_frame(bytes, MessageType::preview_response, frame, error)) {
        return false;
    }

    Reader reader(frame.payload);
    PreviewResponse decoded;
    decoded.request_id = frame.header.request_id;
    std::uint8_t encoded_status{};
    std::uint8_t encoded_provenance{};
    std::uint16_t model_size{};
    std::uint16_t profile_size{};
    std::uint16_t fingerprint_size{};
    std::uint32_t rgba_size{};
    std::uint8_t startup_stage{}, startup_worker{};
    if (!reader.read(encoded_status) || !reader.read(encoded_provenance) ||
        !reader.read(decoded.width) || !reader.read(decoded.height) || !reader.read(model_size) ||
        !reader.read(profile_size) || !reader.read(fingerprint_size) ||
        !reader.read(decoded.page_index) || !reader.read(decoded.page_count) ||
        !reader.read(rgba_size) || !reader.read(startup_stage) || !reader.read(startup_worker) ||
        !reader.read(decoded.startup_diagnostic.system_error) ||
        !reader.read(decoded.startup_diagnostic.exit_code)) {
        fail(error, DecodeErrorCode::truncated, "helper preview response is truncated");
        return false;
    }
    decoded.provenance = static_cast<PreviewProvenance>(encoded_provenance);
    decoded.startup_diagnostic.stage = static_cast<WorkerStartupStage>(startup_stage);
    decoded.startup_diagnostic.worker = static_cast<PreviewWorker>(startup_worker);
    if (decoded.provenance > PreviewProvenance::embedded_preview) {
        fail(error, DecodeErrorCode::invalid_value,
             "helper preview response provenance is unknown");
        return false;
    }
    decoded.status = static_cast<ResponseStatus>(encoded_status);
    if (!valid_status(decoded.status)) {
        fail(error, DecodeErrorCode::invalid_value, "helper preview response status is unknown");
        return false;
    }
    if (model_size > kMaximumColorModelBytes || profile_size > kMaximumColorProfileBytes ||
        fingerprint_size > kProfileFingerprintBytes || rgba_size > kMaximumRgbaBytes) {
        fail(error, DecodeErrorCode::limit_exceeded,
             "helper preview response field exceeds its size limit");
        return false;
    }
    if (!reader.read_string(decoded.source_color_model_utf8, model_size) ||
        !reader.read_string(decoded.source_color_profile_utf8, profile_size) ||
        !reader.read_string(decoded.source_profile_fingerprint, fingerprint_size) ||
        !reader.read_blob(decoded.rgba8, rgba_size)) {
        fail(error, DecodeErrorCode::truncated, "helper preview response payload is truncated");
        return false;
    }
    if (!finish_payload(reader, error)) {
        return false;
    }
    if (!valid_utf8(decoded.source_color_model_utf8) ||
        !valid_utf8(decoded.source_color_profile_utf8) ||
        decoded.source_color_model_utf8.find('\0') != std::string::npos ||
        decoded.source_color_profile_utf8.find('\0') != std::string::npos) {
        fail(error, DecodeErrorCode::invalid_utf8,
             "helper preview response metadata is invalid UTF-8");
        return false;
    }
    if (!valid_fingerprint(decoded.source_profile_fingerprint)) {
        fail(error, DecodeErrorCode::invalid_value,
             "helper preview response profile fingerprint is invalid");
        return false;
    }

    if (is_success(decoded.status)) {
        std::size_t expected{};
        if (!expected_rgba_size(decoded.width, decoded.height, expected) ||
            decoded.rgba8.size() != expected) {
            fail(error, DecodeErrorCode::payload_size_mismatch,
                 "helper RGBA payload does not match its dimensions");
            return false;
        }
        if (decoded.page_count == 0 || decoded.page_count > kMaximumPageCount ||
            decoded.page_index >= decoded.page_count || decoded.request_id == 0) {
            fail(error, DecodeErrorCode::invalid_value,
                 "helper successful response metadata is invalid");
            return false;
        }
    } else if (!valid_response(decoded)) {
        fail(error, DecodeErrorCode::invalid_value,
             "helper failed response must not carry image data or metadata");
        return false;
    }

    if (!valid_response(decoded)) {
        fail(error, DecodeErrorCode::invalid_value, "helper preview response values are invalid");
        return false;
    }
    response = std::move(decoded);
    return true;
}

std::vector<std::byte> encode_cache_request(const CacheRequest request) {
    if (request.request_id == 0 || request.operation > CacheOperation::clear) {
        throw std::invalid_argument("helper cache request is invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(1);
    append_integer(payload, static_cast<std::uint8_t>(request.operation));
    return make_frame(MessageType::cache_request, request.request_id, payload);
}

bool decode_cache_request(const std::span<const std::byte> bytes, CacheRequest &request,
                          DecodeError &error) {
    ParsedFrame frame;
    if (!parse_frame(bytes, MessageType::cache_request, frame, error)) {
        return false;
    }
    Reader reader(frame.payload);
    std::uint8_t operation{};
    if (frame.header.request_id == 0 || !reader.read(operation) || !finish_payload(reader, error)) {
        if (!error) {
            fail(error, DecodeErrorCode::invalid_value, "helper cache request is invalid");
        }
        return false;
    }
    if (operation > static_cast<std::uint8_t>(CacheOperation::clear)) {
        fail(error, DecodeErrorCode::invalid_value, "helper cache operation is unknown");
        return false;
    }
    request = {.request_id = frame.header.request_id,
               .operation = static_cast<CacheOperation>(operation)};
    return true;
}

std::vector<std::byte> encode_cache_response(const CacheResponse response) {
    if (response.request_id == 0 || response.status > CacheStatus::failed) {
        throw std::invalid_argument("helper cache response is invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(17);
    append_integer(payload, static_cast<std::uint8_t>(response.status));
    append_integer(payload, response.entries);
    append_integer(payload, response.bytes);
    return make_frame(MessageType::cache_response, response.request_id, payload);
}

bool decode_cache_response(const std::span<const std::byte> bytes, CacheResponse &response,
                           DecodeError &error) {
    ParsedFrame frame;
    if (!parse_frame(bytes, MessageType::cache_response, frame, error)) {
        return false;
    }
    Reader reader(frame.payload);
    std::uint8_t status{};
    CacheResponse decoded{.request_id = frame.header.request_id};
    if (decoded.request_id == 0 || !reader.read(status) || !reader.read(decoded.entries) ||
        !reader.read(decoded.bytes) || !finish_payload(reader, error)) {
        if (!error) {
            fail(error, DecodeErrorCode::invalid_value, "helper cache response is invalid");
        }
        return false;
    }
    if (status > static_cast<std::uint8_t>(CacheStatus::failed)) {
        fail(error, DecodeErrorCode::invalid_value, "helper cache status is unknown");
        return false;
    }
    decoded.status = static_cast<CacheStatus>(status);
    response = decoded;
    return true;
}

} // namespace vove::preview::helper_protocol
