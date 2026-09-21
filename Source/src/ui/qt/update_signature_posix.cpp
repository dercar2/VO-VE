#include "update_signature.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <memory>

namespace vove::ui::updates {
namespace {

struct PkeyContextDeleter {
    void operator()(EVP_PKEY_CTX *value) const noexcept {
        EVP_PKEY_CTX_free(value);
    }
};

struct PkeyDeleter {
    void operator()(EVP_PKEY *value) const noexcept {
        EVP_PKEY_free(value);
    }
};

struct DigestContextDeleter {
    void operator()(EVP_MD_CTX *value) const noexcept {
        EVP_MD_CTX_free(value);
    }
};

using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter>;
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

[[nodiscard]] QByteArray der_integer(const QByteArray &raw) {
    qsizetype first{};
    while (first + 1 < raw.size() && raw.at(first) == '\0') {
        ++first;
    }
    const auto high_bit = (static_cast<unsigned char>(raw.at(first)) & 0x80U) != 0U;
    const auto value_size = raw.size() - first + (high_bit ? 1 : 0);

    QByteArray encoded;
    encoded.reserve(value_size + 2);
    encoded.append('\x02');
    encoded.append(static_cast<char>(value_size));
    if (high_bit) {
        encoded.append('\0');
    }
    encoded.append(raw.constData() + first, raw.size() - first);
    return encoded;
}

[[nodiscard]] QByteArray der_signature(const QByteArray &raw) {
    const auto r = der_integer(raw.first(32));
    const auto s = der_integer(raw.sliced(32));
    const auto content_size = r.size() + s.size();

    QByteArray encoded;
    encoded.reserve(content_size + 2);
    encoded.append('\x30');
    encoded.append(static_cast<char>(content_size));
    encoded.append(r);
    encoded.append(s);
    return encoded;
}

} // namespace

SignatureStatus verify_p256_sha256(const QByteArray &message, const QByteArray &public_key,
                                   const QByteArray &signature) noexcept {
    constexpr qsizetype sec1_public_key_bytes = 65;
    constexpr qsizetype raw_signature_bytes = 64;
    if (public_key.size() != sec1_public_key_bytes || public_key.front() != '\x04' ||
        signature.size() != raw_signature_bytes) {
        return SignatureStatus::invalid;
    }

    const PkeyContext key_context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    if (!key_context || EVP_PKEY_fromdata_init(key_context.get()) <= 0) {
        return SignatureStatus::unsupported;
    }

    char group_name[] = "prime256v1";
    OSSL_PARAM parameters[]{
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group_name, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                          const_cast<char *>(public_key.constData()),
                                          static_cast<std::size_t>(public_key.size())),
        OSSL_PARAM_construct_end()};
    EVP_PKEY *imported_key{};
    if (EVP_PKEY_fromdata(key_context.get(), &imported_key, EVP_PKEY_PUBLIC_KEY, parameters) <= 0 ||
        imported_key == nullptr) {
        return SignatureStatus::invalid;
    }
    const Pkey key(imported_key);

    const DigestContext digest_context(EVP_MD_CTX_new());
    if (!digest_context || EVP_DigestVerifyInit(digest_context.get(), nullptr, EVP_sha256(),
                                                nullptr, key.get()) <= 0) {
        return SignatureStatus::unsupported;
    }

    const auto encoded_signature = der_signature(signature);
    const auto result =
        EVP_DigestVerify(digest_context.get(),
                         reinterpret_cast<const unsigned char *>(encoded_signature.constData()),
                         static_cast<std::size_t>(encoded_signature.size()),
                         reinterpret_cast<const unsigned char *>(message.constData()),
                         static_cast<std::size_t>(message.size()));
    return result == 1 ? SignatureStatus::verified : SignatureStatus::invalid;
}

} // namespace vove::ui::updates
