#pragma once

#include "vove/cache/artifact_index.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vove::cache {

class SqliteArtifactIndex final : public ArtifactIndex {
  public:
    explicit SqliteArtifactIndex(std::filesystem::path database_path);
    ~SqliteArtifactIndex() override;

    SqliteArtifactIndex(const SqliteArtifactIndex &) = delete;
    SqliteArtifactIndex &operator=(const SqliteArtifactIndex &) = delete;
    SqliteArtifactIndex(SqliteArtifactIndex &&) = delete;
    SqliteArtifactIndex &operator=(SqliteArtifactIndex &&) = delete;

    [[nodiscard]] std::optional<ArtifactIndexRecord> find(std::string_view digest_hex) override;
    bool publish(const ArtifactIndexRecord &record) override;
    TouchResult touch(std::string_view digest_hex, std::int64_t access_unix_ns) override;
    bool erase(std::string_view digest_hex) override;
    [[nodiscard]] std::optional<std::string>
    find_locator(std::string_view locator_digest_hex) override;
    bool publish_locator(std::string_view locator_digest_hex,
                         std::string_view artifact_digest_hex) override;
    bool erase_locator(std::string_view locator_digest_hex) override;

    // Bulk publication keeps large cache-index rebuilds inside one durable transaction.
    bool publish_many(std::span<const ArtifactIndexRecord> records);

    [[nodiscard]] bool ready() const override;
    [[nodiscard]] bool recovered_corruption() const override;
    [[nodiscard]] std::string last_error() const override;
    [[nodiscard]] std::uint64_t count() override;
    [[nodiscard]] std::uint64_t total_bytes() override;
    [[nodiscard]] std::vector<ArtifactIndexRecord> oldest(std::size_t limit) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vove::cache
