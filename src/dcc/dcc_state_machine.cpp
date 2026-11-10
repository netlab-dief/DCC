#include "dcc/dcc_state_machine.h"

#include <limits>
#include <cmath>
#include <stdexcept>

namespace {
constexpr float ACTIVE_1_THRESHOLD = 30.0f;
constexpr float ACTIVE_2_THRESHOLD = 40.0f;
constexpr float ACTIVE_3_THRESHOLD = 50.0f;
constexpr float RESTRICTIVE_THRESHOLD = 65.0f;

float upper_threshold(DccState state) {
    switch (state) {
        case DccState::RELAXED: return ACTIVE_1_THRESHOLD;
        case DccState::ACTIVE_1: return ACTIVE_2_THRESHOLD;
        case DccState::ACTIVE_2: return ACTIVE_3_THRESHOLD;
        case DccState::ACTIVE_3: return RESTRICTIVE_THRESHOLD;
        default: return std::numeric_limits<float>::infinity();
    }
}

float lower_threshold(DccState state) {
    switch (state) {
        case DccState::ACTIVE_1: return ACTIVE_1_THRESHOLD;
        case DccState::ACTIVE_2: return ACTIVE_2_THRESHOLD;
        case DccState::ACTIVE_3: return ACTIVE_3_THRESHOLD;
        case DccState::RESTRICTIVE: return RESTRICTIVE_THRESHOLD;
        default: return -std::numeric_limits<float>::infinity();
    }
}
}

DccState DccStateMachine::state_for_cbr(float cbr_percent) {
    if (cbr_percent < ACTIVE_1_THRESHOLD) return DccState::RELAXED;
    if (cbr_percent < ACTIVE_2_THRESHOLD) return DccState::ACTIVE_1;
    if (cbr_percent < ACTIVE_3_THRESHOLD) return DccState::ACTIVE_2;
    if (cbr_percent < RESTRICTIVE_THRESHOLD) return DccState::ACTIVE_3;
    return DccState::RESTRICTIVE;
}

DccParameters DccStateMachine::parameters_for(DccState state) {
    switch (state) {
        case DccState::RELAXED: return {50, 23, 6};
        case DccState::ACTIVE_1: return {100, 19, 12};
        case DccState::ACTIVE_2: return {200, 16, 18};
        case DccState::ACTIVE_3: return {400, 13, 24};
        case DccState::RESTRICTIVE: return {1000, 10, 24};
        case DccState::UNKNOWN: throw std::invalid_argument("UNKNOWN has no radio parameters");
    }
    throw std::invalid_argument("invalid DCC state");
}

DccState DccStateMachine::update(float cbr_percent, Clock::time_point now) {
    if (!std::isfinite(cbr_percent) || cbr_percent < 0.0f || cbr_percent > 100.0f) {
        invalidate_observation();
        return state_;
    }
    const DccState instantaneous = state_for_cbr(cbr_percent);
    if (state_ == DccState::UNKNOWN) {
        state_ = instantaneous;
        invalidate_observation();
    } else if (cbr_percent >= upper_threshold(state_)) {
        downward_condition_active_ = false;
        if (!upward_condition_active_) {
            upward_condition_since_ = now;
            upward_condition_active_ = true;
        } else if (now - upward_condition_since_ >= std::chrono::seconds(1)) {
            state_ = static_cast<DccState>(static_cast<int>(state_) + 1);
            upward_condition_active_ = false;
        }
    } else if (cbr_percent < lower_threshold(state_)) {
        upward_condition_active_ = false;
        if (!downward_condition_active_) {
            downward_condition_since_ = now;
            downward_condition_active_ = true;
        } else if (now - downward_condition_since_ >= std::chrono::seconds(5)) {
            state_ = static_cast<DccState>(static_cast<int>(state_) - 1);
            downward_condition_active_ = false;
        }
    } else {
        invalidate_observation();
    }
    return state_;
}

void DccStateMachine::invalidate_observation() {
    upward_condition_active_ = false;
    downward_condition_active_ = false;
}

DccState DccStateMachine::state() const {
    return state_;
}
