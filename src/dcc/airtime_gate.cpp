#include "dcc/airtime_gate.h"
#include "dcc/radio_limits.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

std::chrono::microseconds AirtimeGate::spacing(std::uint64_t airtime) const {
    return dcc::admission_interval(airtime, delta_, minimum_, cbr_);
}

AirtimeGate::Clock::time_point AirtimeGate::duty_deadline(std::uint64_t airtime, Clock::time_point now) {
    while (!recent_charges_.empty() && recent_charges_.front().expires_at <= now) {
        recent_airtime_us_ -= recent_charges_.front().airtime;
        recent_charges_.pop_front();
    }
    auto total = recent_airtime_us_ + airtime;
    for (const auto& charge : recent_charges_) {
        if (total <= dcc::one_second_airtime_us) break;
        total -= charge.airtime;
        if (total <= dcc::one_second_airtime_us) return charge.expires_at;
    }
    return now;
}

void AirtimeGate::configure(double delta, std::chrono::microseconds minimum, Clock::time_point now) {
    dcc::AccessControl control;
    control.valid = true;
    control.airtime_fraction = delta;
    control.minimum_admission_interval = minimum;
    control.valid_until = now + std::chrono::milliseconds(500);
    configure(control, now);
}

void AirtimeGate::configure(const dcc::AccessControl& control, Clock::time_point now) {
    const auto delta = control.airtime_fraction;
    const auto minimum = control.minimum_admission_interval;
    if (!std::isfinite(delta) || delta < 0.0006 || delta > 0.03 || minimum.count() < 25000 ||
        !std::isfinite(control.channel_busy_ratio) || control.channel_busy_ratio < 0 || control.channel_busy_ratio > 1)
        throw std::invalid_argument("invalid airtime gate budget");
    std::lock_guard<std::mutex> lock(mutex_);
    delta_ = delta;
    cbr_ = control.channel_busy_ratio;
    if (pending_) {
        pending_delta_ = std::min(pending_delta_, delta);
        pending_cbr_ = std::max(pending_cbr_, cbr_);
        pending_minimum_ = std::max(pending_minimum_, minimum);
    }
    minimum_ = minimum;
    enabled_ = control.valid && now < control.valid_until;
    valid_until_ = control.valid_until;
    // Never erase outstanding debt on an update or create a burst on recovery.
    if (have_completed_) next_ = std::max(next_, completed_ + spacing(charged_us_));
}

AirtimeGate::Admission AirtimeGate::reserve(std::uint64_t airtime, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!airtime || airtime > dcc::maximum_airtime_us) throw std::runtime_error("estimated airtime must be in 1..4000 us");
    if (!enabled_ || now >= valid_until_ || pending_) return {};
    const auto deadline = std::max(next_, duty_deadline(airtime, now));
    if (now < deadline) return {0, std::chrono::ceil<std::chrono::microseconds>(deadline - now)};
    if (highest_id_ == UINT64_MAX) throw std::runtime_error("DCC packet identifiers exhausted");
    const auto id = highest_id_ + 1;
    grant_locked(id, airtime);
    return {id, std::chrono::microseconds(0)};
}

bool AirtimeGate::account(std::uint64_t id, std::uint64_t airtime, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!id || !airtime || airtime > dcc::maximum_airtime_us) return false;
    return account_locked(id, airtime, now);
}

void AirtimeGate::grant_locked(std::uint64_t id, std::uint64_t airtime) {
    highest_id_ = id;
    reserved_us_ = airtime;
    pending_delta_ = delta_;
    pending_cbr_ = cbr_;
    pending_minimum_ = minimum_;
    pending_ = true;
}

bool AirtimeGate::account_locked(std::uint64_t id, std::uint64_t airtime, Clock::time_point now) {
    if (!pending_ && have_completed_ && id == completed_id_) return true;
    if (!pending_ || id != highest_id_) return false;
    // Charge at least the reservation; receipt time and estimated airtime are
    // deliberately conservative, rather than claiming PHY queueing/completion.
    charged_us_ = std::max(reserved_us_, airtime);
    completed_ = now;
    completed_id_ = id;
    have_completed_ = true;
    const auto reserved_spacing = dcc::admission_interval(charged_us_, pending_delta_, pending_minimum_, pending_cbr_);
    next_ = now + std::max(spacing(charged_us_), reserved_spacing);
    // Keep a whole packet until its estimated end is outside every trailing
    // one-second window. Conservative at window edges and across restarts.
    duty_deadline(0, now);
    recent_charges_.push_back({now + std::chrono::microseconds(charged_us_) + std::chrono::seconds(1), charged_us_});
    recent_airtime_us_ += charged_us_;
    pending_ = false;
    return true;
}

dcc::TxFeedback AirtimeGate::feedback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {completed_id_, std::chrono::microseconds(charged_us_),
        have_completed_ ? std::optional<Clock::time_point>(completed_) : std::nullopt};
}

bool AirtimeGate::ready(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_ && now < valid_until_;
}

int AirtimeGate::suggested_interval_ms() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!have_completed_) return 1000; // No packet duration is assumed before enqueue accounting.
    return static_cast<int>((spacing(charged_us_).count() + 999) / 1000);
}

void AirtimeGate::disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
}

std::string AirtimeGate::handle(const std::string& request, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return handle_locked(request, now);
}

std::string AirtimeGate::handle_locked(const std::string& request, Clock::time_point now) {
    std::istringstream input(request);
    std::string command, id_text, airtime_text, extra;
    if (!(input >> command >> id_text >> airtime_text) || (input >> extra)) return "ERROR syntax";
    auto unsigned_number = [](const std::string& value) {
        return !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
    };
    if (!unsigned_number(id_text) || !unsigned_number(airtime_text)) return "ERROR number";
    std::uint64_t id, airtime;
    try { id = std::stoull(id_text); airtime = std::stoull(airtime_text); }
    catch (const std::exception&) { return "ERROR number"; }
    if (id == 0 || airtime == 0 || airtime > dcc::maximum_airtime_us) return "ERROR range";
    const std::string suffix = " " + std::to_string(id);
    if (command == "ACCOUNT") {
        return account_locked(id, airtime, now) ? "DONE" + suffix : "ERROR reservation";
    }
    if (command != "ADMIT") return "ERROR command";
    if (id <= highest_id_) return "ERROR stale_id";
    if (!enabled_ || now >= valid_until_) return "WAIT" + suffix + " 100000";
    if (pending_) return "WAIT" + suffix + " 100000";
    const auto deadline = std::max(next_, duty_deadline(airtime, now));
    if (now < deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::microseconds>(deadline - now).count();
        return "WAIT" + suffix + " " + std::to_string(remaining);
    }
    grant_locked(id, airtime);
    return "GRANT" + suffix;
}
