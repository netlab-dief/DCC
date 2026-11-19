#pragma once

#include "radio/cbr_sampler.h"
#include "dcc/dcc_state_machine.h"
#include <optional>

namespace dcc {
using Clock = CbrSample::Clock;
using Duration = std::chrono::microseconds;

enum class Mode { reactive, adaptive };
enum class CbrSource { local, global };

// Values crossing the management/access boundary. All CBRs are fractions.
struct AccessControl {
    bool valid = false;
    double airtime_fraction = 0.0006;
    Duration minimum_admission_interval{25000};
    int power_limit_dbm = 23;
    int bitrate_mbps = 6;
    Clock::time_point valid_until{};
    double channel_busy_ratio = 0; // Unfiltered CBR for the independent radio limits.
};

// Driver-enqueue feedback, with estimated airtime. Never PHY completion.
struct TxFeedback {
    std::uint64_t packet_id = 0;
    Duration airtime{0};
    std::optional<Clock::time_point> forwarded_at;
};

struct ChannelResources {
    bool valid = false;
    double available_airtime_fraction = 0.0006;
    Duration minimum_admission_interval{25000};
    std::optional<Duration> estimated_idle_time; // Ton * (1 - CBRa) / CBRa.
    Duration suggested_admission_interval{1000000}; // Distinct from Toff.
    TxFeedback last_tx;
    Clock::time_point valid_until{};
};

struct PolicySnapshot {
    Mode mode = Mode::reactive;
    CbrSource source = CbrSource::local;
    DccState state = DccState::UNKNOWN;
    double control_cbr = 0;
    double filtered_cbr = 0;
    bool controller_updated = false;
    AccessControl access;
};

struct NetworkInput {
    std::optional<CbrSample> local;
    // GeoNetworking aggregation threshold; not the adaptive algorithm's 0.68 target.
    double aggregation_target = 0.62;
};
} // namespace dcc
