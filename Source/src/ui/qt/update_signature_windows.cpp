#include "update_signature.hpp"

#include <QCryptographicHash>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <limits>

namespace vove::ui::updates {
namespace {

class AlgorithmHandle final {
  public:
    ~AlgorithmHandle() {
        if (value != nullptr) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
    BCRYPT_ALG_HANDLE value{};
};

class KeyHandle final {
  public:
    ~KeyHandle() {
        if (value != nullptr) {
            BCryptDestroyKey(value);
        }
    }
    BCRYPT_KEY_HANDLE value{};
};

} // namespace

SignatureStatus verify_p256_sha256(const QByteArray &message, const QByteArray &public_key,
                                   const QByteArray &signature) noexcept {
    constexpr qsizetype coordinate_bytes = 32;
    constexpr qsizetype sec1_public_key_bytes = 1 + coordinate_bytes * 2;
    constexpr qsizetype raw_signature_bytes = coordinate_bytes * 2;
    if (public_key.size() != sec1_public_key_bytes || public_key.front() != '\x04' ||
        signature.size() != raw_signature_bytes ||
        message.size() > static_cast<qsizetype>(std::numeric_limits<ULONG>::max())) {
        return SignatureStatus::invalid;
    }

    AlgorithmHandle algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) <
        0) {
        return SignatureStatus::unsupported;
    }

    std::array<unsigned char, sizeof(BCRYPT_ECCKEY_BLOB) + coordinate_bytes * 2> blob{};
    const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC,
                                    static_cast<ULONG>(coordinate_bytes)};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), public_key.constData() + 1, coordinate_bytes * 2);

    KeyHandle key;
    if (BCryptImportKeyPair(algorithm.value, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.value,
                            blob.data(), static_cast<ULONG>(blob.size()), 0) < 0) {
        return SignatureStatus::invalid;
    }

    const auto digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
    const auto status = BCryptVerifySignature(
        key.value, nullptr, reinterpret_cast<PUCHAR>(const_cast<char *>(digest.constData())),
        static_cast<ULONG>(digest.size()),
        reinterpret_cast<PUCHAR>(const_cast<char *>(signature.constData())),
        static_cast<ULONG>(signature.size()), 0);
    return status >= 0 ? SignatureStatus::verified : SignatureStatus::invalid;
}

} // namespace vove::ui::updates
