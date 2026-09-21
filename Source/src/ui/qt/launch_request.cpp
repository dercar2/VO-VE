#include "launch_request.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QtEndian>

#include <cstring>

namespace vove::ui {
namespace {

std::optional<QStringList> validate_paths(QStringList paths) {
    if (paths.size() > kMaximumLaunchPaths) {
        return std::nullopt;
    }
    qsizetype total_units{};
    QStringList accepted;
    accepted.reserve(paths.size());
    for (auto &path : paths) {
        if (path.isEmpty() || path.size() > kMaximumLaunchPathUnits || path.contains(QChar::Null)) {
            return std::nullopt;
        }
        total_units += path.size();
        if (total_units > kMaximumLaunchPayloadBytes) {
            return std::nullopt;
        }
        accepted.push_back(std::move(path));
    }
    return accepted;
}

} // namespace

std::optional<LaunchRequest> launch_request_from_arguments(const QStringList &arguments) {
    QStringList paths;
    QString initial_directory;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const auto &argument = arguments.at(index);
        if ((argument == QStringLiteral("--path") || argument == QStringLiteral("--select")) &&
            index + 1 < arguments.size()) {
            const auto absolute =
                QFileInfo(QDir::fromNativeSeparators(arguments.at(++index))).absoluteFilePath();
            paths.push_back(absolute);
            if (initial_directory.isEmpty()) {
                initial_directory = argument == QStringLiteral("--path")
                                        ? absolute
                                        : QFileInfo(absolute).absolutePath();
            }
        } else if (!argument.startsWith(QLatin1Char('-'))) {
            const auto absolute =
                QFileInfo(QDir::fromNativeSeparators(argument)).absoluteFilePath();
            paths.push_back(absolute);
            if (initial_directory.isEmpty()) {
                initial_directory = QFileInfo(absolute).absolutePath();
            }
        }
    }
    auto validated = validate_paths(std::move(paths));
    if (!validated) {
        return std::nullopt;
    }
    LaunchRequest request{.paths = std::move(*validated),
                          .initial_directory = std::move(initial_directory)};
    if (encode_launch_request(request).isEmpty()) {
        return std::nullopt;
    }
    return request;
}

QByteArray encode_launch_request(const LaunchRequest &request) {
    auto paths = validate_paths(request.paths);
    if (!paths) {
        return {};
    }
    if ((!paths->isEmpty() && request.initial_directory.isEmpty()) ||
        request.initial_directory.size() > kMaximumLaunchPathUnits ||
        request.initial_directory.contains(QChar::Null)) {
        return {};
    }
    QJsonArray encoded_paths;
    for (const auto &path : *paths) {
        encoded_paths.push_back(path);
    }
    const auto payload = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 2},
                                                   {QStringLiteral("paths"), encoded_paths},
                                                   {QStringLiteral("initial_directory"),
                                                    request.initial_directory}})
                             .toJson(QJsonDocument::Compact);
    return payload.size() <= kMaximumLaunchPayloadBytes ? payload : QByteArray{};
}

std::optional<LaunchRequest> decode_launch_request(const QByteArray &payload) {
    if (payload.isEmpty() || payload.size() > kMaximumLaunchPayloadBytes ||
        payload.contains('\0')) {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto object = document.object();
    if (object.size() != 3 || object.value(QStringLiteral("schema")).toInt() != 2 ||
        !object.value(QStringLiteral("paths")).isArray() ||
        !object.value(QStringLiteral("initial_directory")).isString()) {
        return std::nullopt;
    }
    QStringList paths;
    for (const auto &value : object.value(QStringLiteral("paths")).toArray()) {
        if (!value.isString()) {
            return std::nullopt;
        }
        paths.push_back(value.toString());
    }
    auto validated = validate_paths(std::move(paths));
    if (!validated) {
        return std::nullopt;
    }
    auto initial_directory = object.value(QStringLiteral("initial_directory")).toString();
    if ((!validated->isEmpty() && initial_directory.isEmpty()) ||
        initial_directory.size() > kMaximumLaunchPathUnits ||
        initial_directory.contains(QChar::Null)) {
        return std::nullopt;
    }
    return LaunchRequest{.paths = std::move(*validated),
                         .initial_directory = std::move(initial_directory)};
}

QByteArray encode_launch_frame(const LaunchRequest &request) {
    const auto payload = encode_launch_request(request);
    if (payload.isEmpty()) {
        return {};
    }
    const auto payload_size = static_cast<quint32>(payload.size());
    const auto big_endian_size = qToBigEndian(payload_size);
    QByteArray frame;
    frame.reserve(kLaunchHeaderBytes + payload.size());
    frame.append(reinterpret_cast<const char *>(&big_endian_size), kLaunchHeaderBytes);
    frame.append(payload);
    return frame;
}

qsizetype LaunchFrameDecoder::remaining_capacity() const noexcept {
    return status_ == LaunchFrameStatus::pending ? kMaximumLaunchFrameBytes + 1 - buffer_.size()
                                                 : 0;
}

LaunchFrameStatus LaunchFrameDecoder::append(const QByteArrayView bytes) {
    if (status_ != LaunchFrameStatus::pending || bytes.size() > remaining_capacity()) {
        status_ = LaunchFrameStatus::rejected;
        request_.reset();
        return status_;
    }
    buffer_.append(bytes.data(), bytes.size());
    if (buffer_.size() < kLaunchHeaderBytes) {
        return status_;
    }

    quint32 big_endian_size{};
    std::memcpy(&big_endian_size, buffer_.constData(), sizeof(big_endian_size));
    const auto payload_size = qFromBigEndian(big_endian_size);
    if (payload_size > kMaximumLaunchPayloadBytes) {
        status_ = LaunchFrameStatus::rejected;
        return status_;
    }
    const auto expected_size = kLaunchHeaderBytes + static_cast<qsizetype>(payload_size);
    if (buffer_.size() > expected_size) {
        status_ = LaunchFrameStatus::rejected;
        return status_;
    }
    if (buffer_.size() < expected_size) {
        return status_;
    }

    request_ = decode_launch_request(buffer_.mid(kLaunchHeaderBytes, payload_size));
    status_ = request_ ? LaunchFrameStatus::complete : LaunchFrameStatus::rejected;
    return status_;
}

LaunchFrameStatus LaunchFrameDecoder::status() const noexcept {
    return status_;
}

const std::optional<LaunchRequest> &LaunchFrameDecoder::request() const noexcept {
    return request_;
}

LaunchDeliveryStatus deliver_launch_request(const QString &server_name,
                                            const LaunchRequest &request,
                                            const int connect_timeout_ms,
                                            const int delivery_timeout_ms) {
    const auto frame = encode_launch_frame(request);
    if (frame.isEmpty() || connect_timeout_ms <= 0 || delivery_timeout_ms <= 0) {
        return LaunchDeliveryStatus::failed;
    }
    QLocalSocket socket;
    socket.connectToServer(server_name, QIODevice::ReadWrite);
    if (!socket.waitForConnected(connect_timeout_ms)) {
        return socket.error() == QLocalSocket::ServerNotFoundError ||
                       socket.error() == QLocalSocket::ConnectionRefusedError
                   ? LaunchDeliveryStatus::server_unavailable
                   : LaunchDeliveryStatus::failed;
    }
    if (socket.write(frame) != frame.size()) {
        return LaunchDeliveryStatus::failed;
    }

    QElapsedTimer timer;
    timer.start();
    while (socket.bytesToWrite() > 0) {
        const auto remaining = delivery_timeout_ms - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket.waitForBytesWritten(remaining)) {
            return LaunchDeliveryStatus::failed;
        }
    }
    while (socket.bytesAvailable() < 1) {
        const auto remaining = delivery_timeout_ms - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(remaining)) {
            return LaunchDeliveryStatus::failed;
        }
    }
    const auto acknowledged = socket.read(1) == QByteArray(1, kLaunchAckByte);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(qMin(500, delivery_timeout_ms));
    }
    return acknowledged ? LaunchDeliveryStatus::delivered : LaunchDeliveryStatus::failed;
}

} // namespace vove::ui
