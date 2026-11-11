#pragma once

#include "dcc/airtime_gate.h"
#include <functional>
#include <memory>
#include <vector>

// Injectable hardware boundary. The Linux implementation lives in src/radio/;
// policy and integration tests can supply a deterministic radio instead.
class DccRadio {
public:
    virtual ~DccRadio() = default;
    virtual void start() = 0;
    virtual float read_cbr() = 0;
    virtual void configure(int power_dbm, int bitrate_mbps) = 0;
    virtual std::optional<double> configured_power_dbm() = 0;
};

class DccAccess {
public:
    explicit DccAccess(std::unique_ptr<DccRadio> radio, bool require_power_readback = false);
    ~DccAccess() { stop(); }
    void start();
    void stop();
    void disable();
    std::optional<CbrSample> local_sample() const { return sampler_.latest(); }
    std::optional<CbrSample> wait_local(std::uint64_t& cursor) { return sampler_.wait_next(cursor); }
    void apply(const dcc::AccessControl& control);
    bool ready() const;
    int bitrate_mbps() const;
    dcc::TxFeedback feedback() const { return gate_.feedback(); }
    AirtimeGate& admission() { return gate_; } // Shared by every producer on this channel.

    // Serialize radio configuration, optional GN decoration and driver enqueue.
    // The decoration callback receives confirmed configured power, never an assumption.
    void dispatch(std::vector<std::uint8_t>& frame, const std::function<void()>& enqueue,
        const std::function<void(std::vector<std::uint8_t>&, std::optional<double>)>& decorate = {});
private:
    std::unique_ptr<DccRadio> radio_;
    bool require_power_readback_;
    CbrSampler sampler_;
    AirtimeGate gate_;
    mutable std::mutex radio_mutex_;
    std::optional<double> configured_power_;
    std::optional<std::pair<int, int>> requested_radio_;
    int bitrate_ = 6;
    bool started_ = false;
};
