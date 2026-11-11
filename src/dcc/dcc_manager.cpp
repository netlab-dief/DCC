#include "common/console_log.h"
#include "dcc/dcc_manager.h"
#include "radio/dcc_radio.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

DccManager::DccManager(std::string interface, std::string interface_radiotap,
    int initial_interval_ms, bool use_fake_cbr, bool use_dcc_net, double eirp_offset_db)
    : interface_(std::move(interface)), interface_radiotap_(std::move(interface_radiotap)),
      use_dcc_net_(use_dcc_net), eirp_offset_db_(eirp_offset_db),
      access_(make_linux_dcc_radio(interface_, interface_radiotap_, use_fake_cbr), use_dcc_net),
      network_([this] { return cross_.network_input(access_.local_sample()); },
               [this] { return cross_.resources(access_.feedback(), dcc::Clock::now()); }),
      current_interval_ms_(initial_interval_ms) {
    csv_log_file_.open("dcc_telemetry_log.csv", std::ios::out | std::ios::app);
    if (!csv_log_file_) throw std::runtime_error("Cannot open DCC telemetry");
    csv_log_file_.seekp(0, std::ios::end);
    if (csv_log_file_.tellp() == 0) {
        csv_log_file_ << "Timestamp,CBR_Percent,State_Mode,Interval_ms,Tx_Power_dBm,MCS_Netlink,HW_Config_Changed\n";
        csv_log_file_.flush();
    }
}

DccManager::~DccManager() { stop(); }

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

void DccManager::start() { start_worker(dcc::Mode::reactive); }
void DccManager::start_limeric() { start_worker(dcc::Mode::adaptive); }

void DccManager::start_worker(dcc::Mode mode) {
    if (controller_.joinable()) return;
    cross_.reset(mode, use_dcc_net_);
    { std::lock_guard<std::mutex> lock(failure_mutex_); failure_ = {}; }
    stopping_ = false;
    try {
        if (use_dcc_net_) {
            net_log_.open("dcc_net_telemetry_log.csv", std::ios::out | std::ios::trunc);
            if (!net_log_) throw std::runtime_error("Cannot open DCC_NET telemetry");
            net_log_ << "Timestamp,Valid,Local_CBR_Percent,One_Hop_CBR_Percent,Two_Hop_CBR_Percent,Global_CBR_Percent,Neighbors\n";
            net_log_.flush();
        }
        access_.start();
        if (use_dcc_net_) network_.start(interface_radiotap_, interface_);
        controller_ = std::thread([this] {
            try { control_loop(); }
            catch (...) {
                { std::lock_guard<std::mutex> lock(failure_mutex_); failure_ = std::current_exception(); }
                cross_.invalidate();
                access_.disable();
                stopping_ = true;
            }
        });
    } catch (...) {
        stopping_ = true;
        network_.stop();
        access_.stop();
        if (net_log_.is_open()) net_log_.close();
        throw;
    }
    sched_param param{};
    param.sched_priority = 70;
    if (pthread_setschedparam(controller_.native_handle(), SCHED_FIFO, &param) != 0)
        ConsoleLine(std::cerr) << "[DCC] Controller running with ordinary scheduling" << std::endl;
}

void DccManager::stop() {
    if (cam_sender_) {
        cam_sender_->stop();
        try {
            if (cam_telemetry_log_) cam_telemetry_log_->sample(cam_sender_->telemetry());
        } catch (const std::exception& error) {
            ConsoleLine(std::cerr) << "[CAM TELEMETRY] " << error.what() << std::endl;
        }
        cam_sender_.reset();
        cam_telemetry_log_.reset();
    }
    stopping_ = true;
    access_.disable();
    network_.stop();
    access_.stop();
    if (controller_.joinable()) controller_.join();
    cross_.invalidate();
    access_.disable();
    if (net_log_.is_open()) net_log_.close();
}

void DccManager::start_cam(const cam::Options& options) {
    if (!options.enabled || cam_sender_) return;
    if (!controller_.joinable() || stopping_) throw std::runtime_error("Start DCC before CAM sender");
    cam_telemetry_log_ = std::make_unique<cam::TelemetryLog>("cam_telemetry_estimated_log.csv");
    cam_sender_ = std::make_unique<cam::Sender>(access_.admission(), interface_radiotap_, options,
        [this] { return DccFacilities::cam_generation_interval_ms(
            cross_.resources(access_.feedback(), dcc::Clock::now())); },
        [this] { return access_.bitrate_mbps(); },
        [this](auto& frame, const auto& enqueue) { dispatch(frame, enqueue); },
        [this] { return ready(); });
}

void DccManager::check_cam() {
    { std::lock_guard<std::mutex> lock(failure_mutex_); if (failure_) std::rethrow_exception(failure_); }
    if (use_dcc_net_) network_.check();
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
    const auto sample = access_.local_sample();
    return sample && sample->fresh(dcc::Clock::now()) ? sample->value * 100.0f : -1.0f;
}
DccState DccManager::get_current_state() const { return cross_.snapshot(dcc::Clock::now()).state; }
int DccManager::get_current_interval_ms() const { return current_interval_ms_.load(); }
int DccManager::get_current_bitrate_mbps() const { return access_.bitrate_mbps(); }

void DccManager::control_loop() {
    std::uint64_t cursor = 0;
    while (!stopping_) {
        const auto sample = wait_control(cursor);
        if (stopping_ || (use_dcc_net_ && network_.stopped())) break;
        const auto policy = cross_.evaluate(use_dcc_net_ ? access_.local_sample() : sample,
            use_dcc_net_ ? sample : std::nullopt, access_.feedback(), dcc::Clock::now());
        access_.apply(policy.access);
        if (!policy.access.valid) continue;
        const auto resources = cross_.resources(access_.feedback(), dcc::Clock::now());
        const int interval = static_cast<int>((resources.suggested_admission_interval.count() + 999) / 1000);
        current_interval_ms_ = interval;
        if (policy.mode == dcc::Mode::reactive || policy.controller_updated)
            log_telemetry(policy.mode == dcc::Mode::adaptive ? "LIMERIC" : dcc_state_to_string(policy.state),
                policy.control_cbr * 100.0f, interval, policy.access.power_limit_dbm,
                policy.access.bitrate_mbps, policy.controller_updated);
    }
    cross_.invalidate();
    access_.disable();
}

std::optional<CbrSample> DccManager::wait_control(std::uint64_t& cursor) {
    if (!use_dcc_net_) return access_.wait_local(cursor);
    const auto net = network_.wait_next(cursor);
    if (!net) return {};
    net_log_ << get_iso8601_timestamp() << ',' << (net->control.valid ? "YES" : "NO") << ','
        << net->local * 100 << ',' << net->one_hop * 100 << ',' << net->two_hop * 100 << ','
        << net->global * 100 << ',' << net->neighbors << '\n';
    net_log_.flush();
    return net->control;
}

bool DccManager::ready() const {
    return !stopping_ && access_.ready() && cross_.snapshot(dcc::Clock::now()).access.valid &&
        (!use_dcc_net_ || network_.ready());
}

void DccManager::dispatch(std::vector<std::uint8_t>& frame, const std::function<void()>& enqueue) {
    access_.dispatch(frame, enqueue, [this](auto& packet, auto power) {
        if (stopping_ || !cross_.snapshot(dcc::Clock::now()).access.valid)
            throw std::runtime_error("DCC_CROSS control expired after admission");
        if (use_dcc_net_) {
            if (!power) throw std::runtime_error("DCC_NET requires confirmed TX power");
            network_.decorate(packet, *power + eirp_offset_db_);
        }
    });
}
