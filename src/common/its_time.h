#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace its {
inline constexpr std::int64_t unix_epoch_ms = 1072915200000LL;

// Elapsed TAI milliseconds since 2004-01-01 00:00:00 UTC. At that
// instant TAI-UTC was 32 s: add only insertions AFTER the ITS epoch.
// Effective UTC dates: 2006-01-01, 2009-01-01, 2012-07-01,
// 2015-07-01 and 2017-01-01. Extend this table for future insertions.
inline std::uint64_t from_unix_ms(std::int64_t unix_ms) {
    if (unix_ms < unix_epoch_ms) throw std::invalid_argument("time precedes ITS epoch");
    constexpr std::int64_t insertions[]{1136073600000LL, 1230768000000LL,
        1341100800000LL, 1435708800000LL, 1483228800000LL};
    auto elapsed = static_cast<std::uint64_t>(unix_ms - unix_epoch_ms);
    for (auto effective : insertions) if (unix_ms >= effective) elapsed += 1000;
    return elapsed;
}

inline std::uint64_t now_ms() {
    return from_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
} // namespace its
