#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace cam {
// ETSI TS 102 636-4-2 V1.4.1, table 4: TC2 -> UP3 -> AC_BE.
inline constexpr std::uint8_t default_priority = 3;
inline constexpr std::uint8_t traffic_class = 2;
struct Position {
    int latitude = 446292708;
    int longitude = 109479523;
    int speed = 0;       // CAM units: 0.01 m/s
    int heading = 3601;  // CAM units: 0.1 degrees; unavailable by default
    std::optional<std::uint64_t> sample_timestamp_ms; // GPS position measurement time, ITS epoch.
};

struct PathSample {
    Position position;
    std::uint64_t timestamp_ms;
};
struct LowFrequencyData {
    std::uint8_t exterior_lights = 0; // CDD: all bits zero when unavailable.
    std::vector<PathSample> history; // Newest first; at most 23 points encoded.
};

// Accepts a single RMC sentence with a valid fix and checksum.
bool parse_rmc(const std::string& sentence, Position& position);
std::uint64_t timestamp_ms(); // Milliseconds since 2004-01-01 UTC.
std::vector<std::uint8_t> encode(const Position& position, std::uint32_t station_id,
                               std::uint64_t timestamp, const LowFrequencyData* low_frequency = nullptr);
const char* edca_queue(std::uint8_t priority); // 802.1D user priority, 0–7.
std::vector<std::uint8_t> make_frame(const Position& position,
    std::uint32_t station_id, const std::array<std::uint8_t, 6>& mac,
    std::uint16_t sequence, std::uint64_t timestamp, std::uint8_t priority = default_priority,
    const LowFrequencyData* low_frequency = nullptr);
} // namespace cam
