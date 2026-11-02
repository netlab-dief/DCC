#pragma once
#include "airtime_gate.h"
#include <atomic>
#include <thread>

class GateServer {
public:
    explicit GateServer(AirtimeGate& gate, unsigned short port = 5556) : gate_(gate), port_(port) {}
    ~GateServer() { stop(); }
    void start();
    void stop();
    unsigned short port() const { return port_; }
private:
    void run();
    AirtimeGate& gate_;
    unsigned short port_;
    int fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
