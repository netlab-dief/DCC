#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <fstream>

// Sostituisci con i reali path dei tuoi header per CBR e PowerManager
#include "CBRReader.h"
#include "TxPowerManager.h"

#include <pthread.h> // <--- Serve questo per le funzioni POSIX
#include <cstring>   // Per std::strerror

#include "rate_manager.h" // <-- AGGIUNGI

enum class DccState {
    UNKNOWN,
    RELAXED,
    ACTIVE,
    RESTRICTIVE
};

class DccManager {
public:
    explicit DccManager(std::string interface, std::string interface_radiotap, int initial_interval_ms = 100);
    ~DccManager();

    // Avvia e ferma il thread di monitoraggio
    void start();
    void stop();

    // Metodi thread-safe per leggere lo stato attuale da altri moduli
    float get_current_cbr() const;
    DccState get_current_state() const;
    int get_current_interval_ms() const;

private:
    void dcc_worker_loop();
    //void set_fixed_rate(int rate_idx); // DEPRECATED: This function is no longer used since we now use RateManager to set the rate

    std::string interface_;
    std::string interface_radiotap_; // <-- Nuova variabile per l'interfaccia radiotap
    std::thread dcc_thread_;
    std::atomic<bool> stop_dcc_thread_{false};

    // Stati condivisi atomici
    std::atomic<float> last_known_cbr_{0.0f};
    std::atomic<DccState> current_dcc_state_{DccState::RELAXED};
    std::atomic<int> current_interval_ms_;

    // Ipotizziamo i tipi delle tue classi di supporto
    CBRReader cbr_reader; 
    RateManager rate_manager; // <-- AGGIUNGI sulla falsariga di cbr_reader
    
    // Costanti di configurazione DCC
    static constexpr double CBR_LIMIT_ACTIVE = 30.0;
    static constexpr double CBR_LIMIT_RESTRICTIVE = 60.0;
    static constexpr int CBR_POLLING_INTERVAL_MS = 1000; // Regola in base alle tue esigenze
};