#ifndef DCC_MANAGER_H
#define DCC_MANAGER_H

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

// Presumendo le tue inclusioni per la gestione radio e CBR
#include "CBRReader.h"
#include "TxPowerManager.h"
#include "rate_manager.h"

enum class DccState {
    UNKNOWN = 0,
    RELAXED = 1,
    ACTIVE_1 = 2,
    ACTIVE_2 = 3,
    ACTIVE_3 = 4,
    RESTRICTIVE = 5
};

class DccManager {
public:
    DccManager(std::string interface, std::string interface_radiotap, int initial_interval_ms = 100);
    ~DccManager();

    void start();
    void start_limeric();
    void stop();

    float get_current_cbr() const;
    DccState get_current_state() const;
    int get_current_interval_ms() const;

private:
    void dcc_worker_loop();
    void limeric_worker_loop();
    int calculate_limeric_interval(float cbr_percent);

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
    std::atomic<bool> stop_dcc_thread_;
    std::thread dcc_thread_;

    std::atomic<float> last_known_cbr_;
    std::atomic<DccState> current_dcc_state_;
    std::atomic<int> current_interval_ms_;
    double current_tx_rate_ = 10.0; // Inizializzato a 10 Hz (100ms) per LIMERIC

    int sockfd_;
    struct sockaddr_in cam_program_addr_;

    // Oggetti di supporto hardware e rete
    CBRReader cbr_reader;
    RateManager rate_manager;

    // Stream del file di log CSV
    std::ofstream csv_log_file_;

    const int CBR_POLLING_INTERVAL_MS = 100;

    // --- Isteresi / anti-oscillazione ---
    std::chrono::steady_clock::time_point last_transition_time_;
    static constexpr std::chrono::milliseconds MIN_DWELL_TIME{1000}; // 1s minimo per stato
    float cbr_smoothed_ = -1.0f;   // -1 = non ancora inizializzato
    static constexpr float EWMA_ALPHA = 0.3f; // reattività filtro (0=lento, 1=nessun filtro)

    // Helper per calcolo stato con isteresi
    DccState compute_state_with_hysteresis(float cbr_percent, DccState current_state);
};

#endif // DCC_MANAGER_H