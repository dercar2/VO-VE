#pragma once

#include <filesystem>

namespace vove::fileops::detail {

using TrashCatalogAfterRootPinHook = void (*)(const std::filesystem::path &);

void set_trash_catalog_after_root_pin_hook(TrashCatalogAfterRootPinHook hook) noexcept;

} // namespace vove::fileops::detail
