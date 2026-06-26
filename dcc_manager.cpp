#include "dcc_manager.h"

DccManager::DccManager(std::string interface, std::string interface_radiotap, int initial_interval_ms)
    : interface_(std::move(interface)), 
      interface_radiotap_(std::move(interface_radiotap)),
      stop_dcc_thread_(false),
      last_known_cbr_(0.0f),
      current_dcc_state_(DccState::UNKNOWN), 
      current_interval_ms_(initial_interval_ms)   
{
    // Inizializzazione Socket UDP per la notifica Cross-Layer
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        std::cerr << "[DCC ERROR] Impossibile creare il socket UDP per il cross-layer" << std::endl;
    }

    std::memset(&cam_program_addr_, 0, sizeof(cam_program_addr_));
    cam_program_addr_.sin_family = AF_INET;
    cam_program_addr_.sin_port = htons(5555); // Porta d'ascolto sul programma CAM
    cam_program_addr_.sin_addr.s_addr = inet_addr("127.0.0.1"); // Loopback locale
}

DccManager::~DccManager() {
    stop();
    if (sockfd_ >= 0) {
        close(sockfd_);
    }
}

void DccManager::start() {
    if (!dcc_thread_.joinable()) {
        stop_dcc_thread_ = false;
        dcc_thread_ = std::thread(&DccManager::dcc_worker_loop, this);

        pthread_t native_handle = dcc_thread_.native_handle();
        struct sched_param param;
        param.sched_priority = 70;
        
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
    const double CBR_LIMIT_ACTIVE_1 = 30.0;      
    const double CBR_LIMIT_ACTIVE_2 = 40.0;      
    const double CBR_LIMIT_ACTIVE_3 = 50.0;      
    const double CBR_LIMIT_RESTRICTIVE = 65.0; 
    
    TxPowerManager powerManager;
    cbr_reader.setupCBRReader(false, interface_);
    rate_manager.setup_rate_manager(interface_radiotap_); 

    while (!stop_dcc_thread_) {
        cbr_reader.wrapper_start_reading_cbr();
        float raw_cbr = cbr_reader.get_current_cbr();

        if (raw_cbr >= 0.0f) {
            float cbr_percent = raw_cbr * 100.0f;
            last_known_cbr_.store(cbr_percent, std::memory_order_relaxed);

            DccState old_state = current_dcc_state_.load(std::memory_order_relaxed);
            DccState new_state = old_state;
            int new_interval = current_interval_ms_.load(std::memory_order_relaxed);

            // Mappatura degli stati in base al CBR
            if (cbr_percent < CBR_LIMIT_ACTIVE_1) {
                new_state = DccState::RELAXED;
            } else if (cbr_percent < CBR_LIMIT_ACTIVE_2) {
                new_state = DccState::ACTIVE_1;
            } else if (cbr_percent < CBR_LIMIT_ACTIVE_3) {
                new_state = DccState::ACTIVE_2;
            } else if (cbr_percent < CBR_LIMIT_RESTRICTIVE) {
                new_state = DccState::ACTIVE_3;
            } else {
                new_state = DccState::RESTRICTIVE;
            }

            // Se lo stato è cambiato (o al primo avvio in UNKNOWN), aggiorna l'hardware e notifica
            if (old_state != new_state) {
                
                // Mappatura unificata: Potenza, MCS e Intervallo di silenzio ETSI (T_off)
                switch (new_state) {
                    case DccState::RELAXED:
                        new_interval = 50; // Max 20 Hz teorici a livello radio (il CAM si fermerà a 100ms/10Hz)
                        powerManager.set_tx_power(interface_, 23); 
                        rate_manager.set_fixed_rate_netlink(12); 
                        break;
                    case DccState::ACTIVE_1:
                        new_interval = 100; // 10 Hz massimo
                        powerManager.set_tx_power(interface_, 19);
                        rate_manager.set_fixed_rate_netlink(12); 
                        break;
                    case DccState::ACTIVE_2:
                        new_interval = 200; // 5 Hz massimo
                        powerManager.set_tx_power(interface_, 16);
                        rate_manager.set_fixed_rate_netlink(18); 
                        break;
                    case DccState::ACTIVE_3:
                        new_interval = 400; // 2.5 Hz massimo
                        powerManager.set_tx_power(interface_, 13);
                        rate_manager.set_fixed_rate_netlink(24); 
                        break;
                    case DccState::RESTRICTIVE:
                        new_interval = 1000; // 1 Hz minimo (Starvation prevention)
                        powerManager.set_tx_power(interface_, 10);
                        rate_manager.set_fixed_rate_netlink(24); 
                        break;
                    case DccState::UNKNOWN:
                        break;
                }

                current_dcc_state_.store(new_state, std::memory_order_relaxed);
                current_interval_ms_.store(new_interval, std::memory_order_relaxed);

                // NOTIFICA CROSS-LAYER AL PROGRAMMA CAM
                //if (sockfd_ >= 0) {
                //    double t_off_to_send = static_cast<double>(new_interval);
                //    sendto(sockfd_, &t_off_to_send, sizeof(t_off_to_send), 0,
                //           (struct sockaddr*)&cam_program_addr_, sizeof(cam_program_addr_));
                //}
                if (sockfd_ >= 0) {
                    // Castiamo l'enum class a un intero a 32 bit per la trasmissione
                    int32_t state_to_send = static_cast<int32_t>(new_state);

                    sendto(sockfd_, &state_to_send, sizeof(state_to_send), 0,
                           (struct sockaddr*)&cam_program_addr_, sizeof(cam_program_addr_));
                }

                std::cout << "\033[1;33m[DCC CAMBIO STATO] Stato: " << static_cast<int>(new_state)
                          << " | CBR: " << std::fixed << std::setprecision(2) << cbr_percent 
                          << "% | T_off: " << new_interval << "ms\033[0m" << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(CBR_POLLING_INTERVAL_MS));
    }
}


// LIMERIC ALGORITHM IMPLEMENTATION
// Funzione di utilità per mappare linearmente un valore tra due range
static double map_linear(double x, double in_min, double in_max, double out_min, double out_max) {
    if (in_max == in_min) return out_min;
    double mapped = out_min + (x - in_min) * (out_max - out_min) / (in_max - in_min);
    // Applica il clamping per evitare l'estrapolazione fuori dai range
    if (out_min < out_max) return std::clamp(mapped, out_min, out_max);
    return std::clamp(mapped, out_max, out_min);
}

void DccManager::start_limeric() {
    if (!dcc_thread_.joinable()) {
        stop_dcc_thread_ = false;
        dcc_thread_ = std::thread(&DccManager::limeric_worker_loop, this);

        pthread_t native_handle = dcc_thread_.native_handle();
        struct sched_param param;
        param.sched_priority = 70; // Alta priorità Real-Time
        
        if (pthread_setschedparam(native_handle, SCHED_FIFO, &param) != 0) {
            std::cerr << "Errore nell'impostazione della priorità del thread LIMERIC" << std::endl;
        }
    }
}

int DccManager::calculate_limeric_interval(float cbr_percent) {
    const double CBR_TARGET = 0.68; // Target ETSI: 68% di occupazione del canale
    const double ALPHA = 0.016;     // Coefficiente di smorzamento
    const double BETA = 0.0012;     // Guadagno dell'algoritmo
    
    double cbr_fraction = static_cast<double>(cbr_percent) / 100.0;

    // Equazione di controllo dinamico LIMERIC
    double new_rate = (1.0 - ALPHA) * current_tx_rate_ + BETA * (CBR_TARGET - cbr_fraction);

    // Vincoli ETSI sui limiti di trasmissione (Max 40 Hz, Min 1 Hz)
    const double MAX_RATE = 40.0; 
    const double MIN_RATE = 1.0;  
    new_rate = std::clamp(new_rate, MIN_RATE, MAX_RATE);

    current_tx_rate_ = new_rate;

    // Conversione da Frequenza (Hz) a Intervallo di Silenzio (ms) -> T_off = 1000 / Rate
    return static_cast<int>(1000.0 / new_rate);
}

void DccManager::limeric_worker_loop() {
    TxPowerManager powerManager;
    cbr_reader.setupCBRReader(false, interface_);
    rate_manager.setup_rate_manager(interface_radiotap_); 

    // NOTA STANDARD ETSI: Per LIMERIC, l'intervallo di polling dell'algoritmo
    // deve essere tassativamente di 200 ms per garantire la stabilità matematica del ciclo.
    const int LIMERIC_POLLING_MS = 200; 

    std::cout << "[DCC INIT] Loop LIMERIC Adattativo avviato." << std::endl;

    while (!stop_dcc_thread_) {
        cbr_reader.wrapper_start_reading_cbr();
        float raw_cbr = cbr_reader.get_current_cbr();

        if (raw_cbr >= 0.0f) {
            float cbr_percent = raw_cbr * 100.0f;
            last_known_cbr_.store(cbr_percent, std::memory_order_relaxed);

            // 1. Calcola il nuovo intervallo temporale (T_off) tramite LIMERIC
            int new_interval = calculate_limeric_interval(cbr_percent);
            int old_interval = current_interval_ms_.exchange(new_interval, std::memory_order_relaxed);

            // 2. Controllo dinamico e fluido della Potenza (TPC) in base al Rate corrente
            // Se trasmettiamo al massimo (40Hz) usiamo 23dBm, se siamo strozzati a 1Hz scendiamo a 10dBm
            double target_power = map_linear(current_tx_rate_, 1.0, 40.0, 10.0, 23.0);
            powerManager.set_tx_power(interface_, static_cast<int>(target_power));

            // 3. Controllo dinamico dell'MCS (TDC) in base al carico del canale (CBR)
            // Sotto il 30% forziamo MCS 3 (6Mbps, netlink valore 12). Sopra il 65% forziamo MCS 6 (12Mbps, netlink valore 24)
            int target_mcs_netlink = 12; // Default 6 Mbps
            if (cbr_percent > 30.0f) {
                target_mcs_netlink = static_cast<int>(map_linear(cbr_percent, 30.0, 65.0, 12.0, 24.0));
                // Arrotonda ai valori netlink validi dei passi MCS (12=6Mbps, 18=9Mbps, 24=12Mbps)
                if (target_mcs_netlink > 12 && target_mcs_netlink < 18) target_mcs_netlink = 12;
                else if (target_mcs_netlink >= 18 && target_mcs_netlink < 24) target_mcs_netlink = 18;
                else if (target_mcs_netlink >= 24) target_mcs_netlink = 24;
            }
            rate_manager.set_fixed_rate_netlink(target_mcs_netlink);

            // 4. Se l'intervallo è variato in modo significativo, notifica il livello applicativo (CAM)
            // Per evitare di intasare il socket UDP a ogni millesimo di variazione, notifichiamo solo se 
            // lo scostamento è apprezzabile (es. > 5ms) o se è passato del tempo.
            if (std::abs(new_interval - old_interval) >= 5) {
                if (sockfd_ >= 0) {
                    // Inviamo direttamente il valore del T_off calcolato in millisecondi
                    int32_t interval_to_send = static_cast<int32_t>(new_interval);
                    sendto(sockfd_, &interval_to_send, sizeof(interval_to_send), 0,
                           (struct sockaddr*)&cam_program_addr_, sizeof(cam_program_addr_));
                }

                // Debug log con output del comportamento continuo
                std::cout << "[DCC LIMERIC] CBR: " << std::fixed << std::setprecision(1) << cbr_percent 
                          << "% | Tx Rate: " << std::setprecision(2) << current_tx_rate_ << " Hz"
                          << " | T_off Target: " << new_interval << " ms"
                          << " | Power: " << static_cast<int>(target_power) << " dBm"
                          << " | MCS Netlink: " << target_mcs_netlink << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(LIMERIC_POLLING_MS));
    }
}