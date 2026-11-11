#include "dcc/dcc_manager.h"
#include "radio/dcc_radio.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
std::atomic<bool> fail_radio{false};
class FakeRadio final : public DccRadio {
public:
    void start() override {}
    float read_cbr() override { return 0.2f; }
    void configure(int, int) override { if (fail_radio) throw std::runtime_error("injected radio failure"); }
    std::optional<double> configured_power_dbm() override { return 23.0; }
};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> bool eventually(Predicate predicate) {
    const auto end = dcc::Clock::now() + 3s;
    while (dcc::Clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
struct WorkingDirectory {
    std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::path temporary = std::filesystem::temp_directory_path() /
        ("dcc-manager-test-" + std::to_string(dcc::Clock::now().time_since_epoch().count()));
    WorkingDirectory() { std::filesystem::create_directory(temporary); std::filesystem::current_path(temporary); }
    ~WorkingDirectory() { std::filesystem::current_path(original); std::filesystem::remove_all(temporary); }
};
}
// Link the real manager/composition with simulated hardware, on macOS or Linux.
std::unique_ptr<DccRadio> make_linux_dcc_radio(std::string, std::string, bool) {
    return std::make_unique<FakeRadio>();
}
int main() {
    try {
        WorkingDirectory working;
        DccManager manager("survey", "monitor");
        manager.start();
        require(eventually([&] { manager.check_cam(); return manager.get_current_state() == DccState::RELAXED; }),
            "manager connects ACC acquisition, CROSS policy and radio application");
        require(manager.get_current_cbr() >= 0 && manager.get_current_bitrate_mbps() == 6,
            "existing monitoring API remains usable");
        manager.stop();
        manager.start_limeric();
        require(eventually([&] { manager.check_cam(); return manager.get_current_interval_ms() == 1000; }),
            "manager can restart with the adaptive controller and reset pairing");
        manager.stop();
        fail_radio = true;
        manager.start();
        bool propagated = eventually([&] {
            try { manager.check_cam(); }
            catch (const std::runtime_error& error) { return std::string(error.what()) == "injected radio failure"; }
            return false;
        });
        require(propagated, "controller hardware failures propagate even with CAM disabled");
        manager.stop();
        std::cout << "All DCC manager lifecycle tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
