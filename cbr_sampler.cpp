#include "cbr_sampler.h"

#include <cmath>
#include <stdexcept>
#include <utility>

void CbrSampler::start(Read read) {
    if (thread_.joinable()) return;
    if (!read) throw std::invalid_argument("CBR acquisition callback required");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        read_ = std::move(read);
        stopping_ = false;
    }
    try { thread_ = std::thread(&CbrSampler::run, this); }
    catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        throw;
    }
}

void CbrSampler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::optional<CbrSample> CbrSampler::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || samples_.empty()) return std::nullopt;
    return samples_.back();
}

std::optional<CbrSample> CbrSampler::wait_next(std::uint64_t& cursor, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout, [&] {
        return stopping_ || (!samples_.empty() && samples_.back().sequence > cursor);
    });
    if (stopping_) return std::nullopt;
    for (const auto& sample : samples_) {
        if (sample.sequence > cursor) {
            cursor = sample.sequence;
            return sample;
        }
    }
    return std::nullopt;
}

void CbrSampler::run() {
    using namespace std::chrono;
    bool have_previous = false;
    std::int64_t previous_slot = 0;
    std::uint64_t sequence = 0;
    while (true) {
        // UTC defines the phase; an interruptible steady wait prevents a clock
        // adjustment from making shutdown wait for the wall clock to catch up.
        const auto utc_now = system_clock::now();
        const auto slot = duration_cast<milliseconds>(utc_now.time_since_epoch()).count() / 100 + 1;
        const auto boundary = system_clock::time_point(milliseconds(slot * 100));
        const auto deadline = steady_clock::now() + (boundary - utc_now);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (changed_.wait_until(lock, deadline, [&] { return stopping_; })) break;
        }
        CbrSample sample;
        sample.utc_slot = slot;
        sample.sequence = ++sequence;
        try { sample.value = read_(); }
        catch (...) { sample.value = -1.0f; }
        sample.measured_at = steady_clock::now();
        const auto finished_slot = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count() / 100;
        sample.valid = have_previous && previous_slot + 1 == slot && finished_slot == slot &&
            std::isfinite(sample.value) && sample.value >= 0.0f && sample.value <= 1.0f;
        // A late/missed read changes the measurement window; prime a new pair
        // instead of labelling an aggregate measurement as a 100 ms sample.
        previous_slot = finished_slot;
        have_previous = finished_slot == slot && std::isfinite(sample.value) &&
            sample.value >= 0.0f && sample.value <= 1.0f;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
            samples_.push_back(sample);
            if (samples_.size() > 16) samples_.pop_front();
        }
        changed_.notify_all();
    }
}
