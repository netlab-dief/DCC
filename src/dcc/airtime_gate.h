#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include "dcc/dcc_contracts.h"

// One shared admission gate per channel, independent of producer/service.
// A grant reserves one packet. ACCOUNT charges estimated airtime after
// successful driver enqueue (no PHY completion claim).
class AirtimeGate {
public:
    using Clock = std::chrono::steady_clock;
    void configure(double delta, std::chrono::microseconds minimum_interval, Clock::time_point now);
    void configure(const dcc::AccessControl& control, Clock::time_point now);
    void disable();
    int suggested_interval_ms();
    struct Admission {
        std::uint64_t packet_id = 0; // Allocated by the shared gate only on grant.
        std::chrono::microseconds retry_after{100000};
    };
    Admission reserve(std::uint64_t airtime_us, Clock::time_point now);
    bool account(std::uint64_t packet_id, std::uint64_t airtime_us, Clock::time_point now);
    dcc::TxFeedback feedback() const;
    bool ready(Clock::time_point now) const;
    // Compatibility parser for existing diagnostics/tests; no external service.
    std::string handle(const std::string& request, Clock::time_point now);

private:
    std::chrono::microseconds spacing(std::uint64_t airtime) const;
    void grant_locked(std::uint64_t id, std::uint64_t airtime);
    bool account_locked(std::uint64_t id, std::uint64_t airtime, Clock::time_point now);
    std::string handle_locked(const std::string& request, Clock::time_point now);
    mutable std::mutex mutex_;
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
