#include "vove/handlers/raster/qt_raster_decoder.hpp"

#include "vove/color/color_transform.h"
#include "vove/handlers/raster/profile_fingerprint.hpp"

#include <QByteArray>
#include <QBuffer>
#include <QIODevice>
#include <QImageIOHandler>
#include <QImageReader>
#include <QList>
#include <QSize>
#include <QtTypes>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace vove::handlers::raster {
namespace {

[[nodiscard]] QtRasterDecodeErrorCode source_error_code(const NativeSourceError &error) noexcept {
    return error.code == NativeSourceErrorCode::disconnected
               ? QtRasterDecodeErrorCode::source_disconnected
               : QtRasterDecodeErrorCode::source_io_error;
}

constexpr qsizetype kMaximumErrorDetailCharacters = 256;

[[nodiscard]] QtRasterDecodeResult failure(const QtRasterDecodeErrorCode code,
                                           QString detail = {}) {
    QtRasterDecodeResult result;
    result.error = {.code = code, .detail = std::move(detail).left(kMaximumErrorDetailCharacters)};
    return result;
}

class NativeSourceDevice final : public QIODevice {
  public:
    explicit NativeSourceDevice(const NativeSource &source) : source_(source) {
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
        if (position < 0 || static_cast<std::uint64_t>(position) > source_.size()) {
            return false;
        }
        return QIODevice::seek(position);
    }

    [[nodiscard]] const NativeSourceError &last_source_error() const noexcept {
        return last_source_error_;
    }

  protected:
    qint64 readData(char *data, const qint64 maximum_size) override {
        if (maximum_size <= 0) {
            return 0;
        }
        const auto current_position = pos();
        if (current_position < 0 || static_cast<std::uint64_t>(current_position) > source_.size()) {
            return -1;
        }

        const auto bounded_size = static_cast<std::size_t>(
            std::min<quint64>(static_cast<quint64>(maximum_size),
                              static_cast<quint64>(std::numeric_limits<std::size_t>::max())));
        const auto read_result = source_.read_at(
            static_cast<std::uint64_t>(current_position),
            std::span<std::byte>(reinterpret_cast<std::byte *>(data), bounded_size));
        if (!read_result.ok()) {
            last_source_error_ = read_result.error;
            return -1;
        }
        return static_cast<qint64>(read_result.bytes_read);
    }

    qint64 writeData(const char *, qint64) override {
        return -1;
    }

  private:
    const NativeSource &source_;
    NativeSourceError last_source_error_;
};

[[nodiscard]] bool valid_limits(const QtRasterDecodeLimits &limits) noexcept {
    if (limits.maximum_source_bytes == 0 || limits.maximum_source_pixels == 0 ||
        limits.maximum_source_bytes > kMaximumQtRasterSourceBytes ||
        limits.maximum_source_pixels > kMaximumQtRasterSourcePixels ||
        limits.maximum_output_edge == 0 ||
        limits.maximum_output_edge > kMaximumCanonicalRasterEdge ||
        limits.maximum_output_bytes == 0) {
        return false;
    }
    return limits.maximum_source_bytes <=
           static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
}

[[nodiscard]] QByteArray reader_format(const RasterFormat format) {
    switch (format) {
    case RasterFormat::Jpeg:
        return QByteArrayLiteral("jpeg");
    case RasterFormat::Png:
        return QByteArrayLiteral("png");
    case RasterFormat::Bmp:
        return QByteArrayLiteral("bmp");
    case RasterFormat::Gif:
        return QByteArrayLiteral("gif");
    case RasterFormat::Ico:
        return QByteArrayLiteral("ico");
    case RasterFormat::Tiff:
        return QByteArrayLiteral("tiff");
    case RasterFormat::Webp:
        return QByteArrayLiteral("webp");
    case RasterFormat::Unknown:
    case RasterFormat::Heif:
    case RasterFormat::Avif:
    case RasterFormat::Psd:
    case RasterFormat::Jpegxl:
        return {};
    }
    return {};
}

[[nodiscard]] bool format_available(const RasterFormat format) {
    const auto expected = reader_format(format);
    if (expected.isEmpty()) {
        return false;
    }
    const auto formats = QImageReader::supportedImageFormats();
    return std::any_of(formats.cbegin(), formats.cend(), [&expected](const QByteArray &candidate) {
        if (candidate.compare(expected, Qt::CaseInsensitive) == 0) {
            return true;
        }
        return expected == QByteArrayLiteral("jpeg") &&
               candidate.compare(QByteArrayLiteral("jpg"), Qt::CaseInsensitive) == 0;
    });
}

[[nodiscard]] bool detected_format_matches(const RasterFormat expected,
                                           const QByteArray &detected) {
    const auto canonical = reader_format(expected);
    if (canonical.isEmpty()) {
        return false;
    }
    if (detected.compare(canonical, Qt::CaseInsensitive) == 0) {
        return true;
    }
    return canonical == QByteArrayLiteral("jpeg") &&
           detected.compare(QByteArrayLiteral("jpg"), Qt::CaseInsensitive) == 0;
}

[[nodiscard]] QString color_model_name(const QColorSpace::ColorModel model) {
    switch (model) {
    case QColorSpace::ColorModel::Rgb:
        return QStringLiteral("RGB");
    case QColorSpace::ColorModel::Gray:
        return QStringLiteral("GRAY");
    case QColorSpace::ColorModel::Cmyk:
        return QStringLiteral("CMYK");
    case QColorSpace::ColorModel::Undefined:
        return QStringLiteral("COLOR");
    }
    return QStringLiteral("COLOR");
}

[[nodiscard]] QString transfer_name(const QColorSpace::TransferFunction transfer) {
    switch (transfer) {
    case QColorSpace::TransferFunction::Linear:
        return QStringLiteral("linear");
    case QColorSpace::TransferFunction::Gamma:
        return QStringLiteral("gamma");
    case QColorSpace::TransferFunction::SRgb:
        return QStringLiteral("sRGB");
    case QColorSpace::TransferFunction::ProPhotoRgb:
        return QStringLiteral("ProPhoto RGB");
    case QColorSpace::TransferFunction::Bt2020:
        return QStringLiteral("BT.2020");
    case QColorSpace::TransferFunction::St2084:
        return QStringLiteral("PQ/ST 2084");
    case QColorSpace::TransferFunction::Hlg:
        return QStringLiteral("HLG");
    case QColorSpace::TransferFunction::Custom:
        return QStringLiteral("ICC");
    }
    return QStringLiteral("ICC");
}

[[nodiscard]] QString color_summary(const QColorSpace &space) {
    auto description = space.description().trimmed();
    if (description.isEmpty()) {
        description = transfer_name(space.transferFunction());
    }
    description = description.simplified();
    for (auto &character : description) {
        if (!character.isPrint()) {
            character = QLatin1Char('?');
        }
    }
    return QStringLiteral("%1: %2").arg(color_model_name(space.colorModel()), description.left(96));
}

[[nodiscard]] bool is_unsupported_hdr(const QColorSpace &space) noexcept {
    return space.transferFunction() == QColorSpace::TransferFunction::St2084 ||
           space.transferFunction() == QColorSpace::TransferFunction::Hlg;
}

[[nodiscard]] QSize canonical_size(const QSize &source_size, const std::uint32_t edge) {
    QSize result = source_size;
    const auto maximum_edge = static_cast<int>(edge);
    if (result.width() > maximum_edge || result.height() > maximum_edge) {
        result.scale(maximum_edge, maximum_edge, Qt::KeepAspectRatio);
    }
    return result;
}

[[nodiscard]] std::optional<std::uint32_t> known_page_count(QImageReader &reader) {
    const auto count = reader.imageCount();
    if (count <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(count);
}

// Qt exposes an invalid color space both for absent and for rejected ICC data.
// Only a confirmed absence may use the approximate JPEG path.
[[nodiscard]] bool jpeg_allows_unprofiled_approximation(QIODevice &device) {
    if (!device.seek(0) || device.read(2) != QByteArray::fromHex("ffd8")) {
        return false;
    }
    bool adobe_polarity = false;
    for (unsigned segments = 0; segments < 4096 && device.pos() < 16 * 1024 * 1024; ++segments) {
        char prefix{}, marker{};
        if (!device.getChar(&prefix) || static_cast<unsigned char>(prefix) != 0xffU ||
            !device.getChar(&marker)) {
            return false;
        }
        while (static_cast<unsigned char>(marker) == 0xffU) {
            if (device.pos() >= 16 * 1024 * 1024 || !device.getChar(&marker)) {
                return false;
            }
        }
        const auto type = static_cast<unsigned char>(marker);
        if (type == 0xdaU) {
            return adobe_polarity;
        }
        if (type == 0 || type == 0xd8U || type == 0xd9U) {
            return false;
        }
        if (type == 1 || (type >= 0xd0U && type <= 0xd7U)) {
            continue;
        }
        const auto length_bytes = device.read(2);
        if (length_bytes.size() != 2) {
            return false;
        }
        const auto length = static_cast<unsigned char>(length_bytes[0]) * 256 +
                            static_cast<unsigned char>(length_bytes[1]);
        const auto end = device.pos() + length - 2;
        if (length < 2 || end > device.size() || end > 16 * 1024 * 1024) {
            return false;
        }
        if (type == 0xe2U && length >= 14) {
            const auto signature = device.read(12);
            if (signature.size() != 12 || signature == QByteArray("ICC_PROFILE\0", 12)) {
                return false;
            }
        }
        if (type == 0xeeU && length >= 14) {
            const auto adobe = device.read(12);
            if (adobe.size() != 12)
                return false;
            if (adobe.startsWith("Adobe")) {
                const auto transform = static_cast<unsigned char>(adobe[11]);
                if (adobe_polarity || (transform != 0 && transform != 2))
                    return false;
                adobe_polarity = true;
            }
        }
        if (!device.seek(end)) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] QtRasterDecodeResult
decode_qt_raster_device(QIODevice &device, const RasterFormat format,
                        const QtRasterDecodeLimits &limits,
                        const NativeSourceDevice *const native_device) {
    if (!format_available(format)) {
        return failure(QtRasterDecodeErrorCode::decoder_unavailable,
                       QString::fromLatin1(reader_format(format)));
    }

    QImageReader reader(&device);
    reader.setFormat(reader_format(format));
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(limits.apply_orientation);
    if (!reader.canRead()) {
        if (native_device != nullptr && native_device->last_source_error()) {
            return failure(source_error_code(native_device->last_source_error()));
        }
        return failure(QtRasterDecodeErrorCode::malformed_input, reader.errorString());
    }
    if (!detected_format_matches(format, reader.format())) {
        return failure(QtRasterDecodeErrorCode::malformed_input,
                       QStringLiteral("signature/decoder format mismatch"));
    }

    const auto source_size = reader.size();
    if (!source_size.isValid() || source_size.isEmpty()) {
        return failure(QtRasterDecodeErrorCode::malformed_input, reader.errorString());
    }
    const auto source_width = static_cast<std::uint64_t>(source_size.width());
    const auto source_height = static_cast<std::uint64_t>(source_size.height());
    if (source_width > limits.maximum_source_pixels ||
        source_height > limits.maximum_source_pixels ||
        source_width > limits.maximum_source_pixels / source_height) {
        return failure(QtRasterDecodeErrorCode::dimension_limit_exceeded);
    }

    const auto page_count = known_page_count(reader);
    const auto requested_size = canonical_size(source_size, limits.maximum_output_edge);
    if (requested_size != source_size) {
        reader.setScaledSize(requested_size);
    }

    auto decoded = reader.read();
    if (decoded.isNull()) {
        if (native_device != nullptr && native_device->last_source_error()) {
            return failure(source_error_code(native_device->last_source_error()));
        }
        return failure(QtRasterDecodeErrorCode::decode_failed, reader.errorString());
    }

    const auto source_color_space = decoded.colorSpace();
    const auto unprofiled_cmyk =
        !source_color_space.isValid() && decoded.format() == QImage::Format_CMYK8888;
    if (source_color_space.isValid() && is_unsupported_hdr(source_color_space)) {
        return failure(QtRasterDecodeErrorCode::unsupported_color,
                       color_summary(source_color_space));
    }

    const auto output_size = canonical_size(decoded.size(), limits.maximum_output_edge);
    if (decoded.size() != output_size) {
        decoded = decoded.scaled(output_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    const QColorSpace output_color_space(QColorSpace::SRgb);
    const auto approximate_cmyk =
        unprofiled_cmyk && limits.fallback_cmyk_icc.empty() && format == RasterFormat::Jpeg;
    if (approximate_cmyk && !jpeg_allows_unprofiled_approximation(device)) {
        if (native_device != nullptr && native_device->last_source_error()) {
            return failure(source_error_code(native_device->last_source_error()));
        }
        return failure(QtRasterDecodeErrorCode::unsupported_color,
                       QStringLiteral("CMYK JPEG profile or channel polarity is unsupported"));
    }
    if (unprofiled_cmyk && limits.fallback_cmyk_icc.empty() && !approximate_cmyk) {
        return failure(QtRasterDecodeErrorCode::color_profile_required,
                       QStringLiteral("CMYK: profile not specified"));
    }
    const auto assigned_default_cmyk = unprofiled_cmyk && !approximate_cmyk;
    const auto assumed_srgb = !source_color_space.isValid() && !unprofiled_cmyk;
    const auto source_profile_bytes =
        assigned_default_cmyk ? QByteArray::fromRawData(
                                    reinterpret_cast<const char *>(limits.fallback_cmyk_icc.data()),
                                    static_cast<qsizetype>(limits.fallback_cmyk_icc.size()))
                              : source_color_space.iccProfile();
    QString decoded_color_summary;
    if (approximate_cmyk) {
        // Qt converts normalized CMYK channels, including JPEG's Adobe/YCCK polarity.
        // No printing profile is invented; only the approximate output is tagged sRGB.
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
        decoded.setColorSpace(output_color_space);
        decoded_color_summary =
            QStringLiteral("CMYK: ") + QString::fromLatin1(color::kUnprofiledCmykApproximation);
    } else if (assigned_default_cmyk) {
        const auto pixels =
            std::span<const std::byte>(reinterpret_cast<const std::byte *>(decoded.constBits()),
                                       static_cast<std::size_t>(decoded.sizeInBytes()));
        auto transformed = color::to_srgb_rgba8({
            .width = static_cast<std::uint32_t>(decoded.width()),
            .height = static_cast<std::uint32_t>(decoded.height()),
            .source_stride = static_cast<std::size_t>(decoded.bytesPerLine()),
            .source_format = color::PixelFormat::cmyk8,
            .source_pixels = pixels,
            .embedded_icc = {},
            .fallback_cmyk_icc = limits.fallback_cmyk_icc,
        });
        if (!transformed.ok()) {
            return failure(QtRasterDecodeErrorCode::unsupported_color,
                           QString::fromStdString(transformed.detail));
        }
        const QImage borrowed(
            reinterpret_cast<const uchar *>(transformed.rgba_pixels.data()),
            static_cast<int>(transformed.width), static_cast<int>(transformed.height),
            static_cast<qsizetype>(transformed.rgba_stride), QImage::Format_RGBA8888);
        decoded = borrowed.copy();
        decoded.setColorSpace(output_color_space);
        decoded_color_summary =
            QStringLiteral("CMYK: %1 (default)")
                .arg(QString::fromStdString(transformed.metadata.source_profile));
    } else if (assumed_srgb) {
        decoded.setColorSpace(output_color_space);
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
    } else {
        decoded = decoded.convertedToColorSpace(output_color_space, QImage::Format_RGBA8888);
    }
    if (decoded.isNull() || decoded.format() != QImage::Format_RGBA8888) {
        return failure(QtRasterDecodeErrorCode::unsupported_color,
                       QStringLiteral("color conversion to sRGB failed"));
    }

    const auto output_bytes = static_cast<std::uint64_t>(decoded.sizeInBytes());
    if (decoded.width() <= 0 || decoded.height() <= 0 ||
        decoded.width() > static_cast<int>(limits.maximum_output_edge) ||
        decoded.height() > static_cast<int>(limits.maximum_output_edge) ||
        output_bytes > limits.maximum_output_bytes) {
        return failure(QtRasterDecodeErrorCode::output_limit_exceeded);
    }

    QtRasterDecodeResult result;
    result.image.rgba8 = std::move(decoded);
    result.image.metadata = {
        .format = format,
        .source_width = static_cast<std::uint32_t>(source_width),
        .source_height = static_cast<std::uint32_t>(source_height),
        .page_count = page_count,
        .source_color_space = source_color_space,
        .output_color_space = output_color_space,
        .color_summary = (assigned_default_cmyk || approximate_cmyk)
                             ? decoded_color_summary
                             : (assumed_srgb ? QStringLiteral("RGB: sRGB (assumed)")
                                             : color_summary(source_color_space)),
        .source_profile_fingerprint =
            profile_sha256(std::as_bytes(std::span(source_profile_bytes))),
        .assumed_srgb = assumed_srgb,
    };
    return result;
}

} // namespace

QtRasterDecodeResult decode_qt_raster(const NativeSource &source,
                                      const QtRasterDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(QtRasterDecodeErrorCode::invalid_limits);
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(QtRasterDecodeErrorCode::source_limit_exceeded);
    }

    const auto prefix_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), kMaxSignatureProbeBytes));
    std::vector<std::byte> prefix(prefix_size);
    const auto prefix_read = source.read_at(0, prefix);
    if (!prefix_read.ok() || prefix_read.bytes_read != prefix.size()) {
        return failure(prefix_read.error ? source_error_code(prefix_read.error)
                                         : QtRasterDecodeErrorCode::source_io_error);
    }

    const auto probe = probe_raster_signature(prefix);
    if (probe.format == RasterFormat::Heif) {
        return failure(QtRasterDecodeErrorCode::unsupported_heif,
                       QStringLiteral("HEIF requires the isolated HEIF decoder"));
    }
    if (probe.format == RasterFormat::Avif) {
        return failure(QtRasterDecodeErrorCode::unsupported_format,
                       QStringLiteral("AVIF is not enabled"));
    }
    if (probe.status == ProbeStatus::Truncated) {
        return failure(QtRasterDecodeErrorCode::truncated_input);
    }
    if (probe.status == ProbeStatus::Malformed) {
        return failure(QtRasterDecodeErrorCode::malformed_input);
    }
    if (!probe.matched() || reader_format(probe.format).isEmpty()) {
        return failure(QtRasterDecodeErrorCode::unsupported_format);
    }

    NativeSourceDevice device(source);
    return decode_qt_raster_device(device, probe.format, limits, &device);
}

QtRasterDecodeResult decode_qt_raster_bytes(const std::span<const std::byte> encoded,
                                            const QtRasterDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(QtRasterDecodeErrorCode::invalid_limits);
    }
    if (encoded.empty() || encoded.size() > limits.maximum_source_bytes ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        return failure(QtRasterDecodeErrorCode::source_limit_exceeded);
    }
    const auto prefix = encoded.first(std::min(encoded.size(), kMaxSignatureProbeBytes));
    const auto probe = probe_raster_signature(prefix);
    if (probe.status == ProbeStatus::Truncated) {
        return failure(QtRasterDecodeErrorCode::truncated_input);
    }
    if (probe.status == ProbeStatus::Malformed) {
        return failure(QtRasterDecodeErrorCode::malformed_input);
    }
    if (!probe.matched() ||
        (probe.format != RasterFormat::Jpeg && probe.format != RasterFormat::Png &&
         probe.format != RasterFormat::Bmp)) {
        return failure(QtRasterDecodeErrorCode::unsupported_format);
    }
    auto bytes = QByteArray::fromRawData(reinterpret_cast<const char *>(encoded.data()),
                                         static_cast<qsizetype>(encoded.size()));
    QBuffer device(&bytes);
    if (!device.open(QIODevice::ReadOnly)) {
        return failure(QtRasterDecodeErrorCode::source_io_error);
    }
    return decode_qt_raster_device(device, probe.format, limits, nullptr);
}

} // namespace vove::handlers::raster
