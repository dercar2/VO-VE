#include "vove/handlers/raster/gif_animation_decoder.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QImageReader>

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace vove::handlers::raster {
namespace {

constexpr std::uint64_t kMaximumInputBytes = 64ULL * 1024 * 1024;
constexpr std::uint64_t kMaximumPixels = 16ULL * 1024 * 1024;
constexpr std::uint32_t kMaximumFrames = 100'000;
constexpr std::uint32_t kMaximumOperations = 1'000'000;

class InputBuffer final : public QBuffer {
  public:
    explicit InputBuffer(QByteArray *bytes) : QBuffer(bytes) {}
    bool exhausted{};

  protected:
    qint64 readData(char *data, qint64 count) override {
        if (++read_calls_ > kMaximumOperations) {
            exhausted = true;
            return -1;
        }
        return QBuffer::readData(data, count);
    }

  private:
    std::uint32_t read_calls_{};
};

QtRasterDecodeErrorCode source_error(const NativeSourceError &error) {
    return error.code == NativeSourceErrorCode::disconnected
               ? QtRasterDecodeErrorCode::source_disconnected
               : QtRasterDecodeErrorCode::source_io_error;
}

} // namespace

struct GifAnimationDecoder::Impl {
    QByteArray encoded;
    std::unique_ptr<InputBuffer> buffer;
    std::unique_ptr<QImageReader> reader;
    qsizetype cursor{};
    qsizetype first_block{};
    std::uint32_t operations{};
    std::uint32_t frames{};
    std::uint32_t repeats_done{};
    std::uint32_t delay_ms{100};
    std::uint32_t edge;
    int width{};
    int height{};
    int loop_count{};
    bool global_palette{};
    bool pending_frame{};
    bool trailer{};
    bool restart{};
    bool finished{};
    bool animated{};
    QtRasterDecodeErrorCode error_code{QtRasterDecodeErrorCode::none};
    std::string detail;

    Impl(const NativeSource &source, std::uint32_t maximum_output_edge)
        : edge(maximum_output_edge) {
        if (edge == 0 || edge > 2048) {
            fail(QtRasterDecodeErrorCode::invalid_limits, "GIF output edge must be 1..2048");
            return;
        }
        if (source.size() > kMaximumInputBytes) {
            fail(QtRasterDecodeErrorCode::source_limit_exceeded, "GIF input exceeds 64 MiB");
            return;
        }
        const auto before = source.validate_unchanged();
        if (!before.unchanged) {
            fail(source_error(before.error), "GIF source changed or is unavailable");
            return;
        }
        encoded.resize(static_cast<qsizetype>(source.size()));
        const auto read =
            source.read_at(0, std::span<std::byte>(reinterpret_cast<std::byte *>(encoded.data()),
                                                   static_cast<std::size_t>(encoded.size())));
        if (!read.ok() || read.bytes_read != static_cast<std::size_t>(encoded.size())) {
            fail(read.error ? source_error(read.error) : QtRasterDecodeErrorCode::truncated_input,
                 "Cannot copy complete GIF source");
            return;
        }
        const auto after = source.validate_unchanged();
        if (!after.unchanged) {
            fail(source_error(after.error), "GIF source changed during copy");
            return;
        }
        if (!require(6))
            return;
        if (std::memcmp(encoded.constData(), "GIF87a", 6) != 0 &&
            std::memcmp(encoded.constData(), "GIF89a", 6) != 0) {
            fail(QtRasterDecodeErrorCode::unsupported_format,
                 "Expected GIF87a or GIF89a signature");
            return;
        }
        cursor = 6;
        if (!require(7))
            return;
        width = word(cursor);
        height = word(cursor + 2);
        if (!dimensions(width, height))
            return;
        const auto packed = byte(cursor + 4);
        global_palette = (packed & 0x80) != 0;
        cursor += 7;
        if (global_palette && !skip(3 * (2 << (packed & 7))))
            return;
        first_block = cursor;
    }

    void fail(QtRasterDecodeErrorCode code, const char *message) {
        error_code = code;
        detail = message;
    }

    bool require(qsizetype count) {
        if (count > encoded.size() - cursor) {
            fail(QtRasterDecodeErrorCode::truncated_input,
                 "Incomplete GIF block or missing trailer");
            return false;
        }
        return true;
    }

    bool skip(qsizetype count) {
        if (!require(count))
            return false;
        cursor += count;
        return true;
    }

    unsigned byte(qsizetype at) const {
        return static_cast<unsigned char>(encoded.at(at));
    }

    int word(qsizetype at) const {
        return static_cast<int>(byte(at) | (byte(at + 1) << 8));
    }

    bool operation() {
        if (++operations > kMaximumOperations) {
            fail(QtRasterDecodeErrorCode::source_limit_exceeded,
                 "GIF metadata operation limit exceeded");
            return false;
        }
        return true;
    }

    bool dimensions(int w, int h) {
        if (w <= 0 || h <= 0) {
            fail(QtRasterDecodeErrorCode::malformed_input, "Zero GIF dimensions");
            return false;
        }
        if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaximumPixels) {
            fail(QtRasterDecodeErrorCode::dimension_limit_exceeded, "GIF exceeds 16 Mi pixels");
            return false;
        }
        return true;
    }

    bool subblocks(bool image_data = false) {
        bool has_data = false;
        for (;;) {
            if (!operation() || !require(1))
                return false;
            const auto count = byte(cursor++);
            if (count == 0) {
                if (image_data && !has_data) {
                    fail(QtRasterDecodeErrorCode::malformed_input,
                         "GIF frame has no compressed data");
                    return false;
                }
                return true;
            }
            has_data = true;
            if (!skip(count))
                return false;
        }
    }

    // Only framing/metadata are parsed here; Qt owns LZW decoding and composition.
    // Look ahead to one image header, never through all subsequent frame payloads.
    void prepare_frame() {
        pending_frame = false;
        delay_ms = 100;
        while (error_code == QtRasterDecodeErrorCode::none) {
            if (!operation() || !require(1))
                return;
            const auto marker = byte(cursor++);
            if (marker == 0x3b) {
                trailer = true;
                if (frames == 0)
                    fail(QtRasterDecodeErrorCode::malformed_input, "GIF contains no images");
                return;
            }
            if (marker == 0x2c) {
                if (frames >= kMaximumFrames) {
                    fail(QtRasterDecodeErrorCode::source_limit_exceeded,
                         "GIF exceeds 100000 frames per loop");
                    return;
                }
                if (!require(9))
                    return;
                const auto left = word(cursor);
                const auto top = word(cursor + 2);
                const auto w = word(cursor + 4);
                const auto h = word(cursor + 6);
                const auto packed = byte(cursor + 8);
                if (!dimensions(w, h))
                    return;
                if (left + w > width || top + h > height) {
                    fail(QtRasterDecodeErrorCode::malformed_input,
                         "GIF frame extends outside canvas");
                    return;
                }
                // Qt's GIF pass transitions misplace rows when interlace pass 2
                // is empty but pass 3 is not (image height 3 or 4), with or
                // without transparency. Reject before Qt decodes this frame.
                if ((packed & 0x40) != 0 && (h == 3 || h == 4)) {
                    fail(QtRasterDecodeErrorCode::unsupported_format,
                         "Qt GIF decoder cannot correctly render interlaced frames of height 3 or "
                         "4");
                    return;
                }
                cursor += 9;
                const bool local_palette = (packed & 0x80) != 0;
                if (!local_palette && !global_palette) {
                    fail(QtRasterDecodeErrorCode::malformed_input, "GIF frame has no color table");
                    return;
                }
                if (local_palette && !skip(3 * (2 << (packed & 7))))
                    return;
                if (!require(1))
                    return;
                const auto code_size = byte(cursor++);
                if (code_size < 2 || code_size > 8) {
                    fail(QtRasterDecodeErrorCode::malformed_input,
                         "Invalid GIF LZW minimum code size");
                    return;
                }
                pending_frame = true;
                animated = animated || frames != 0;
                return;
            }
            if (marker != 0x21) {
                fail(QtRasterDecodeErrorCode::malformed_input, "Invalid GIF block introducer");
                return;
            }
            if (!require(1))
                return;
            const auto label = byte(cursor++);
            if (label == 0xf9) {
                if (!require(6))
                    return;
                if (byte(cursor) != 4 || byte(cursor + 5) != 0) {
                    fail(QtRasterDecodeErrorCode::malformed_input,
                         "Invalid GIF graphic control extension");
                    return;
                }
                const auto delay = static_cast<std::uint32_t>(word(cursor + 2)) * 10;
                delay_ms = delay <= 10 ? 100 : delay;
                cursor += 6;
            } else if (label == 0xff) {
                if (!require(12))
                    return;
                if (byte(cursor) != 11) {
                    fail(QtRasterDecodeErrorCode::malformed_input,
                         "Invalid GIF application extension");
                    return;
                }
                const auto *application = encoded.constData() + cursor + 1;
                const bool looping = std::memcmp(application, "NETSCAPE2.0", 11) == 0 ||
                                     std::memcmp(application, "ANIMEXTS1.0", 11) == 0;
                cursor += 12;
                if (looping) {
                    if (!require(5))
                        return;
                    if (byte(cursor) != 3 || byte(cursor + 1) != 1 || byte(cursor + 4) != 0) {
                        fail(QtRasterDecodeErrorCode::malformed_input,
                             "Invalid GIF loop extension");
                        return;
                    }
                    const auto repeats = word(cursor + 2);
                    loop_count = repeats == 0 ? -1 : repeats;
                }
                if (!subblocks())
                    return;
            } else {
                if (label == 0x01) {
                    fail(QtRasterDecodeErrorCode::unsupported_format,
                         "GIF plain-text rendering is unsupported");
                    return;
                }
                if (!subblocks())
                    return;
            }
        }
    }

    bool start_reader() {
        reader.reset();
        buffer = std::make_unique<InputBuffer>(&encoded);
        if (!buffer->open(QIODevice::ReadOnly)) {
            fail(QtRasterDecodeErrorCode::decode_failed, "Cannot open GIF memory buffer");
            return false;
        }
        reader = std::make_unique<QImageReader>(buffer.get(), QByteArrayLiteral("gif"));
        reader->setAutoDetectImageFormat(false);
        reader->setAutoTransform(false);
        // size(), imageCount() and loopCount() each scan/cache every frame in Qt GIF.
        if (!reader->canRead()) {
            fail(reader->error() == QImageReader::UnsupportedFormatError
                     ? QtRasterDecodeErrorCode::decoder_unavailable
                     : QtRasterDecodeErrorCode::malformed_input,
                 "Qt GIF reader is unavailable or rejected the header");
            return false;
        }
        return true;
    }

    GifAnimationFrame terminal() {
        finished = true;
        reader.reset();
        buffer.reset();
        return {.finished = true, .animated = animated, .error_code = error_code, .detail = detail};
    }

    GifAnimationFrame next() {
        if (finished || error_code != QtRasterDecodeErrorCode::none)
            return terminal();
        if (restart) {
            cursor = first_block;
            operations = 0;
            frames = 0;
            loop_count = 0;
            trailer = false;
            restart = false;
            if (!start_reader())
                return terminal();
        }
        if (!pending_frame)
            prepare_frame();
        if (error_code != QtRasterDecodeErrorCode::none || !pending_frame)
            return terminal();
        const auto frame_delay = delay_ms;
        if (!subblocks(true))
            return terminal();
        if (!reader && !start_reader())
            return terminal();
        auto image = reader->read();
        if (buffer->exhausted || image.isNull()) {
            fail(buffer->exhausted ? QtRasterDecodeErrorCode::source_limit_exceeded
                                   : QtRasterDecodeErrorCode::malformed_input,
                 buffer->exhausted ? "GIF reader call limit exceeded"
                                   : "Qt could not decode GIF frame");
            return terminal();
        }
        if (image.width() != width || image.height() != height) {
            fail(QtRasterDecodeErrorCode::dimension_limit_exceeded,
                 "Qt GIF canvas differs from validated dimensions");
            return terminal();
        }
        if (image.width() > static_cast<int>(edge) || image.height() > static_cast<int>(edge)) {
            const auto longest = static_cast<std::uint64_t>(std::max(width, height));
            const QSize output(
                std::max(1, static_cast<int>(static_cast<std::uint64_t>(width) * edge / longest)),
                std::max(1, static_cast<int>(static_cast<std::uint64_t>(height) * edge / longest)));
            image = image.scaled(output, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        image = image.convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            fail(QtRasterDecodeErrorCode::decode_failed, "Cannot allocate canonical GIF frame");
            return terminal();
        }
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        ++frames;
        prepare_frame();
        if (trailer && error_code == QtRasterDecodeErrorCode::none) {
            if (frames == 1 ||
                (loop_count >= 0 && repeats_done >= static_cast<std::uint32_t>(loop_count))) {
                finished = true;
                reader.reset();
                buffer.reset();
            } else {
                restart = true;
                if (loop_count > 0)
                    ++repeats_done;
            }
        }
        return {.image = std::move(image),
                .delay_ms = frame_delay,
                .finished = finished,
                .animated = animated};
    }
};

GifAnimationDecoder::GifAnimationDecoder(const NativeSource &source,
                                         std::uint32_t maximum_output_edge)
    : impl_(std::make_unique<Impl>(source, maximum_output_edge)) {}

GifAnimationDecoder::~GifAnimationDecoder() = default;
GifAnimationDecoder::GifAnimationDecoder(GifAnimationDecoder &&) noexcept = default;
GifAnimationDecoder &GifAnimationDecoder::operator=(GifAnimationDecoder &&) noexcept = default;

GifAnimationFrame GifAnimationDecoder::next() {
    if (!impl_)
        return {.finished = true,
                .error_code = QtRasterDecodeErrorCode::invalid_limits,
                .detail = "Moved-from GIF decoder"};
    return impl_->next();
}

} // namespace vove::handlers::raster
