#include "gif_stream.hpp"

#include "vove/cache/qoi_codec.hpp"
#include "vove/platform/read_only_source.hpp"
#include "vove/worker/protocol.hpp"
#include "vove/worker/supervisor.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QtEndian>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace vove::preview::helper {
namespace {

constexpr std::uint64_t kMaximumSourceBytes = 64ULL * 1024 * 1024;
constexpr std::uint64_t kMaximumQoiBytes = 24ULL * 1024 * 1024;
constexpr std::uint32_t kMaximumEdge = 2048;

std::filesystem::path native_path(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}

class OutputSlot final {
  public:
    OutputSlot() {
#ifdef _WIN32
        const auto path = QDir(QDir::tempPath())
                              .filePath(QStringLiteral("vove-gif-") +
                                        QUuid::createUuid().toString(QUuid::WithoutBraces));
        writer_ = CreateFileW(
            reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (writer_ == INVALID_HANDLE_VALUE)
            return;
        // ReOpenFile gives the helper a separate file position from the worker's inherited slot.
        reader_ = ReOpenFile(writer_, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
#else
        path_ =
            QFile::encodeName(QDir(QDir::tempPath()).filePath(QStringLiteral("vove-gif-XXXXXX")));
        reader_ = ::mkstemp(path_.data());
        if (reader_ < 0) {
            path_.clear();
            return;
        }
        if (::fcntl(reader_, F_SETFD, FD_CLOEXEC) == 0)
            writer_ = ::open(path_.constData(), O_WRONLY | O_CLOEXEC);
        if (!remove_path() && writer_ >= 0) {
            ::close(writer_);
            writer_ = -1;
        }
#endif
    }

    ~OutputSlot() {
#ifdef _WIN32
        if (reader_ != INVALID_HANDLE_VALUE)
            CloseHandle(reader_);
        if (writer_ != INVALID_HANDLE_VALUE)
            CloseHandle(writer_);
#else
        if (reader_ >= 0)
            ::close(reader_);
        if (writer_ >= 0)
            ::close(writer_);
        static_cast<void>(remove_path());
#endif
    }
    OutputSlot(const OutputSlot &) = delete;
    OutputSlot &operator=(const OutputSlot &) = delete;

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return reader_ != INVALID_HANDLE_VALUE && writer_ != INVALID_HANDLE_VALUE;
#else
        return reader_ >= 0 && writer_ >= 0;
#endif
    }

    [[nodiscard]] worker::NativeObject writer() const noexcept {
#ifdef _WIN32
        return reinterpret_cast<worker::NativeObject>(writer_);
#else
        return writer_;
#endif
    }

    bool read(std::span<std::byte> output) const {
#ifdef _WIN32
        LARGE_INTEGER size{};
        LARGE_INTEGER beginning{};
        if (!GetFileSizeEx(reader_, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != output.size() ||
            !SetFilePointerEx(reader_, beginning, nullptr, FILE_BEGIN))
            return false;
        std::size_t offset{};
        while (offset < output.size()) {
            DWORD count{};
            if (!ReadFile(reader_, output.data() + offset,
                          static_cast<DWORD>(output.size() - offset), &count, nullptr) ||
                count == 0)
                return false;
            offset += count;
        }
#else
        struct stat info{};
        if (::fstat(reader_, &info) != 0 || info.st_size < 0 ||
            static_cast<std::uint64_t>(info.st_size) != output.size())
            return false;
        std::size_t offset{};
        while (offset < output.size()) {
            const auto count = ::pread(reader_, output.data() + offset, output.size() - offset,
                                       static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            offset += static_cast<std::size_t>(count);
        }
#endif
        return true;
    }

  private:
#ifdef _WIN32
    HANDLE reader_{INVALID_HANDLE_VALUE};
    HANDLE writer_{INVALID_HANDLE_VALUE};
#else
    bool remove_path() noexcept {
        if (path_.isEmpty())
            return true;
        int result{};
        do {
            result = ::unlink(path_.constData());
        } while (result != 0 && errno == EINTR);
        if (result == 0 || errno == ENOENT) {
            path_.clear();
            return true;
        }
        return false;
    }

    QByteArray path_;
    int reader_{-1};
    int writer_{-1};
#endif
};

bool write_exact(QFile &output, const std::span<const std::byte> bytes) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto written = output.write(reinterpret_cast<const char *>(bytes.data() + offset),
                                          static_cast<qint64>(bytes.size() - offset));
        if (written <= 0)
            return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

worker::WorkerResult failure(worker::ResultStatus status, const char *detail) {
    worker::WorkerResult result;
    result.job_id = 1;
    result.status = status;
    result.diagnostic_utf8 = detail;
    return result;
}

std::string bounded_diagnostic(const std::string &detail) {
    auto encoded = QString::fromUtf8(detail).toUtf8();
    const auto maximum = static_cast<qsizetype>(worker::kMaximumDiagnosticBytes);
    if (encoded.size() > maximum) {
        auto end = maximum;
        while (end > 0 && (static_cast<unsigned char>(encoded[end]) & 0xC0U) == 0x80U)
            --end;
        encoded.truncate(end);
    }
    return encoded.toStdString();
}

int terminal(QFile &output, const worker::WorkerResult &result) {
    const auto encoded = worker::encode_worker_result(result);
    if (!write_exact(output, encoded) || !output.flush())
        return 1;
    return result.status == worker::ResultStatus::success ||
                   result.status == worker::ResultStatus::cancelled
               ? 0
               : 1;
}

bool valid_qoi(const std::vector<std::byte> &payload, const worker::AnimationFrame &frame) {
    const auto *data = reinterpret_cast<const uchar *>(payload.data());
    if (payload.size() < 22 || data[0] != 'q' || data[1] != 'o' || data[2] != 'i' ||
        data[3] != 'f' || qFromBigEndian<quint32>(data + 4) != frame.image.width ||
        qFromBigEndian<quint32>(data + 8) != frame.image.height || data[12] != 4 || data[13] > 1)
        return false;
    const auto decoded = cache::decode_qoi_rgba8(payload);
    return decoded.ok() && decoded.width == frame.image.width &&
           decoded.height == frame.image.height;
}

} // namespace

int run_gif_stream(const QString &path, const quint64 source_size, const qint64 source_modified,
                   const QString &source_revision) {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return 1;
#endif
    QFile input;
    QFile output;
    if (!input.open(stdin, QIODevice::ReadOnly | QIODevice::Unbuffered,
                    QFileDevice::DontCloseHandle) ||
        !output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered,
                     QFileDevice::DontCloseHandle))
        return 1;
    try {
        if (path.isEmpty() || path.contains(QChar::Null) || source_size == 0 ||
            source_size > kMaximumSourceBytes || source_revision.isEmpty() ||
            source_revision.size() > 4096 || source_revision.contains(QChar::Null))
            return terminal(output, failure(worker::ResultStatus::source_changed,
                                            "GIF source identity is invalid"));

        auto opened = platform::open_read_only_source(native_path(path), kMaximumSourceBytes);
        if (!opened.source)
            return terminal(output, failure(worker::ResultStatus::source_unavailable,
                                            "GIF source is unavailable"));
        if (opened.source->size_bytes() != source_size ||
            opened.source->modified_unix_ns() != source_modified ||
            opened.source->source_revision_utf8() != source_revision.toUtf8().toStdString())
            return terminal(output, failure(worker::ResultStatus::source_changed,
                                            "GIF source identity changed before decoding"));

        OutputSlot slot;
        if (!slot.valid())
            return terminal(output, failure(worker::ResultStatus::internal_error,
                                            "GIF temporary output is unavailable"));
#ifdef _WIN32
        const auto worker_name = QStringLiteral("vove-raster-worker.exe");
#else
        const auto worker_name = QStringLiteral("vove-raster-worker");
#endif
        bool cancelled{};
        std::optional<worker::WorkerResult> bridge_failure;
        std::uint64_t expected_sequence{};
        worker::SupervisorRequest request;
        request.worker_executable =
            native_path(QDir(QCoreApplication::applicationDirPath()).filePath(worker_name));
        request.source_object = opened.source->native_object();
        request.output_object = slot.writer();
        request.job.job_id = 1;
        request.job.generation = 1;
        request.job.source_token = 1;
        request.job.output_token = 2;
        request.job.source_format_hint = worker::SourceFormatHint::gif_animation;
        request.job.limits = {.maximum_input_bytes = kMaximumSourceBytes,
                              .maximum_output_bytes = kMaximumQoiBytes,
                              .memory_limit_bytes = 512ULL * 1024 * 1024,
                              .wall_timeout_ms = 10'000,
                              .canonical_edge = kMaximumEdge};
        request.expected_build_id = "vove-raster-jpeg-3";
        request.timeout = std::chrono::milliseconds{10'000};
        request.cancelled = [&] { return cancelled; };
        request.animation_frame_handler = [&](const worker::AnimationFrame &frame) {
            if (opened.source) {
                const auto identity = opened.source->identity_status();
                // The worker already owns an encoded snapshot and has closed its source copy.
                opened.source.reset();
                if (identity != platform::SourceIdentityStatus::unchanged) {
                    bridge_failure = failure(identity == platform::SourceIdentityStatus::changed
                                                 ? worker::ResultStatus::source_changed
                                                 : worker::ResultStatus::source_unavailable,
                                             "GIF source changed while taking its snapshot");
                    return false;
                }
            }
            if (frame.image.job_id != 1 || frame.image.status != worker::ResultStatus::success ||
                frame.image.width == 0 || frame.image.height == 0 ||
                frame.image.width > kMaximumEdge || frame.image.height > kMaximumEdge ||
                frame.image.bytes_written < 22 || frame.image.bytes_written > kMaximumQoiBytes ||
                frame.sequence != expected_sequence ||
                expected_sequence == std::numeric_limits<std::uint64_t>::max()) {
                bridge_failure = failure(worker::ResultStatus::internal_error,
                                         "GIF frame exceeds stream limits");
                return false;
            }
            std::vector<std::byte> payload(static_cast<std::size_t>(frame.image.bytes_written));
            if (!slot.read(payload) || !valid_qoi(payload, frame)) {
                bridge_failure =
                    failure(worker::ResultStatus::internal_error, "GIF output slot is invalid");
                return false;
            }
            const auto metadata = worker::encode_animation_frame(frame);
            if (!write_exact(output, metadata) || !write_exact(output, payload) ||
                !output.flush()) {
                cancelled = true;
                return false;
            }
            ++expected_sequence;
            char acknowledgement{};
            if (input.read(&acknowledgement, 1) != 1 || acknowledgement != 1) {
                cancelled = true;
                if (acknowledgement != 0 && acknowledgement != 1)
                    bridge_failure = failure(worker::ResultStatus::internal_error,
                                             "GIF acknowledgement is invalid");
                return false;
            }
            return true;
        };
        const auto result = worker::run_worker_once(request);
        if (bridge_failure)
            return terminal(output, *bridge_failure);
        if (result.error != worker::SupervisorError::none) {
            auto failed = failure(result.error == worker::SupervisorError::timed_out
                                      ? worker::ResultStatus::timed_out
                                      : worker::ResultStatus::internal_error,
                                  "GIF decoder process failed");
            if (!result.detail.empty())
                failed.diagnostic_utf8 = bounded_diagnostic(result.detail);
            return terminal(output, failed);
        }
        return terminal(output, result.worker_result);
    } catch (...) {
        return terminal(output, failure(worker::ResultStatus::internal_error, "GIF stream failed"));
    }
}

} // namespace vove::preview::helper
