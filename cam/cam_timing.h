#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <cmath>
#include "cam_message.h"

namespace cam {
// Vehicle CAM generation bounds: ETSI EN 302 637-2 V1.4.1, 6.1.3.
// This is the generation lower bound, separate from radio admission/feedback.
class GenerationTiming {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int minimum_ms = 100;
    static constexpr int maximum_ms = 1000;

    // Missing/below-minimum DCC input defaults to T_GenCamMin.
    static std::chrono::milliseconds dcc_interval(int provided_ms = 0) {
        return std::chrono::milliseconds(std::clamp(provided_ms, minimum_ms, maximum_ms));
    }

    bool ready(Clock::time_point now, int provided_ms = 0) const {
        return !last_generation_ || now - *last_generation_ >= dcc_interval(provided_ms);
    }

    void generated(Clock::time_point now) { last_generation_ = now; }

private:
    std::optional<Clock::time_point> last_generation_;
};

enum class Trigger { initial, dynamics, periodic, forced };
struct GenerationDecision {
    Trigger trigger;
    bool low_frequency;
};

// EN 302 637-2 V1.4.1, 6.1.3. Evaluating never consumes a trigger;
// commit only after a CAM has been successfully constructed.
class GenerationPolicy {
public:
    using Clock = GenerationTiming::Clock;
    static constexpr unsigned n_gen_cam = 3;
    static constexpr int check_ms = 10;
    explicit GenerationPolicy(bool force_100ms = false) : force_100ms_(force_100ms) {
        if (force_100ms_) generation_interval_ = std::chrono::milliseconds(100);
    }

    static double distance_m(const Position& a, const Position& b) {
        constexpr double radians = 3.14159265358979323846 / 180.0;
        const double lat1 = a.latitude * 1e-7 * radians, lat2 = b.latitude * 1e-7 * radians;
        const double dlat = lat2 - lat1;
        const double dlon = (static_cast<double>(b.longitude) - a.longitude) * 1e-7 * radians;
        const double h = std::pow(std::sin(dlat / 2), 2) +
            std::cos(lat1) * std::cos(lat2) * std::pow(std::sin(dlon / 2), 2);
        return 6371000.0 * 2 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
    }

    static bool dynamics_changed(const Position& current, const Position& transmitted) {
        if (distance_m(current, transmitted) > 4.0) return true;
        if (current.speed >= 0 && current.speed < 16383 && transmitted.speed >= 0 &&
            transmitted.speed < 16383 && std::abs(current.speed - transmitted.speed) > 50) return true;
        if (current.heading >= 0 && current.heading < 3600 && transmitted.heading >= 0 && transmitted.heading < 3600) {
            const int difference = std::abs(current.heading - transmitted.heading);
            if (std::min(difference, 3600 - difference) > 40) return true;
        }
        return false;
    }

    std::optional<GenerationDecision> evaluate(const Position& current, Clock::time_point now,
                                              int dcc_ms = 0) const {
        const bool lf = !last_low_frequency_ || now - *last_low_frequency_ >= std::chrono::milliseconds(500);
        if (!last_generation_) return GenerationDecision{Trigger::initial, true};
        const auto elapsed = now - *last_generation_;
        if (force_100ms_) {
            if (elapsed >= std::chrono::milliseconds(100)) return GenerationDecision{Trigger::forced, lf};
            return std::nullopt;
        }
        if (elapsed < GenerationTiming::dcc_interval(dcc_ms)) return std::nullopt;
        if (last_transmitted_ && dynamics_changed(current, *last_transmitted_))
            return GenerationDecision{Trigger::dynamics, lf};
        if (elapsed >= generation_interval_) return GenerationDecision{Trigger::periodic, lf};
        return std::nullopt;
    }

    void generated(const GenerationDecision& decision, Clock::time_point now) {
        if (force_100ms_) {
            generation_interval_ = std::chrono::milliseconds(100);
            consecutive_periodic_ = 0;
        } else if (decision.trigger == Trigger::dynamics && last_generation_) {
            // Keep the actual elapsed interval, including DCC-induced delays.
            generation_interval_ = std::clamp(now - *last_generation_,
                Clock::duration(std::chrono::milliseconds(100)), Clock::duration(std::chrono::milliseconds(1000)));
            consecutive_periodic_ = 0;
        } else if (decision.trigger == Trigger::periodic) {
            consecutive_periodic_ = std::min(consecutive_periodic_ + 1, n_gen_cam);
            if (consecutive_periodic_ >= n_gen_cam) generation_interval_ = std::chrono::milliseconds(1000);
        } else {
            generation_interval_ = std::chrono::milliseconds(1000);
            consecutive_periodic_ = 0;
        }
        last_generation_ = now;
        if (decision.low_frequency) last_low_frequency_ = now;
    }

    void transmitted(const Position& position) { last_transmitted_ = position; }
    Clock::duration generation_interval() const { return generation_interval_; }
    unsigned consecutive_periodic() const { return consecutive_periodic_; }

private:
    bool force_100ms_ = false;
    std::optional<Clock::time_point> last_generation_, last_low_frequency_;
    std::optional<Position> last_transmitted_;
    Clock::duration generation_interval_{std::chrono::milliseconds(1000)};
    unsigned consecutive_periodic_ = 0;
};
} // namespace cam
