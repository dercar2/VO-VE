#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QStringList>

#include <optional>

namespace vove::ui {

inline constexpr qsizetype kMaximumLaunchPaths = 1'024;
inline constexpr qsizetype kMaximumLaunchPathUnits = 32'767;
inline constexpr qsizetype kMaximumLaunchPayloadBytes = 1024 * 1024;
inline constexpr qsizetype kLaunchHeaderBytes = sizeof(quint32);
inline constexpr qsizetype kMaximumLaunchFrameBytes =
    kLaunchHeaderBytes + kMaximumLaunchPayloadBytes;
inline constexpr char kLaunchAckByte = '\x06';

struct LaunchRequest {
    QStringList paths;
    QString initial_directory;
};

enum class LaunchFrameStatus { pending, complete, rejected };

class LaunchFrameDecoder final {
  public:
    [[nodiscard]] qsizetype remaining_capacity() const noexcept;
    [[nodiscard]] LaunchFrameStatus append(QByteArrayView bytes);
    [[nodiscard]] LaunchFrameStatus status() const noexcept;
    [[nodiscard]] const std::optional<LaunchRequest> &request() const noexcept;

  private:
    QByteArray buffer_;
    std::optional<LaunchRequest> request_;
    LaunchFrameStatus status_{LaunchFrameStatus::pending};
};

enum class LaunchDeliveryStatus { delivered, server_unavailable, failed };

[[nodiscard]] std::optional<LaunchRequest>
launch_request_from_arguments(const QStringList &arguments);
[[nodiscard]] QByteArray encode_launch_request(const LaunchRequest &request);
[[nodiscard]] std::optional<LaunchRequest> decode_launch_request(const QByteArray &payload);
[[nodiscard]] QByteArray encode_launch_frame(const LaunchRequest &request);
[[nodiscard]] LaunchDeliveryStatus deliver_launch_request(const QString &server_name,
                                                          const LaunchRequest &request,
                                                          int connect_timeout_ms = 500,
                                                          int delivery_timeout_ms = 1'000);

} // namespace vove::ui
