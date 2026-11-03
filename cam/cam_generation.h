#pragma once

#include "cam_timing.h"
#include <deque>
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
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now).count();
        if (remaining < 50) return false; // Cannot represent a smaller positive GN lifetime.
        frame[44] = static_cast<std::uint8_t>(std::min<std::int64_t>(20, remaining / 50) << 2);
        const auto seq = (sequence & 4095U) << 4;
        frame[30] = seq & 255;
        frame[31] = seq >> 8;
        return true;
    }
};

// One producer and one transmitter. Pending CAMs are replaced, never queued in
// a backlog. The generation policy continues while admission/feedback blocks.
class GenerationBuffer {
public:
    using Clock = GenerationTiming::Clock;
    GenerationBuffer(std::uint32_t station_id, std::array<std::uint8_t, 6> mac,
                     std::uint8_t priority = default_priority, bool force_100ms = false)
        : station_id_(station_id), mac_(mac), priority_(priority), policy_(force_100ms) {}

    bool generate(const Position& position, Clock::time_point now, std::uint64_t wall_ms,
                  int dcc_ms, Clock::time_point source_valid_until = Clock::time_point::max()) {
        const auto started = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (position.latitude < -900000000 || position.latitude > 900000000 ||
            position.longitude < -1800000000 || position.longitude > 1800000000 ||
            position.speed < 0 || position.speed > 16383 || position.heading < 0 || position.heading > 3601 ||
            now >= source_valid_until || (position.sample_timestamp_ms &&
            ((*position.sample_timestamp_ms > wall_ms && *position.sample_timestamp_ms - wall_ms > 2000) ||
             (wall_ms > *position.sample_timestamp_ms && wall_ms - *position.sample_timestamp_ms > 2000)))) {
            pending_.reset();
            return false;
        }
        observe(position, wall_ms);
        auto decision = policy_.evaluate(position, now, dcc_ms);
        if (!decision) return false;
        // Preserve an LF container if the CAM carrying it was replaced before
        // transmission. In particular, the first transmitted CAM contains LF.
        decision->low_frequency = decision->low_frequency || !lf_transmitted_ || (pending_ && pending_->low_frequency);
        LowFrequencyData lf;
        lf.history.assign(history_.rbegin(), history_.rend());
        auto frame = make_frame(position, station_id_, mac_, 0, wall_ms, priority_, decision->low_frequency ? &lf : nullptr);
        // 6.1.5.1: discard a construction that missed its 50 ms budget. Leave
        // the trigger unconsumed, so the next check can construct fresh data.
        if (Clock::now() - started >= std::chrono::milliseconds(50)) return false;
        pending_ = GeneratedCam{++version_, position, std::move(frame), now,
            std::min(source_valid_until, now + std::chrono::milliseconds(1000)), decision->trigger, decision->low_frequency};
        policy_.generated(*decision, now);
        return true;
    }

    std::optional<GeneratedCam> latest(Clock::time_point now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_ || now >= pending_->expires_at) return std::nullopt;
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
        pending_.reset();
        history_.clear();
        last_observation_.reset();
    }

private:
    void observe(const Position& position, std::uint64_t wall_ms) {
        const auto sample_time = position.sample_timestamp_ms.value_or(wall_ms);
        if (last_observation_ && sample_time > last_observation_->timestamp_ms &&
            (position.latitude != last_observation_->position.latitude || position.longitude != last_observation_->position.longitude)) {
            history_.push_back(*last_observation_);
            if (history_.size() > 23) history_.pop_front();
        }
        if (!last_observation_ || sample_time > last_observation_->timestamp_ms)
            last_observation_ = PathSample{position, sample_time};
    }

    std::uint32_t station_id_;
    std::array<std::uint8_t, 6> mac_;
    std::uint8_t priority_;
    mutable std::mutex mutex_;
    GenerationPolicy policy_;
    std::optional<GeneratedCam> pending_;
    std::optional<PathSample> last_observation_;
    std::deque<PathSample> history_;
    std::uint64_t version_ = 0;
    bool lf_transmitted_ = false;
};
} // namespace cam
