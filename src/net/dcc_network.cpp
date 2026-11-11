#include "net/dcc_network.h"
#include <cmath>
#include <stdexcept>

void DccNetwork::start(const std::string& monitor, const std::string& survey) {
    if (!local_ || !resources_) throw std::invalid_argument("DCC_NET requires CROSS interfaces");
    receiver_.start(monitor, survey, [this] {
        const auto input = local_();
        if (input.aggregation_target != dccnet::State::target)
            throw std::runtime_error("DCC_NET/CROSS aggregation target mismatch");
        return input.local;
    });
}

bool DccNetwork::ready() const {
    const auto local = local_().local;
    const auto now = dcc::Clock::now();
    return !stopped() && local && local->fresh(now) && state_.snapshot().control.fresh(now) &&
        channel_resources().valid;
}

void DccNetwork::decorate(std::vector<std::uint8_t>& frame, double confirmed_eirp_dbm) const {
    const auto local = local_().local;
    const auto net = state_.snapshot();
    const auto now = dcc::Clock::now();
    // CROSS resources are exposed to GN, but SHB load exchange needs no
    // forwarding decision. Recheck the inputs after waiting for radio settings.
    if (stopped() || !local || !local->fresh(now) || !net.control.fresh(now) || !std::isfinite(confirmed_eirp_dbm))
        throw std::runtime_error("DCC_NET data/power expired after admission");
    dccnet::write_extension(frame, local->value, net.one_hop, confirmed_eirp_dbm);
}
