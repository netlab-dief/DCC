#include "dcc/dcc_cross.h"
#include "dcc/radio_limits.h"
#include <algorithm>
#include <cmath>

namespace {
bool usable(const std::optional<CbrSample>& sample, dcc::Clock::time_point now) {
    return sample && sample->fresh(now) && std::isfinite(sample->value) && sample->value >= 0 && sample->value <= 1;
}
int adaptive_power(int interval_ms) {
    const double rate = 1000.0 / interval_ms;
    return static_cast<int>(std::clamp(10.0 + (rate - 1.0) * 13.0 / 39.0, 10.0, 23.0));
}
dcc::Duration interval(const dcc::AccessControl& control, const dcc::TxFeedback& feedback) {
    if (!feedback.forwarded_at || feedback.airtime.count() <= 0) return std::chrono::seconds(1);
    return dcc::admission_interval(feedback.airtime.count(), control.airtime_fraction,
        control.minimum_admission_interval, control.channel_busy_ratio);
}
}

void DccCross::reset(dcc::Mode mode, bool network_enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    network_enabled_ = network_enabled;
    adaptive_ready_ = false;
    previous_slot_.reset();
    previous_sequence_.reset();
    reactive_ = DccStateMachine{};
    adaptive_ = DccAdaptive{};
    policy_ = {};
    policy_.mode = mode;
    policy_.source = network_enabled ? dcc::CbrSource::global : dcc::CbrSource::local;
}

dcc::PolicySnapshot DccCross::evaluate(const std::optional<CbrSample>& local,
    const std::optional<CbrSample>& global, const dcc::TxFeedback& feedback, dcc::Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_.controller_updated = false;
    const auto& selected = network_enabled_ ? global : local;
    // A remote load cannot replace a missing local measurement. NET mode never
    // silently switches to local control when its global snapshot is invalid.
    if (!usable(local, now) || !usable(selected, now)) {
        reactive_.invalidate_observation();
        adaptive_.invalidate();
        adaptive_ready_ = false;
        previous_slot_.reset();
        policy_.access.valid = false;
        return policy_;
    }
    const auto& sample = *selected;
    // Replayed input must not refresh either the controller or its deadline.
    if (previous_sequence_ && sample.sequence <= *previous_sequence_) return policy_;
    if ((previous_slot_ && sample.utc_slot != *previous_slot_ + 1) ||
        (previous_sequence_ && sample.sequence != *previous_sequence_ + 1)) {
        reactive_.invalidate_observation();
        adaptive_.invalidate();
        adaptive_ready_ = false;
    }
    previous_sequence_ = sample.sequence;
    previous_slot_ = sample.utc_slot;
    policy_.control_cbr = sample.value;
    auto& access = policy_.access;
    // Local channel occupancy is always a bound, including when NET's global
    // value contains the preceding local interval. Do not use filtered CBR here.
    access.channel_busy_ratio = std::max(double(sample.value), double(local->value));
    access.valid_until = sample.measured_at + std::chrono::milliseconds(150);
    if (network_enabled_) access.valid_until = std::min(access.valid_until,
        local->measured_at + std::chrono::milliseconds(150));
    if (policy_.mode == dcc::Mode::reactive) {
        const auto old = reactive_.state();
        policy_.state = reactive_.update(sample.value * 100.0f, sample.measured_at);
        policy_.controller_updated = old != policy_.state;
        const auto parameters = DccStateMachine::parameters_for(policy_.state);
        access.airtime_fraction = 0.03;
        access.minimum_admission_interval = std::chrono::milliseconds(parameters.interval_ms);
        access.power_limit_dbm = parameters.tx_power_dbm;
        access.bitrate_mbps = parameters.mcs_mbps;
        access.valid = true;
    } else {
        policy_.controller_updated = adaptive_.observe(sample.value, sample.utc_slot);
        adaptive_ready_ = adaptive_ready_ || policy_.controller_updated;
        access.valid = adaptive_ready_;
        access.airtime_fraction = adaptive_.delta();
        access.minimum_admission_interval = std::chrono::milliseconds(25);
        policy_.filtered_cbr = adaptive_.filtered_cbr();
        if (policy_.controller_updated) {
            const auto milliseconds = (interval(access, feedback).count() + 999) / 1000;
            // Preserved project radio policy, not part of the ETSI delta equations.
            access.power_limit_dbm = adaptive_power(static_cast<int>(milliseconds));
            const int mapped = static_cast<int>(std::clamp(12.0 + (sample.value * 100.0 - 30.0) * 12.0 / 35.0, 12.0, 24.0));
            access.bitrate_mbps = mapped >= 24 ? 24 : (mapped >= 18 ? 18 : 12);
        }
    }
    return policy_;
}

void DccCross::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_.access.valid = false;
    reactive_.invalidate_observation();
    adaptive_.invalidate();
    adaptive_ready_ = false;
    previous_slot_.reset();
}

dcc::PolicySnapshot DccCross::snapshot(dcc::Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = policy_;
    result.access.valid = result.access.valid && now < result.access.valid_until;
    return result;
}

dcc::ChannelResources DccCross::resources(const dcc::TxFeedback& feedback, dcc::Clock::time_point now) const {
    const auto policy = snapshot(now);
    dcc::ChannelResources result;
    result.valid = policy.access.valid;
    result.available_airtime_fraction = policy.access.airtime_fraction;
    result.minimum_admission_interval = policy.access.minimum_admission_interval;
    result.valid_until = policy.access.valid_until;
    result.last_tx = feedback;
    if (feedback.forwarded_at && feedback.airtime.count() > 0) {
        result.estimated_idle_time = dcc::Duration(static_cast<std::int64_t>(std::ceil(
            feedback.airtime.count() * (1.0 - policy.access.airtime_fraction) / policy.access.airtime_fraction)));
        result.suggested_admission_interval = interval(policy.access, feedback);
    } else if (policy.mode == dcc::Mode::reactive) {
        result.suggested_admission_interval = policy.access.minimum_admission_interval;
    }
    return result;
}

dcc::NetworkInput DccCross::network_input(const std::optional<CbrSample>& local) const {
    return {local, 0.62};
}
