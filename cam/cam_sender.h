#pragma once

#include "cam_message.h"
#include "cam_timing.h"
#include "cam_generation.h"
#include "../airtime_gate.h"
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace cam {
struct Options {
    bool enabled = false;
    bool force_100ms = false; // Force generation only; radio admission still applies.
    std::string gps_device; // Empty: use static position.
    std::uint32_t station_id = 4316;
    std::uint16_t feedback_port = 5557;
    std::uint8_t priority = default_priority; // CAM TC2 -> UP3 -> AC_BE.
};

class Sender {
public:
    Sender(AirtimeGate& gate, std::string interface, Options options,
           std::function<int()> dcc_interval = {});
    ~Sender();
    void stop();
    TelemetrySnapshot telemetry() const { return telemetry_.snapshot(); }
    void check(); // Propagate worker errors to the application.

    // Callback must send once, wait for actual TX completion, then return PHY us.
    // Exceptions deliberately leave any outstanding reservation closed.
    static bool transmit(AirtimeGate& gate, std::uint64_t id, std::uint64_t estimate,
        const std::function<std::uint64_t()>& send_and_wait, const std::atomic<bool>& stopping,
        const std::function<bool()>& ready = {},
        const std::function<std::uint64_t()>& current_estimate = {},
        Telemetry* telemetry = nullptr);

private:
    void run();
    AirtimeGate& gate_;
    std::string interface_;
    Options options_;
    std::function<int()> dcc_interval_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::exception_ptr failure_;
    Telemetry telemetry_;
    std::thread thread_;
};
} // namespace cam
