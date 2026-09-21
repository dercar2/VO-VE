#include "vove/handlers/xcf/kimageformats_xcf_decoder.hpp"

#include "vove/handlers/raster/profile_fingerprint.hpp"

#include "xcf_p.h"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QIODevice>
#include <QSize>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace vove::handlers::xcf {
namespace {

class NativeSourceDevice final : public QIODevice {
  public:
    explicit NativeSourceDevice(const raster::NativeSource &source) : source_(source) {
        open(QIODevice::ReadOnly);
    }

    [[nodiscard]] bool isSequential() const override {
        return false;
    }

    [[nodiscard]] qint64 size() const override {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
        return source_.size() <= maximum ? static_cast<qint64>(source_.size()) : -1;
    }

    bool seek(const qint64 position) override {
        return position >= 0 && static_cast<std::uint64_t>(position) <= source_.size() &&
               QIODevice::seek(position);
    }

    [[nodiscard]] const raster::NativeSourceError &last_source_error() const noexcept {
        return last_source_error_;
    }

  protected:
    qint64 readData(char *data, const qint64 maximum_size) override {
        if (maximum_size <= 0) {
            return 0;
        }
        const auto position = pos();
        if (position < 0 || static_cast<std::uint64_t>(position) > source_.size()) {
            return -1;
        }
        const auto requested = static_cast<std::size_t>(std::min<quint64>(
            static_cast<quint64>(maximum_size), std::numeric_limits<std::size_t>::max()));
        const auto result = source_.read_at(
            static_cast<std::uint64_t>(position),
            std::span<std::byte>(reinterpret_cast<std::byte *>(data), requested));
        if (!result.ok()) {
            last_source_error_ = result.error;
            return -1;
        }
        return static_cast<qint64>(result.bytes_read);
    }

    qint64 writeData(const char *, qint64) override {
        return -1;
    }

  private:
    const raster::NativeSource &source_;
    raster::NativeSourceError last_source_error_;
};

[[nodiscard]] XcfDecodeResult failure(const XcfDecodeErrorCode code, std::string detail = {}) {
    return {.image = {}, .error = {.code = code, .detail = std::move(detail)}};
}

[[nodiscard]] XcfDecodeErrorCode
source_error_code(const raster::NativeSourceErrorCode code) noexcept {
    return code == raster::NativeSourceErrorCode::disconnected
               ? XcfDecodeErrorCode::source_disconnected
               : XcfDecodeErrorCode::source_io_error;
}

[[nodiscard]] bool valid_limits(const XcfDecodeLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 &&
           limits.maximum_source_bytes <= kMaximumXcfSourceBytes &&
           limits.maximum_source_pixels != 0 &&
           limits.maximum_source_pixels <= kMaximumXcfSourcePixels &&
           limits.maximum_output_edge != 0 && limits.maximum_output_edge <= kMaximumXcfOutputEdge &&
           limits.maximum_output_bytes != 0 &&
           limits.maximum_output_bytes <= kMaximumXcfOutputBytes;
}

struct XcfHeader {
    std::uint32_t version{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct XcfHeaderReadResult {
    std::optional<XcfHeader> header;
    raster::NativeSourceError source_error;
};

[[nodiscard]] XcfHeaderReadResult read_header(const raster::NativeSource &source) {
    std::array<std::byte, 22> bytes{};
    const auto result = source.read_at(0, bytes);
    if (!result.ok()) {
        return {.header = std::nullopt, .source_error = result.error};
    }
    if (result.bytes_read != bytes.size() || std::memcmp(bytes.data(), "gimp xcf ", 9) != 0 ||
        bytes[13] != std::byte{}) {
        return {};
    }
    std::uint32_t version{};
    const auto *tag = reinterpret_cast<const char *>(bytes.data() + 9);
    if (std::memcmp(tag, "file", 4) != 0) {
        if (tag[0] != 'v' || tag[1] < '0' || tag[1] > '9' || tag[2] < '0' || tag[2] > '9' ||
            tag[3] < '0' || tag[3] > '9') {
            return {};
        }
        version = static_cast<std::uint32_t>((tag[1] - '0') * 100 + (tag[2] - '0') * 10 +
                                             (tag[3] - '0'));
    }
    std::uint32_t width{};
    std::uint32_t height{};
    std::memcpy(&width, bytes.data() + 14, sizeof(width));
    std::memcpy(&height, bytes.data() + 18, sizeof(height));
    return {.header = XcfHeader{.version = version,
                                .width = qFromBigEndian(width),
                                .height = qFromBigEndian(height)}};
}

[[nodiscard]] QSize canonical_size(const QSize &source, const std::uint32_t edge) {
    auto result = source;
    if (result.width() > static_cast<int>(edge) || result.height() > static_cast<int>(edge)) {
        result.scale(static_cast<int>(edge), static_cast<int>(edge), Qt::KeepAspectRatio);
    }
    return result;
}

} // namespace

XcfDecodeResult decode_xcf(const raster::NativeSource &source, const XcfDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(XcfDecodeErrorCode::invalid_limits, "invalid XCF decode limits");
    }
    if (source.size() == 0 || source.size() > limits.maximum_source_bytes) {
        return failure(XcfDecodeErrorCode::source_limit_exceeded, "XCF source exceeds its limit");
    }
    const auto header_result = read_header(source);
    if (header_result.source_error) {
        return failure(source_error_code(header_result.source_error.code),
                       "XCF source became unavailable while reading its header");
    }
    if (!header_result.header) {
        return failure(XcfDecodeErrorCode::malformed_input, "invalid XCF header");
    }
    const auto &header = *header_result.header;
    if (header.version > 19) {
        return failure(XcfDecodeErrorCode::unsupported_version,
                       "XCF versions newer than 19 are not supported");
    }
    if (header.width == 0 || header.height == 0 ||
        header.width > limits.maximum_source_pixels ||
        header.height > limits.maximum_source_pixels ||
        header.width > limits.maximum_source_pixels / header.height) {
        return failure(XcfDecodeErrorCode::dimension_limit_exceeded,
                       "XCF canvas exceeds its pixel limit");
    }

    NativeSourceDevice device(source);
    XCFHandler handler;
    handler.setDevice(&device);
    QImage decoded;
    if (!handler.canRead() || !handler.read(&decoded) || decoded.isNull()) {
        if (device.last_source_error()) {
            return failure(source_error_code(device.last_source_error().code),
                           "XCF source became unavailable while decoding");
        }
        if (handler.resourceLimitExceeded()) {
            return failure(XcfDecodeErrorCode::decoder_resource_limit_exceeded,
                           "XCF layer exceeds the embedded decoder allocation limit");
        }
        if (handler.unsupportedCompression()) {
            return failure(XcfDecodeErrorCode::compression_not_supported,
                           "XCF tile compression is not supported");
        }
        if (handler.malformedTileData()) {
            return failure(XcfDecodeErrorCode::malformed_input,
                           "XCF tile stream is malformed");
        }
        return failure(XcfDecodeErrorCode::decode_failed,
                       "XCF layers could not be composed by the embedded decoder");
    }

    const auto source_space = decoded.colorSpace();
    const auto profile = source_space.isValid() ? source_space.iccProfile() : QByteArray{};
    auto profile_name = source_space.isValid() ? source_space.description().simplified().toUtf8()
                                               : QByteArray{};
    if (source_space.isValid() && source_space != QColorSpace::SRgb) {
        decoded.convertToColorSpace(QColorSpace::SRgb);
    } else if (!source_space.isValid()) {
        decoded.setColorSpace(QColorSpace::SRgb);
    }
    const auto output_size = canonical_size(decoded.size(), limits.maximum_output_edge);
    if (decoded.size() != output_size) {
        decoded = decoded.scaled(output_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
    const auto output_bytes = static_cast<std::uint64_t>(decoded.sizeInBytes());
    if (decoded.isNull() || output_bytes == 0 || output_bytes > limits.maximum_output_bytes) {
        return failure(XcfDecodeErrorCode::output_limit_exceeded,
                       "XCF preview exceeds its output limit");
    }

    XcfDecodeResult result;
    result.image.width = static_cast<std::uint32_t>(decoded.width());
    result.image.height = static_cast<std::uint32_t>(decoded.height());
    result.image.rgba8.resize(static_cast<std::size_t>(output_bytes));
    std::memcpy(result.image.rgba8.data(), decoded.constBits(), result.image.rgba8.size());
    result.image.source_color_profile = profile_name.toStdString();
    result.image.source_profile_fingerprint = raster::profile_sha256(
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(profile.constData()),
                                   static_cast<std::size_t>(profile.size())));
    return result;
}

} // namespace vove::handlers::xcf
