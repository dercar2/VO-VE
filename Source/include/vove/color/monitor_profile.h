#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace vove::color {

// Returns the active primary-display ICC profile, bounded by kMaximumIccProfileBytes.
// An empty vector means that the operating system has no usable profile configured.
[[nodiscard]] std::vector<std::byte> load_primary_monitor_icc();

// Loads the profile assigned to a concrete display device (for example QScreen::name()).
// An empty name falls back to the primary display.
[[nodiscard]] std::vector<std::byte> load_monitor_icc(std::string_view display_name_utf8);

} // namespace vove::color
