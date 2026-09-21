#pragma once

#include "vove/cache/cache_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vove::cache {

struct ArtifactIndexRecord {
    std::string digest_hex;
    SourceStamp source;
    ArtifactMetadata metadata;
    std::string source_profile_name;
    std::string source_profile_fingerprint;
    std::string relative_artifact_path_utf8;
    std::uint64_t artifact_bytes{};
    std::int64_t last_access_unix_ns{};
};

enum class TouchResult : std::uint8_t {
    updated,
    missing,
    error,
};

class ArtifactIndex {
  public:
    virtual ~ArtifactIndex() = default;

    [[nodiscard]] virtual std::optional<ArtifactIndexRecord> find(std::string_view digest_hex) = 0;
    virtual bool publish(const ArtifactIndexRecord &record) = 0;
    virtual TouchResult touch(std::string_view digest_hex, std::int64_t access_unix_ns) = 0;
    virtual bool erase(std::string_view digest_hex) = 0;
    [[nodiscard]] virtual std::optional<std::string>
    find_locator(std::string_view locator_digest_hex) = 0;
    virtual bool publish_locator(std::string_view locator_digest_hex,
                                 std::string_view artifact_digest_hex) = 0;
    virtual bool erase_locator(std::string_view locator_digest_hex) = 0;
    [[nodiscard]] virtual bool ready() const = 0;
    [[nodiscard]] virtual bool recovered_corruption() const {
        return false;
    }
    [[nodiscard]] virtual std::string last_error() const = 0;
    [[nodiscard]] virtual std::uint64_t count() = 0;
    [[nodiscard]] virtual std::uint64_t total_bytes() = 0;
    [[nodiscard]] virtual std::vector<ArtifactIndexRecord> oldest(std::size_t limit) = 0;
};

} // namespace vove::cache
