#ifndef DCC_MANAGER_H
#define DCC_MANAGER_H

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <limits>

// Presumendo le tue inclusioni per la gestione radio e CBR
#include "CBRReader.h"
#include "TxPowerManager.h"
#include "rate_manager.h"
#include "dcc_state_machine.h"
#include "dcc_adaptive.h"
#include "cbr_sampler.h"
#include "gn_receiver.h"
#include "cam/cam_sender.h"
#include <memory>

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
    void start_worker(bool adaptive);
    float acquire_cbr();
    std::optional<CbrSample> wait_control(std::uint64_t& cursor);
    void update_power(TxPowerManager& manager, int power);
    bool net_ready();
    void dispatch_net(std::vector<std::uint8_t>& frame, const std::function<void()>& enqueue);
    void dcc_worker_loop();
    void limeric_worker_loop();

    // Funzioni helper interne per il logging e timestamp
    std::string get_iso8601_timestamp();
    std::string dcc_state_to_string(DccState state);
    void log_telemetry(const std::string& state_or_mode, 
                       float cbr, 
                       int interval_ms, 
                       int tx_power, 
                       int mcs, 
                       bool hw_updated);

    std::string interface_;
    std::string interface_radiotap_;
    bool use_fake_cbr_;
    bool use_dcc_net_;
    double eirp_offset_db_;
    dccnet::State net_state_;
    GnReceiver net_receiver_{net_state_};
    std::ofstream net_log_;
    std::mutex power_mutex_;
    std::optional<double> confirmed_power_;
    bool power_failed_ = false;
    bool last_cbr_from_file_ = false; // Accessed only by the acquisition thread.
    std::atomic<bool> stop_dcc_thread_;
    std::thread dcc_thread_;

    std::atomic<DccState> current_dcc_state_;
    std::atomic<int> current_interval_ms_;
    std::atomic<int> current_bitrate_mbps_{6};
    double current_tx_rate_ = 10.0; // Inizializzato a 10 Hz (100ms) per LIMERIC

    // Oggetti di supporto hardware e rete
    CBRReader cbr_reader;
    CbrSampler cbr_sampler_;
    RateManager rate_manager;

    AirtimeGate airtime_gate_;
    std::unique_ptr<cam::Sender> cam_sender_;
    std::unique_ptr<cam::TelemetryLog> cam_telemetry_log_;

    // Stream del file di log CSV
    std::ofstream csv_log_file_;

};

#endif // DCC_MANAGER_H
