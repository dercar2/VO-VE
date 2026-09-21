#pragma once

#include "vove/cache/artifact_store.hpp"

namespace vove::cache::detail {

[[nodiscard]] std::optional<ArtifactData> decode_vvt1(std::span<const std::byte> bytes,
                                                      StoreError &error);

} // namespace vove::cache::detail
