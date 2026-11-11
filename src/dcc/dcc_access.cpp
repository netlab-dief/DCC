#include "dcc/dcc_access.h"
#include <cmath>
#include <stdexcept>

DccAccess::DccAccess(std::unique_ptr<DccRadio> radio, bool require_power_readback)
    : radio_(std::move(radio)), require_power_readback_(require_power_readback) {
    if (!radio_) throw std::invalid_argument("DCC_ACC needs a radio backend");
}

void DccAccess::start() {
    std::lock_guard<std::mutex> lock(radio_mutex_);
    if (started_) return;
    radio_->start();
    requested_radio_.reset();
    configured_power_.reset();
    sampler_.start([this] { return radio_->read_cbr(); });
    started_ = true;
}

void DccAccess::stop() {
    disable();
    sampler_.stop();
    std::lock_guard<std::mutex> lock(radio_mutex_);
    started_ = false;
    requested_radio_.reset();
    configured_power_.reset();
}

void DccAccess::disable() { gate_.disable(); }

void DccAccess::apply(const dcc::AccessControl& control) {
    std::lock_guard<std::mutex> lock(radio_mutex_);
    if (!started_ || !control.valid || dcc::Clock::now() >= control.valid_until) {
        gate_.disable();
        return;
    }
    if (!std::isfinite(control.airtime_fraction) || control.airtime_fraction < 0.0006 ||
        control.airtime_fraction > 0.03 || control.minimum_admission_interval.count() < 25000) {
        gate_.disable();
        throw std::invalid_argument("invalid DCC_ACC resource budget");
    }
    const auto requested = std::make_pair(control.power_limit_dbm, control.bitrate_mbps);
    if (!requested_radio_ || requested != *requested_radio_) {
        gate_.disable();
        configured_power_.reset();
        requested_radio_.reset();
        radio_->configure(requested.first, requested.second);
        if (require_power_readback_) {
            configured_power_ = radio_->configured_power_dbm();
            if (!configured_power_ || !std::isfinite(*configured_power_))
                throw std::runtime_error("DCC_NET requires nl80211 TX power readback");
        }
        requested_radio_ = requested;
        bitrate_ = requested.second;
    }
    // Bounded I/O may still consume the remaining lifetime. Never extend it.
    gate_.configure(control, dcc::Clock::now());
}

bool DccAccess::ready() const {
    std::lock_guard<std::mutex> lock(radio_mutex_);
    const auto now = dcc::Clock::now();
    const auto local = sampler_.latest();
    return started_ && requested_radio_ && (!require_power_readback_ || configured_power_) &&
        local && local->fresh(now) && gate_.ready(now);
}

int DccAccess::bitrate_mbps() const {
    std::lock_guard<std::mutex> lock(radio_mutex_);
    return bitrate_;
}

void DccAccess::dispatch(std::vector<std::uint8_t>& frame, const std::function<void()>& enqueue,
    const std::function<void(std::vector<std::uint8_t>&, std::optional<double>)>& decorate) {
    std::lock_guard<std::mutex> lock(radio_mutex_);
    const auto now = dcc::Clock::now();
    const auto local = sampler_.latest();
    if (!started_ || !requested_radio_ || !local || !local->fresh(now) || !gate_.ready(now) ||
        (require_power_readback_ && !configured_power_))
        throw std::runtime_error("DCC_ACC input/power expired after admission; transmission stopped");
    if (decorate) decorate(frame, configured_power_);
    enqueue();
}
