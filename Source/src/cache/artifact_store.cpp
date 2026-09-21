#include "vove/cache/artifact_store.hpp"

#include "artifact_codec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vove::cache {
namespace {

constexpr std::array<std::byte, 4> kMagic{std::byte{'V'}, std::byte{'V'}, std::byte{'T'},
                                          std::byte{'1'}};
std::atomic<std::uint64_t> staging_counter{};

[[nodiscard]] consteval std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        auto value = index;
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
        }
        table[index] = value;
    }
    return table;
}

constexpr auto kCrc32Table = make_crc32_table();

void write_u16(std::span<std::byte> output, const std::size_t offset, const std::uint16_t value) {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(std::span<std::byte> output, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
        output[offset + byte] =
            static_cast<std::byte>((value >> static_cast<unsigned>(byte * 8)) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::byte> input,
                                     const std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(input[offset]) |
                                      (std::to_integer<std::uint16_t>(input[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> input,
                                     const std::size_t offset) {
    std::uint32_t result{};
    for (std::size_t byte = 0; byte < 4; ++byte) {
        result |= std::to_integer<std::uint32_t>(input[offset + byte])
                  << static_cast<unsigned>(byte * 8);
    }
    return result;
}

[[nodiscard]] StoreError make_error(const StoreErrorCode code, std::string message) {
    return StoreError{code, std::move(message)};
}

[[nodiscard]] bool valid_utf8(const std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if (first <= 0x7fU) {
            if (first == 0U) {
                return false;
            }
            code_point = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto byte = static_cast<unsigned char>(value[offset + index]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3fU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

[[nodiscard]] bool valid_color_identity(const ArtifactColorIdentity &identity) {
    return identity.source_profile_name.size() <= kMaxSourceProfileNameBytes &&
           identity.source_profile_fingerprint.size() <= kMaxSourceProfileFingerprintBytes &&
           valid_utf8(identity.source_profile_name) &&
           valid_utf8(identity.source_profile_fingerprint);
}

[[nodiscard]] std::optional<ArtifactData> decode_vvt1_impl(const std::span<const std::byte> bytes,
                                                           StoreError &error) {
    if (bytes.size() < kVvt1HeaderBytes || bytes.size() > kMaxArtifactBytes) {
        error = make_error(StoreErrorCode::invalid_artifact, "artifact size is outside limits");
        return std::nullopt;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        error = make_error(StoreErrorCode::invalid_artifact, "artifact magic is invalid");
        return std::nullopt;
    }
    const auto version = read_u16(bytes, 4);
    const auto header_bytes = static_cast<std::size_t>(read_u16(bytes, 6));
    if (version != kVvt1Version || header_bytes < kVvt1HeaderBytes || header_bytes > bytes.size()) {
        error = make_error(StoreErrorCode::invalid_artifact, "artifact version is unsupported");
        return std::nullopt;
    }

    const auto profile_name_bytes = static_cast<std::size_t>(read_u16(bytes, 64));
    const auto profile_fingerprint_bytes = static_cast<std::size_t>(read_u16(bytes, 66));
    const auto expected_header_bytes =
        kVvt1HeaderBytes + profile_name_bytes + profile_fingerprint_bytes;
    if (profile_name_bytes > kMaxSourceProfileNameBytes ||
        profile_fingerprint_bytes > kMaxSourceProfileFingerprintBytes ||
        header_bytes != expected_header_bytes) {
        error = make_error(StoreErrorCode::invalid_artifact,
                           "artifact color identity length is invalid");
        return std::nullopt;
    }

    const auto header = bytes.first(header_bytes);
    auto checked_header = std::vector<std::byte>(header.begin(), header.end());
    const auto expected_header_crc = read_u32(bytes, 28);
    write_u32(checked_header, 28, 0);
    if (crc32(checked_header) != expected_header_crc) {
        error = make_error(StoreErrorCode::invalid_artifact, "artifact header checksum is invalid");
        return std::nullopt;
    }

    ArtifactMetadata metadata;
    metadata.encoding = static_cast<ArtifactEncoding>(read_u16(bytes, 8));
    metadata.source_color_model = static_cast<ColorModel>(std::to_integer<std::uint8_t>(bytes[10]));
    metadata.provenance = static_cast<PreviewProvenance>(std::to_integer<std::uint8_t>(bytes[11]));
    metadata.width = read_u32(bytes, 12);
    metadata.height = read_u32(bytes, 16);
    metadata.payload_bytes = read_u32(bytes, 20);
    metadata.payload_crc32 = read_u32(bytes, 24);
    metadata.page_count = read_u32(bytes, 68);

    if (metadata.encoding != ArtifactEncoding::qoi_rgba8 ||
        metadata.source_color_model > ColorModel::indexed ||
        metadata.provenance > PreviewProvenance::embedded_preview || metadata.width == 0 ||
        metadata.height == 0 || metadata.width > kMaxArtifactDimension ||
        metadata.height > kMaxArtifactDimension || metadata.page_count == 0 ||
        metadata.page_count > kMaxArtifactPageCount) {
        error = make_error(StoreErrorCode::invalid_artifact, "artifact header fields are invalid");
        return std::nullopt;
    }
    if (metadata.payload_bytes > kMaxArtifactBytes - header_bytes ||
        bytes.size() != header_bytes + metadata.payload_bytes) {
        error = make_error(StoreErrorCode::invalid_artifact, "artifact payload length is invalid");
        return std::nullopt;
    }

    CacheDigest cache_key_digest;
    std::copy_n(bytes.begin() + 32, cache_key_digest.bytes.size(), cache_key_digest.bytes.begin());
    const auto *profile_data = reinterpret_cast<const char *>(bytes.data() + kVvt1HeaderBytes);
    ArtifactColorIdentity color_identity{
        .source_profile_name = std::string(profile_data, profile_name_bytes),
        .source_profile_fingerprint =
            std::string(profile_data + profile_name_bytes, profile_fingerprint_bytes),
    };
    if (!valid_color_identity(color_identity)) {
        error = make_error(StoreErrorCode::invalid_artifact,
                           "artifact color identity is invalid UTF-8");
        return std::nullopt;
    }

    const auto payload = bytes.subspan(header_bytes, metadata.payload_bytes);
    if (crc32(payload) != metadata.payload_crc32) {
        error =
            make_error(StoreErrorCode::invalid_artifact, "artifact payload checksum is invalid");
        return std::nullopt;
    }
    return ArtifactData{.metadata = metadata,
                        .cache_key_digest = cache_key_digest,
                        .color_identity = std::move(color_identity),
                        .payload = std::vector<std::byte>(payload.begin(), payload.end())};
}

[[nodiscard]] std::FILE *open_exclusive(const std::filesystem::path &path) {
#ifdef _WIN32
    int descriptor = -1;
    const auto result =
        _wsopen_s(&descriptor, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW,
                  _S_IREAD | _S_IWRITE);
    if (result != 0 || descriptor < 0) {
        return nullptr;
    }
    auto *file = _wfdopen(descriptor, L"wb");
#else
    const auto descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return nullptr;
    }
    auto *file = ::fdopen(descriptor, "wb");
#endif
    if (file == nullptr) {
#ifdef _WIN32
        _close(descriptor);
#else
        ::close(descriptor);
#endif
    }
    return file;
}

[[nodiscard]] bool flush_stream(std::FILE *file) {
    // Cache artifacts are disposable and self-validating. A process-safe flush followed by an
    // atomic rename prevents partial publication; power-loss durability would add two media
    // barriers per thumbnail and is deliberately not part of the cache contract.
    return std::fflush(file) == 0;
}

struct AtomicReplaceResult {
    bool replaced{};
    StoreError error;
};

[[nodiscard]] AtomicReplaceResult atomic_replace(const std::filesystem::path &source,
                                                 const std::filesystem::path &destination) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        return {.replaced = false,
                .error = make_error(StoreErrorCode::io_error,
                                    "failed to atomically replace artifact: Windows error " +
                                        std::to_string(GetLastError()))};
    }
    return {.replaced = true, .error = {}};
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        const auto rename_error = errno;
        return {.replaced = false,
                .error = make_error(StoreErrorCode::io_error,
                                    "failed to atomically replace artifact: " +
                                        std::string(std::strerror(rename_error)))};
    }
    return {.replaced = true, .error = {}};
#endif
}

[[nodiscard]] bool is_staging_name(const std::filesystem::path &path) {
    const auto name = path.filename().string();
    return name.find(".vvt.tmp-") != std::string::npos;
}

[[nodiscard]] std::filesystem::path make_staging_path(const std::filesystem::path &destination) {
    const auto count = staging_counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.parent_path() / (destination.filename().string() + ".tmp-" +
                                        std::to_string(ticks) + "-" + std::to_string(count));
}

[[nodiscard]] bool valid_digest(const std::string_view digest) noexcept {
    return digest.size() == 64U && std::ranges::all_of(digest, [](const char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

} // namespace

std::optional<ArtifactData> detail::decode_vvt1(const std::span<const std::byte> bytes,
                                                StoreError &error) {
    return decode_vvt1_impl(bytes, error);
}

StagingWrite::StagingWrite(std::FILE *file, std::filesystem::path path,
                           std::filesystem::path destination) noexcept
    : file_(file), path_(std::move(path)), destination_(std::move(destination)) {}

StagingWrite::~StagingWrite() {
    close_and_remove();
}

StagingWrite::StagingWrite(StagingWrite &&other) noexcept
    : file_(std::exchange(other.file_, nullptr)), path_(std::move(other.path_)),
      destination_(std::move(other.destination_)), bytes_written_(other.bytes_written_),
      error_(std::move(other.error_)) {
    other.path_.clear();
    other.destination_.clear();
    other.bytes_written_ = 0;
}

StagingWrite &StagingWrite::operator=(StagingWrite &&other) noexcept {
    if (this != &other) {
        close_and_remove();
        file_ = std::exchange(other.file_, nullptr);
        path_ = std::move(other.path_);
        destination_ = std::move(other.destination_);
        bytes_written_ = other.bytes_written_;
        error_ = std::move(other.error_);
        other.path_.clear();
        other.destination_.clear();
        other.bytes_written_ = 0;
    }
    return *this;
}

bool StagingWrite::write(const std::span<const std::byte> bytes) {
    if (file_ == nullptr || error_) {
        return false;
    }
    if (bytes.size() > kMaxArtifactBytes - std::min(bytes_written_, kMaxArtifactBytes)) {
        error_ = make_error(StoreErrorCode::artifact_too_large,
                            "staging write exceeds the artifact size limit");
        return false;
    }
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
        error_ = make_error(StoreErrorCode::io_error, "failed to write staging artifact");
        return false;
    }
    bytes_written_ += bytes.size();
    return true;
}

bool StagingWrite::valid() const noexcept {
    return file_ != nullptr && !error_;
}

std::size_t StagingWrite::bytes_written() const noexcept {
    return bytes_written_;
}

std::intptr_t StagingWrite::native_object() const noexcept {
    if (file_ == nullptr) {
        return static_cast<std::intptr_t>(-1);
    }
#ifdef _WIN32
    return static_cast<std::intptr_t>(_get_osfhandle(_fileno(file_)));
#else
    return static_cast<std::intptr_t>(::fileno(file_));
#endif
}

const std::filesystem::path &StagingWrite::path() const noexcept {
    return path_;
}

const StoreError &StagingWrite::error() const noexcept {
    return error_;
}

bool StagingWrite::flush_and_close() {
    if (file_ == nullptr) {
        if (!error_) {
            error_ = make_error(StoreErrorCode::io_error, "staging artifact is not open");
        }
        return false;
    }
    const auto flushed = flush_stream(file_);
    const auto closed = std::fclose(file_) == 0;
    file_ = nullptr;
    if (!flushed || !closed) {
        error_ = make_error(StoreErrorCode::io_error, "failed to flush staging artifact");
        return false;
    }
    return !error_;
}

void StagingWrite::close_and_remove() noexcept {
    if (file_ != nullptr) {
        static_cast<void>(std::fclose(file_));
        file_ = nullptr;
    }
    if (!path_.empty()) {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }
}

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t value = 0xffffffffU;
    for (const auto byte : bytes) {
        const auto table_index = (value ^ std::to_integer<std::uint8_t>(byte)) & 0xffU;
        value = (value >> 8U) ^ kCrc32Table[table_index];
    }
    return ~value;
}

EncodedArtifact encode_vvt1(ArtifactMetadata metadata, const std::span<const std::byte> payload,
                            const CacheDigest &cache_key_digest,
                            const ArtifactColorIdentity &color_identity) {
    EncodedArtifact result;
    if (metadata.encoding != ArtifactEncoding::qoi_rgba8 || metadata.width == 0 ||
        metadata.height == 0 || metadata.width > kMaxArtifactDimension ||
        metadata.height > kMaxArtifactDimension || metadata.page_count == 0 ||
        metadata.page_count > kMaxArtifactPageCount ||
        metadata.source_color_model > ColorModel::indexed ||
        metadata.provenance > PreviewProvenance::embedded_preview) {
        result.error = make_error(StoreErrorCode::invalid_argument,
                                  "artifact dimensions or encoding are invalid");
        return result;
    }
    if (!valid_color_identity(color_identity)) {
        result.error = make_error(StoreErrorCode::invalid_argument,
                                  "artifact color identity is invalid or exceeds its limit");
        return result;
    }
    const auto header_bytes = kVvt1HeaderBytes + color_identity.source_profile_name.size() +
                              color_identity.source_profile_fingerprint.size();
    if (header_bytes > std::numeric_limits<std::uint16_t>::max() ||
        payload.size() > kMaxArtifactBytes - header_bytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        result.error = make_error(StoreErrorCode::artifact_too_large,
                                  "artifact payload exceeds the size limit");
        return result;
    }
    metadata.payload_bytes = static_cast<std::uint32_t>(payload.size());
    metadata.payload_crc32 = crc32(payload);
    result.bytes.resize(header_bytes + payload.size());
    std::copy(kMagic.begin(), kMagic.end(), result.bytes.begin());
    auto output = std::span<std::byte>(result.bytes);
    write_u16(output, 4, kVvt1Version);
    write_u16(output, 6, static_cast<std::uint16_t>(header_bytes));
    write_u16(output, 8, static_cast<std::uint16_t>(metadata.encoding));
    output[10] = static_cast<std::byte>(metadata.source_color_model);
    output[11] = static_cast<std::byte>(metadata.provenance);
    write_u32(output, 12, metadata.width);
    write_u32(output, 16, metadata.height);
    write_u32(output, 20, metadata.payload_bytes);
    write_u32(output, 24, metadata.payload_crc32);
    write_u32(output, 28, 0);
    std::copy(cache_key_digest.bytes.begin(), cache_key_digest.bytes.end(),
              result.bytes.begin() + 32);
    write_u16(output, 64, static_cast<std::uint16_t>(color_identity.source_profile_name.size()));
    write_u16(output, 66,
              static_cast<std::uint16_t>(color_identity.source_profile_fingerprint.size()));
    write_u32(output, 68, metadata.page_count);
    auto destination = result.bytes.begin() + kVvt1HeaderBytes;
    const auto profile_name_bytes = std::as_bytes(std::span<const char>(
        color_identity.source_profile_name.data(), color_identity.source_profile_name.size()));
    destination = std::copy(profile_name_bytes.begin(), profile_name_bytes.end(), destination);
    const auto profile_fingerprint_bytes =
        std::as_bytes(std::span<const char>(color_identity.source_profile_fingerprint.data(),
                                            color_identity.source_profile_fingerprint.size()));
    destination =
        std::copy(profile_fingerprint_bytes.begin(), profile_fingerprint_bytes.end(), destination);
    std::copy(payload.begin(), payload.end(), destination);
    write_u32(output, 28, crc32(std::span<const std::byte>(result.bytes).first(header_bytes)));
    result.metadata = metadata;
    result.cache_key_digest = cache_key_digest;
    result.color_identity = color_identity;
    return result;
}

ArtifactStore::ArtifactStore(std::filesystem::path root) : root_(std::move(root)) {}

const std::filesystem::path &ArtifactStore::root() const noexcept {
    return root_;
}

std::filesystem::path ArtifactStore::artifact_path(const CacheKey &key) const {
    const auto digest = hash_cache_key(key).hex();
    return root_ / digest.substr(0, 2) / digest.substr(2, 2) / (digest + ".vvt");
}

std::optional<std::filesystem::path>
ArtifactStore::artifact_path_from_digest(const std::string_view digest_hex) const {
    if (!valid_digest(digest_hex)) {
        return std::nullopt;
    }
    return root_ / std::string(digest_hex.substr(0, 2)) / std::string(digest_hex.substr(2, 2)) /
           (std::string(digest_hex) + ".vvt");
}

BeginWriteResult ArtifactStore::begin_write(const CacheKey &key) const {
    BeginWriteResult result;
    const auto destination = artifact_path(key);
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        result.error = make_error(StoreErrorCode::io_error,
                                  "failed to create artifact shard: " + error.message());
        return result;
    }
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        auto path = make_staging_path(destination);
        if (auto *file = open_exclusive(path)) {
            result.staging = StagingWrite(file, std::move(path), destination);
            return result;
        }
        if (errno != EEXIST) {
            result.error =
                make_error(StoreErrorCode::io_error, "failed to create staging artifact: " +
                                                         std::string(std::strerror(errno)));
            return result;
        }
    }
    result.error = make_error(StoreErrorCode::io_error, "failed to allocate a unique staging name");
    return result;
}

CommitResult ArtifactStore::commit(StagingWrite &&staging) const {
    CommitResult result;
    result.path = staging.destination_;
    if (staging.error_) {
        result.error = staging.error_;
        staging.close_and_remove();
        return result;
    }
    if (!staging.flush_and_close()) {
        result.error = staging.error_;
        staging.close_and_remove();
        return result;
    }

    std::error_code file_error;
    const auto size = std::filesystem::file_size(staging.path_, file_error);
    if (file_error || size > kMaxArtifactBytes || size < kVvt1HeaderBytes) {
        result.error =
            make_error(StoreErrorCode::invalid_artifact, "staging artifact has an invalid size");
        staging.close_and_remove();
        return result;
    }
    std::ifstream input(staging.path_, std::ios::binary);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        result.error = make_error(StoreErrorCode::io_error, "failed to read staging artifact");
        staging.close_and_remove();
        return result;
    }
    input.close();
    StoreError validation_error;
    const auto artifact = detail::decode_vvt1(bytes, validation_error);
    if (!artifact) {
        result.error = std::move(validation_error);
        staging.close_and_remove();
        return result;
    }
    if (staging.destination_.filename() != artifact->cache_key_digest.hex() + ".vvt") {
        result.error = make_error(StoreErrorCode::invalid_artifact,
                                  "artifact cache-key digest does not match its destination");
        staging.close_and_remove();
        return result;
    }
    const auto replacement = atomic_replace(staging.path_, staging.destination_);
    if (replacement.replaced) {
        staging.path_.clear();
    }
    if (replacement.error) {
        result.error = replacement.error;
        staging.close_and_remove();
        return result;
    }
    result.committed = true;
    result.metadata = artifact->metadata;
    result.cache_key_digest = artifact->cache_key_digest;
    result.color_identity = artifact->color_identity;
    return result;
}

LookupResult ArtifactStore::lookup(const CacheKey &key, const bool remove_invalid) const {
    return lookup_digest(hash_cache_key(key).hex(), remove_invalid);
}

LookupResult ArtifactStore::lookup_digest(const std::string_view digest_hex,
                                          const bool remove_invalid) const {
    LookupResult result;
    const auto path = artifact_path_from_digest(digest_hex);
    if (!path) {
        result.error = make_error(StoreErrorCode::invalid_argument,
                                  "artifact digest is not canonical lowercase SHA-256");
        return result;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(*path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return result;
    }
    if (error) {
        result.error =
            make_error(StoreErrorCode::io_error, "failed to inspect artifact: " + error.message());
        return result;
    }
    if (size < kVvt1HeaderBytes || size > kMaxArtifactBytes) {
        if (remove_invalid) {
            const auto removal = remove_digest_checked(digest_hex);
            result.invalid_artifact_removed = removal.status == RemoveStatus::removed;
            result.error = removal.error;
        }
        return result;
    }

    std::ifstream input(*path, std::ios::binary);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        result.error = make_error(StoreErrorCode::io_error, "failed to read artifact");
        return result;
    }
    input.close();

    StoreError validation_error;
    auto artifact = detail::decode_vvt1(bytes, validation_error);
    if (!artifact) {
        if (remove_invalid) {
            const auto removal = remove_digest_checked(digest_hex);
            result.invalid_artifact_removed = removal.status == RemoveStatus::removed;
            result.error = removal.error;
        }
        return result;
    }
    if (artifact->cache_key_digest.hex() != digest_hex) {
        if (remove_invalid) {
            const auto removal = remove_digest_checked(digest_hex);
            result.invalid_artifact_removed = removal.status == RemoveStatus::removed;
            result.error = removal.error;
        }
        return result;
    }
    result.status = LookupStatus::hit;
    result.artifact = std::move(artifact);
    return result;
}

bool ArtifactStore::remove(const CacheKey &key) const {
    return remove_digest_checked(hash_cache_key(key).hex()).status == RemoveStatus::removed;
}

bool ArtifactStore::remove_digest(const std::string_view digest_hex) const {
    return remove_digest_checked(digest_hex).status == RemoveStatus::removed;
}

RemoveResult ArtifactStore::remove_digest_checked(const std::string_view digest_hex) const {
    const auto path = artifact_path_from_digest(digest_hex);
    if (!path) {
        return {.status = RemoveStatus::error,
                .error = make_error(StoreErrorCode::invalid_argument,
                                    "artifact digest is not canonical lowercase SHA-256")};
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(*path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {.status = RemoveStatus::not_found, .error = {}};
    }
    if (error) {
        return {.status = RemoveStatus::error,
                .error =
                    make_error(StoreErrorCode::io_error,
                               "failed to inspect artifact before removal: " + error.message())};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {.status = RemoveStatus::error,
                .error = make_error(StoreErrorCode::invalid_artifact,
                                    "refusing to remove a non-regular artifact path")};
    }
    if (!std::filesystem::remove(*path, error) || error) {
        return {.status = RemoveStatus::error,
                .error = make_error(StoreErrorCode::io_error,
                                    "failed to remove artifact: " + error.message())};
    }
    return {.status = RemoveStatus::removed, .error = {}};
}

DigestScanResult ArtifactStore::scan_digest_artifacts(const std::size_t maximum_inspected) const {
    DigestScanResult result;
    if (maximum_inspected == 0) {
        result.error = make_error(StoreErrorCode::invalid_argument,
                                  "digest artifact scan limit must be non-zero");
        return result;
    }
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) {
        if (error) {
            result.error = make_error(StoreErrorCode::io_error,
                                      "failed to inspect cache root: " + error.message());
        }
        return result;
    }

    std::filesystem::recursive_directory_iterator iterator(
        root_, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end && result.inspected < maximum_inspected) {
        const auto entry = *iterator;
        ++result.inspected;
        if (entry.is_regular_file(error) && !error) {
            const auto filename = entry.path().filename().string();
            if (filename.size() == 68U && filename.ends_with(".vvt")) {
                const auto digest = filename.substr(0, 64);
                const auto expected = artifact_path_from_digest(digest);
                if (expected && expected->lexically_normal() == entry.path().lexically_normal()) {
                    const auto bytes = entry.file_size(error);
                    if (!error) {
                        result.artifacts.push_back(
                            {.digest_hex = digest, .bytes = static_cast<std::uint64_t>(bytes)});
                    }
                }
            }
        }
        if (!error) {
            iterator.increment(error);
        }
    }
    if (error) {
        result.error =
            make_error(StoreErrorCode::io_error, "digest artifact scan failed: " + error.message());
        return result;
    }
    result.inspection_limit_reached = iterator != end;
    return result;
}

SweepResult ArtifactStore::sweep_orphans(const std::size_t maximum_inspected,
                                         const std::size_t maximum_removed,
                                         const std::chrono::seconds minimum_age) const {
    SweepResult result;
    if (maximum_inspected == 0 || maximum_removed == 0) {
        return result;
    }
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) {
        if (error) {
            result.error = make_error(StoreErrorCode::io_error,
                                      "failed to inspect cache root: " + error.message());
        }
        return result;
    }

    const auto cutoff = std::filesystem::file_time_type::clock::now() - minimum_age;
    std::filesystem::recursive_directory_iterator iterator(
        root_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end && result.inspected < maximum_inspected &&
           result.removed < maximum_removed) {
        const auto entry = *iterator;
        ++result.inspected;
        if (entry.is_regular_file(error) && !error && is_staging_name(entry.path())) {
            const auto modified = entry.last_write_time(error);
            if (!error && modified <= cutoff) {
                if (std::filesystem::remove(entry.path(), error) && !error) {
                    ++result.removed;
                }
            }
        }
        if (error) {
            break;
        }
        iterator.increment(error);
    }
    if (error) {
        result.error =
            make_error(StoreErrorCode::io_error, "orphan sweep failed: " + error.message());
    }
    return result;
}

} // namespace vove::cache
