#include "dcc_manager.h"

DccManager::DccManager(std::string interface, std::string interface_radiotap, int initial_interval_ms)
    : interface_(std::move(interface)), 
      interface_radiotap_(std::move(interface_radiotap)), // <--- Inizializza la nuova variabile
      current_interval_ms_(initial_interval_ms),
      last_known_cbr_(0.0f),                  
      current_dcc_state_(DccState::UNKNOWN)   
{}

DccManager::~DccManager() {
    stop();
}

void DccManager::start() {
    if (!dcc_thread_.joinable()) {
        stop_dcc_thread_ = false;
        dcc_thread_ = std::thread(&DccManager::dcc_worker_loop, this);

        pthread_t native_handle = dcc_thread_.native_handle();

        struct sched_param param;
        // Impostiamo una priorità Real-Time alta (es. 70).
        // Il thread principale è attorno a 80, quindi DCC sarà subito dietro.
        param.sched_priority = 70;
        
        // CORRETTO: Rimosso il doppio posix call duplicato che avevi nell'if
        if (pthread_setschedparam(native_handle, SCHED_FIFO, &param) != 0) {
            std::cerr << "Errore nell'impostazione della priorità del thread DCC" << std::endl;
        }
    }
}

void DccManager::stop() {
    stop_dcc_thread_ = true;
    if (dcc_thread_.joinable()) {
        dcc_thread_.join();
    }
}

float DccManager::get_current_cbr() const {
    return last_known_cbr_.load(std::memory_order_relaxed);
}

DccState DccManager::get_current_state() const {
    return current_dcc_state_.load(std::memory_order_relaxed);
}

int DccManager::get_current_interval_ms() const {
    return current_interval_ms_.load(std::memory_order_relaxed);
}

void DccManager::dcc_worker_loop() {
    const double CBR_LIMIT_ACTIVE = 30.0;      // Oltre il 30% il canale si sta occupando
    const double CBR_LIMIT_RESTRICTIVE = 60.0; // Oltre il 60% siamo in forte congestione
    TxPowerManager powerManager;
    cbr_reader.setupCBRReader(false, interface_);
    rate_manager.setup_rate_manager(interface_radiotap_); // <-- Usa l'interfaccia radiotap per il RateManager

    while (!stop_dcc_thread_) {
        cbr_reader.wrapper_start_reading_cbr();
        float raw_cbr = cbr_reader.get_current_cbr();

        if (raw_cbr >= 0.0f) {
            float cbr_percent = raw_cbr * 100.0f;
            last_known_cbr_.store(cbr_percent, std::memory_order_relaxed);

            DccState old_state = current_dcc_state_.load(std::memory_order_relaxed);
            DccState new_state = old_state;
            int new_interval = current_interval_ms_.load(std::memory_order_relaxed);

            // Determina solo il nuovo stato
            if (cbr_percent < CBR_LIMIT_ACTIVE) {
                new_state = DccState::RELAXED;
            } else if (cbr_percent < CBR_LIMIT_RESTRICTIVE) {
                new_state = DccState::ACTIVE;
            } else {
                new_state = DccState::RESTRICTIVE;
            }

            // Adesso al primo ciclo 'old_state' è UNKNOWN e 'new_state' sarà (es.) RELAXED. 
            // Essendo diversi, entrerà nell'if e configurerà i parametri di rete istantaneamente.
            if (old_state != new_state) {
                current_dcc_state_.store(new_state, std::memory_order_relaxed);
                current_interval_ms_.store(new_interval, std::memory_order_relaxed);

                switch (new_state) {
                    case DccState::RELAXED:
                        powerManager.set_tx_power(interface_, 33);
                        rate_manager.set_fixed_rate(6);
                        break;
                    case DccState::ACTIVE:
                        powerManager.set_tx_power(interface_, 15);
                        rate_manager.set_fixed_rate(12);
                        break;
                    case DccState::RESTRICTIVE:
                        powerManager.set_tx_power(interface_, 1);
                        rate_manager.set_fixed_rate(24);
                        break;
                    case DccState::UNKNOWN:
                        break;
                }

                std::cout << "\033[1;33m[DCC CAMBIO STATO] CBR: " << std::fixed <<  std::setprecision(2) 
                          << cbr_percent << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(CBR_POLLING_INTERVAL_MS));
    }
}