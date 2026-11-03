#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// One gate per radio channel, shared by every transmitting service.
// A grant reserves one packet. REPORT accounts measured completion; ACCOUNT
// accounts estimated airtime after successful driver enqueue (no PHY claim).
class AirtimeGate {
public:
    using Clock = std::chrono::steady_clock;
    void configure(double delta, std::chrono::microseconds minimum_interval, Clock::time_point now);
    void disable();
    int suggested_interval_ms();
    std::string handle(const std::string& request, Clock::time_point now);

private:
    std::chrono::microseconds spacing(std::uint64_t airtime) const;
    std::mutex mutex_;
    double delta_ = 0.0006;
    double pending_delta_ = 0.0006;
    std::chrono::microseconds minimum_{25000};
    Clock::time_point valid_until_{};
    Clock::time_point next_{};
    Clock::time_point completed_{};
    bool enabled_ = false;
    bool pending_ = false;
    bool have_completed_ = false;
    std::uint64_t highest_id_ = 0;
    std::uint64_t completed_id_ = 0;
    std::uint64_t reserved_us_ = 0;
    std::uint64_t charged_us_ = 0;
};
