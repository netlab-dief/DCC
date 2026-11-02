#ifndef DCC_STATE_MACHINE_H
#define DCC_STATE_MACHINE_H

#include <chrono>

enum class DccState {
    UNKNOWN = 0,
    RELAXED = 1,
    ACTIVE_1 = 2,
    ACTIVE_2 = 3,
    ACTIVE_3 = 4,
    RESTRICTIVE = 5
};

struct DccParameters {
    int interval_ms;
    int tx_power_dbm;
    int mcs_mbps;
};

class DccStateMachine {
public:
    using Clock = std::chrono::steady_clock;

    DccState update(float cbr_percent, Clock::time_point now);
    void invalidate_observation();
    DccState state() const;

    static DccState state_for_cbr(float cbr_percent);
    static DccParameters parameters_for(DccState state);

private:
    DccState state_ = DccState::UNKNOWN;
    Clock::time_point upward_condition_since_{};
    Clock::time_point downward_condition_since_{};
    bool upward_condition_active_ = false;
    bool downward_condition_active_ = false;
};

#endif
