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

        // Commenta questo blocco se vuoi leggere il CBR da un file esterno invece che dal CBRReader
        float raw_cbr = cbr_reader.get_current_cbr();

        // Se vuoi leggere il CBR da un file esterno, puoi decommentare questo blocco e commentare la riga sopra
        //float raw_cbr = -1.0f;
        //bool read_from_file_success = false;
        //std::ifstream file("fake_cbr_value"); // Inserisci qui il percorso //corretto del file
        //if (file.is_open()) {
        //    if (file >> raw_cbr) {
        //        read_from_file_success = true;
        //    }
        //}

        //// Se il file non esiste o la lettura è fallita, usa il metodo normale
        //if (!read_from_file_success) {
        //    raw_cbr = cbr_reader.get_current_cbr();
        //}

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
                        rate_manager.set_fixed_rate_netlink(6); 
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
// CBR Rilevato,Valore Netlink impostato,Bitrate Reale (Modulazione 802.11p / C-V2X),Comportamento logico
// <30%,12,6 Mbps (QPSK 1/2),Canale libero: si usa la modulazione più robusta per avere massima affidabilità.
// Tra 30% e 48%,18,9 Mbps (QPSK 3/4),Il canale inizia a caricarsi: si aumenta la velocità per far durare meno il pacchetto in aria.
// >48% (fino a 65%),24,12 Mbps (16-QAM 1/2),"Canale congestionato: si forza il bitrate alto (12 Mbps) per liberare lo spettro il prima possibile, accettando una leggera perdita di sensibilità."

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

    // =========================================================================
    // PARAMETRI STANDARD LIMERIC & ETSI DCC
    // =========================================================================
    const int LIMERIC_POLLING_MS = 200; // Intervallo di controllo fisso (200ms)
    
    // Parametri dell'algoritmo di controllo lineare
    const float CBR_TARGET = 60.0f;     // Target CBR (es. 60%)
    const float ALPHA = 0.1f;          // Coefficiente di sbiadimento/oblio (0 < alpha < 1)
    const float BETA = 1.0f / 150.0f;  // Guadagno lineare dell'errore

    // Limiti di saturazione per il delta_offset (Guadagno massimo/minimo per iterazione)
    const float G_MAX = 0.005f;
    const float G_MIN = -0.005f;

    // Limiti fisici e standard di occupazione del canale (Delta)
    const float DELTA_MIN = 0.0025f;   // Equivalente a ~1 Hz per pacchetti standard
    const float DELTA_MAX = 0.03f;     // Equivalente a ~40 Hz per pacchetti standard

    // Durata della trasmissione del pacchetto (T_on) in millisecondi.
    // Supponendo una dimensione media del pacchetto CAM di 300 byte a 6 Mbps:
    // 300 byte * 8 = 2400 bit. 2400 bit / 6,000,000 bps = 400 µs. Aggiungendo il preambolo: ~0.5 ms.
    const float T_ON_MS = 0.5f; 

    // Stato locale dell'algoritmo
    float current_delta = DELTA_MIN;   // Inizializzazione al minimo
    float cbr_its = 0.0f;              // CBR filtrato (Moving Average)
    bool is_first_iteration = true;
    // =========================================================================

    std::cout << "[DCC INIT] Loop LIMERIC Adattativo avviato." << std::endl;

    while (!stop_dcc_thread_) {
        cbr_reader.wrapper_start_reading_cbr();
        float raw_cbr = cbr_reader.get_current_cbr();

        if (raw_cbr >= 0.0f) {
            float cbr_percent = raw_cbr * 100.0f;
            last_known_cbr_.store(cbr_percent, std::memory_order_relaxed);

            // STEP 1: Filtro a media mobile esponenziale sul CBR per stabilizzare l'input
            if (is_first_iteration) {
                cbr_its = cbr_percent;
                is_first_iteration = false;
            } else {
                cbr_its = 0.5f * cbr_its + 0.5f * cbr_percent;
            }

            // STEP 2: Calcolo del guadagno d'errore proporzionale (delta_offset)
            float error = CBR_TARGET - cbr_its;
            float delta_offset = BETA * error;

            // Applicazione dei limiti di saturazione (clipping) sull'offset per passo di tempo
            if (error > 0) {
                if (delta_offset > G_MAX) delta_offset = G_MAX;
            } else {
                if (delta_offset < G_MIN) delta_offset = G_MIN;
            }

            // STEP 3: Equazione di controllo lineare LIMERIC
            float new_delta = (1.0f - ALPHA) * current_delta + delta_offset;

            // STEP 4: Clipping del Delta finale entro i limiti fisici consentiti
            if (new_delta > DELTA_MAX) new_delta = DELTA_MAX;
            if (new_delta < DELTA_MIN) new_delta = DELTA_MIN;
            current_delta = new_delta;

            // STEP 5: Traduzione del Delta in Intervallo di spegnimento (T_off) in millisecondi
            // Formula: T_off = T_on / Delta
            int new_interval = static_cast<int>(T_ON_MS / current_delta);

            // Vincolo di sicurezza ETSI DCC (T_off ammesso tra 25 ms e 1000 ms)
            if (new_interval < 25) new_interval = 25;
            if (new_interval > 1000) new_interval = 1000;

            // Calcolo del tasso di trasmissione teorico (Hz) per i passaggi successivi
            current_tx_rate_ = 1000.0f / static_cast<float>(new_interval);

            // Aggiorna l'intervallo atomico e recupera il vecchio valore
            int old_interval = current_interval_ms_.exchange(new_interval, std::memory_order_relaxed);

            // 2. Controllo dinamico e fluido della Potenza (TPC) in base al Rate corrente
            double target_power = map_linear(current_tx_rate_, 1.0, 40.0, 10.0, 23.0);
            powerManager.set_tx_power(interface_, static_cast<int>(target_power));

            // 3. Controllo dinamico dell'MCS (TDC) in base al carico del canale (CBR)
            int target_mcs_netlink = 12; // Default 6 Mbps
            if (cbr_percent > 30.0f) {
                target_mcs_netlink = static_cast<int>(map_linear(cbr_percent, 30.0, 65.0, 12.0, 24.0));
                if (target_mcs_netlink > 12 && target_mcs_netlink < 18) target_mcs_netlink = 12;
                else if (target_mcs_netlink >= 18 && target_mcs_netlink < 24) target_mcs_netlink = 18;
                else if (target_mcs_netlink >= 24) target_mcs_netlink = 24;
            }
            rate_manager.set_fixed_rate_netlink(target_mcs_netlink);

            // 4. Notifica il livello applicativo se lo scostamento è apprezzabile
            if (std::abs(new_interval - old_interval) >= 5) {
                if (sockfd_ >= 0) {
                    int32_t interval_to_send = static_cast<int32_t>(new_interval);
                    sendto(sockfd_, &interval_to_send, sizeof(interval_to_send), 0,
                           (struct sockaddr*)&cam_program_addr_, sizeof(cam_program_addr_));
                }

                std::cout << "[DCC LIMERIC] CBR Sensed: " << std::fixed << std::setprecision(1) << cbr_percent 
                          << "% | CBR Filtered: " << cbr_its << "%"
                          << " | Delta: " << std::setprecision(5) << current_delta
                          << " | Tx Rate: " << std::setprecision(2) << current_tx_rate_ << " Hz"
                          << " | T_off Target: " << new_interval << " ms"
                          << " | Power: " << static_cast<int>(target_power) << " dBm"
                          << " | MCS Netlink: " << target_mcs_netlink << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(LIMERIC_POLLING_MS));
    }
}