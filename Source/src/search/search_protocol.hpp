#pragma once

#include "vove/search/global_search.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace vove::search::protocol {

inline constexpr std::uint32_t kSearchProtocolVersion = 2;
inline constexpr std::size_t kSearchMaximumFrameBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kSearchFrameHeaderBytes = sizeof(std::uint32_t);

[[nodiscard]] std::vector<std::byte> encode_request_payload(const SearchRequest &request);
[[nodiscard]] bool decode_request_payload(std::span<const std::byte> payload,
                                          SearchRequest &request, std::string &error);

[[nodiscard]] std::vector<std::byte> encode_batch_payload(const SearchBatch &batch);
[[nodiscard]] bool decode_batch_payload(std::span<const std::byte> payload, SearchBatch &batch,
                                        std::string &error);

[[nodiscard]] std::vector<std::byte> frame_payload(std::span<const std::byte> payload);
[[nodiscard]] bool decode_frame_size(std::span<const std::byte> header, std::size_t &size,
                                     std::string &error);
[[nodiscard]] bool read_stream_frame(std::istream &input, std::vector<std::byte> &payload,
                                     std::string &error);
[[nodiscard]] bool write_stream_frame(std::ostream &output, std::span<const std::byte> payload);

} // namespace vove::search::protocol
