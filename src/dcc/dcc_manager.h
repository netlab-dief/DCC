#pragma once

#include "dcc/dcc_access.h"
#include "dcc/dcc_cross.h"
#include "dcc/dcc_facilities.h"
#include "net/dcc_network.h"
#include "cam/cam_sender.h"
#include <atomic>
#include <exception>
#include <fstream>
#include <thread>

// Application lifecycle/composition root. Algorithms live in CROSS, hardware
// and admission in ACC, neighbour exchange in NET, generation advice in FAC.
class DccManager {
public:
    DccManager(std::string interface, std::string interface_radiotap,
        int initial_interval_ms = 100, bool use_fake_cbr = false,
        bool use_dcc_net = false, double eirp_offset_db = 0);
    ~DccManager();
    void start();
    void start_limeric();
    void stop();
    void start_cam(const cam::Options& options);
    void check_cam();
    float get_current_cbr() const;
    DccState get_current_state() const;
    int get_current_interval_ms() const;
    int get_current_bitrate_mbps() const;
private:
    void start_worker(dcc::Mode mode);
    void control_loop();
    std::optional<CbrSample> wait_control(std::uint64_t& cursor);
    void dispatch(std::vector<std::uint8_t>& frame, const std::function<void()>& enqueue);
    bool ready() const;
    std::string get_iso8601_timestamp();
    std::string dcc_state_to_string(DccState state);
    void log_telemetry(const std::string& state_or_mode, float cbr, int interval_ms,
        int tx_power, int bitrate, bool updated);

    std::string interface_, interface_radiotap_;
    bool use_dcc_net_;
    double eirp_offset_db_;
    DccAccess access_;
    DccCross cross_;
    DccNetwork network_;
    std::atomic<bool> stopping_{true};
    std::thread controller_;
    std::atomic<int> current_interval_ms_;
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    std::unique_ptr<cam::Sender> cam_sender_;
    std::unique_ptr<cam::TelemetryLog> cam_telemetry_log_;
    std::ofstream csv_log_file_, net_log_;
};
