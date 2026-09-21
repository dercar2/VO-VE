#pragma once

#include "vove/catalog/catalog_types.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace vove::catalog {

enum class CatalogSessionState : std::uint8_t {
    loading,
    waiting_retry,
    ready,
    network_disconnected,
    timed_out,
    authentication_required,
    permission_denied,
    unavailable,
};

struct CatalogSessionUpdate {
    CatalogSessionState state{CatalogSessionState::loading};
    std::size_t entries_received{};
    bool model_changed{false};
    bool truncated{false};
    CatalogError error;
    std::optional<std::chrono::milliseconds> retry_in;
    std::uint32_t directories_visited{};
};

class CatalogSession {
  public:
    CatalogSession(DirectorySource &source, DirectorySink &sink);
    ~CatalogSession();

    CatalogSession(const CatalogSession &) = delete;
    CatalogSession &operator=(const CatalogSession &) = delete;

    void open(std::filesystem::path path, std::size_t maximum_entries = 100'000,
              bool recursive = false, bool preserve_visible_entries = false);
    void refresh();
    void suspend();

    [[nodiscard]] CatalogSessionUpdate
    poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    [[nodiscard]] CatalogSessionState state() const noexcept;
    [[nodiscard]] const CatalogError &last_error() const noexcept;
    [[nodiscard]] const std::filesystem::path &path() const noexcept;

  private:
    void begin_request(bool preserve_visible_entries);
    void finish_sink_if_open();
    void abort_sink_if_open();
    [[nodiscard]] CatalogSessionUpdate make_update() const;
    [[nodiscard]] bool should_retry(const CatalogError &error) const noexcept;

    DirectorySource &source_;
    DirectorySink &sink_;
    std::filesystem::path path_;
    std::size_t maximumEntries_{100'000};
    bool recursive_{false};
    std::uint32_t directoriesVisited_{};
    RequestGeneration nextGeneration_{};
    RequestGeneration activeGeneration_{};
    CatalogSessionState state_{CatalogSessionState::unavailable};
    CatalogError lastError_;
    std::size_t retryIndex_{};
    std::chrono::steady_clock::time_point retryAt_{};
    bool stageUntilSuccess_{false};
    std::vector<core::DirectoryEntry> stagedEntries_;
    bool sinkOpen_{false};
    bool truncated_{false};
};

} // namespace vove::catalog
