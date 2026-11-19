#pragma once

#include "cam_timing.h"
#include "cam_telemetry.h"
#include "airtime_estimator.h"
#include "path_history.h"
#include <mutex>

namespace cam {
struct GeneratedCam {
    std::uint64_t version;
    Position position;
    std::vector<std::uint8_t> frame;
    GenerationTiming::Clock::time_point generated_at, expires_at;
    Trigger trigger;
    bool low_frequency;

    bool prepare_for_tx(GenerationTiming::Clock::time_point now, std::uint16_t sequence) {
        if (frame.size() < 86 || now >= expires_at) return false;
        const auto rt = radiotap_length(frame);
        if (frame.size() < rt + 78) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now).count();
        if (remaining < 50) return false; // Cannot represent a smaller positive GN lifetime.
        frame[rt + 36] = static_cast<std::uint8_t>(std::min<std::int64_t>(20, remaining / 50) << 2);
        const auto seq = (sequence & 4095U) << 4;
        frame[rt + 22] = seq & 255;
        frame[rt + 23] = seq >> 8;
        return true;
    }
};

// One producer and one transmitter. Pending CAMs are replaced, never queued in
// a backlog. The generation policy continues while admission blocks.
class GenerationBuffer {
public:
    using Clock = GenerationTiming::Clock;
    GenerationBuffer(std::uint32_t station_id, std::array<std::uint8_t, 6> mac,
                     std::uint8_t priority = default_priority, bool force_100ms = false,
                     Telemetry* telemetry = nullptr)
        : station_id_(station_id), mac_(mac), priority_(priority), policy_(force_100ms), telemetry_(telemetry) {}

    bool generate(const Position& position, Clock::time_point now, std::uint64_t wall_ms,
                  int dcc_ms, Clock::time_point source_valid_until = Clock::time_point::max()) {
        const auto started = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        expire(now);
        if (position.latitude < -900000000 || position.latitude > 900000000 ||
            position.longitude < -1800000000 || position.longitude > 1800000000 ||
            position.speed < 0 || position.speed > 16383 || position.heading < 0 || position.heading > 3601 ||
            now >= source_valid_until || (position.sample_timestamp_ms &&
            ((*position.sample_timestamp_ms > wall_ms && *position.sample_timestamp_ms - wall_ms > 2000) ||
             (wall_ms > *position.sample_timestamp_ms && wall_ms - *position.sample_timestamp_ms > 2000)))) {
            discard(Telemetry::Event::invalidated);
            history_.clear();
            return false;
        }
        history_.observe(position, position.sample_timestamp_ms.value_or(wall_ms));
        auto decision = policy_.evaluate(position, now, dcc_ms);
        if (!decision) return false;
        // Preserve an LF container if the CAM carrying it was replaced before
        // transmission. In particular, the first transmitted CAM contains LF.
        decision->low_frequency = decision->low_frequency || !lf_transmitted_ || (pending_ && pending_->low_frequency);
        LowFrequencyData lf;
        lf.history = history_.points();
        auto frame = make_frame(position, station_id_, mac_, 0, wall_ms, priority_, decision->low_frequency ? &lf : nullptr);
        // 6.1.5.1: discard a construction that missed its 50 ms budget. Leave
        // the trigger unconsumed, so the next check can construct fresh data.
        if (Clock::now() - started >= std::chrono::milliseconds(50)) {
            count(Telemetry::Event::construction_overrun);
            return false;
        }
        expire(now);
        if (pending_) count(Telemetry::Event::replaced);
        pending_ = GeneratedCam{++version_, position, std::move(frame), now,
            std::min(source_valid_until, now + std::chrono::milliseconds(1000)), decision->trigger, decision->low_frequency};
        count(Telemetry::Event::generated);
        policy_.generated(*decision, now);
        return true;
    }

    std::optional<GeneratedCam> latest(Clock::time_point now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        expire(now);
        if (!pending_) return std::nullopt;
        return pending_;
    }

    void submitted(std::uint64_t version) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ && pending_->version == version) pending_.reset();
    }

    void transmitted(const GeneratedCam& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        policy_.transmitted(packet.position);
        if (packet.low_frequency) lf_transmitted_ = true;
    }

    void invalidate_source() {
        std::lock_guard<std::mutex> lock(mutex_);
        discard(Telemetry::Event::invalidated);
        history_.clear();
    }

private:
    void count(Telemetry::Event event) const { if (telemetry_) telemetry_->count(event); }
    void discard(Telemetry::Event reason) const {
        if (pending_) { count(reason); pending_.reset(); }
    }
    void expire(Clock::time_point now) const {
        // A remaining GN lifetime below 50 ms cannot be transmitted either.
        if (pending_ && pending_->expires_at - now < std::chrono::milliseconds(50))
            discard(Telemetry::Event::expired);
    }
    std::uint32_t station_id_;
    std::array<std::uint8_t, 6> mac_;
    std::uint8_t priority_;
    mutable std::mutex mutex_;
    GenerationPolicy policy_;
    mutable std::optional<GeneratedCam> pending_;
    PathHistory history_;
    std::uint64_t version_ = 0;
    bool lf_transmitted_ = false;
    Telemetry* telemetry_;
};
} // namespace cam
