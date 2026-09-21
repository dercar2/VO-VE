#pragma once

#include "vove/cache/cache_key.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vove::cache {

enum class StoreErrorCode : std::uint8_t {
    none,
    invalid_argument,
    io_error,
    artifact_too_large,
    invalid_artifact,
};

struct StoreError {
    StoreErrorCode code{StoreErrorCode::none};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != StoreErrorCode::none;
    }
};

class StagingWrite {
  public:
    StagingWrite() = default;
    ~StagingWrite();
    StagingWrite(StagingWrite &&other) noexcept;
    StagingWrite &operator=(StagingWrite &&other) noexcept;

    StagingWrite(const StagingWrite &) = delete;
    StagingWrite &operator=(const StagingWrite &) = delete;

    [[nodiscard]] bool write(std::span<const std::byte> bytes);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t bytes_written() const noexcept;
    [[nodiscard]] std::intptr_t native_object() const noexcept;
    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] const StoreError &error() const noexcept;

  private:
    friend class ArtifactStore;

    StagingWrite(std::FILE *file, std::filesystem::path path,
                 std::filesystem::path destination) noexcept;
    [[nodiscard]] bool flush_and_close();
    void close_and_remove() noexcept;

    std::FILE *file_{};
    std::filesystem::path path_;
    std::filesystem::path destination_;
    std::size_t bytes_written_{};
    StoreError error_;
};

struct BeginWriteResult {
    std::optional<StagingWrite> staging;
    StoreError error;
};

struct CommitResult {
    bool committed{};
    std::filesystem::path path;
    ArtifactMetadata metadata;
    CacheDigest cache_key_digest;
    ArtifactColorIdentity color_identity;
    StoreError error;
};

enum class LookupStatus : std::uint8_t {
    hit,
    miss,
};

struct ArtifactData {
    ArtifactMetadata metadata;
    CacheDigest cache_key_digest;
    ArtifactColorIdentity color_identity;
    std::vector<std::byte> payload;
};

struct LookupResult {
    LookupStatus status{LookupStatus::miss};
    std::optional<ArtifactData> artifact;
    bool invalid_artifact_removed{};
    StoreError error;
};

struct SweepResult {
    std::size_t inspected{};
    std::size_t removed{};
    StoreError error;
};

enum class RemoveStatus : std::uint8_t {
    removed,
    not_found,
    error,
};

struct RemoveResult {
    RemoveStatus status{RemoveStatus::not_found};
    StoreError error;

    [[nodiscard]] bool ok() const noexcept {
        return status != RemoveStatus::error;
    }
};

struct DigestArtifact {
    std::string digest_hex;
    std::uint64_t bytes{};
};

struct DigestScanResult {
    std::vector<DigestArtifact> artifacts;
    std::size_t inspected{};
    bool inspection_limit_reached{};
    StoreError error;
};

struct EncodedArtifact {
    std::vector<std::byte> bytes;
    ArtifactMetadata metadata;
    CacheDigest cache_key_digest;
    ArtifactColorIdentity color_identity;
    StoreError error;
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] EncodedArtifact encode_vvt1(ArtifactMetadata metadata,
                                          std::span<const std::byte> payload,
                                          const CacheDigest &cache_key_digest,
                                          const ArtifactColorIdentity &color_identity);

class ArtifactStore {
  public:
    explicit ArtifactStore(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path &root() const noexcept;
    [[nodiscard]] std::filesystem::path artifact_path(const CacheKey &key) const;
    [[nodiscard]] std::optional<std::filesystem::path>
    artifact_path_from_digest(std::string_view digest_hex) const;
    [[nodiscard]] BeginWriteResult begin_write(const CacheKey &key) const;
    [[nodiscard]] CommitResult commit(StagingWrite &&staging) const;
    [[nodiscard]] LookupResult lookup(const CacheKey &key, bool remove_invalid = true) const;
    [[nodiscard]] LookupResult lookup_digest(std::string_view digest_hex,
                                             bool remove_invalid = true) const;
    [[nodiscard]] bool remove(const CacheKey &key) const;
    [[nodiscard]] bool remove_digest(std::string_view digest_hex) const;
    [[nodiscard]] RemoveResult remove_digest_checked(std::string_view digest_hex) const;
    [[nodiscard]] DigestScanResult scan_digest_artifacts(std::size_t maximum_inspected) const;
    [[nodiscard]] SweepResult sweep_orphans(std::size_t maximum_inspected,
                                            std::size_t maximum_removed,
                                            std::chrono::seconds minimum_age) const;

  private:
    std::filesystem::path root_;
};

} // namespace vove::cache
