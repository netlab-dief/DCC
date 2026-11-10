#include "console_log.h"
#include "dcc_manager.h"

// Funzione di utilità per mappare linearmente un valore tra due range (con clamping)
static double map_linear(double x, double in_min, double in_max, double out_min, double out_max) {
    if (in_max == in_min) return out_min;
    double mapped = out_min + (x - in_min) * (out_max - out_min) / (in_max - in_min);
    if (out_min < out_max) return std::clamp(mapped, out_min, out_max);
    return std::clamp(mapped, out_max, out_min);
}

DccManager::DccManager(std::string interface, std::string interface_radiotap,
                       int initial_interval_ms, bool use_fake_cbr, bool use_dcc_net, double eirp_offset_db)
    : interface_(std::move(interface)), 
      interface_radiotap_(std::move(interface_radiotap)),
      use_fake_cbr_(use_fake_cbr),
      use_dcc_net_(use_dcc_net),
      eirp_offset_db_(eirp_offset_db),
      stop_dcc_thread_(false),
      current_dcc_state_(DccState::UNKNOWN), 
      current_interval_ms_(initial_interval_ms)   
{
    if (use_dcc_net_) {
        net_log_.open("dcc_net_telemetry_log.csv", std::ios::out | std::ios::trunc);
        if (!net_log_) throw std::runtime_error("Cannot open DCC_NET telemetry");
        net_log_ << "Timestamp,Valid,Local_CBR_Percent,One_Hop_CBR_Percent,Two_Hop_CBR_Percent,Global_CBR_Percent,Neighbors\n";
        net_log_.flush();
    }
    // Inizializzazione Socket UDP per la notifica Cross-Layer
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        ConsoleLine(std::cerr) << "[DCC ERROR] Impossibile creare il socket UDP per il cross-layer" << std::endl;
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
        ConsoleLine(std::cerr) << "[DCC ERROR] Impossibile aprire il file di log CSV!" << std::endl;
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
    
    // Readers such as the dashboard must see samples even in a stable state.
    csv_log_file_.flush();
}

float DccManager::acquire_cbr() {
    float value = -1.0f;
    bool from_file = false;
    if (use_fake_cbr_) {
        std::ifstream file("fake_cbr_value");
        from_file = static_cast<bool>(file >> value);
    }
    if (!from_file) {
        if (last_cbr_from_file_) cbr_reader.setupCBRReader(false, interface_);
        cbr_reader.wrapper_start_reading_cbr();
        value = cbr_reader.get_current_cbr();
    }
    const bool source_changed = from_file != last_cbr_from_file_;
    last_cbr_from_file_ = from_file;
    return source_changed ? -1.0f : value;
}

void DccManager::start() { start_worker(false); }
void DccManager::start_limeric() { start_worker(true); }

void DccManager::start_worker(bool adaptive) {
    if (dcc_thread_.joinable()) return;
    last_cbr_from_file_ = false;
    cbr_reader.setupCBRReader(false, interface_);
    gate_server_.start();
    stop_dcc_thread_ = false;
    try {
        cbr_sampler_.start([this] { return acquire_cbr(); });
        if (use_dcc_net_) net_receiver_.start(interface_radiotap_, interface_, [this] { return cbr_sampler_.latest(); });
        dcc_thread_ = std::thread(adaptive ? &DccManager::limeric_worker_loop :
                                           &DccManager::dcc_worker_loop, this);
    } catch (...) {
        stop_dcc_thread_ = true;
        net_receiver_.stop();
        cbr_sampler_.stop();
        gate_server_.stop();
        throw;
    }
    struct sched_param param{};
    param.sched_priority = 70;
    if (pthread_setschedparam(dcc_thread_.native_handle(), SCHED_FIFO, &param) != 0)
        ConsoleLine(std::cerr) << "Errore nell'impostazione della priorità del thread DCC" << std::endl;
}

void DccManager::stop() {
    if (cam_sender_) {
        cam_sender_->stop();
        // Preserve completions/errors that arrived during shutdown, including
        // exception unwinding. Logging must not prevent worker cleanup.
        try {
            if (cam_telemetry_log_) cam_telemetry_log_->sample(cam_sender_->telemetry());
        } catch (const std::exception& error) {
            ConsoleLine(std::cerr) << "[CAM TELEMETRY] " << error.what() << std::endl;
        }
        cam_sender_.reset();
        cam_telemetry_log_.reset();
    }
    stop_dcc_thread_ = true;
    airtime_gate_.disable();
    net_receiver_.stop();
    cbr_sampler_.stop(); // Also wakes a controller waiting for a sample.
    if (dcc_thread_.joinable()) dcc_thread_.join();
    airtime_gate_.disable();
    gate_server_.stop();
}

void DccManager::start_cam(const cam::Options& options) {
    if (!options.enabled || cam_sender_) return;
    if (!dcc_thread_.joinable()) throw std::runtime_error("Start DCC before CAM sender");
    cam_telemetry_log_ = std::make_unique<cam::TelemetryLog>("cam_telemetry_estimated_log.csv");
    cam_sender_ = std::make_unique<cam::Sender>(airtime_gate_, interface_radiotap_, options,
        [this] { return get_current_interval_ms(); },
        [this] { return get_current_bitrate_mbps(); },
        use_dcc_net_ ? std::function<void(std::vector<std::uint8_t>&, const std::function<void()>&)>(
            [this](auto& frame, const auto& enqueue) { dispatch_net(frame, enqueue); }) :
            std::function<void(std::vector<std::uint8_t>&, const std::function<void()>&)>{},
        use_dcc_net_ ? std::function<bool()>([this] { return net_ready(); }) : std::function<bool()>{});
}

void DccManager::check_cam() {
    if (use_dcc_net_) {
        net_receiver_.check();
        std::lock_guard<std::mutex> lock(power_mutex_);
        if (power_failed_) throw std::runtime_error("DCC_NET requires successful TX power configuration and nl80211 power readback");
    }
    if (!cam_sender_) return;
    const auto data = cam_sender_->telemetry();
    const double hz = cam_telemetry_log_->sample(data);
    std::ostringstream line;
    line << std::fixed << std::setprecision(2)
         << "[CAM MONITOR] Generati: " << data.generated << " | Sostituiti: " << data.replaced
         << " | Scaduti: " << data.expired << " | Invalidati: " << data.invalidated
         << " | Accodati: " << data.submitted << " | Contabilizzati (stima): " << data.accounted
         << " | Invii al driver: " << hz << " Hz | Errori TX: " << data.tx_errors
         << " | Attesa media: " << (data.submitted ? data.queue_ms / data.submitted : 0) << " ms"
         << " | Invio medio: " << (data.accounted ? data.send_ms / data.accounted : 0) << " ms";
    ConsoleLine() << line.str() << std::endl;
    cam_sender_->check();
}

float DccManager::get_current_cbr() const {
    const auto sample = cbr_sampler_.latest();
    return sample && sample->fresh(CbrSample::Clock::now()) ? sample->value * 100.0f : -1.0f;
}

DccState DccManager::get_current_state() const {
    return current_dcc_state_.load(std::memory_order_relaxed);
}

int DccManager::get_current_interval_ms() const {
    return current_interval_ms_.load(std::memory_order_relaxed);
}

void DccManager::dcc_worker_loop() {
    TxPowerManager powerManager;
    rate_manager.setup_rate_manager(interface_radiotap_); 

    // Variabili per il tracking del log e del fallback
    int current_tx_power = 23;
    int current_mcs = 6;

    DccStateMachine state_machine;

    std::uint64_t cursor = 0;
    std::optional<std::int64_t> previous_slot;
    while (!stop_dcc_thread_) {
        const auto sample = wait_control(cursor);
        if (stop_dcc_thread_ || (use_dcc_net_ && net_receiver_.stopped())) {
            airtime_gate_.disable();
            break;
        }
        if (!sample || !sample->fresh(CbrSample::Clock::now())) {
            state_machine.invalidate_observation();
            airtime_gate_.disable();
            continue;
        }
        if (previous_slot && sample->utc_slot != *previous_slot + 1)
            state_machine.invalidate_observation();
        previous_slot = sample->utc_slot;
        const float raw_cbr = sample->value;
        float cbr_percent = raw_cbr * 100.0f;

        DccState old_state = current_dcc_state_.load(std::memory_order_relaxed);
        DccState new_state = old_state;
        int new_interval = current_interval_ms_.load(std::memory_order_relaxed);
        new_state = state_machine.update(cbr_percent, sample->measured_at);

        bool state_changed = (old_state != new_state);
        airtime_gate_.configure(0.03, std::chrono::milliseconds(
            DccStateMachine::parameters_for(new_state).interval_ms), sample->measured_at);

        if (state_changed) {
            const DccParameters parameters = DccStateMachine::parameters_for(new_state);
            new_interval = parameters.interval_ms;
            current_tx_power = parameters.tx_power_dbm;
            current_mcs = parameters.mcs_mbps;

            update_power(powerManager, current_tx_power);
            current_bitrate_mbps_.store(current_mcs, std::memory_order_relaxed);
            rate_manager.set_fixed_rate_netlink(current_mcs);

            current_dcc_state_.store(new_state, std::memory_order_relaxed);
            current_interval_ms_.store(new_interval, std::memory_order_relaxed);

            if (sockfd_ >= 0) {
                int32_t state_to_send = static_cast<int32_t>(new_state);
                sendto(sockfd_, &state_to_send, sizeof(state_to_send), 0,
                       (struct sockaddr*)&cam_program_addr_, sizeof(cam_program_addr_));
            }

            ConsoleLine() << "\033[1;33m[DCC CAMBIO STATO] Stato: " << static_cast<int>(new_state)
                      << " | CBR: " << std::fixed << std::setprecision(2) << cbr_percent
                      << "% | T_off: " << new_interval << "ms\033[0m" << std::endl;
        }

        log_telemetry(dcc_state_to_string(new_state), cbr_percent, new_interval,
                      current_tx_power, current_mcs, state_changed);
    }
}

void DccManager::limeric_worker_loop() {
    TxPowerManager powerManager;
    rate_manager.setup_rate_manager(interface_radiotap_);
    DccAdaptive controller;
    int last_notified_interval = -1;
    ConsoleLine() << "[DCC INIT] ETSI adaptive DCC; airtime gate on 127.0.0.1:5556" << std::endl;

    std::uint64_t cursor = 0;
    std::optional<std::int64_t> previous_slot;
    while (!stop_dcc_thread_) {
        const auto sample = wait_control(cursor);
        if (stop_dcc_thread_ || (use_dcc_net_ && net_receiver_.stopped())) {
            airtime_gate_.disable();
            break;
        }
        if (!sample || !sample->fresh(CbrSample::Clock::now())) {
            controller.invalidate();
            airtime_gate_.disable();
            continue;
        }
        if (previous_slot && sample->utc_slot != *previous_slot + 1) controller.invalidate();
        previous_slot = sample->utc_slot;
        const float raw_cbr = sample->value;
        if (!controller.observe(raw_cbr, sample->utc_slot)) continue;
        airtime_gate_.configure(controller.delta(), std::chrono::milliseconds(25), sample->measured_at);
        // Advisory CAM interval only; per-packet grants use reported TX airtime.
        const int interval = airtime_gate_.suggested_interval_ms();
        current_interval_ms_.store(interval, std::memory_order_relaxed);
        current_tx_rate_ = 1000.0 / interval;

        // Existing project-specific radio policy, separate from the ETSI delta controller.
        const int power = static_cast<int>(map_linear(current_tx_rate_, 1.0, 40.0, 10.0, 23.0));
        int rate = 12;
        if (raw_cbr > 0.30f) {
            const int mapped = static_cast<int>(map_linear(raw_cbr * 100.0, 30.0, 65.0, 12.0, 24.0));
            rate = mapped >= 24 ? 24 : (mapped >= 18 ? 18 : 12);
        }
        update_power(powerManager, power);
        current_bitrate_mbps_.store(rate, std::memory_order_relaxed);
        rate_manager.set_fixed_rate_netlink(rate);
        const bool notify = last_notified_interval < 0 || std::abs(interval - last_notified_interval) >= 5;
        if (notify) {
            if (sockfd_ >= 0) {
                const int32_t value = interval;
                if (sendto(sockfd_, &value, sizeof(value), 0,
                    reinterpret_cast<sockaddr*>(&cam_program_addr_), sizeof(cam_program_addr_)) >= 0)
                    last_notified_interval = interval;
            }
            ConsoleLine() << "[DCC ADAPTIVE] CBR: " << raw_cbr * 100.0f
                      << "% | filtered: " << controller.filtered_cbr() * 100.0
                      << "% | delta: " << controller.delta()
                      << " | advisory interval: " << interval << " ms" << std::endl;
        }
        log_telemetry("LIMERIC", raw_cbr * 100.0f, interval, power, rate, notify);
    }
}

int DccManager::get_current_bitrate_mbps() const {
    return current_bitrate_mbps_.load(std::memory_order_relaxed);
}


std::optional<CbrSample> DccManager::wait_control(std::uint64_t& cursor) {
    if (!use_dcc_net_) return cbr_sampler_.wait_next(cursor);
    const auto snapshot = net_state_.wait_next(cursor);
    if (!snapshot) return {};
    net_log_ << get_iso8601_timestamp() << ',' << (snapshot->control.valid ? "YES" : "NO") << ','
             << snapshot->local * 100 << ',' << snapshot->one_hop * 100 << ','
             << snapshot->two_hop * 100 << ',' << snapshot->global * 100 << ',' << snapshot->neighbors << '\n';
    net_log_.flush();
    return snapshot->control;
}

void DccManager::update_power(TxPowerManager& manager, int power) {
    if (!use_dcc_net_) { manager.set_tx_power(interface_, power); return; }
    // Serialize configuration/readback with DCC-MCO patching and driver enqueue.
    std::lock_guard<std::mutex> lock(power_mutex_);
    confirmed_power_.reset();
    if (manager.set_tx_power(interface_, power, true)) confirmed_power_ = manager.get_tx_power(interface_);
    power_failed_ = !confirmed_power_;
    if (power_failed_) {
        stop_dcc_thread_ = true;
        airtime_gate_.disable();
    }
}

void DccManager::dispatch_net(std::vector<std::uint8_t>& frame, const std::function<void()>& enqueue) {
    std::lock_guard<std::mutex> lock(power_mutex_);
    const auto snapshot = net_state_.snapshot();
    const auto local = cbr_sampler_.latest();
    const auto now = CbrSample::Clock::now();
    if (net_receiver_.stopped() || !confirmed_power_ || !snapshot.control.fresh(now) || !local || !local->fresh(now))
        throw std::runtime_error("DCC_NET data/power expired after admission; transmission stopped");
    dccnet::write_extension(frame, local->value, snapshot.one_hop, *confirmed_power_ + eirp_offset_db_);
    enqueue();
}

bool DccManager::net_ready() {
    std::lock_guard<std::mutex> lock(power_mutex_);
    const auto local = cbr_sampler_.latest();
    const auto now = CbrSample::Clock::now();
    return !net_receiver_.stopped() && confirmed_power_ && net_state_.snapshot().control.fresh(now) && local && local->fresh(now);
}
