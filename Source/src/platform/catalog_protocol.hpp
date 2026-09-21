#pragma once

#include "vove/catalog/catalog_types.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace vove::platform::detail {

inline constexpr std::uint32_t kCatalogProtocolVersion = 6;
inline constexpr std::size_t kCatalogMaximumFrameBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kCatalogFrameHeaderBytes = sizeof(std::uint32_t);

[[nodiscard]] std::vector<std::byte> encode_request_payload(const catalog::CatalogRequest &request);
[[nodiscard]] bool decode_request_payload(std::span<const std::byte> payload,
                                          catalog::CatalogRequest &request, std::string &error);

[[nodiscard]] std::vector<std::byte> encode_batch_payload(const catalog::CatalogBatch &batch);
[[nodiscard]] bool decode_batch_payload(std::span<const std::byte> payload,
                                        catalog::CatalogBatch &batch, std::string &error);

[[nodiscard]] std::vector<std::byte> frame_payload(std::span<const std::byte> payload);
[[nodiscard]] bool decode_frame_size(std::span<const std::byte> header, std::size_t &size,
                                     std::string &error);

[[nodiscard]] bool read_stream_frame(std::istream &input, std::vector<std::byte> &payload,
                                     std::string &error,
                                     std::size_t maximum_bytes = kCatalogMaximumFrameBytes);
[[nodiscard]] bool write_stream_frame(std::ostream &output, std::span<const std::byte> payload);

} // namespace vove::platform::detail
