#pragma once
#include "net/dcc_net.h"
#include <atomic>
#include <exception>
#include <functional>
#include <string>
#include <thread>

// One ITS-G5 radio/channel per instance. The monitor must stay on this channel.
class GnReceiver {
public:
    explicit GnReceiver(dccnet::State& state) : state_(state) {}
    ~GnReceiver() { stop(); }
    void start(const std::string& radio, const std::string& survey, std::function<std::optional<CbrSample>()> local);
    void stop();
    void check();
    bool stopped() const { return stopping_.load(); }
private:
    void run();
    dccnet::State& state_;
    std::function<std::optional<CbrSample>()> local_;
    [[maybe_unused]] std::array<std::uint8_t, 6> own_mac_{};
    int fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::exception_ptr failure_;
};
