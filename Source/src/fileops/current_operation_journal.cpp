#include "vove/fileops/current_operation_journal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace vove::fileops {
namespace {

constexpr std::array<std::byte, 8> kMagic{std::byte{0x56}, std::byte{0x4f}, std::byte{0x56},
                                          std::byte{0x45}, std::byte{0x43}, std::byte{0x55},
                                          std::byte{0x52}, std::byte{0x52}};
constexpr std::array<std::byte, 4> kReservedPrefix{std::byte{0x56}, std::byte{0x4f},
                                                   std::byte{0x56}, std::byte{0x45}};

std::uint32_t read_u32_le(const std::span<const std::byte> bytes,
                          const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u32_le(const std::span<std::byte> bytes, const std::size_t offset,
                  const std::uint32_t value) noexcept {
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
    bytes[offset + 1U] = std::byte{static_cast<std::uint8_t>(value >> 8U)};
    bytes[offset + 2U] = std::byte{static_cast<std::uint8_t>(value >> 16U)};
    bytes[offset + 3U] = std::byte{static_cast<std::uint8_t>(value >> 24U)};
}

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (unsigned bit{}; bit < 8U; ++bit) {
            const auto mask =
                static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & std::uint32_t{1}));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::vector<std::byte> encode(const CurrentOperationKind kind,
                              const std::span<const std::byte> payload) {
    std::vector<std::byte> bytes(kCurrentOperationJournalHeaderBytes + payload.size());
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    const auto writable = std::span<std::byte>(bytes);
    write_u32_le(writable, 8U, kCurrentOperationJournalSchemaVersion);
    write_u32_le(writable, 12U, static_cast<std::uint32_t>(kind));
    write_u32_le(writable, 16U, static_cast<std::uint32_t>(payload.size()));
    write_u32_le(writable, 20U, crc32(payload));
    std::copy(payload.begin(), payload.end(), bytes.begin() + kCurrentOperationJournalHeaderBytes);
    return bytes;
}

} // namespace

CurrentOperationJournalStore::CurrentOperationJournalStore(std::filesystem::path primary_path)
    : durable_(std::move(primary_path)) {}

DurableJournalResult
CurrentOperationJournalStore::write(const CurrentOperationKind kind,
                                    const std::span<const std::byte> payload) const {
    if (static_cast<std::uint32_t>(kind) == 0U) {
        return {.status = DurableJournalStatus::corrupt, .error = {}};
    }
    if (payload.size() > kMaximumCurrentOperationPayloadBytes) {
        return {.status = DurableJournalStatus::payload_too_large, .error = {}};
    }
    return durable_.write(encode(kind, payload));
}

CurrentOperationJournalReadResult CurrentOperationJournalStore::read() const {
    const auto stored = durable_.read();
    CurrentOperationJournalReadResult result;
    result.status = stored.status;
    result.error = stored.error;
    result.source = stored.source;
    if (!stored.ok()) {
        return result;
    }

    const auto bytes = std::span<const std::byte>(stored.payload);
    const bool reserved_prefix =
        bytes.size() >= kReservedPrefix.size() &&
        std::equal(kReservedPrefix.begin(), kReservedPrefix.end(), bytes.begin());
    const bool exact_magic =
        bytes.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
    if (!exact_magic) {
        if (reserved_prefix) {
            result.status = DurableJournalStatus::corrupt;
            result.source = DurableJournalReadSource::none;
            return result;
        }
        result.encoding = CurrentOperationJournalEncoding::legacy_untyped;
        result.payload = stored.payload;
        return result;
    }

    if (bytes.size() < kCurrentOperationJournalHeaderBytes ||
        read_u32_le(bytes, 8U) != kCurrentOperationJournalSchemaVersion) {
        result.status = DurableJournalStatus::corrupt;
        result.source = DurableJournalReadSource::none;
        return result;
    }
    const auto kind = read_u32_le(bytes, 12U);
    const auto payload_size = static_cast<std::size_t>(read_u32_le(bytes, 16U));
    if (kind == 0U || payload_size > kMaximumCurrentOperationPayloadBytes ||
        bytes.size() != kCurrentOperationJournalHeaderBytes + payload_size) {
        result.status = DurableJournalStatus::corrupt;
        result.source = DurableJournalReadSource::none;
        return result;
    }
    const auto payload = bytes.subspan(kCurrentOperationJournalHeaderBytes, payload_size);
    if (read_u32_le(bytes, 20U) != crc32(payload)) {
        result.status = DurableJournalStatus::corrupt;
        result.source = DurableJournalReadSource::none;
        return result;
    }

    result.encoding = CurrentOperationJournalEncoding::typed;
    result.kind = static_cast<CurrentOperationKind>(kind);
    result.payload.assign(payload.begin(), payload.end());
    return result;
}

DurableJournalResult CurrentOperationJournalStore::remove() const {
    return durable_.remove();
}

const std::filesystem::path &CurrentOperationJournalStore::primary_path() const noexcept {
    return durable_.primary_path();
}

const std::filesystem::path &CurrentOperationJournalStore::previous_path() const noexcept {
    return durable_.previous_path();
}

const std::filesystem::path &CurrentOperationJournalStore::temporary_path() const noexcept {
    return durable_.temporary_path();
}

} // namespace vove::fileops
