#pragma once

#include "cbr_sampler.h"
#include <array>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace dccnet {
using Address = std::array<std::uint8_t, 8>;
using Clock = CbrSample::Clock;
struct Observation {
    Address address{};
    std::uint32_t source_timestamp = 0;
    double local = 0, one_hop = 0;
    int tx_eirp_dbm = 0;
    std::optional<int> rssi_dbm;
    std::optional<unsigned> frequency_mhz;
};
struct Neighbor {
    Observation observation;
    Clock::time_point received_at;
};
struct Snapshot {
    double local = 0, one_hop = 0, two_hop = 0, global = 0;
    std::size_t neighbors = 0;
    CbrSample control;
};

// SHB's four media-dependent octets, in network bit order (bit 0 is MSB).
std::array<std::uint8_t, 4> encode(double local, double one_hop, double eirp_dbm);
std::optional<Observation> parse(const std::vector<std::uint8_t>& radiotap_frame);
void write_extension(std::vector<std::uint8_t>& frame, double local, double one_hop, double eirp_dbm);

class State {
public:
    static constexpr double target = 0.62; // CTH, distinct from adaptive DCC's target.
    static constexpr auto lifetime = std::chrono::milliseconds(1000);
    static constexpr auto interval = std::chrono::milliseconds(100);
    bool receive(const Observation& observation, Clock::time_point now);
    Snapshot trigger(const std::optional<CbrSample>& local, Clock::time_point now, std::int64_t utc_slot);
    Snapshot snapshot() const;
    std::optional<Snapshot> wait_next(std::uint64_t& cursor);
    void stop();
    void reset();
    std::vector<Neighbor> neighbors() const;
private:
    void expire(Clock::time_point now);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<Address, Neighbor> neighbors_;
    Snapshot snapshot_;
    std::optional<CbrSample> previous_local_;
    bool stopped_ = false;
};
} // namespace dccnet
