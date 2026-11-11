#include "dcc/dcc_access.h"
#include "dcc/dcc_cross.h"
#include "dcc/dcc_facilities.h"
#include "net/dcc_network.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>

namespace {
using namespace std::chrono_literals;
using Clock = dcc::Clock;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
CbrSample sample(float value, std::int64_t slot, Clock::time_point at) {
    return {value, slot, static_cast<std::uint64_t>(slot), at, true};
}
void test_adaptive_ports() {
    DccCross cross;
    cross.reset(dcc::Mode::adaptive, false);
    const auto t = Clock::time_point{};
    auto first = cross.evaluate(sample(0.68f, 1, t), {}, {}, t);
    require(!first.access.valid, "adaptive admission waits for a complete UTC pair");
    auto second = cross.evaluate(sample(0.68f, 2, t + 100ms), {}, {}, t + 100ms);
    require(second.access.valid && second.controller_updated, "second sample enables access");
    require(std::abs(second.access.airtime_fraction - 0.0009984) < 1e-10, "CROSS uses ETSI adaptive equations");
    auto third = cross.evaluate(sample(0.68f, 3, t + 200ms), {}, {}, t + 200ms);
    require(third.access.valid && !third.controller_updated, "odd slot refreshes freshness without another delta update");
    require(third.access.airtime_fraction == second.access.airtime_fraction, "budget preserved between updates");
    require(cross.snapshot(t + 349ms).access.valid && !cross.snapshot(t + 350ms).access.valid,
        "management and access use the same explicit validity deadline");
    auto replay = sample(0.0f, 3, t + 240ms);
    cross.evaluate(replay, {}, {}, t + 240ms);
    require(cross.snapshot(t + 240ms).access.valid_until == t + 350ms, "replay cannot extend validity");
    auto gap = cross.evaluate(sample(0.1f, 5, t + 400ms), {}, {}, t + 400ms);
    require(!gap.access.valid, "missing sample invalidates adaptive pair continuity");
    require(cross.evaluate(sample(0.1f, 6, t + 500ms), {}, {}, t + 500ms).access.valid,
        "consecutive pair recovers after a gap");
    cross.invalidate();
    require(!cross.snapshot(t + 500ms).access.valid, "explicit shutdown invalidates all resource consumers");
}
void test_global_selection() {
    DccCross cross;
    cross.reset(dcc::Mode::reactive, true);
    const auto t = Clock::time_point{};
    const auto local = sample(0.1f, 1, t);
    const auto global = sample(0.9f, 1, t);
    auto policy = cross.evaluate(local, global, {}, t);
    require(policy.source == dcc::CbrSource::global && policy.state == DccState::RESTRICTIVE,
        "global CBR drives the controller even if local load is low");
    require(!cross.evaluate(sample(0.1f, 2, t + 100ms), {}, {}, t + 100ms).access.valid,
        "missing NET input never falls back silently to local control");
    require(!cross.evaluate({}, sample(0.9f, 2, t + 100ms), {}, t + 100ms).access.valid,
        "neighbours cannot replace missing local measurements");
    auto bad = sample(std::numeric_limits<float>::quiet_NaN(), 3, t + 200ms);
    require(!cross.evaluate(bad, sample(0.9f, 3, t + 200ms), {}, t + 200ms).access.valid,
        "NaN local input invalidates NET-based control");
    auto input = cross.network_input(local);
    require(input.local->value == local.value && input.aggregation_target == 0.62,
        "CROSS supplies measured local CBR and the GN aggregation threshold");
    cross.reset(dcc::Mode::reactive, true);
    const auto bounded = cross.evaluate(local, sample(0.9f, 2, t + 100ms), {}, t + 100ms);
    require(bounded.access.valid_until == t + 150ms,
        "NET control lifetime cannot outlive the local measurement it relies on");
}
void test_net_to_cross() {
    dccnet::State state;
    DccCross cross;
    cross.reset(dcc::Mode::reactive, true);
    const auto t = Clock::time_point{};
    dccnet::Observation remote;
    remote.address[7] = 1;
    remote.source_timestamp = 1;
    remote.local = 0.9;
    remote.one_hop = 0.8;
    require(state.receive(remote, t), "remote DCC observation accepted");
    state.trigger(sample(0.1f, 1, t), t, 1);
    auto net = state.trigger(sample(0.1f, 2, t + 100ms), t + 100ms, 2);
    auto policy = cross.evaluate(sample(0.1f, 2, t + 100ms), net.control, {}, t + 100ms);
    require(policy.access.valid && policy.state == DccState::RESTRICTIVE, "real NET aggregation feeds CROSS policy");
    require(!cross.evaluate(sample(0.1f, 3, t + 300ms), net.control, {}, t + 300ms).access.valid,
        "expired global snapshots cannot drive access");
}
void test_resources_and_facilities() {
    DccCross cross;
    cross.reset(dcc::Mode::reactive, false);
    const auto t = Clock::time_point{};
    cross.evaluate(sample(0.1f, 1, t), {}, {}, t);
    auto unknown = cross.resources({}, t);
    require(!unknown.estimated_idle_time && !unknown.last_tx.forwarded_at, "no fabricated TX feedback before enqueue");
    dcc::TxFeedback feedback{1, 1000us, t};
    auto resources = cross.resources(feedback, t);
    require(resources.estimated_idle_time == 32334us, "idle time uses Ton * (1-CBRa)/CBRa");
    require(resources.suggested_admission_interval == 50ms, "reactive admission minimum remains separate from idle time");
    require(DccFacilities::cam_generation_interval_ms(resources) == 100, "CAM generation respects its own 100ms lower bound");
    feedback.airtime = 60000us;
    resources = cross.resources(feedback, t);
    require(resources.suggested_admission_interval == 2s && DccFacilities::cam_generation_interval_ms(resources) == 1000,
        "CAM generation cap does not cap conservative radio admission debt");
    resources = cross.resources(feedback, t + 200ms);
    require(!resources.valid && DccFacilities::cam_generation_interval_ms(resources) == 1000,
        "expired resources yield conservative CAM advice");
    DccNetwork network([&] { return cross.network_input(sample(0.1f, 1, t)); },
        [&] { return cross.resources(feedback, t); });
    require(network.channel_resources().last_tx.packet_id == 1,
        "NET has an explicit CROSS resource/feedback port");
}
void test_shared_gate() {
    AirtimeGate concurrent;
    const auto now = Clock::time_point{};
    concurrent.configure(0.01, 25ms, now);
    std::promise<void> start;
    const auto signal = start.get_future().share();
    auto producer = [&] { signal.wait(); return concurrent.reserve(500, now); };
    auto first = std::async(std::launch::async, producer);
    auto second = std::async(std::launch::async, producer);
    start.set_value();
    const auto one = first.get(), two = second.get();
    require(bool(one.packet_id) != bool(two.packet_id), "concurrent producers cannot both receive the channel reservation");
    require(concurrent.account(one.packet_id ? one.packet_id : two.packet_id, 500, now),
        "the winning producer owns the only accounting ticket");
    AirtimeGate gate;
    const auto t = Clock::time_point{};
    gate.configure(0.01, 25ms, t);
    auto a = gate.reserve(500, t);
    require(a.packet_id != 0 && !gate.reserve(100, t).packet_id, "two producers share one outstanding reservation");
    require(gate.account(a.packet_id, 500, t), "first producer accounted");
    auto early = gate.reserve(100, t + 49ms);
    require(!early.packet_id && early.retry_after == 1ms, "second producer consumes the same debt");
    auto b = gate.reserve(100, t + 50ms);
    require(b.packet_id > a.packet_id, "ticket allocation is channel-wide and independent of producer IDs");
    gate.configure(0.001, 25ms, t + 51ms);
    gate.configure(0.03, 25ms, t + 52ms);
    gate.account(b.packet_id, 100, t + 53ms);
    require(!gate.reserve(100, t + 152ms).packet_id, "tightest budget during reservation retained across producers");
    require(gate.feedback().packet_id == b.packet_id && gate.feedback().airtime == 100us,
        "ACC reports charged estimate and enqueue accounting timestamp");
    auto c = gate.reserve(100, t + 153ms);
    require(c.packet_id && !gate.account(c.packet_id + 1, 100, t + 154ms), "foreign ticket cannot release a reservation");
    require(!gate.reserve(100, t + 300ms).packet_id, "unaccounted/uncertain enqueue stays closed");
}
class FakeRadio final : public DccRadio {
public:
    int configurations = 0;
    bool fail = false, missing_readback = false;
    std::chrono::milliseconds delay{0};
    void start() override {}
    float read_cbr() override { return 0.1f; }
    void configure(int, int) override {
        ++configurations;
        if (delay.count()) std::this_thread::sleep_for(delay);
        if (fail) throw std::runtime_error("radio failure");
    }
    std::optional<double> configured_power_dbm() override {
        return missing_readback ? std::nullopt : std::optional<double>(20.0);
    }
};
void test_access_boundary() {
    auto radio = std::make_unique<FakeRadio>();
    auto* hardware = radio.get();
    DccAccess access(std::move(radio), true);
    access.start();
    std::uint64_t cursor = 0;
    std::optional<CbrSample> local;
    for (int i = 0; i < 10; ++i) {
        local = access.wait_local(cursor);
        if (local && local->fresh(Clock::now())) break;
    }
    require(local && local->fresh(Clock::now()), "ACC owns independent CBR acquisition");
    dcc::AccessControl control;
    control.valid = true;
    control.valid_until = Clock::now() + 500ms;
    access.apply(control);
    access.apply(control);
    require(hardware->configurations == 1, "unchanged radio settings do not cause repeated hardware I/O");
    auto malformed = control;
    malformed.airtime_fraction = std::numeric_limits<double>::quiet_NaN();
    malformed.power_limit_dbm = 15;
    bool invalid = false;
    try { access.apply(malformed); } catch (const std::invalid_argument&) { invalid = true; }
    require(invalid && hardware->configurations == 1 && !access.ready(),
        "invalid resource contracts cannot mutate hardware or leave admission open");
    access.apply(control);
    auto ticket = access.admission().reserve(500, Clock::now());
    require(ticket.packet_id && access.ready(), "valid control and power readback enable the shared gate");
    std::vector<std::uint8_t> frame{0};
    bool sent = false;
    access.dispatch(frame, [&] { sent = true; }, [&](auto& packet, auto power) {
        require(power == 20.0, "NET decoration gets confirmed configured power");
        packet[0] = 42;
    });
    require(sent && frame[0] == 42, "radio dispatch decorates before enqueue under one lock");
    access.admission().account(ticket.packet_id, 500, Clock::now());
    control.valid_until = Clock::now() - 1ms;
    access.apply(control);
    require(!access.ready(), "expired control cannot enable access");
    sent = false;
    try { access.dispatch(frame, [&] { sent = true; }); } catch (const std::runtime_error&) {}
    require(!sent, "dispatch rechecks freshness even after earlier admission");
    control.valid_until = Clock::now() + 500ms;
    control.power_limit_dbm = 19;
    hardware->missing_readback = true;
    bool failed = false;
    try { access.apply(control); } catch (const std::runtime_error&) { failed = true; }
    require(failed && !access.ready(), "failed readback disables admission and propagates an error");
    hardware->missing_readback = false;
    hardware->delay = 20ms;
    control.valid_until = Clock::now() + 5ms;
    access.apply(control);
    require(!access.ready(), "slow configuration never extends sample validity");
    access.stop();
}
}
int main() {
    test_adaptive_ports();
    test_global_selection();
    test_net_to_cross();
    test_resources_and_facilities();
    test_shared_gate();
    test_access_boundary();
    std::cout << "All DCC architecture integration tests passed\n";
}
