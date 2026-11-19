#include "radio/dcc_radio.h"
#include "radio/CBRReader.h"
#include "radio/TxPowerManager.h"
#include "radio/rate_manager.h"
#include <fstream>
#include <stdexcept>

namespace {
class LinuxDccRadio final : public DccRadio {
public:
    LinuxDccRadio(std::string survey, std::string monitor, bool fake)
        : survey_(std::move(survey)), monitor_(std::move(monitor)), fake_(fake) {}
    void start() override {
        from_file_ = false;
        reader_.setupCBRReader(false, survey_);
        // Configure the controlled radio, not the radiotap injection interface.
        // A monitor interface may reject SET_TX_BITRATE_MASK entirely.
        rate_.setup_rate_manager(survey_);
    }
    float read_cbr() override {
        float value = -1;
        bool file_read = false;
        if (fake_) {
            std::ifstream input("fake_cbr_value");
            file_read = static_cast<bool>(input >> value);
        }
        if (!file_read) {
            if (from_file_) reader_.setupCBRReader(false, survey_);
            reader_.wrapper_start_reading_cbr();
            value = reader_.get_current_cbr();
        }
        const bool changed = file_read != from_file_;
        from_file_ = file_read;
        return changed ? -1 : value;
    }
    void configure(int power, int bitrate) override {
        if (!power_.set_tx_power(survey_, power, true))
            throw std::runtime_error("DCC_ACC TX power configuration failed");
        if (!rate_.set_fixed_rate_netlink(bitrate))
            throw std::runtime_error("DCC_ACC bitrate configuration failed");
    }
    std::optional<double> configured_power_dbm() override { return power_.get_tx_power(survey_); }
private:
    std::string survey_, monitor_;
    bool fake_, from_file_ = false;
    CBRReader reader_;
    TxPowerManager power_;
    RateManager rate_;
};
}

std::unique_ptr<DccRadio> make_linux_dcc_radio(std::string survey, std::string monitor, bool fake_cbr) {
    return std::make_unique<LinuxDccRadio>(std::move(survey), std::move(monitor), fake_cbr);
}
