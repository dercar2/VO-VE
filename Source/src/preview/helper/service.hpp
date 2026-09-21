#pragma once

#include "vove/cache/artifact_index.hpp"
#include "vove/preview/thumbnail_pipeline.hpp"

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace vove::preview::helper {

struct ServiceOptions {
    std::string server_name_utf8;
    std::string auth_token_utf8;
    std::string build_id_utf8;
    std::filesystem::path cache_root;
    cache::DiskCacheLimits disk_cache_limits;
    std::function<void()> client_disconnected;
    bool abstract_namespace{};
    std::uint64_t expected_peer_process_id{};
};

class Service final : public QObject {
  public:
    Service(ServiceOptions options, std::unique_ptr<cache::ArtifactIndex> index,
            ThumbnailRenderer renderer, ThumbnailSourceResolver source_resolver = {},
            ThumbnailColorPolicyResolver color_policy_resolver = {}, QObject *parent = nullptr);
    ~Service() override;

    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;
    Service(Service &&) = delete;
    Service &operator=(Service &&) = delete;

    [[nodiscard]] bool listen();
    void close();
    [[nodiscard]] bool is_listening() const noexcept;
    [[nodiscard]] std::string last_error() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vove::preview::helper
