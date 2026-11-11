#pragma once
#include "dcc/dcc_contracts.h"
#include "net/gn_receiver.h"

// DCC_NET entity: owns neighbour state/RX and exposes explicit CROSS ports.
class DccNetwork {
public:
    using LocalInput = std::function<dcc::NetworkInput()>;
    using Resources = std::function<dcc::ChannelResources()>;
    DccNetwork(LocalInput local, Resources resources)
        : local_(std::move(local)), resources_(std::move(resources)) {}
    void start(const std::string& monitor, const std::string& survey);
    void stop() { receiver_.stop(); }
    void check() { receiver_.check(); }
    bool stopped() const { return receiver_.stopped(); }
    std::optional<dccnet::Snapshot> wait_next(std::uint64_t& cursor) { return state_.wait_next(cursor); }
    dccnet::Snapshot snapshot() const { return state_.snapshot(); }
    dcc::ChannelResources channel_resources() const { return resources_(); }
    bool ready() const;
    void decorate(std::vector<std::uint8_t>& frame, double confirmed_eirp_dbm) const;
private:
    LocalInput local_;
    Resources resources_;
    dccnet::State state_;
    GnReceiver receiver_{state_};
};
