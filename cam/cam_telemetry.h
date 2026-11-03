#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <string>

namespace cam {
struct TelemetrySnapshot {
    std::uint64_t generated = 0, replaced = 0, expired = 0, invalidated = 0;
    std::uint64_t construction_overruns = 0, submitted = 0, confirmed = 0, tx_errors = 0;
    std::uint64_t airtime_us = 0;
    double queue_ms = 0, gate_ms = 0, feedback_ms = 0;
};

// Independent of the radio backend; snapshots never hold locks during I/O.
class Telemetry {
public:
    enum class Event { generated, replaced, expired, invalidated, construction_overrun, tx_error };
    void count(Event event) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (event) {
        case Event::generated: ++data_.generated; break;
        case Event::replaced: ++data_.replaced; break;
        case Event::expired: ++data_.expired; break;
        case Event::invalidated: ++data_.invalidated; break;
        case Event::construction_overrun: ++data_.construction_overruns; break;
        case Event::tx_error: ++data_.tx_errors; break;
        }
    }
    void submitted(double queue_ms, double gate_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++data_.submitted;
        data_.queue_ms += queue_ms;
        data_.gate_ms += gate_ms;
    }
    void confirmed(double feedback_ms, std::uint64_t airtime_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++data_.confirmed;
        data_.feedback_ms += feedback_ms;
        data_.airtime_us += airtime_us;
    }
    TelemetrySnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }
private:
    mutable std::mutex mutex_;
    TelemetrySnapshot data_;
};

class TelemetryLog {
public:
    using Clock = std::chrono::steady_clock;
    explicit TelemetryLog(const std::string& path) : output_(path, std::ios::app) {
        if (!output_) throw std::runtime_error("Cannot open CAM telemetry: " + path);
        if (output_.tellp() == 0)
            output_ << "Session_Unix_ms,Timestamp_Unix_ms,Elapsed_s,Generated,Replaced,Expired,Invalidated,Construction_Overruns,Submitted,TX_Confirmed,TX_Errors,TX_Hz,Queue_Mean_ms,Gate_Mean_ms,Feedback_Mean_ms,PHY_Airtime_us\n";
    }
    double sample(const TelemetrySnapshot& data, Clock::time_point now = Clock::now()) {
        const double interval = std::chrono::duration<double>(now - previous_time_).count();
        const double hz = interval > 0 ? (data.confirmed - previous_confirmed_) / interval : 0;
        const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        output_ << session_ms_ << ',' << wall << ',' << std::fixed << std::setprecision(6)
                << std::chrono::duration<double>(now - start_).count() << ','
                << data.generated << ',' << data.replaced << ',' << data.expired << ',' << data.invalidated << ','
                << data.construction_overruns << ',' << data.submitted << ',' << data.confirmed << ',' << data.tx_errors << ','
                << hz << ',' << (data.submitted ? data.queue_ms / data.submitted : 0) << ','
                << (data.submitted ? data.gate_ms / data.submitted : 0) << ','
                << (data.confirmed ? data.feedback_ms / data.confirmed : 0) << ',' << data.airtime_us << '\n';
        output_.flush();
        if (!output_) throw std::runtime_error("Cannot write CAM telemetry");
        previous_time_ = now;
        previous_confirmed_ = data.confirmed;
        return hz;
    }
private:
    std::ofstream output_;
    const std::int64_t session_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const Clock::time_point start_ = Clock::now();
    Clock::time_point previous_time_ = start_;
    std::uint64_t previous_confirmed_ = 0;
};
} // namespace cam
