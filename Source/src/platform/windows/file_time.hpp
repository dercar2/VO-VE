#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <limits>

namespace vove::platform::windows_detail {

inline std::int64_t filetime_ticks_to_unix_ns(const std::uint64_t ticks) noexcept {
    constexpr std::uint64_t windowsToUnixEpoch100ns = 116'444'736'000'000'000ULL;
    constexpr std::uint64_t nanosecondsPerTick = 100ULL;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximumMagnitude = static_cast<std::uint64_t>(maximum);
    constexpr auto minimumMagnitude = maximumMagnitude + 1ULL;

    if (ticks >= windowsToUnixEpoch100ns) {
        const auto delta = ticks - windowsToUnixEpoch100ns;
        if (delta > maximumMagnitude / nanosecondsPerTick) {
            return maximum;
        }
        return static_cast<std::int64_t>(delta * nanosecondsPerTick);
    }

    const auto delta = windowsToUnixEpoch100ns - ticks;
    if (delta > minimumMagnitude / nanosecondsPerTick) {
        return minimum;
    }
    const auto nanoseconds = delta * nanosecondsPerTick;
    if (nanoseconds == minimumMagnitude) {
        return minimum;
    }
    return -static_cast<std::int64_t>(nanoseconds);
}

inline std::int64_t filetime_to_unix_ns(const FILETIME &value) noexcept {
    const auto ticks = (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
                       static_cast<std::uint64_t>(value.dwLowDateTime);
    return filetime_ticks_to_unix_ns(ticks);
}

inline std::int64_t filetime_to_unix_ns(const LARGE_INTEGER &value) noexcept {
    const auto ticks =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(value.HighPart)) << 32U) |
        static_cast<std::uint64_t>(value.LowPart);
    return filetime_ticks_to_unix_ns(ticks);
}

} // namespace vove::platform::windows_detail
