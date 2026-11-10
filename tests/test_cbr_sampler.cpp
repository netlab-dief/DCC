#include "radio/cbr_sampler.h"
#include "dcc/dcc_adaptive.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
CbrSample take(CbrSampler& sampler, std::uint64_t& cursor) {
    const auto result = sampler.wait_next(cursor, 2s);
    require(result.has_value(), "sampler publishes within deadline");
    return *result;
}

void test_freshness() {
    CbrSample sample;
    sample.valid = true;
    sample.measured_at = CbrSample::Clock::time_point{} + 1s;
    require(sample.fresh(sample.measured_at), "new measurement is fresh");
    require(sample.fresh(sample.measured_at + 150ms), "freshness boundary inclusive");
    require(!sample.fresh(sample.measured_at + 151ms), "old measurement rejected");
    require(!sample.fresh(sample.measured_at - 1ms), "future timestamp rejected");
    sample.valid = false;
    require(!sample.fresh(sample.measured_at), "invalid sample never fresh");
}

void test_acquisition_independent_of_consumer() {
    CbrSampler sampler;
    std::mutex mutex;
    std::condition_variable called;
    int reads = 0;
    sampler.start([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++reads;
        called.notify_all();
        return 0.4f;
    });
    // Simulate a controller occupied with radio configuration: do not consume.
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(called.wait_for(lock, 2s, [&] { return reads >= 5; }),
                "measurement continues while controller is blocked");
    }
    std::uint64_t cursor = 0;
    const auto first = take(sampler, cursor);
    require(first.sequence == 1 && !first.valid, "first read primes measurement baseline");
    DccAdaptive controller;
    bool updated = false;
    auto previous = first;
    for (int i = 0; i < 4; ++i) {
        const auto next = take(sampler, cursor);
        require(next.sequence == previous.sequence + 1, "buffer preserves every sample in order");
        require(next.utc_slot == previous.utc_slot + 1, "acquisition uses consecutive 100ms slots");
        require(next.measured_at > previous.measured_at, "monotonic measurement timestamps");
        if (next.valid) updated = controller.observe(next.value, next.utc_slot) || updated;
        previous = next;
    }
    require(updated, "adaptive controller consumes two distinct windows");
    require(sampler.latest()->sequence >= previous.sequence, "monitor sees acquisition independently");
    const auto stopped_at = CbrSample::Clock::now();
    sampler.stop();
    require(CbrSample::Clock::now() - stopped_at < 500ms, "idle sampler stop interrupts timer");
    require(!sampler.latest(), "stopped sampler does not expose old samples");
    require(!sampler.wait_next(cursor, 2s), "stop wakes consumers without replaying data");
    sampler.start([] { return 0.6f; });
    cursor = 0;
    const auto restarted = take(sampler, cursor);
    require(restarted.sequence == 1 && !restarted.valid, "restart resets buffer and measurement baseline");
}

void test_invalid_reads_and_recovery() {
    CbrSampler sampler;
    int reads = 0;
    sampler.start([&]() -> float {
        ++reads;
        if (reads == 2) return std::numeric_limits<float>::quiet_NaN();
        if (reads == 3) throw std::runtime_error("driver failure");
        return 0.4f;
    });
    std::uint64_t cursor = 0;
    require(!take(sampler, cursor).valid, "initial baseline invalid");
    require(!take(sampler, cursor).valid, "NaN explicitly published invalid");
    require(!take(sampler, cursor).valid, "read exception published invalid without killing acquisition");
    require(!take(sampler, cursor).valid, "first successful read after error reprimes counters");
    require(take(sampler, cursor).valid, "subsequent complete window recovers");
}

void test_late_acquisition() {
    CbrSampler sampler;
    int reads = 0;
    sampler.start([&] {
        if (++reads == 1) std::this_thread::sleep_for(220ms);
        return 0.4f;
    });
    std::uint64_t cursor = 0;
    const auto late = take(sampler, cursor);
    require(!late.valid, "read crossing its UTC slot is invalid");
    const auto priming = take(sampler, cursor);
    require(!priming.valid, "late read cannot become a fabricated 100ms window");
    require(priming.utc_slot >= late.utc_slot + 3, "missed deadlines skipped without catch-up bursts");
    require(take(sampler, cursor).valid, "acquisition recovers after late window");
}
}

int main() {
    test_freshness();
    test_acquisition_independent_of_consumer();
    test_invalid_reads_and_recovery();
    test_late_acquisition();
    std::cout << "All independent CBR acquisition tests passed\n";
}
