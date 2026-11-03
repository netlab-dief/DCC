#include "cam/cam_message.h"
#include "cam/cam_sender.h"
extern "C" {
#include "CAM.h"
#include "per_decoder.h"
}
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <future>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::string nmea(const std::string& body) {
    unsigned checksum = 0;
    for (unsigned char c : body) checksum ^= c;
    const char* hex = "0123456789ABCDEF";
    return "$" + body + "*" + hex[checksum >> 4] + hex[checksum & 15];
}
void test_position() {
    cam::Position p;
    const auto sentence = nmea("GPRMC,161151.00,A,4437.75625,N,01056.87714,E,10.0,123.4,210325,,,A");
    require(cam::parse_rmc(sentence, p), "valid GPS fix");
    require(p.latitude == 446292708 && p.longitude == 109479523, "coordinates scaled by 1e7");
    require(p.speed == 514 && p.heading == 1234, "speed in cm/s and heading in tenths");
    require(cam::parse_rmc(nmea("GNRMC,161151.00,A,0130.000,S,00130.000,W,0,,210325,,,A"), p), "GN talker and negative coordinates");
    require(p.latitude == -15000000 && p.longitude == -15000000 && p.heading == 3601, "coordinates near zero and unavailable heading");
    require(!cam::parse_rmc(sentence.substr(0, sentence.size() - 2) + "00", p), "invalid checksum rejected");
    require(!cam::parse_rmc(nmea("GPRMC,161151.00,V,,,,,,,210325,,,A"), p), "invalid fix rejected");
    require(!cam::parse_rmc(nmea("GPRMC,161151.00,A,4460.0,N,01056.0,E,0,0,210325,,,A"), p), "invalid minutes rejected");
    require(!cam::parse_rmc(nmea("GPRMC,161151.00,A,4437.0,N,01056.0,E,nan,0,210325,,,A"), p), "nonfinite speed rejected");
    require(cam::parse_rmc(nmea("GPRMC,000000.123,A,0000.0,N,00000.0,E,0,0,010104,,,A"), p) &&
        p.sample_timestamp_ms == 123, "RMC measurement timestamp uses ITS epoch and fractional milliseconds");
    require(!cam::parse_rmc(nmea("GPRMC,000000.00,A,0000.0,N,00000.0,E,0,0,290225,,,A"), p), "invalid calendar date rejected");
    require(cam::parse_rmc(nmea("GPRMC,235959.999,A,0000.0,N,00000.0,E,0,0,290224,,,A"), p), "valid leap day accepted");
    require(!cam::parse_rmc(nmea("GPRMC,250000.00,A,0000.0,N,00000.0,E,0,0,010124,,,A"), p), "invalid UTC hour rejected");
}

void test_encoding() {
    cam::Position p;
    p.speed = 514; p.heading = 1234;
    const auto payload = cam::encode(p, 123456, 0x12345678);
    CAM_t* decoded = nullptr;
    const auto result = uper_decode_complete(nullptr, &asn_DEF_CAM, reinterpret_cast<void**>(&decoded), payload.data(), payload.size());
    require(result.code == RC_OK && decoded, "UPER decode round trip");
    require(decoded->header.stationID == 123456 && decoded->header.messageID == 2, "CAM header");
    require(decoded->cam.generationDeltaTime == 0x5678, "generation time modulo 65536");
    const auto& hf = decoded->cam.camParameters.highFrequencyContainer.choice.basicVehicleContainerHighFrequency;
    require(hf.speed.speedValue == 514 && hf.heading.headingValue == 1234, "encoded units");
    ASN_STRUCT_FREE(asn_DEF_CAM, decoded);
    const std::array<std::uint8_t, 6> mac{1, 2, 3, 4, 5, 6};
    const auto frame = cam::make_frame(p, 123456, mac, 7, 0x12345678);
    require(frame.size() == payload.size() + 86, "no truncation or padding of CAM payload");
    require(frame[2] == 8 && frame[8] == 0x88, "radiotap and QoS data headers");
    require(frame[44] == 0x50, "CAM GN lifetime is at most 1000 ms");
    require(std::equal(mac.begin(), mac.end(), frame.begin() + 18), "radio source MAC");
    require(frame[30] == 0x70 && frame[31] == 0, "802.11 sequence");
    require(frame[50] * 256U + frame[51] == payload.size() + 4, "GN payload length includes BTP");
    require(frame[74] == (514 >> 8) && frame[75] == (514 & 255), "GN speed precedes heading");
    require(frame[82] == 7 && frame[83] == 0xd1, "CAM BTP port 2001");
    require(std::equal(payload.begin(), payload.end(), frame.begin() + 86), "complete UPER bytes");
    require(cam::Options{}.priority == 3 && frame[32] == 3 && frame[33] == 0, "CAM defaults to UP3 / AC_BE");
    require(frame[48] == 2, "CAM GeoNetworking traffic class is TC2");
    require(cam::make_frame(p, 123456, mac, 7, 0x12345678, 3) == frame, "omitted flag equals explicit CAM priority 3");
    const char* expected[]{"AC_BE", "AC_BK", "AC_BK", "AC_BE", "AC_VI", "AC_VI", "AC_VO", "AC_VO"};
    for (std::uint8_t priority = 0; priority < 8; ++priority) {
        const auto prioritized = cam::make_frame(p, 123456, mac, 7, 0x12345678, priority);
        require(std::string(cam::edca_queue(priority)) == expected[priority], "priority maps to EDCA category");
        require(prioritized[32] == priority && prioritized[33] == 0, "QoS control carries selected TID only");
        auto restored = prioritized;
        restored[32] = 3;
        require(restored == frame, "priority preserves all other frame bytes");
    }
    for (const std::uint8_t invalid : {8, 255}) {
        bool rejected = false;
        try { cam::make_frame(p, 123456, mac, 7, 0x12345678, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid priority rejected before encoding");
    }
}

void test_admission() {
    AirtimeGate gate;
    std::atomic<bool> stopping{false};
    int calls = 0;
    cam::Telemetry telemetry;
    gate.configure(0.03, std::chrono::milliseconds(100), AirtimeGate::Clock::now());
    require(cam::Sender::transmit(gate, 1, 500, [&] { ++calls; return 900; }, stopping, {}, {}, &telemetry), "admitted CAM transmitted");
    require(calls == 1, "one send per grant");
    require(telemetry.snapshot().confirmed == 1 && telemetry.snapshot().airtime_us == 900,
        "admission path records only real completion and actual airtime");
    require(gate.suggested_interval_ms() == 100, "TX feedback accounted");
    require(gate.handle("ADMIT 2 500", AirtimeGate::Clock::now()).rfind("WAIT 2 ", 0) == 0, "next packet must wait");
    stopping = true;
    require(!cam::Sender::transmit(gate, 2, 500, [&] { ++calls; return 500; }, stopping, {}, {}, &telemetry), "shutdown cancels waiting transmission");
    require(calls == 1, "no send during shutdown");
    AirtimeGate failed;
    failed.configure(0.03, std::chrono::milliseconds(100), AirtimeGate::Clock::now());
    stopping = false;
    bool threw = false;
    try {
        cam::Sender::transmit(failed, 1, 500, []() -> std::uint64_t { throw std::runtime_error("missing driver feedback"); }, stopping, {}, {}, &telemetry);
    } catch (const std::runtime_error&) { threw = true; }
    require(threw, "uncertain TX fails");
    require(telemetry.snapshot().confirmed == 1 && telemetry.snapshot().tx_errors == 1,
        "shutdown and failed feedback never fabricate TX confirmations");
    require(failed.handle("ADMIT 2 500", AirtimeGate::Clock::now()).rfind("WAIT 2 ", 0) == 0, "uncertain TX keeps gate closed");
    require(failed.handle("REPORT 1 500", AirtimeGate::Clock::now()) == "DONE 1", "no fabricated completion on error");
    AirtimeGate invalid_feedback;
    invalid_feedback.configure(0.03, std::chrono::milliseconds(100), AirtimeGate::Clock::now());
    try {
        cam::Sender::transmit(invalid_feedback, 1, 500, [] { return 0; }, stopping, {}, {}, &telemetry);
        require(false, "invalid airtime must fail");
    } catch (const std::runtime_error&) {}
    require(telemetry.snapshot().confirmed == 1 && telemetry.snapshot().tx_errors == 2,
        "invalid PHY feedback is an error and never contributes to frequency");
    AirtimeGate source_gate;
    source_gate.configure(0.03, std::chrono::milliseconds(100), AirtimeGate::Clock::now());
    int fixes = 0;
    require(cam::Sender::transmit(source_gate, 1, 500, [&] {
        require(fixes == 3, "no transmission until source is ready");
        return 500;
    }, stopping, [&] { return ++fixes == 3; }), "source refreshed while awaiting admission");
    AirtimeGate variable_size;
    variable_size.configure(0.03, std::chrono::milliseconds(25), AirtimeGate::Clock::now());
    require(cam::Sender::transmit(variable_size, 1, 1, [] { return 500; }, stopping,
        [] { return true; }, [] { return 2000; }), "reserve size of selected CAM including LF");
    require(variable_size.suggested_interval_ms() == 67, "reservation uses selected packet rather than stale size estimate");
}

void test_generation_timing() {
    using namespace std::chrono_literals;
    using Timing = cam::GenerationTiming;
    require(Timing::dcc_interval() == 100ms, "missing DCC interval defaults to 100 ms");
    require(Timing::dcc_interval(-1) == 100ms && Timing::dcc_interval(50) == 100ms, "DCC lower bound is 100 ms");
    require(Timing::dcc_interval(200) == 200ms && Timing::dcc_interval(400) == 400ms, "valid DCC intervals preserved");
    require(Timing::dcc_interval(1000) == 1000ms && Timing::dcc_interval(5000) == 1000ms, "DCC generation input capped at 1000 ms");
    Timing timing;
    const Timing::Clock::time_point start{};
    require(timing.ready(start, 1000), "first CAM immediately eligible");
    timing.generated(start);
    require(!timing.ready(start + 99ms, 50), "no second CAM before 100 ms");
    require(timing.ready(start + 100ms, 50), "100 ms measured from generation, independent of delayed TX feedback");
    require(!timing.ready(start + 199ms, 200) && timing.ready(start + 200ms, 200), "reactive DCC lower bound applied");
    require(!timing.ready(start + 399ms, 400) && timing.ready(start + 400ms, 400), "new slower DCC interval applies while waiting");
    require(!timing.ready(start + 999ms, 2000) && timing.ready(start + 1000ms, 2000), "adaptive input limited at generation layer");
    timing.generated(start + 1500ms);
    require(!timing.ready(start + 1500ms, 100) && !timing.ready(start + 1599ms, 100), "late generation does not trigger catch-up burst");
    require(timing.ready(start + 1600ms, 100), "interval resets at actual generation");
    AirtimeGate closed_gate;
    require(closed_gate.handle("ADMIT 1 500", start + 5000ms).rfind("WAIT 1 ", 0) == 0,
        "generation upper clamp never overrides radio admission");
}

void test_generation_policy() {
    using namespace std::chrono_literals;
    using Policy = cam::GenerationPolicy;
    const Policy::Clock::time_point start{};
    cam::Position p;
    p.latitude = 0; p.longitude = 0; p.heading = 0; p.speed = 100;
    Policy stationary;
    auto first = stationary.evaluate(p, start);
    require(first && first->trigger == cam::Trigger::initial && first->low_frequency, "first CAM and LF immediately due");
    stationary.generated(*first, start);
    stationary.transmitted(p);
    require(!stationary.evaluate(p, start + 100ms) && !stationary.evaluate(p, start + 999ms), "stationary CAMs are not sent at 10 Hz");
    auto timed = stationary.evaluate(p, start + 1000ms);
    require(timed && timed->trigger == cam::Trigger::periodic && timed->low_frequency, "stationary CAM at 1 Hz");

    cam::Position changed = p;
    changed.heading = 40;
    require(!stationary.evaluate(changed, start + 100ms), "exactly 4 degrees does not trigger");
    changed.heading = 41;
    require(!stationary.evaluate(changed, start + 99ms), "motion cannot bypass 100 ms minimum");
    require(!stationary.evaluate(changed, start + 399ms, 400), "motion cannot bypass live DCC interval");
    require(stationary.evaluate(changed, start + 400ms, 400)->trigger == cam::Trigger::dynamics, "motion triggers at DCC boundary");
    auto dynamics = stationary.evaluate(changed, start + 100ms);
    require(dynamics && dynamics->trigger == cam::Trigger::dynamics && !dynamics->low_frequency, "heading trigger and LF rate");
    stationary.generated(*dynamics, start + 100ms);
    stationary.transmitted(changed);
    require(stationary.generation_interval() == 100ms, "motion captures actual generation interval");
    for (int i = 1; i <= 3; ++i) {
        const auto time = start + 100ms * (i + 1);
        require(!stationary.evaluate(changed, time - 1ms), "periodic follow-up cannot fire early");
        auto repeat = stationary.evaluate(changed, time);
        require(repeat && repeat->trigger == cam::Trigger::periodic, "three periodic follow-up CAMs");
        stationary.generated(*repeat, time);
        require(stationary.consecutive_periodic() == static_cast<unsigned>(i), "N_GenCam counts timed CAMs");
    }
    require(stationary.generation_interval() == 1000ms, "after N_GenCam reset T_GenCam to 1 second");
    require(!stationary.evaluate(changed, start + 500ms) && stationary.evaluate(changed, start + 1400ms), "return to 1 Hz after motion stops");
    changed.speed += 51;
    auto renewed = stationary.evaluate(changed, start + 500ms);
    require(renewed && renewed->trigger == cam::Trigger::dynamics && renewed->low_frequency, "new motion resets sequence and LF due at 500 ms");
    stationary.generated(*renewed, start + 500ms);
    require(stationary.consecutive_periodic() == 0 && stationary.generation_interval() == 100ms, "new motion restarts fast period");

    changed = p; changed.speed += 50;
    require(!Policy::dynamics_changed(changed, p), "exactly 0.5 m/s does not trigger");
    ++changed.speed;
    require(Policy::dynamics_changed(changed, p), "speed change over 0.5 m/s triggers");
    changed = p; changed.latitude = 359;
    require(!Policy::dynamics_changed(changed, p), "displacement below 4 m does not trigger");
    changed.latitude = 360;
    require(Policy::dynamics_changed(changed, p), "displacement over 4 m triggers");
    changed = p; changed.heading = 3590;
    require(!Policy::dynamics_changed(changed, p), "heading wraps around north");
    changed.heading = 3559;
    require(Policy::dynamics_changed(changed, p), "wrapped heading difference over 4 degrees triggers");
    changed = p; changed.speed = 16383; changed.heading = 3601;
    require(!Policy::dynamics_changed(changed, p) && !Policy::dynamics_changed(p, changed), "unavailable speed/heading never cause false triggers");
    changed = p; changed.longitude = 1799999990;
    auto across = changed; across.longitude = -1799999990;
    require(!Policy::dynamics_changed(changed, across), "dateline crossing uses short geographic distance");

    Policy delayed;
    delayed.generated(*delayed.evaluate(p, start), start);
    delayed.transmitted(p);
    changed = p; changed.speed += 51;
    auto late = delayed.evaluate(changed, start + 750ms, 400);
    delayed.generated(*late, start + 750ms);
    delayed.transmitted(changed);
    require(delayed.generation_interval() == 750ms, "actual elapsed period retained when DCC delays motion trigger");
    require(!delayed.evaluate(changed, start + 1499ms) && delayed.evaluate(changed, start + 1500ms), "follow-up uses retained elapsed period");
    require(!delayed.evaluate(changed, start + 1749ms, 2000) && delayed.evaluate(changed, start + 1750ms, 2000), "DCC clamp still enforced for timed triggers");
}

void test_generation_buffer() {
    using namespace std::chrono_literals;
    const cam::GenerationBuffer::Clock::time_point start{};
    const std::array<std::uint8_t, 6> mac{1, 2, 3, 4, 5, 6};
    cam::Position p;
    p.heading = 0;
    cam::GenerationBuffer buffer(4316, mac);
    require(buffer.generate(p, start, 100000, 50), "construct initial CAM before radio admission");
    auto first = *buffer.latest(start);
    require(first.low_frequency && first.frame[32] == 3, "first pending CAM includes LF and CAM priority");
    require(first.prepare_for_tx(start + 250ms, 4097) && first.frame[44] == (15 << 2) && first.frame[30] == 16,
        "TX preparation subtracts queue age and wraps sequence counter");
    require(first.prepare_for_tx(start + 950ms, 1) && first.frame[44] == 4, "last representable 50 ms lifetime");
    require(!first.prepare_for_tx(start + 951ms, 1), "near-expired packet cannot reach driver with overstated lifetime");
    require(!buffer.generate(p, start + 100ms, 100100, 50), "stationary producer waits 1 second");
    require(buffer.generate(p, start + 1000ms, 101000, 50), "generation continues even without TX feedback");
    auto latest = *buffer.latest(start + 1000ms);
    require(latest.version > first.version && latest.low_frequency, "new CAM replaces old pending CAM and preserves LF");
    buffer.submitted(first.version);
    require(buffer.latest(start + 1000ms)->version == latest.version, "submitting old snapshot cannot discard newer CAM");
    buffer.submitted(latest.version);
    buffer.transmitted(latest);
    require(!buffer.latest(start + 1000ms), "no backlog after submitting latest CAM");
    require(!buffer.generate(p, start + 1999ms, 101999, 50), "radio completion does not reset generation timer");
    require(buffer.generate(p, start + 2000ms, 102000, 50), "stationary deadline preserved after delayed feedback");
    require(!buffer.latest(start + 3000ms), "stale queued CAM expires");
    buffer.invalidate_source();
    require(!buffer.latest(start + 2100ms), "invalid GPS clears pending CAM");
    p.sample_timestamp_ms = 100;
    require(!buffer.generate(p, start + 3000ms, 103000, 50), "old GPS UTC timestamp rejected");
    p.sample_timestamp_ms = 200000;
    require(!buffer.generate(p, start + 3000ms, 103000, 50), "future GPS UTC timestamp rejected");
    p.sample_timestamp_ms.reset();
    require(!buffer.generate(p, start + 3000ms, 103000, 50, start + 2999ms), "expired source cannot create CAM");
    require(buffer.generate(p, start + 3000ms, 103000, 50), "valid source recovers without burst");

    cam::GenerationBuffer fast(4316, mac);
    fast.generate(p, start, 100000, 100);
    auto initial = *fast.latest(start);
    fast.submitted(initial.version); fast.transmitted(initial);
    p.speed += 51;
    require(fast.generate(p, start + 100ms, 100100, 100), "motion creates a fast CAM");
    auto moving = *fast.latest(start + 100ms);
    require(!moving.low_frequency, "LF omitted before 500 ms once previous LF transmitted");
    fast.submitted(moving.version); fast.transmitted(moving);
    require(fast.generate(p, start + 200ms, 100200, 100), "first timed fast follow-up generated");
    auto two = *fast.latest(start + 200ms);
    fast.submitted(two.version); fast.transmitted(two);
    fast.generate(p, start + 300ms, 100300, 100);
    auto three = *fast.latest(start + 300ms);
    fast.submitted(three.version); fast.transmitted(three);
    fast.generate(p, start + 400ms, 100400, 100);
    auto four = *fast.latest(start + 400ms);
    fast.submitted(four.version); fast.transmitted(four);
    require(!fast.generate(p, start + 500ms, 100500, 100), "LF deadline alone does not trigger a CAM");
    require(fast.generate(p, start + 1400ms, 101400, 100) && fast.latest(start + 1400ms)->low_frequency,
        "LF included in first CAM after its 500 ms deadline");
}

void test_low_frequency_encoding() {
    cam::Position p;
    p.sample_timestamp_ms = 100000;
    cam::LowFrequencyData lf;
    lf.exterior_lights = 0x80;
    for (int i = 1; i <= 30; ++i) {
        auto past = p;
        past.latitude -= i * 10;
        past.longitude -= i * 20;
        lf.history.push_back({past, 100000U - static_cast<unsigned>(i) * 100});
    }
    const auto payload = cam::encode(p, 42, 200000, &lf);
    CAM_t* decoded = nullptr;
    const auto result = uper_decode_complete(nullptr, &asn_DEF_CAM, reinterpret_cast<void**>(&decoded), payload.data(), payload.size());
    require(result.code == RC_OK && decoded, "LF CAM UPER round trip");
    require(decoded->cam.generationDeltaTime == (100000 & 65535), "CAM timestamp describes measured position rather than encoding time");
    require(decoded->cam.camParameters.lowFrequencyContainer != nullptr, "LF container encoded");
    const auto& low = decoded->cam.camParameters.lowFrequencyContainer->choice.basicVehicleContainerLowFrequency;
    require(low.vehicleRole == VehicleRole_default && low.exteriorLights.buf[0] == 0x80, "LF vehicle role and exterior lights");
    require(low.pathHistory.list.count == 23, "CAM path history limited to 23 points");
    for (int i = 0; i < 23; ++i) {
        const auto* point = low.pathHistory.list.array[i];
        require(point->pathPosition.deltaLatitude == -10 && point->pathPosition.deltaLongitude == -20,
            "path points are offsets from preceding path point");
        require(point->pathPosition.deltaAltitude == 12800 && *point->pathDeltaTime == 10,
            "unavailable altitude and path time in 10 ms units");
    }
    const auto& hf = decoded->cam.camParameters.highFrequencyContainer.choice.basicVehicleContainerHighFrequency;
    require(hf.longitudinalAcceleration.longitudinalAccelerationValue == 161 && hf.curvature.curvatureValue == 1023 &&
        hf.yawRate.yawRateValue == 32767 && !hf.accelerationControl && !hf.steeringWheelAngle && !hf.lateralAcceleration,
        "unmeasured vehicle sensor values are not fabricated");
    ASN_STRUCT_FREE(asn_DEF_CAM, decoded);
    cam::LowFrequencyData empty;
    const auto basic_lf = cam::encode(p, 42, 200000, &empty);
    decoded = nullptr;
    require(uper_decode_complete(nullptr, &asn_DEF_CAM, reinterpret_cast<void**>(&decoded), basic_lf.data(), basic_lf.size()).code == RC_OK,
        "first CAM supports empty history");
    require(decoded->cam.camParameters.lowFrequencyContainer->choice.basicVehicleContainerLowFrequency.pathHistory.list.count == 0,
        "no invented path history at startup");
    ASN_STRUCT_FREE(asn_DEF_CAM, decoded);
}

void test_generation_during_tx_feedback() {
    using namespace std::chrono_literals;
    const cam::GenerationBuffer::Clock::time_point start{};
    cam::GenerationBuffer buffer(4316, {1, 2, 3, 4, 5, 6});
    cam::Position p;
    buffer.generate(p, start, 100000, 100);
    const auto initial = *buffer.latest(start);
    AirtimeGate gate;
    gate.configure(0.03, 100ms, AirtimeGate::Clock::now());
    std::atomic<bool> stopping{false};
    std::promise<void> waiting, completed;
    auto waiting_signal = waiting.get_future();
    auto completion_signal = completed.get_future();
    auto tx = std::async(std::launch::async, [&] {
        cam::Sender::transmit(gate, 1, 500, [&] {
            buffer.submitted(initial.version);
            waiting.set_value();
            completion_signal.wait();
            return 900;
        }, stopping);
        buffer.transmitted(initial);
    });
    require(waiting_signal.wait_for(2s) == std::future_status::ready, "mock transmitter reached feedback wait");
    require(buffer.generate(p, start + 1000ms, 101000, 100), "generate while previous packet awaits completion");
    require(buffer.generate(p, start + 2000ms, 102000, 100), "continue generating while TX remains pending");
    auto pending = *buffer.latest(start + 2000ms);
    require(pending.version == initial.version + 2, "pending slot contains newest generation only");
    require(gate.handle("ADMIT 2 500", AirtimeGate::Clock::now()).rfind("WAIT 2 ", 0) == 0,
        "independent generation cannot transmit while another reservation is open");
    completed.set_value();
    tx.get();
    require(buffer.latest(start + 2000ms)->version == pending.version, "late feedback preserves newer pending CAM");
}

void test_forced_generation() {
    using namespace std::chrono_literals;
    using Policy = cam::GenerationPolicy;
    const Policy::Clock::time_point start{};
    cam::Position p;
    require(!cam::Options{}.force_100ms, "forced mode disabled by default");
    Policy forced(true);
    forced.generated(*forced.evaluate(p, start, 1000), start);
    forced.transmitted(p);
    for (int i = 1; i <= 15; ++i) {
        const auto now = start + 100ms * i;
        require(!forced.evaluate(p, now - 1ms, 1000), "forced generation cannot run faster than 100 ms");
        auto decision = forced.evaluate(p, now, 1000);
        require(decision && decision->trigger == cam::Trigger::forced, "forced generation ignores motion and DCC interval");
        require(decision->low_frequency == (i % 5 == 0), "forced generation preserves LF 500 ms schedule");
        forced.generated(*decision, now);
        require(forced.generation_interval() == 100ms && forced.consecutive_periodic() == 0,
            "forced cadence does not return to 1 Hz after N_GenCam");
    }
    auto late = forced.evaluate(p, start + 1900ms, 1000);
    forced.generated(*late, start + 1900ms);
    require(!forced.evaluate(p, start + 1900ms, 1000) && !forced.evaluate(p, start + 1999ms, 1000),
        "forced mode does not catch up with a burst after a delay");
    require(forced.evaluate(p, start + 2000ms, 1000).has_value(), "forced cadence restarts after delayed generation");

    cam::GenerationBuffer buffer(4316, {1, 2, 3, 4, 5, 6}, cam::default_priority, true);
    require(buffer.generate(p, start, 100000, 1000), "buffer receives forced generation setting");
    require(buffer.generate(p, start + 100ms, 100100, 1000), "forced CAM generated despite restrictive DCC");
    const auto packet = *buffer.latest(start + 100ms);
    require(packet.version == 2 && packet.trigger == cam::Trigger::forced, "forced mode retains only newest pending CAM");
    AirtimeGate gate;
    require(gate.handle("ADMIT 1 500", AirtimeGate::Clock::now()).rfind("WAIT 1 ", 0) == 0,
        "forced generation does not bypass radio admission");
    require(!buffer.generate(p, start + 200ms, 100200, 1000, start + 199ms), "forced mode still requires valid source");
    require(!buffer.latest(start + 200ms), "invalid source clears forced pending CAM");
}
void test_cam_telemetry() {
    using namespace std::chrono_literals;
    cam::Telemetry telemetry;
    cam::GenerationBuffer buffer(4316, {1, 2, 3, 4, 5, 6}, cam::default_priority, true, &telemetry);
    const cam::GenerationBuffer::Clock::time_point start{};
    cam::Position p;
    require(buffer.generate(p, start, 100000, 100), "telemetry first CAM");
    const auto old = *buffer.latest(start);
    require(buffer.generate(p, start + 100ms, 100100, 100), "telemetry replacement CAM");
    buffer.submitted(old.version);
    require(buffer.latest(start + 100ms).has_value(), "older submission retains replacement");
    require(!buffer.latest(start + 1051ms), "unrepresentable lifetime expires pending CAM");
    require(!buffer.latest(start + 1100ms), "repeated expiry check stays empty");
    require(buffer.generate(p, start + 1200ms, 101200, 100), "new CAM after expiry");
    buffer.invalidate_source();
    buffer.invalidate_source();
    require(buffer.generate(p, start + 1300ms, 101300, 100), "CAM before successful submission");
    const auto packet = *buffer.latest(start + 1300ms);
    buffer.submitted(packet.version);
    telemetry.submitted(100, 40);
    auto data = telemetry.snapshot();
    require(data.generated == 4 && data.replaced == 1 && data.expired == 1 && data.invalidated == 1,
        "lifecycle counts distinguish replacement, expiry and invalidation without double counting");
    require(data.submitted == 1 && data.confirmed == 0, "queueing does not count as TX confirmation");
    telemetry.confirmed(25, 700);
    buffer.transmitted(packet);
    telemetry.count(cam::Telemetry::Event::tx_error);
    telemetry.submitted(300, 60); // A second TX with no successful completion.
    data = telemetry.snapshot();
    require(data.confirmed == 1 && data.tx_errors == 1 && data.airtime_us == 700 &&
            data.queue_ms == 400 && data.gate_ms == 100 && data.feedback_ms == 25,
        "confirmed airtime and timing aggregates exclude missing feedback");

    const auto path = "/tmp/dcc-cam-telemetry-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".csv";
    {
        cam::TelemetryLog log(path);
        const auto now = cam::TelemetryLog::Clock::now() + 1s;
        log.sample(data, now);
        data.confirmed += 2;
        require(std::abs(log.sample(data, now + 1s) - 2.0) < 1e-9,
            "TX Hz uses confirmed delta and actual sampling duration");
    }
    {
        cam::TelemetryLog log(path);
        log.sample({}); // A fresh run appends its own zeroed counters.
    }
    std::ifstream input(path);
    std::string line;
    int headers = 0, rows = 0;
    while (std::getline(input, line)) {
        if (line.rfind("Session_Unix_ms,", 0) == 0) { ++headers; continue; }
        ++rows;
        std::istringstream fields(line);
        std::vector<std::string> values;
        while (std::getline(fields, line, ',')) values.push_back(line);
        require(values.size() == 16, "CAM CSV has complete rows");
        if (rows == 1) {
            require(values[3] == "4" && values[8] == "2" && values[9] == "1" && values[10] == "1",
                "CSV preserves lifecycle counters");
            require(std::stod(values[12]) == 200 && std::stod(values[13]) == 50 &&
                    std::stod(values[14]) == 25 && values[15] == "700",
                "CSV uses submission and confirmation denominators correctly");
        }
    }
    require(headers == 1 && rows == 3, "CSV append writes header once and preserves runs");
    input.close();
    std::remove(path.c_str());

    cam::Telemetry concurrent;
    auto writer = std::async(std::launch::async, [&] {
        for (int i = 0; i < 2000; ++i) concurrent.count(cam::Telemetry::Event::generated);
    });
    for (int i = 0; i < 2000; ++i) concurrent.snapshot();
    writer.get();
    require(concurrent.snapshot().generated == 2000, "concurrent snapshots retain every event");
}
} // namespace
int main() {
    test_position(); test_encoding(); test_admission(); test_generation_timing();
    test_generation_policy(); test_generation_buffer(); test_low_frequency_encoding();
    test_generation_during_tx_feedback();
    test_forced_generation();
    test_cam_telemetry();
    std::cout << "CAM tests passed\n";
}
