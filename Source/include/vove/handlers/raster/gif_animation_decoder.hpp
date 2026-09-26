#pragma once

#include "vove/handlers/raster/qt_raster_decoder.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace vove::handlers::raster {

struct GifAnimationFrame {
    QImage image{};
    std::uint32_t delay_ms{};
    bool finished{};
    bool animated{};
    QtRasterDecodeErrorCode error_code{QtRasterDecodeErrorCode::none};
    std::string detail{};
};

// Worker-only, sequential decoder. Construction snapshots at most 64 MiB from source;
// neither source nor its handle is needed afterward. No pathname is passed to Qt.
// Limits: 16*1024*1024 canvas/frame pixels, 100000 frames and 1000000 metadata
// steps/device reads per pass. The caller must still enforce a worker time limit.
class GifAnimationDecoder final {
  public:
    explicit GifAnimationDecoder(const NativeSource &source,
                                 std::uint32_t maximum_output_edge = 2048);
    ~GifAnimationDecoder();
    GifAnimationDecoder(GifAnimationDecoder &&) noexcept;
    GifAnimationDecoder &operator=(GifAnimationDecoder &&) noexcept;
    GifAnimationDecoder(const GifAnimationDecoder &) = delete;
    GifAnimationDecoder &operator=(const GifAnimationDecoder &) = delete;

    // A successful last frame has an image and finished=true; subsequent calls are
    // empty, finished results. Errors are empty, finished and sticky. A discovered
    // error after a good frame is reported on the following call. animated becomes
    // true once a second image header is found and remains true, including at EOF.
    // Single-frame GIFs finish immediately even if they contain a looping extension.
    // Images are composed sRGB RGBA8888; delays <=10 ms become 100 ms.
    // Loop metadata follows Qt semantics: absent=0, positive=additional repeats,
    // Netscape zero=-1 (indefinite). Plain-text rendering and interlaced frames
    // of height 3 or 4 are unsupported (Qt misplaces rows, even with transparency).
    // Framing is validated, not the full LZW code stream: Qt can recover some
    // damaged payloads inside complete blocks without reporting an error.
    [[nodiscard]] GifAnimationFrame next();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vove::handlers::raster
