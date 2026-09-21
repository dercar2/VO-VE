#pragma once

#include "vove/catalog/catalog_types.hpp"
#include "catalog_protocol.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vove::platform::detail {

class CatalogProcess {
  public:
    virtual ~CatalogProcess() = default;

    CatalogProcess(const CatalogProcess &) = delete;
    CatalogProcess &operator=(const CatalogProcess &) = delete;

    [[nodiscard]] virtual bool write_frame(std::span<const std::byte> payload,
                                           std::string &error) = 0;
    virtual void close_input() noexcept = 0;
    [[nodiscard]] virtual bool
    read_frame(std::vector<std::byte> &payload, std::string &error,
               std::size_t maximum_bytes = kCatalogMaximumFrameBytes) = 0;
    virtual void terminate() noexcept = 0;
    [[nodiscard]] virtual int wait(std::string &error) noexcept = 0;

  protected:
    CatalogProcess() = default;
};

[[nodiscard]] std::filesystem::path default_catalog_helper_path();
[[nodiscard]] std::shared_ptr<CatalogProcess>
start_catalog_process(const std::filesystem::path &helper_path, catalog::CatalogError &error);

} // namespace vove::platform::detail
