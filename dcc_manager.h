#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <fstream>

#include "CBRReader.h"
#include "TxPowerManager.h"
#include "rate_manager.h"

#include <pthread.h> 
#include <cstring>   

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm> // Per std::clamp

#include <fstream>

enum class DccState {
    UNKNOWN,
    RELAXED,
    ACTIVE_1,
    ACTIVE_2,
    ACTIVE_3,
    RESTRICTIVE
};

class DccManager {
public:
    explicit DccManager(std::string interface, std::string interface_radiotap, int initial_interval_ms = 100);
    ~DccManager();

    void start();
    void stop();

    void start_limeric();

    float get_current_cbr() const;
    DccState get_current_state() const;
    int get_current_interval_ms() const;

private:
    void dcc_worker_loop();

    std::string interface_;
    std::string interface_radiotap_; 
    std::thread dcc_thread_;
    std::atomic<bool> stop_dcc_thread_{false};

    std::atomic<float> last_known_cbr_{0.0f};
    std::atomic<DccState> current_dcc_state_{DccState::RELAXED};
    std::atomic<int> current_interval_ms_;

    CBRReader cbr_reader; 
    RateManager rate_manager; 
    
    static constexpr int CBR_POLLING_INTERVAL_MS = 1000; 

    // Membri per la comunicazione Cross-Layer verso il processo CAM
    int sockfd_{-1};
    sockaddr_in cam_program_addr_{};

    // Membri per l'algoritmo LIMERIC
    int calculate_limeric_interval(float cbr_percent);
    void limeric_worker_loop();
    double current_tx_rate_{20.0}; // Frequenza di trasmissione corrente (Hz)
};