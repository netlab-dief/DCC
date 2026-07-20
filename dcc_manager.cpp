#include "dcc_manager.h"

// Funzione di utilità per mappare linearmente un valore tra due range (con clamping)
static double map_linear(double x, double in_min, double in_max, double out_min, double out_max) {
    if (in_max == in_min) return out_min;
    double mapped = out_min + (x - in_min) * (out_max - out_min) / (in_max - in_min);
    if (out_min < out_max) return std::clamp(mapped, out_min, out_max);
    return std::clamp(mapped, out_max, out_min);
}

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

    // Apertura file CSV e creazione dell'header
    csv_log_file_.open("dcc_telemetry_log.csv", std::ios::out | std::ios::app);
    if (csv_log_file_.is_open()) {
        csv_log_file_.seekp(0, std::ios::end);
        if (csv_log_file_.tellp() == 0) {
            csv_log_file_ << "Timestamp,CBR_Percent,State_Mode,Interval_ms,Tx_Power_dBm,MCS_Netlink,HW_Config_Changed\n";
            csv_log_file_.flush();
        }
    } else {
        std::cerr << "[DCC ERROR] Impossibile aprire il file di log CSV!" << std::endl;
    }
}

DccManager::~DccManager() {
    stop();
    if (sockfd_ >= 0) {
        close(sockfd_);
    }
    if (csv_log_file_.is_open()) {
        csv_log_file_.close();
    }
}

std::string DccManager::get_iso8601_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_micros = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()) % 1000000;

    struct tm time_info;
    localtime_r(&now_time_t, &time_info);

    std::stringstream ss;
    ss << std::put_time(&time_info, "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(6) << now_micros.count();
    
    return ss.str();
}

std::string DccManager::dcc_state_to_string(DccState state) {
    switch (state) {
        case DccState::RELAXED:     return "RELAXED";
        case DccState::ACTIVE_1:    return "ACTIVE_1";
        case DccState::ACTIVE_2:    return "ACTIVE_2";
        case DccState::ACTIVE_3:    return "ACTIVE_3";
        case DccState::RESTRICTIVE: return "RESTRICTIVE";
        default:                    return "UNKNOWN";
    }
}

void DccManager::log_telemetry(const std::string& state_or_mode, float cbr, int interval_ms, int tx_power, int mcs, bool hw_updated) {
    if (!csv_log_file_.is_open()) return;

    csv_log_file_ << get_iso8601_timestamp() << ","
                  << std::fixed << std::setprecision(2) << cbr << ","
                  << state_or_mode << ","
                  << interval_ms << ","
                  << tx_power << ","
                  << mcs << ","
                  << (hw_updated ? "YES" : "NO") << "\n";
    
    if (hw_updated) {
        csv_log_file_.flush();
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

    // Variabili per il tracking del log e del fallback
    int current_tx_power = 23;
    int current_mcs = 6;

    while (!stop_dcc_thread_) {
        cbr_reader.wrapper_start_reading_cbr();

        // float raw_cbr = cbr_reader.get_current_cbr();
        float raw_cbr = -1.0f;
        bool read_from_file_success = false;
        std::ifstream file("fake_cbr_value"); 
        if (file.is_open()) {
            if (file >> raw_cbr) {
                read_from_file_success = true;
            }
        }

        if (!read_from_file_success) {
            raw_cbr = cbr_reader.get_current_cbr();
        }

        if (raw_cbr >= 0.0f) {
            float cbr_percent = raw_cbr * 100.0f;
            last_known_cbr_.store(cbr_percent, std::memory_order_relaxed);

            DccState old_state = current_dcc_state_.load(std::memory_order_relaxed);
            DccState new_state = old_state;
            int new_interval = current_interval_ms_.load(std::memory_order_relaxed);

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

            bool state_changed = (old_state != new_state);

            if (state_changed) {
                switch (new_state) {
                    case DccState::RELAXED:
                        new_interval = 50; 
                        current_tx_power = 23; current_mcs = 6;
                        break;
                    case DccState::ACTIVE_1:
                        new_interval = 100; 
                        current_tx_power = 19; current_mcs = 12;
                        break;
                    case DccState::ACTIVE_2:
                        new_interval = 200; 
                        current_tx_power = 16; current_mcs = 18;
                        break;
                    case DccState::ACTIVE_3:
                        new_interval = 400; 
                        current_tx_power = 13; current_mcs = 24;
                        break;
                    case DccState::RESTRICTIVE:
                        new_interval = 1000; 
                        current_tx_power = 10; current_mcs = 24;
                        break;
                    case DccState::UNKNOWN:
                        break;
                }

                powerManager.set_tx_power(interface_, current_tx_power);
                rate_manager.set_fixed_rate_netlink(current_mcs);

                current_dcc_state_.store(new_state, std::memory_order_relaxed);
                current_interval_ms_.store(new_interval, std::memory_order_relaxed);

                if (sockfd_ >= 0) {
                    int32_t state_to_send = static_cast<int32_t>(new_state);
                    sendto(sockfd_, &state_to_send, sizeof(state_to_send), 0,
                           (struct sockaddr*)&cam_program_addr_, sizeof(cam_program_addr_));
                }

                std::cout << "\033[1;33m[DCC CAMBIO STATO] Stato: " << static_cast<int>(new_state)
                          << " | CBR: " << std::fixed << std::setprecision(2) << cbr_percent 
                          << "% | T_off: " << new_interval << "ms\033[0m" << std::endl;
            }

            log_telemetry(dcc_state_to_string(new_state), cbr_percent, new_interval, 
                          current_tx_power, current_mcs, state_changed);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(CBR_POLLING_INTERVAL_MS));
    }
}

void DccManager::start_limeric() {
    if (!dcc_thread_.joinable()) {
        stop_dcc_thread_ = false;
        dcc_thread_ = std::thread(&DccManager::limeric_worker_loop, this);

        pthread_t native_handle = dcc_thread_.native_handle();
        struct sched_param param;
        param.sched_priority = 70; 
        
        if (pthread_setschedparam(native_handle, SCHED_FIFO, &param) != 0) {
            std::cerr << "Errore nell'impostazione della priorità del thread LIMERIC" << std::endl;
        }
    }
}

int DccManager::calculate_limeric_interval(float cbr_percent) {
    const double CBR_TARGET = 0.68; 
    const double ALPHA = 0.016;     
    const double BETA = 0.0012;     
    
    double cbr_fraction = static_cast<double>(cbr_percent) / 100.0;
    double new_rate = (1.0 - ALPHA) * current_tx_rate_ + BETA * (CBR_TARGET - cbr_fraction);

    const double MAX_RATE = 40.0; 
    const double MIN_RATE = 1.0;  
    new_rate = std::clamp(new_rate, MIN_RATE, MAX_RATE);

    current_tx_rate_ = new_rate;
    return static_cast<int>(1000.0 / new_rate);
}

void DccManager::limeric_worker_loop() {
    TxPowerManager powerManager;
    cbr_reader.setupCBRReader(false, interface_);
    rate_manager.setup_rate_manager(interface_radiotap_); 

    const int LIMERIC_POLLING_MS = 200; 
    const float CBR_TARGET = 60.0f;     
    const float ALPHA = 0.1f;          
    const float BETA = 1.0f / 150.0f;  

    const float G_MAX = 0.005f;
    const float G_MIN = -0.005f;

    const float DELTA_MIN = 0.0025f;   
    const float DELTA_MAX = 0.03f;     
    const float T_ON_MS = 0.5f; 

    float current_delta = DELTA_MIN;   
    float cbr_its = 0.0f;              
    bool is_first_iteration = true;

    std::cout << "[DCC INIT] Loop LIMERIC Adattativo avviato." << std::endl;

    while (!stop_dcc_thread_) {
        cbr_reader.wrapper_start_reading_cbr();
        float raw_cbr = cbr_reader.get_current_cbr();

        if (raw_cbr >= 0.0f) {
            float cbr_percent = raw_cbr * 100.0f;
            last_known_cbr_.store(cbr_percent, std::memory_order_relaxed);

            if (is_first_iteration) {
                cbr_its = cbr_percent;
                is_first_iteration = false;
            } else {
                cbr_its = 0.5f * cbr_its + 0.5f * cbr_percent;
            }

            float error = CBR_TARGET - cbr_its;
            float delta_offset = BETA * error;

            if (error > 0) {
                if (delta_offset > G_MAX) delta_offset = G_MAX;
            } else {
                if (delta_offset < G_MIN) delta_offset = G_MIN;
            }

            float new_delta = (1.0f - ALPHA) * current_delta + delta_offset;

            if (new_delta > DELTA_MAX) new_delta = DELTA_MAX;
            if (new_delta < DELTA_MIN) new_delta = DELTA_MIN;
            current_delta = new_delta;

            int new_interval = static_cast<int>(T_ON_MS / current_delta);

            if (new_interval < 25) new_interval = 25;
            if (new_interval > 1000) new_interval = 1000;

            current_tx_rate_ = 1000.0f / static_cast<float>(new_interval);
            int old_interval = current_interval_ms_.exchange(new_interval, std::memory_order_relaxed);

            double target_power = map_linear(current_tx_rate_, 1.0, 40.0, 10.0, 23.0);
            int tx_power_int = static_cast<int>(target_power);
            powerManager.set_tx_power(interface_, tx_power_int);

            int target_mcs_netlink = 12; 
            if (cbr_percent > 30.0f) {
                target_mcs_netlink = static_cast<int>(map_linear(cbr_percent, 30.0, 65.0, 12.0, 24.0));
                if (target_mcs_netlink > 12 && target_mcs_netlink < 18) target_mcs_netlink = 12;
                else if (target_mcs_netlink >= 18 && target_mcs_netlink < 24) target_mcs_netlink = 18;
                else if (target_mcs_netlink >= 24) target_mcs_netlink = 24;
            }
            rate_manager.set_fixed_rate_netlink(target_mcs_netlink);

            bool significant_change = (std::abs(new_interval - old_interval) >= 5);

            if (significant_change) {
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
                          << " | Power: " << tx_power_int << " dBm"
                          << " | MCS Netlink: " << target_mcs_netlink << std::endl;
            }

            log_telemetry("LIMERIC", cbr_percent, new_interval, tx_power_int, target_mcs_netlink, significant_change);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(LIMERIC_POLLING_MS));
    }
}