#include "airtime_gate.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

std::chrono::microseconds AirtimeGate::spacing(std::uint64_t airtime) const {
    return std::max(minimum_, std::chrono::microseconds(
        static_cast<std::int64_t>(std::ceil(static_cast<double>(airtime) / delta_))));
}

void AirtimeGate::configure(double delta, std::chrono::microseconds minimum, Clock::time_point now) {
    if (!std::isfinite(delta) || delta < 0.0006 || delta > 0.03 || minimum.count() < 25000)
        throw std::invalid_argument("invalid airtime gate budget");
    std::lock_guard<std::mutex> lock(mutex_);
    delta_ = delta;
    if (pending_) pending_delta_ = std::min(pending_delta_, delta);
    minimum_ = minimum;
    enabled_ = true;
    valid_until_ = now + std::chrono::milliseconds(500);
    // Never erase outstanding debt on an update or create a burst on recovery.
    if (have_completed_) next_ = std::max(next_, completed_ + spacing(charged_us_));
}

int AirtimeGate::suggested_interval_ms() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!have_completed_) return 1000; // No packet duration is assumed before TX feedback.
    return static_cast<int>((spacing(charged_us_).count() + 999) / 1000);
}

void AirtimeGate::disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
}

std::string AirtimeGate::handle(const std::string& request, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    if (id == 0 || airtime == 0 || airtime > 1000000) return "ERROR range";
    const std::string suffix = " " + std::to_string(id);
    if (command == "REPORT" || command == "ACCOUNT") {
        if (!pending_ && have_completed_ && id == completed_id_) return "DONE" + suffix;
        if (!pending_ || id != highest_id_) return "ERROR reservation";
        // Charge at least the reservation, for either measured or estimated accounting.
        // Starting the whole spacing at accounting receipt is deliberately conservative:
        // budget updates cannot erase the charged debt. ACCOUNT does not measure queueing.
        charged_us_ = std::max(reserved_us_, airtime);
        completed_ = now;
        completed_id_ = id;
        have_completed_ = true;
        const auto reserved_spacing = std::chrono::microseconds(static_cast<std::int64_t>(
            std::ceil(static_cast<double>(charged_us_) / pending_delta_)));
        next_ = now + std::max(spacing(charged_us_), reserved_spacing);
        pending_ = false;
        return "DONE" + suffix;
    }
    if (command != "ADMIT") return "ERROR command";
    if (id <= highest_id_) return "ERROR stale_id";
    if (!enabled_ || now >= valid_until_) return "WAIT" + suffix + " 100000";
    if (pending_) return "WAIT" + suffix + " 100000";
    if (now < next_) {
        const auto remaining = std::chrono::ceil<std::chrono::microseconds>(next_ - now).count();
        return "WAIT" + suffix + " " + std::to_string(remaining);
    }
    highest_id_ = id;
    reserved_us_ = airtime;
    pending_delta_ = delta_;
    pending_ = true;
    return "GRANT" + suffix;
}
