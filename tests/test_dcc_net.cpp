#include "dcc_net.h"
#include "dcc_adaptive.h"
#include "dcc_state_machine.h"
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>

using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
// Independent, minimal capture: radiotap / QoS data / SNAP / GN SHB / BTP.
std::vector<std::uint8_t> capture() {
    std::vector<std::uint8_t> frame{
        0,0,8,0,0,0,0,0,
        0x88,0,0,0, 255,255,255,255,255,255, 2,3,4,5,6,7,
        255,255,255,255,255,255, 0,0, 3,0,
        0xaa,0xaa,3,0,0,0,0x89,0x47,
        0x11,0,0x50,1, 0x20,0x50,2,0, 0,4,1,0,
        0x14,0,2,3,4,5,6,7, 0xff,0xff,0xff,0xf0,
        0,0,0,0, 0,0,0,0, 0,0,0,0,
        128,64,0xa0,0, 7,0xd1,0,0
    };
    return frame;
}
CbrSample local(double value, dccnet::Clock::time_point time, unsigned sequence) {
    CbrSample sample;
    sample.value = value; sample.measured_at = time; sample.valid = true; sample.sequence = sequence; sample.utc_slot = sequence;
    return sample;
}
dccnet::Observation observation(unsigned station, double zero, double one, unsigned timestamp = 1) {
    dccnet::Observation o;
    o.address[7] = station; o.local = zero; o.one_hop = one; o.source_timestamp = timestamp;
    return o;
}
void codec() {
    const auto encoded = dccnet::encode(0.5, 1, 20);
    require(encoded == std::array<std::uint8_t, 4>{127,255,160,0}, "ETSI wire bits and floor quantization");
    require(dccnet::encode(0, 0, 40)[2] == 248, "EIRP upper saturation");
    require(dccnet::encode(0, 0, -4)[2] == 0, "negative EIRP lower saturation");
    bool threw = false;
    try { dccnet::encode(std::numeric_limits<double>::quiet_NaN(), 0, 0); }
    catch (const std::invalid_argument&) { threw = true; }
    require(threw, "NaN rejected");
    auto frame = capture();
    require(frame.size() == 86, "independent fixture length");
    auto o = dccnet::parse(frame);
    require(o && near(o->local, 128 / 255.0) && near(o->one_hop, 64 / 255.0) && o->tx_eirp_dbm == 20,
            "decode captured extension");
    require(o->address[2] == 2 && o->source_timestamp == 0xfffffff0U && !o->rssi_dbm, "source and optional RSSI");
    const auto before = frame;
    dccnet::write_extension(frame, 0.5, 1, 23);
    require(frame[78] == 127 && frame[79] == 255 && frame[80] == 184 && frame[81] == 0, "patch exact SHB extension");
    for (unsigned i = 0; i < frame.size(); ++i)
        if (i < 78 || i > 81) require(frame[i] == before[i], "patch preserves CAM and other headers");
    dccnet::write_extension(frame, 0, 0, 0);
    require(dccnet::parse(frame).has_value(), "all-zero extension has legitimate values, no invented presence flag");
    for (unsigned n = 0; n < before.size(); ++n) {
        auto truncated = before; truncated.resize(n);
        require(!dccnet::parse(truncated), "all truncated capture prefixes rejected");
    }
    for (auto offset : {0, 8, 35, 41, 42, 47, 52}) {
        auto bad = before; bad[offset] = 0xff;
        require(!dccnet::parse(bad), "invalid radiotap/MAC/SNAP/GN rejected");
    }
    // Radiotap with two presence words; TSFT(align 8), flags, rate, channel,
    // signed RSSI. Remaining frame is identical and includes a captured FCS.
    frame = before;
    const std::vector<std::uint8_t> rt{0,0,31,0,0x2f,0,0,0x80, 0,0,0,0,
        0,0,0,0, 0,0,0,0,0,0,0,0, 0x10,12,0x0c,0x17,0,0,0xd6};
    frame.erase(frame.begin(), frame.begin() + 8);
    frame.insert(frame.begin(), rt.begin(), rt.end());
    frame.insert(frame.end(), {0,0,0,0});
    o = dccnet::parse(frame);
    require(o && o->rssi_dbm == -42 && o->frequency_mhz == 5900, "extended bitmap, field alignment, RSSI, channel, FCS");
    frame[24] |= 0x40;
    require(!dccnet::parse(frame), "bad FCS discarded");
    frame = before;
    frame[9] = 0x80; // QoS + Order: insert HT Control
    frame.insert(frame.begin() + 34, 4, 0);
    require(dccnet::parse(frame).has_value(), "variable MAC HT header");
    frame = before;
    frame[8] = 8; // non-QoS data
    frame.erase(frame.begin() + 32, frame.begin() + 34);
    require(dccnet::parse(frame).has_value(), "ordinary data header");
    frame = before; frame[9] |= 0x40;
    require(!dccnet::parse(frame), "protected MAC payload not interpreted as plaintext");
    frame = before; frame[44] = 1;
    require(!dccnet::parse(frame), "zero lifetime multiplier rejected for every time base");
    frame = before; frame[42] = 0x12;
    require(!dccnet::parse(frame), "secured GN not interpreted as plaintext");
    frame = before; frame[30] = 1;
    require(!dccnet::parse(frame), "fragment discarded");
    frame = before; frame[32] |= 0x80;
    require(!dccnet::parse(frame), "A-MSDU discarded");
}
void aggregation() {
    dccnet::State state;
    const auto t = dccnet::Clock::now();
    auto s = state.trigger(local(0.3, t, 1), t, 1);
    require(!s.control.valid && s.one_hop == 0 && s.two_hop == 0, "startup has no previous interval");
    s = state.trigger(local(0.4, t + 100ms, 2), t + 100ms, 2);
    require(s.control.valid && near(s.global, 0.3) && near(s.local, 0.4), "previous trigger's local CBR, not current");
    require(state.receive(observation(1, 0.9, 0.8), t + 100ms), "first neighbor");
    s = state.trigger(local(0.4, t + 200ms, 3), t + 200ms, 3);
    require(near(s.global, 0.9) && near(s.one_hop, 0.9) && near(s.two_hop, 0.8), "high average selects maxima");
    require(state.receive(observation(2, 0.2, 0.1), t + 200ms), "second neighbor");
    s = state.trigger(local(0.1, t + 300ms, 4), t + 300ms, 4);
    require(near(s.one_hop, 0.2) && near(s.two_hop, 0.1) && near(s.global, 0.4), "low average selects second largest independently");
    require(state.receive(observation(3, 0.9, 0.8), t + 300ms), "equal maximum from another station");
    s = state.trigger(local(0.1, t + 400ms, 5), t + 400ms, 5);
    require(near(s.one_hop, 0.9), "second largest counts distinct entries with equal values");
    require(!state.receive(observation(1, 0.1, 0.1), t + 900ms), "duplicate does not refresh lifetime or values");
    s = state.trigger(local(0.1, t + 1100ms, 6), t + 1100ms, 11);
    require(s.neighbors == 2 && !s.control.valid, "expire at one second and invalidate missed trigger");
    s = state.trigger(local(0.1, t + 1200ms, 7), t + 1200ms, 12);
    require(s.neighbors == 1 && near(s.one_hop, 0.9), "independent expiration");
    s = state.trigger(local(0.1, t + 1300ms, 8), t + 1300ms, 13);
    require(s.neighbors == 0 && s.one_hop == 0 && s.two_hop == 0 && near(s.global, 0.1), "no sticky neighbor load after expiration");
    state.reset();
    state.receive(observation(1, 0.6, 0.2), t);
    s = state.trigger(local(0.1, t, 1), t, 0);
    require(s.one_hop == 0 && s.two_hop == 0, "single low neighbor: missing second largest is zero");
    state.receive(observation(2, 0.64, 0.3), t);
    s = state.trigger(local(0.1, t + 100ms, 2), t + 100ms, 1);
    require(near(s.one_hop, 0.6), "average exactly CTH takes second largest branch");
}
void freshness_and_wrap() {
    dccnet::State state;
    const auto t = dccnet::Clock::now();
    auto o = observation(1, 0.8, 0.7, 0xfffffff0U);
    require(state.receive(o, t), "timestamp before wrap");
    o.source_timestamp = 16;
    require(state.receive(o, t + 100ms), "timestamp wrap advances");
    o.source_timestamp = 15;
    require(!state.receive(o, t + 200ms), "out-of-order update discarded");
    auto entries = state.neighbors();
    require(entries.size() == 1 && entries[0].received_at == t + 100ms && entries[0].observation.source_timestamp == 16,
            "rejected packets preserve local/source timestamps");
    state.trigger(local(0.3, t, 1), t + 90ms, 0);
    auto s = state.trigger(local(0.4, t + 100ms, 2), t + 190ms, 1);
    require(s.control.valid && near(s.global, 0.8), "late random phase still accepts previous interval");
    s = state.trigger(local(0.4, t + 100ms, 2), t + 200ms, 2);
    require(!s.control.valid, "same local sample cannot produce two control updates");
    s = state.trigger({}, t + 300ms, 3);
    require(!s.control.valid, "remote load cannot mask invalid local sample");
    s = state.trigger(local(0.4, t + 400ms, 4), t + 400ms, 4);
    require(!s.control.valid, "first local sample after outage only establishes baseline");
    s = state.trigger(local(0.4, t + 500ms, 5), t + 500ms, 5);
    require(s.control.valid, "next complete interval recovers");
    std::uint64_t cursor = 0;
    const auto next = state.wait_next(cursor);
    require(next && cursor == s.control.sequence && next->control.utc_slot == 5, "consumer gets global control sample once");
    auto waiting = std::async(std::launch::async, [&] { return state.wait_next(cursor); });
    state.stop();
    require(waiting.wait_for(100ms) == std::future_status::ready && !waiting.get(), "stop wakes waiting controller");
}
void controllers() {
    dccnet::State state;
    DccAdaptive adaptive;
    DccStateMachine reactive;
    auto t = dccnet::Clock::now();
    for (unsigned i = 0; i <= 4; ++i) {
        state.receive(observation(1, 0.9, 0.8, i + 1), t + i * 100ms);
        const auto s = state.trigger(local(0.1, t + i * 100ms, i + 1), t + i * 100ms, i);
        if (!s.control.valid) continue;
        require(reactive.update(s.control.value * 100, s.control.measured_at) == DccState::RESTRICTIVE,
                "reactive uses remote congestion despite low local load");
        adaptive.observe(s.control.value, s.control.utc_slot);
    }
    require(adaptive.filtered_cbr() > 0.6, "adaptive filtering consumes global samples at its existing 200ms cadence");
}
}
int main() {
    codec(); aggregation(); freshness_and_wrap(); controllers();
    std::cout << "DCC_NET tests passed\n";
}
