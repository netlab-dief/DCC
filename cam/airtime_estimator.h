#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace cam {
inline bool legacy_ofdm_rate(int mbps) {
    switch (mbps) {
    case 6: case 9: case 12: case 18: case 24: case 36: case 48: case 54: return true;
    default: return false;
    }
}

inline std::size_t radiotap_length(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 8 || frame[0] != 0) throw std::invalid_argument("Invalid radiotap header");
    const auto size = std::size_t(frame[2]) | (std::size_t(frame[3]) << 8);
    if (size < 8 || size >= frame.size()) throw std::invalid_argument("Invalid radiotap length");
    return size;
}

// DCC/netlink legacy values use the nominal 20 MHz rate table. At 10 MHz the
// same modulation/coding has half that physical rate. Use 10 MHz timings as a
// conservative default; if the radio actually uses 20 MHz, the estimate is 2x.
inline std::uint64_t estimate_airtime_us(const std::vector<std::uint8_t>& frame,
                                       int legacy_mbps, int channel_mhz = 10) {
    if (!legacy_ofdm_rate(legacy_mbps) || (channel_mhz != 10 && channel_mhz != 20))
        throw std::invalid_argument("Unsupported OFDM rate/channel width");
    const auto radio_header = radiotap_length(frame);
    const auto mac_bytes = frame.size() - radio_header;
    if (mac_bytes > 65531) throw std::invalid_argument("Frame too large for airtime estimation");
    // PSDU includes 802.11 headers, LLC/GN/BTP/CAM and the driver-added FCS.
    const std::uint64_t bits = 16 + 8 * (mac_bytes + 4) + 6; // SERVICE + PSDU + TAIL.
    const std::uint64_t bits_per_symbol = 4 * legacy_mbps;
    const auto symbols = (bits + bits_per_symbol - 1) / bits_per_symbol;
    const std::uint64_t scale = channel_mhz == 10 ? 2 : 1;
    return scale * (20 + 4 * symbols); // Preamble + SIGNAL + padded OFDM symbols.
}

// This sender builds minimal radiotap headers. Inject RATE explicitly to keep
// each packet's estimated rate stable across concurrent DCC rate changes.
inline void set_legacy_rate(std::vector<std::uint8_t>& frame, int legacy_mbps) {
    if (!legacy_ofdm_rate(legacy_mbps)) throw std::invalid_argument("Unsupported CAM legacy rate");
    const auto size = radiotap_length(frame);
    if (size == 8 && frame[4] == 0 && frame[5] == 0 && frame[6] == 0 && frame[7] == 0) {
        frame.insert(frame.begin() + 8, static_cast<std::uint8_t>(legacy_mbps * 2));
        frame[2] = 9;
        frame[4] = 4; // IEEE80211_RADIOTAP_RATE, units of 500 kb/s.
    } else if (size == 9 && frame[4] == 4 && frame[5] == 0 && frame[6] == 0 && frame[7] == 0) {
        frame[8] = static_cast<std::uint8_t>(legacy_mbps * 2);
    } else throw std::invalid_argument("Unsupported CAM radiotap layout");
}
} // namespace cam
