#pragma once

#include <QByteArray>

#include <cstdint>

namespace vove::ui::updates {

enum class SignatureStatus : std::uint8_t {
    verified,
    invalid,
    unsupported,
};

// The public key is SEC1 uncompressed P-256 (04 || X || Y). The signature is raw r || s.
[[nodiscard]] SignatureStatus verify_p256_sha256(const QByteArray &message,
                                                 const QByteArray &public_key,
                                                 const QByteArray &signature) noexcept;

} // namespace vove::ui::updates
