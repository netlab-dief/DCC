#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace dcc {
// EN 302 571 V2.1.1, 4.2.10.2. These bounds apply independently of
// the reactive/adaptive controller. Durations here use estimated PHY airtime.
inline constexpr std::uint64_t maximum_airtime_us = 4000;
inline constexpr std::uint64_t one_second_airtime_us = 30000;

inline std::chrono::microseconds regulatory_idle_time(std::uint64_t ton_us, double cbr) {
    double idle_us = 25000;
    if (cbr >= 0.62) {
        idle_us = std::max(idle_us, std::min(1000000.0,
            ton_us * (4000.0 * (cbr - 0.62) / cbr - 1.0)));
    }
    return std::chrono::microseconds(static_cast<std::int64_t>(std::ceil(idle_us)));
}

inline std::chrono::microseconds admission_interval(std::uint64_t ton_us,
    double delta, std::chrono::microseconds minimum, double cbr) {
    // A delay measured from driver enqueue must include Ton before Toff.
    return std::max({minimum, std::chrono::microseconds(static_cast<std::int64_t>(
        std::ceil(ton_us / delta))), std::chrono::microseconds(ton_us) + regulatory_idle_time(ton_us, cbr)});
}
} // namespace dcc
