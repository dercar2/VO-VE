#include "vove/handlers/raster/wic_raster_decoder.hpp"
#include "vove/color/color_transform.h"
#include "vove/handlers/raster/profile_fingerprint.hpp"

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

extern "C" HRESULT WINAPI WICCreateImagingFactory_Proxy(UINT sdk_version,
                                                        IWICImagingFactory **factory);

namespace vove::handlers::raster {
namespace {

constexpr HRESULT kSourceReadFailure = static_cast<HRESULT>(0x80040201L);
constexpr std::size_t kMaximumDetailBytes = 192;

template <typename Interface> class ComPtr final {
  public:
    ComPtr() = default;
    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    ComPtr(ComPtr &&other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ComPtr &operator=(ComPtr &&other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] Interface *get() const noexcept {
        return pointer_;
    }

    [[nodiscard]] Interface **put() noexcept {
        reset();
        return &pointer_;
    }

    [[nodiscard]] Interface *operator->() const noexcept {
        return pointer_;
    }

    void reset(Interface *pointer = nullptr) noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
        pointer_ = pointer;
    }

  private:
    Interface *pointer_{};
};

struct SourceStreamState final {
    explicit SourceStreamState(const NativeSource &native_source) : source(native_source) {}

    void record_error(const NativeSourceError value) noexcept {
        const std::scoped_lock lock(error_mutex);
        if (!last_error) {
            last_error = value;
        }
    }

    [[nodiscard]] NativeSourceError error() const noexcept {
        const std::scoped_lock lock(error_mutex);
        return last_error;
    }

    const NativeSource &source;
    mutable std::mutex error_mutex;
    NativeSourceError last_error;
};

class NativeSourceStream final : public IStream {
  public:
    explicit NativeSourceStream(const NativeSource &source)
        : state_(std::make_shared<SourceStreamState>(source)) {}

    NativeSourceStream(std::shared_ptr<SourceStreamState> state, const std::uint64_t position)
        : state_(std::move(state)), position_(position) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void **object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(interface_id, IID_IUnknown) ||
            IsEqualIID(interface_id, IID_ISequentialStream) ||
            IsEqualIID(interface_id, IID_IStream)) {
            *object = static_cast<IStream *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Read(void *destination, const ULONG requested,
                                   ULONG *bytes_read) override {
        if (bytes_read != nullptr) {
            *bytes_read = 0;
        }
        if (requested != 0 && destination == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        if (requested == 0) {
            return S_OK;
        }

        const std::scoped_lock lock(position_mutex_);
        const auto result = state_->source.read_at(
            position_, std::span<std::byte>(static_cast<std::byte *>(destination), requested));
        if (!result.ok()) {
            state_->record_error(result.error);
            return kSourceReadFailure;
        }
        position_ += static_cast<std::uint64_t>(result.bytes_read);
        if (bytes_read != nullptr) {
            *bytes_read = static_cast<ULONG>(result.bytes_read);
        }
        return result.bytes_read == requested ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *bytes_written) override {
        if (bytes_written != nullptr) {
            *bytes_written = 0;
        }
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(const LARGE_INTEGER move, const DWORD origin,
                                   ULARGE_INTEGER *new_position) override {
        const std::scoped_lock lock(position_mutex_);
        std::int64_t base{};
        switch (origin) {
        case STREAM_SEEK_SET:
            base = 0;
            break;
        case STREAM_SEEK_CUR:
            base = static_cast<std::int64_t>(position_);
            break;
        case STREAM_SEEK_END:
            base = static_cast<std::int64_t>(state_->source.size());
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }

        const auto delta = static_cast<std::int64_t>(move.QuadPart);
        if ((delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta) ||
            (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta)) {
            return STG_E_INVALIDFUNCTION;
        }
        const auto candidate = base + delta;
        if (candidate < 0 || static_cast<std::uint64_t>(candidate) > state_->source.size()) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<std::uint64_t>(candidate);
        if (new_position != nullptr) {
            new_position->QuadPart = position_;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream *destination, const ULARGE_INTEGER count,
                                     ULARGE_INTEGER *bytes_read,
                                     ULARGE_INTEGER *bytes_written) override {
        if (bytes_read != nullptr) {
            bytes_read->QuadPart = 0;
        }
        if (bytes_written != nullptr) {
            bytes_written->QuadPart = 0;
        }
        if (destination == nullptr) {
            return STG_E_INVALIDPOINTER;
        }

        std::array<std::byte, static_cast<std::size_t>(64U) * 1024U> buffer{};
        std::uint64_t total_read{};
        std::uint64_t total_written{};
        while (total_read < count.QuadPart) {
            const auto remaining = count.QuadPart - total_read;
            const auto chunk = static_cast<ULONG>(
                std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
            ULONG current_read{};
            const auto read_result = Read(buffer.data(), chunk, &current_read);
            if (FAILED(read_result)) {
                return read_result;
            }
            if (current_read == 0) {
                break;
            }
            ULONG current_written{};
            const auto write_result =
                destination->Write(buffer.data(), current_read, &current_written);
            total_read += current_read;
            total_written += current_written;
            if (FAILED(write_result)) {
                return write_result;
            }
            if (current_written != current_read) {
                break;
            }
        }
        if (bytes_read != nullptr) {
            bytes_read->QuadPart = total_read;
        }
        if (bytes_written != nullptr) {
            bytes_written->QuadPart = total_written;
        }
        return total_read == count.QuadPart ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Revert() override {
        return STG_E_REVERTED;
    }

    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG *status, DWORD) override {
        if (status == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        *status = {};
        status->type = STGTY_STREAM;
        status->cbSize.QuadPart = state_->source.size();
        status->grfMode = STGM_READ;
        status->clsid = CLSID_NULL;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream **clone) override {
        if (clone == nullptr) {
            return E_POINTER;
        }
        *clone = nullptr;
        const std::scoped_lock lock(position_mutex_);
        auto *const created = new (std::nothrow) NativeSourceStream(state_, position_);
        if (created == nullptr) {
            return E_OUTOFMEMORY;
        }
        *clone = created;
        return S_OK;
    }

    [[nodiscard]] NativeSourceError source_error() const noexcept {
        return state_->error();
    }

  private:
    ~NativeSourceStream() = default;

    std::atomic<ULONG> references_{1};
    std::shared_ptr<SourceStreamState> state_;
    std::mutex position_mutex_;
    std::uint64_t position_{};
};

class ComApartment final {
  public:
    ComApartment() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_initialization_ = result_ == S_OK || result_ == S_FALSE;
    }

    ~ComApartment() {
        if (owns_initialization_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return result_ == RPC_E_CHANGED_MODE ? S_OK : result_;
    }

  private:
    HRESULT result_{E_FAIL};
    bool owns_initialization_{};
};

[[nodiscard]] WicRasterDecodeResult failure(const WicRasterDecodeErrorCode code,
                                            const HRESULT system_code = S_OK,
                                            std::string detail = {}) {
    if (detail.size() > kMaximumDetailBytes) {
        detail.resize(kMaximumDetailBytes);
    }
    WicRasterDecodeResult result;
    result.error = {.code = code,
                    .system_code = static_cast<std::uint32_t>(system_code),
                    .detail = std::move(detail)};
    return result;
}

[[nodiscard]] std::string hresult_detail(const char *operation, const HRESULT result) {
    std::array<char, 48> buffer{};
    static_cast<void>(
        std::snprintf(buffer.data(), buffer.size(), "%s (0x%08lX)", operation,
                      static_cast<unsigned long>(static_cast<std::uint32_t>(result))));
    return buffer.data();
}

[[nodiscard]] bool valid_limits(const WicRasterDecodeLimits &limits) noexcept {
    return limits.maximum_source_bytes != 0 &&
           limits.maximum_source_bytes <= kMaximumWicRasterSourceBytes &&
           limits.maximum_source_pixels != 0 &&
           limits.maximum_source_pixels <= kMaximumWicRasterSourcePixels &&
           limits.maximum_output_edge != 0 && limits.maximum_output_edge <= kMaximumWicRasterEdge &&
           limits.maximum_output_bytes != 0 &&
           limits.maximum_output_bytes <= kMaximumWicRasterOutputBytes;
}

[[nodiscard]] WicRasterDecodeResult validation_failure(const NativeSourceError &error) {
    const auto code = error.code == NativeSourceErrorCode::source_changed
                          ? WicRasterDecodeErrorCode::source_changed
                      : error.code == NativeSourceErrorCode::disconnected
                          ? WicRasterDecodeErrorCode::source_disconnected
                          : WicRasterDecodeErrorCode::source_io_error;
    return failure(code, static_cast<HRESULT>(error.system_code), "source validation failed");
}

[[nodiscard]] WicRasterDecodeResult wic_failure(const HRESULT result, const char *operation,
                                                const NativeSourceError source_error = {}) {
    if (source_error) {
        return failure(source_error.code == NativeSourceErrorCode::disconnected
                           ? WicRasterDecodeErrorCode::source_disconnected
                           : WicRasterDecodeErrorCode::source_io_error,
                       static_cast<HRESULT>(source_error.system_code), "native source read failed");
    }
    WicRasterDecodeErrorCode code = WicRasterDecodeErrorCode::decode_failed;
    if (result == WINCODEC_ERR_STREAMREAD || result == STG_E_READFAULT ||
        result == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)) {
        code = WicRasterDecodeErrorCode::truncated_input;
    } else if (result == WINCODEC_ERR_BADIMAGE || result == WINCODEC_ERR_UNKNOWNIMAGEFORMAT) {
        code = WicRasterDecodeErrorCode::malformed_input;
    } else if (result == WINCODEC_ERR_COMPONENTNOTFOUND || result == REGDB_E_CLASSNOTREG) {
        code = WicRasterDecodeErrorCode::decoder_unavailable;
    } else if (result == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT) {
        code = WicRasterDecodeErrorCode::unsupported_color;
    }
    return failure(code, result, hresult_detail(operation, result));
}

[[nodiscard]] const GUID *container_guid(const RasterFormat format) noexcept {
    switch (format) {
    case RasterFormat::Jpeg:
        return &GUID_ContainerFormatJpeg;
    case RasterFormat::Png:
        return &GUID_ContainerFormatPng;
    case RasterFormat::Bmp:
        return &GUID_ContainerFormatBmp;
    case RasterFormat::Gif:
        return &GUID_ContainerFormatGif;
    case RasterFormat::Ico:
        return &GUID_ContainerFormatIco;
    case RasterFormat::Tiff:
        return &GUID_ContainerFormatTiff;
    case RasterFormat::Unknown:
    case RasterFormat::Webp:
    case RasterFormat::Heif:
    case RasterFormat::Avif:
    case RasterFormat::Psd:
    case RasterFormat::Jpegxl:
        return nullptr;
    }
    return nullptr;
}

[[nodiscard]] bool guid_is(const WICPixelFormatGUID &value,
                           const WICPixelFormatGUID &expected) noexcept {
    return IsEqualGUID(value, expected) != FALSE;
}

[[nodiscard]] bool is_cmyk(const WICPixelFormatGUID &format) noexcept {
    return guid_is(format, GUID_WICPixelFormat32bppCMYK) ||
           guid_is(format, GUID_WICPixelFormat64bppCMYK) ||
           guid_is(format, GUID_WICPixelFormat40bppCMYKAlpha) ||
           guid_is(format, GUID_WICPixelFormat80bppCMYKAlpha);
}

[[nodiscard]] bool is_cmyk_alpha(const WICPixelFormatGUID &format) noexcept {
    return guid_is(format, GUID_WICPixelFormat40bppCMYKAlpha) ||
           guid_is(format, GUID_WICPixelFormat80bppCMYKAlpha);
}

[[nodiscard]] bool is_generic_three_channel(const WICPixelFormatGUID &format) noexcept {
    return guid_is(format, GUID_WICPixelFormat24bpp3Channels) ||
           guid_is(format, GUID_WICPixelFormat48bpp3Channels);
}

[[nodiscard]] bool is_generic_four_channel(const WICPixelFormatGUID &format) noexcept {
    return guid_is(format, GUID_WICPixelFormat32bpp4Channels) ||
           guid_is(format, GUID_WICPixelFormat64bpp4Channels);
}

[[nodiscard]] bool is_gray(const WICPixelFormatGUID &format) noexcept {
    return guid_is(format, GUID_WICPixelFormatBlackWhite) ||
           guid_is(format, GUID_WICPixelFormat2bppGray) ||
           guid_is(format, GUID_WICPixelFormat4bppGray) ||
           guid_is(format, GUID_WICPixelFormat8bppGray) ||
           guid_is(format, GUID_WICPixelFormat16bppGray);
}

[[nodiscard]] bool is_hdr_or_float(const WICPixelFormatGUID &format) noexcept {
    return guid_is(format, GUID_WICPixelFormat16bppGrayFixedPoint) ||
           guid_is(format, GUID_WICPixelFormat32bppGrayFloat) ||
           guid_is(format, GUID_WICPixelFormat48bppRGBFixedPoint) ||
           guid_is(format, GUID_WICPixelFormat64bppRGBAFixedPoint) ||
           guid_is(format, GUID_WICPixelFormat64bppRGBFixedPoint) ||
           guid_is(format, GUID_WICPixelFormat128bppRGBAFloat) ||
           guid_is(format, GUID_WICPixelFormat128bppRGBFloat) ||
           guid_is(format, GUID_WICPixelFormat32bppRGBE);
}

[[nodiscard]] bool is_safe_unprofiled_sdr(const WICPixelFormatGUID &format) noexcept {
    return is_gray(format) || guid_is(format, GUID_WICPixelFormat1bppIndexed) ||
           guid_is(format, GUID_WICPixelFormat2bppIndexed) ||
           guid_is(format, GUID_WICPixelFormat4bppIndexed) ||
           guid_is(format, GUID_WICPixelFormat8bppIndexed) ||
           guid_is(format, GUID_WICPixelFormat16bppBGR555) ||
           guid_is(format, GUID_WICPixelFormat16bppBGR565) ||
           guid_is(format, GUID_WICPixelFormat24bppBGR) ||
           guid_is(format, GUID_WICPixelFormat24bppRGB) ||
           guid_is(format, GUID_WICPixelFormat32bppBGR) ||
           guid_is(format, GUID_WICPixelFormat32bppBGRA) ||
           guid_is(format, GUID_WICPixelFormat32bppPBGRA) ||
           guid_is(format, GUID_WICPixelFormat32bppRGBA) ||
           guid_is(format, GUID_WICPixelFormat32bppPRGBA) ||
           guid_is(format, GUID_WICPixelFormat48bppRGB) ||
           guid_is(format, GUID_WICPixelFormat48bppBGR) ||
           guid_is(format, GUID_WICPixelFormat64bppRGBA) ||
           guid_is(format, GUID_WICPixelFormat64bppPRGBA) ||
           guid_is(format, GUID_WICPixelFormat64bppBGRA) ||
           guid_is(format, GUID_WICPixelFormat64bppPBGRA);
}

[[nodiscard]] std::uint16_t exif_orientation(IWICBitmapFrameDecode *frame) noexcept {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put()))) {
        return 1;
    }
    constexpr const wchar_t *queries[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    for (const auto *query : queries) {
        PROPVARIANT value{};
        PropVariantInit(&value);
        const auto result = reader->GetMetadataByName(query, &value);
        std::uint16_t orientation = 1;
        if (SUCCEEDED(result)) {
            if (value.vt == VT_UI2) {
                orientation = value.uiVal;
            } else if (value.vt == VT_UI4 && value.ulVal <= 8) {
                orientation = static_cast<std::uint16_t>(value.ulVal);
            }
        }
        static_cast<void>(PropVariantClear(&value));
        if (orientation >= 2 && orientation <= 8) {
            return orientation;
        }
    }
    return 1;
}

[[nodiscard]] WICBitmapTransformOptions
orientation_transform(const std::uint16_t orientation) noexcept {
    switch (orientation) {
    case 2:
        return WICBitmapTransformFlipHorizontal;
    case 3:
        return WICBitmapTransformRotate180;
    case 4:
        return WICBitmapTransformFlipVertical;
    case 5:
        return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 |
                                                      WICBitmapTransformFlipHorizontal);
    case 6:
        return WICBitmapTransformRotate90;
    case 7:
        return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 |
                                                      WICBitmapTransformFlipHorizontal);
    case 8:
        return WICBitmapTransformRotate270;
    case 1:
    default:
        return WICBitmapTransformRotate0;
    }
}

struct CanonicalSize {
    UINT width{};
    UINT height{};
};

[[nodiscard]] CanonicalSize canonical_size(const UINT width, const UINT height,
                                           const std::uint32_t edge) noexcept {
    if (width <= edge && height <= edge) {
        return {width, height};
    }
    if (width >= height) {
        const auto scaled =
            std::max<std::uint64_t>(1, static_cast<std::uint64_t>(height) * edge / width);
        return {edge, static_cast<UINT>(scaled)};
    }
    const auto scaled =
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(width) * edge / height);
    return {static_cast<UINT>(scaled), edge};
}

enum class ProfileContextStatus : std::uint8_t {
    none,
    embedded,
    exif_srgb,
    external_or_unknown,
    invalid,
    resource_limit,
};

struct ProfileContext final {
    ProfileContextStatus status{ProfileContextStatus::none};
    HRESULT system_code{S_OK};
    std::vector<std::byte> embedded_icc;
};

enum class ExifColorSpace : std::uint8_t {
    none,
    srgb,
    external_or_unknown,
};

[[nodiscard]] ExifColorSpace classify_exif_color_space(const UINT value) noexcept {
    // Zero/uncalibrated EXIF does not supply a profile. Keep the ordinary untagged-pixel path;
    // embedded ICC still wins, and CMYK/Lab still require their own color interpretation.
    if (value == 0U || value == 0xffffU) {
        return ExifColorSpace::none;
    }
    return value == 1U ? ExifColorSpace::srgb : ExifColorSpace::external_or_unknown;
}

[[nodiscard]] ExifColorSpace exif_color_space(IWICBitmapFrameDecode *frame) noexcept {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put()))) {
        return ExifColorSpace::none;
    }
    constexpr const wchar_t *queries[] = {L"/app1/ifd/exif/{ushort=40961}",
                                          L"/ifd/exif/{ushort=40961}"};
    for (const auto *query : queries) {
        PROPVARIANT value{};
        PropVariantInit(&value);
        const auto result = reader->GetMetadataByName(query, &value);
        std::optional<UINT> color_space;
        if (SUCCEEDED(result)) {
            if (value.vt == VT_UI2) {
                color_space = value.uiVal;
            } else if (value.vt == VT_UI4) {
                color_space = value.ulVal;
            }
        }
        static_cast<void>(PropVariantClear(&value));
        if (color_space.has_value()) {
            return classify_exif_color_space(*color_space);
        }
    }
    return ExifColorSpace::none;
}

[[nodiscard]] ProfileContext embedded_color_profile(IWICBitmapFrameDecode *frame,
                                                    IWICImagingFactory *factory) {
    UINT count{};
    const auto count_result = frame->GetColorContexts(0, nullptr, &count);
    bool exif_srgb{};
    bool external_or_unknown{};
    if (SUCCEEDED(count_result) && count != 0 && count <= 8) {
        std::vector<ComPtr<IWICColorContext>> owned_contexts(count);
        std::vector<IWICColorContext *> raw(count, nullptr);
        for (UINT index = 0; index < count; ++index) {
            if (FAILED(factory->CreateColorContext(owned_contexts[index].put()))) {
                return {.status = ProfileContextStatus::invalid,
                        .system_code = E_OUTOFMEMORY,
                        .embedded_icc = {}};
            }
            raw[index] = owned_contexts[index].get();
        }
        UINT actual{};
        if (FAILED(frame->GetColorContexts(count, raw.data(), &actual))) {
            return {
                .status = ProfileContextStatus::invalid, .system_code = E_FAIL, .embedded_icc = {}};
        }
        for (UINT index = 0; index < actual; ++index) {
            WICColorContextType type{WICColorContextUninitialized};
            const auto type_result = raw[index]->GetType(&type);
            if (FAILED(type_result)) {
                return {.status = ProfileContextStatus::invalid,
                        .system_code = type_result,
                        .embedded_icc = {}};
            }
            if (type == WICColorContextProfile) {
                UINT required{};
                auto profile_result = raw[index]->GetProfileBytes(0, nullptr, &required);
                if (FAILED(profile_result) || required == 0) {
                    return {.status = ProfileContextStatus::invalid,
                            .system_code = profile_result,
                            .embedded_icc = {}};
                }
                if (required > vove::color::kMaximumIccProfileBytes) {
                    return {.status = ProfileContextStatus::resource_limit,
                            .system_code = S_OK,
                            .embedded_icc = {}};
                }
                ProfileContext profile;
                profile.status = ProfileContextStatus::embedded;
                profile.embedded_icc.resize(required);
                UINT actual_bytes{};
                profile_result = raw[index]->GetProfileBytes(
                    required, reinterpret_cast<BYTE *>(profile.embedded_icc.data()), &actual_bytes);
                if (FAILED(profile_result) || actual_bytes != required) {
                    return {.status = ProfileContextStatus::invalid,
                            .system_code = profile_result,
                            .embedded_icc = {}};
                }
                return profile;
            }
            if (type == WICColorContextExifColorSpace) {
                UINT value{};
                const auto exif_result = raw[index]->GetExifColorSpace(&value);
                if (FAILED(exif_result)) {
                    return {.status = ProfileContextStatus::invalid,
                            .system_code = exif_result,
                            .embedded_icc = {}};
                }
                switch (classify_exif_color_space(value)) {
                case ExifColorSpace::srgb:
                    exif_srgb = true;
                    break;
                case ExifColorSpace::external_or_unknown:
                    external_or_unknown = true;
                    break;
                case ExifColorSpace::none:
                    break;
                }
            }
        }
    }
    switch (exif_color_space(frame)) {
    case ExifColorSpace::srgb:
        exif_srgb = true;
        break;
    case ExifColorSpace::external_or_unknown:
        external_or_unknown = true;
        break;
    case ExifColorSpace::none:
        break;
    }
    if (external_or_unknown) {
        return {.status = ProfileContextStatus::external_or_unknown,
                .system_code = S_OK,
                .embedded_icc = {}};
    }
    return {.status = exif_srgb ? ProfileContextStatus::exif_srgb : ProfileContextStatus::none,
            .system_code = S_OK,
            .embedded_icc = {}};
}

struct TaggedPixelPlan final {
    WICPixelFormatGUID wic_format{};
    vove::color::PixelFormat color_format{vove::color::PixelFormat::rgba8};
    UINT channels{};
    bool infer_three_channel_profile{};
    bool swap_red_blue{};
};

[[nodiscard]] std::optional<TaggedPixelPlan>
tagged_pixel_plan(const WICPixelFormatGUID &format) noexcept {
    if (is_cmyk_alpha(format)) {
        return std::nullopt;
    }
    if (is_cmyk(format) || is_generic_four_channel(format)) {
        return TaggedPixelPlan{.wic_format = GUID_WICPixelFormat32bppCMYK,
                               .color_format = vove::color::PixelFormat::cmyk8,
                               .channels = 4};
    }
    if (is_generic_three_channel(format)) {
        return TaggedPixelPlan{.wic_format = GUID_WICPixelFormat24bpp3Channels,
                               .color_format = vove::color::PixelFormat::rgb8,
                               .channels = 3,
                               .infer_three_channel_profile = true};
    }
    if (is_gray(format) || is_hdr_or_float(format)) {
        return std::nullopt;
    }
    return TaggedPixelPlan{.wic_format = GUID_WICPixelFormat32bppBGRA,
                           .color_format = vove::color::PixelFormat::rgba8,
                           .channels = 4,
                           .swap_red_blue = true};
}

[[nodiscard]] WicRasterDecodeErrorCode
transform_error_code(const vove::color::TransformError error) noexcept {
    switch (error) {
    case vove::color::TransformError::profile_required:
        return WicRasterDecodeErrorCode::color_profile_required;
    case vove::color::TransformError::invalid_profile:
        return WicRasterDecodeErrorCode::invalid_color_profile;
    case vove::color::TransformError::profile_mismatch:
        return WicRasterDecodeErrorCode::color_profile_mismatch;
    case vove::color::TransformError::resource_limit:
        return WicRasterDecodeErrorCode::output_limit_exceeded;
    case vove::color::TransformError::invalid_dimensions:
    case vove::color::TransformError::invalid_buffer:
    case vove::color::TransformError::transform_failed:
        return WicRasterDecodeErrorCode::color_transform_failed;
    case vove::color::TransformError::none:
        return WicRasterDecodeErrorCode::none;
    }
    return WicRasterDecodeErrorCode::color_transform_failed;
}

[[nodiscard]] std::string color_summary(const vove::color::ColorMetadata &metadata) {
    std::string model = metadata.source_model;
    if (model == "Lab") {
        model = "LAB";
    }
    return model + ": " + metadata.source_profile;
}

} // namespace

namespace detail {

IStream *create_wic_source_stream(const NativeSource &source) noexcept {
    try {
        return new (std::nothrow) NativeSourceStream(source);
    } catch (...) {
        return nullptr;
    }
}

} // namespace detail

WicRasterDecodeResult decode_wic_raster(const NativeSource &source,
                                        const WicRasterDecodeLimits &limits) {
    if (!valid_limits(limits)) {
        return failure(WicRasterDecodeErrorCode::invalid_limits);
    }
    if (source.size() > limits.maximum_source_bytes) {
        return failure(WicRasterDecodeErrorCode::source_limit_exceeded);
    }
    const auto initial_validation = source.validate_unchanged();
    if (!initial_validation.unchanged) {
        return validation_failure(initial_validation.error);
    }

    const auto prefix_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), kMaxSignatureProbeBytes));
    std::vector<std::byte> prefix(prefix_size);
    const auto prefix_read = source.read_at(0, prefix);
    if (!prefix_read.ok()) {
        return failure(prefix_read.error.code == NativeSourceErrorCode::disconnected
                           ? WicRasterDecodeErrorCode::source_disconnected
                           : WicRasterDecodeErrorCode::source_io_error,
                       static_cast<HRESULT>(prefix_read.error.system_code));
    }
    if (prefix_read.bytes_read != prefix.size()) {
        return failure(WicRasterDecodeErrorCode::truncated_input);
    }
    const auto probe = probe_raster_signature(prefix);
    if (probe.format == RasterFormat::Webp) {
        return failure(WicRasterDecodeErrorCode::unsupported_webp);
    }
    if (probe.format == RasterFormat::Heif) {
        return failure(WicRasterDecodeErrorCode::unsupported_heif);
    }
    if (probe.format == RasterFormat::Avif) {
        return failure(WicRasterDecodeErrorCode::unsupported_avif);
    }
    if (probe.status == ProbeStatus::Truncated) {
        return failure(WicRasterDecodeErrorCode::truncated_input);
    }
    if (probe.status == ProbeStatus::Malformed) {
        return failure(WicRasterDecodeErrorCode::malformed_input);
    }
    const auto *expected_container = container_guid(probe.format);
    if (!probe.matched() || expected_container == nullptr) {
        return failure(WicRasterDecodeErrorCode::unsupported_format);
    }

    const ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    auto result = apartment.result();
    if (SUCCEEDED(result)) {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    } else if (result == E_ACCESSDENIED) {
        // The capability-free worker intentionally has no COM broker authority. WIC's
        // in-process factory keeps raster decoding inside that boundary.
        result = WICCreateImagingFactory_Proxy(WINCODEC_SDK_VERSION, factory.put());
    }
    if (FAILED(result)) {
        return wic_failure(result, "create in-process WIC factory");
    }

    ComPtr<IStream> stream;
    stream.reset(detail::create_wic_source_stream(source));
    if (stream.get() == nullptr) {
        return failure(WicRasterDecodeErrorCode::decode_failed, E_OUTOFMEMORY);
    }
    auto *const native_stream = static_cast<NativeSourceStream *>(stream.get());

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                              decoder.put());
    if (FAILED(result)) {
        return wic_failure(result, "create WIC decoder", native_stream->source_error());
    }
    GUID actual_container{};
    result = decoder->GetContainerFormat(&actual_container);
    if (FAILED(result)) {
        return wic_failure(result, "read WIC container", native_stream->source_error());
    }
    if (IsEqualGUID(actual_container, *expected_container) == FALSE) {
        return failure(WicRasterDecodeErrorCode::malformed_input, S_OK,
                       "signature and WIC container disagree");
    }

    UINT frame_count{};
    result = decoder->GetFrameCount(&frame_count);
    if (FAILED(result) || frame_count == 0) {
        return FAILED(result)
                   ? wic_failure(result, "read frame count", native_stream->source_error())
                   : failure(WicRasterDecodeErrorCode::malformed_input);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.put());
    if (FAILED(result)) {
        return wic_failure(result, "decode first frame", native_stream->source_error());
    }

    UINT source_width{};
    UINT source_height{};
    result = frame->GetSize(&source_width, &source_height);
    if (FAILED(result)) {
        return wic_failure(result, "read frame size", native_stream->source_error());
    }
    if (source_width == 0 || source_height == 0 ||
        static_cast<std::uint64_t>(source_width) * source_height > limits.maximum_source_pixels) {
        return failure(WicRasterDecodeErrorCode::dimension_limit_exceeded);
    }

    WICPixelFormatGUID pixel_format{};
    result = frame->GetPixelFormat(&pixel_format);
    if (FAILED(result)) {
        return wic_failure(result, "read pixel format", native_stream->source_error());
    }
    if (is_hdr_or_float(pixel_format)) {
        return failure(WicRasterDecodeErrorCode::unsupported_color, S_OK,
                       "HDR and floating-point color require a dedicated transform");
    }

    const auto profile = embedded_color_profile(frame.get(), factory.get());
    if (profile.status == ProfileContextStatus::invalid) {
        return failure(WicRasterDecodeErrorCode::invalid_color_profile, profile.system_code,
                       "cannot read embedded ICC profile");
    }
    if (profile.status == ProfileContextStatus::resource_limit) {
        return failure(WicRasterDecodeErrorCode::invalid_color_profile, S_OK,
                       "embedded ICC profile exceeds the bounded limit");
    }
    if (profile.status == ProfileContextStatus::external_or_unknown &&
        !limits.allow_external_rgb_color_space) {
        return failure(WicRasterDecodeErrorCode::color_profile_required, S_OK,
                       "non-embedded color profile is unavailable in the sandbox");
    }

    const bool has_embedded_profile = profile.status == ProfileContextStatus::embedded;
    ComPtr<IWICFormatConverter> format_converter;
    IWICBitmapSource *raw_source{};
    bool assumed_srgb{};
    bool assigned_default_cmyk{};
    std::string decoded_color_summary;
    TaggedPixelPlan pixel_plan{.wic_format = GUID_WICPixelFormat32bppBGRA,
                               .color_format = vove::color::PixelFormat::rgba8,
                               .channels = 4,
                               .swap_red_blue = true};
    if (has_embedded_profile) {
        const auto plan = tagged_pixel_plan(pixel_format);
        if (!plan.has_value()) {
            return failure(WicRasterDecodeErrorCode::unsupported_color, S_OK,
                           "profiled alpha-CMYK, Gray, HDR, or unknown pixels are unsupported");
        }
        pixel_plan = *plan;
    } else if (is_cmyk(pixel_format)) {
        const auto plan = tagged_pixel_plan(pixel_format);
        if (!plan.has_value() || plan->color_format != vove::color::PixelFormat::cmyk8) {
            return failure(WicRasterDecodeErrorCode::unsupported_color, S_OK,
                           "unprofiled CMYK pixel layout is unsupported");
        }
        if (limits.fallback_cmyk_icc.empty()) {
            return failure(WicRasterDecodeErrorCode::color_profile_required, S_OK,
                           "CMYK: profile not specified");
        }
        pixel_plan = *plan;
        assigned_default_cmyk = true;
    } else if (is_generic_three_channel(pixel_format) || is_generic_four_channel(pixel_format) ||
               !is_safe_unprofiled_sdr(pixel_format)) {
        return failure(WicRasterDecodeErrorCode::color_profile_required, S_OK,
                       "unprofiled CMYK, LAB, or unknown color model");
    } else {
        assumed_srgb = true;
        decoded_color_summary =
            is_gray(pixel_format) ? "GRAY: sRGB tone (assumed)" : "RGB: sRGB (assumed)";
    }

    result = factory->CreateFormatConverter(format_converter.put());
    BOOL can_convert{};
    if (SUCCEEDED(result)) {
        result = format_converter->CanConvert(pixel_format, pixel_plan.wic_format, &can_convert);
    }
    if (SUCCEEDED(result) && can_convert == FALSE) {
        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (SUCCEEDED(result)) {
        result = format_converter->Initialize(frame.get(), pixel_plan.wic_format,
                                              WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom);
    }
    if (FAILED(result)) {
        return wic_failure(result, "normalize raster channels", native_stream->source_error());
    }
    raw_source = format_converter.get();

    const auto orientation = exif_orientation(frame.get());
    ComPtr<IWICBitmapFlipRotator> rotator;
    IWICBitmapSource *oriented_source = raw_source;
    if (limits.apply_orientation && orientation != 1) {
        result = factory->CreateBitmapFlipRotator(rotator.put());
        if (SUCCEEDED(result)) {
            result = rotator->Initialize(raw_source, orientation_transform(orientation));
        }
        if (FAILED(result)) {
            return wic_failure(result, "apply EXIF orientation", native_stream->source_error());
        }
        oriented_source = rotator.get();
    }

    UINT oriented_width{};
    UINT oriented_height{};
    result = oriented_source->GetSize(&oriented_width, &oriented_height);
    if (FAILED(result) || oriented_width == 0 || oriented_height == 0) {
        return FAILED(result)
                   ? wic_failure(result, "read oriented size", native_stream->source_error())
                   : failure(WicRasterDecodeErrorCode::decode_failed);
    }
    const auto output_size =
        canonical_size(oriented_width, oriented_height, limits.maximum_output_edge);
    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource *output_source = oriented_source;
    if (output_size.width != oriented_width || output_size.height != oriented_height) {
        result = factory->CreateBitmapScaler(scaler.put());
        if (SUCCEEDED(result)) {
            result = scaler->Initialize(oriented_source, output_size.width, output_size.height,
                                        WICBitmapInterpolationModeFant);
        }
        if (FAILED(result)) {
            return wic_failure(result, "scale raster", native_stream->source_error());
        }
        output_source = scaler.get();
    }

    const auto rgba_stride64 = static_cast<std::uint64_t>(output_size.width) * 4ULL;
    const auto rgba_bytes64 = rgba_stride64 * output_size.height;
    const auto raw_stride64 = static_cast<std::uint64_t>(output_size.width) * pixel_plan.channels;
    const auto raw_bytes64 = raw_stride64 * output_size.height;
    if (rgba_stride64 > std::numeric_limits<UINT>::max() ||
        rgba_bytes64 > limits.maximum_output_bytes ||
        rgba_bytes64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        raw_stride64 > std::numeric_limits<UINT>::max() ||
        raw_bytes64 > std::numeric_limits<UINT>::max() ||
        raw_bytes64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return failure(WicRasterDecodeErrorCode::output_limit_exceeded);
    }

    std::vector<std::byte> raw_pixels(static_cast<std::size_t>(raw_bytes64));
    result = output_source->CopyPixels(nullptr, static_cast<UINT>(raw_stride64),
                                       static_cast<UINT>(raw_bytes64),
                                       reinterpret_cast<BYTE *>(raw_pixels.data()));
    if (FAILED(result)) {
        return wic_failure(result, "copy normalized pixels", native_stream->source_error());
    }
    if (pixel_plan.swap_red_blue) {
        for (std::size_t offset = 0; offset < raw_pixels.size(); offset += 4U) {
            std::swap(raw_pixels[offset], raw_pixels[offset + 2U]);
        }
    }
    const auto final_validation = source.validate_unchanged();
    if (!final_validation.unchanged) {
        return validation_failure(final_validation.error);
    }

    std::vector<std::byte> rgba8;
    if (has_embedded_profile || assigned_default_cmyk) {
        const auto fallback_cmyk =
            assigned_default_cmyk ? limits.fallback_cmyk_icc : std::span<const std::byte>{};
        auto transform = vove::color::to_srgb_rgba8(vove::color::TransformRequest{
            .width = output_size.width,
            .height = output_size.height,
            .source_stride = static_cast<std::size_t>(raw_stride64),
            .source_format = pixel_plan.color_format,
            .source_pixels = raw_pixels,
            .embedded_icc = has_embedded_profile ? std::span<const std::byte>(profile.embedded_icc)
                                                 : std::span<const std::byte>{},
            .fallback_cmyk_icc = fallback_cmyk,
        });
        if (!transform.ok() && pixel_plan.infer_three_channel_profile &&
            transform.error == vove::color::TransformError::profile_mismatch) {
            transform = vove::color::to_srgb_rgba8(vove::color::TransformRequest{
                .width = output_size.width,
                .height = output_size.height,
                .source_stride = static_cast<std::size_t>(raw_stride64),
                .source_format = vove::color::PixelFormat::lab8,
                .source_pixels = raw_pixels,
                .embedded_icc = profile.embedded_icc,
                .fallback_cmyk_icc = {},
            });
        }
        if (!transform.ok()) {
            return failure(transform_error_code(transform.error), S_OK,
                           transform.detail.empty() ? "embedded ICC conversion failed"
                                                    : std::move(transform.detail));
        }
        if (transform.rgba_stride != static_cast<std::size_t>(rgba_stride64) ||
            transform.rgba_pixels.size() != static_cast<std::size_t>(rgba_bytes64)) {
            return failure(WicRasterDecodeErrorCode::color_transform_failed, S_OK,
                           "ICC transform returned an invalid RGBA buffer");
        }
        decoded_color_summary = color_summary(transform.metadata);
        if (assigned_default_cmyk) {
            decoded_color_summary += " (default)";
        }
        rgba8 = std::move(transform.rgba_pixels);
    } else {
        rgba8 = std::move(raw_pixels);
    }

    WicRasterDecodeResult decoded;
    const auto identity_profile = assigned_default_cmyk
                                      ? limits.fallback_cmyk_icc
                                      : std::span<const std::byte>(profile.embedded_icc);
    decoded.image.rgba8 = std::move(rgba8);
    decoded.image.width = output_size.width;
    decoded.image.height = output_size.height;
    decoded.image.metadata = {.format = probe.format,
                              .source_width = source_width,
                              .source_height = source_height,
                              .page_count = frame_count,
                              .exif_orientation = orientation,
                              .color_summary = std::move(decoded_color_summary),
                              .source_profile_fingerprint = profile_sha256(identity_profile),
                              .assumed_srgb = assumed_srgb};
    return decoded;
}

} // namespace vove::handlers::raster

#else

namespace vove::handlers::raster {

WicRasterDecodeResult decode_wic_raster(const NativeSource &, const WicRasterDecodeLimits &) {
    WicRasterDecodeResult result;
    result.error.code = WicRasterDecodeErrorCode::decoder_unavailable;
    result.error.detail = "WIC is available only on Windows";
    return result;
}

namespace detail {
IStream *create_wic_source_stream(const NativeSource &) noexcept {
    return nullptr;
}
} // namespace detail

} // namespace vove::handlers::raster

#endif
