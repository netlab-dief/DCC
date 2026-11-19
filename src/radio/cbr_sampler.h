#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

// A survey snapshot can occur anywhere inside its read callback. Accept a
// nominal 100 ms window only if ALL possible durations lie within 90..110 ms.
// The +/-10 ms tolerance is a project acquisition policy, not an ETSI constant.
struct CbrMeasurementWindow {
    using Clock = std::chrono::steady_clock;
    Clock::duration minimum{}, maximum{};
    bool valid() const {
        return minimum >= std::chrono::milliseconds(90) &&
            maximum <= std::chrono::milliseconds(110) && minimum <= maximum;
    }
    static CbrMeasurementWindow between(Clock::time_point previous_start, Clock::time_point previous_end,
        Clock::time_point start, Clock::time_point end) {
        if (previous_end < previous_start || start < previous_end || end < start) return {};
        return {start - previous_end, end - previous_start};
    }
};

struct CbrSample {
    using Clock = std::chrono::steady_clock;
    float value = -1.0f; // Fraction, never percentage.
    std::int64_t utc_slot = 0; // End of the measured 100 ms window.
    std::uint64_t sequence = 0;
    Clock::time_point measured_at{};
    bool valid = false;
    CbrMeasurementWindow measurement_window{}; // Local survey uncertainty, absent for NET aggregation.

    bool fresh(Clock::time_point now) const {
        return valid && now >= measured_at && now - measured_at <= std::chrono::milliseconds(150);
    }
};

// The read callback belongs exclusively to the acquisition thread. It must
// return the CBR since its previous invocation and have bounded I/O latency.
class CbrSampler {
public:
    using Read = std::function<float()>;
    ~CbrSampler() { stop(); }
    void start(Read read);
    void stop();
    std::optional<CbrSample> latest() const;
    std::optional<CbrSample> wait_next(std::uint64_t& cursor,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(150));
private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = true;
    std::deque<CbrSample> samples_;
    Read read_;
    std::thread thread_;
};
