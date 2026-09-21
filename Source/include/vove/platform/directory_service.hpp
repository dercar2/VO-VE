#pragma once

#include "vove/catalog/catalog_types.hpp"

#include <chrono>
#include <filesystem>
#include <memory>

namespace vove::platform {

namespace detail {
struct DirectoryServiceState;
}

struct DirectoryServiceOptions {
    std::filesystem::path helper_path;
    std::chrono::milliseconds inactivity_timeout{catalog::kCatalogInactivityTimeout};
};

// Each enumeration runs in a separate helper process. The UI only polls a bounded result queue;
// timeout and cancellation terminate the helper without waiting on an SMB call in the UI process.
class DirectoryService final : public catalog::DirectorySource {
  public:
    explicit DirectoryService(DirectoryServiceOptions options);
    DirectoryService();
    ~DirectoryService() override;

    DirectoryService(const DirectoryService &) = delete;
    DirectoryService &operator=(const DirectoryService &) = delete;
    DirectoryService(DirectoryService &&) = delete;
    DirectoryService &operator=(DirectoryService &&) = delete;

    void submit(catalog::CatalogRequest request) override;
    void cancel(catalog::RequestGeneration generation) override;
    [[nodiscard]] std::optional<catalog::CatalogBatch> poll() override;
    [[nodiscard]] bool idle() const noexcept;

  private:
    std::shared_ptr<detail::DirectoryServiceState> state_;
};

} // namespace vove::platform
