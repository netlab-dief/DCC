#pragma once

#include "dcc/dcc_contracts.h"
#include "dcc/dcc_adaptive.h"
#include <mutex>

// One channel per instance. Policy only: no sockets, radio I/O or CAM ownership.
class DccCross {
public:
    void reset(dcc::Mode mode, bool network_enabled);
    dcc::PolicySnapshot evaluate(const std::optional<CbrSample>& local,
        const std::optional<CbrSample>& global, const dcc::TxFeedback& feedback,
        dcc::Clock::time_point now);
    void invalidate();
    dcc::PolicySnapshot snapshot(dcc::Clock::time_point now) const;
    dcc::ChannelResources resources(const dcc::TxFeedback& feedback, dcc::Clock::time_point now) const;
    dcc::NetworkInput network_input(const std::optional<CbrSample>& local) const;
private:
    mutable std::mutex mutex_;
    bool network_enabled_ = false;
    bool adaptive_ready_ = false;
    std::optional<std::int64_t> previous_slot_;
    std::optional<std::uint64_t> previous_sequence_;
    DccStateMachine reactive_;
    DccAdaptive adaptive_;
    dcc::PolicySnapshot policy_;
};
