// Opt-in hardware check: changes radio settings; never run by `make test`.
#include "radio/dcc_radio.h"
#include "radio/rate_manager.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: check_radio_hardware <OCB-interface>\n"; return 2; }
    // A nonexistent injection interface proves configuration uses the first argument.
    auto radio = make_linux_dcc_radio(argv[1], "dcc-test-absent", false);
    radio->start();
    const auto original = radio->configured_power_dbm();
    if (!original || std::round(*original) != *original) return 2;
    bool ok = true;
    try {
        for (int power : {10, 15, 20}) {
            for (int rate : {6, 9, 12, 18, 24}) {
                radio->configure(power, rate);
                const auto actual = radio->configured_power_dbm();
                if (!actual || std::abs(*actual - power) > 0.01)
                    throw std::runtime_error("power readback mismatch");
                std::cout << "PASS power=" << power << " dBm (read back), rate=" << rate << " (ACK)\n";
            }
        }
        RateManager rates;
        rates.setup_rate_manager(argv[1]);
        if (rates.set_fixed_rate_netlink(999)) throw std::runtime_error("invalid rate accepted");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; ok = false; }
    // The kernel does not expose the previous multicast-rate setting: reset to DCC's initial rate.
    try { radio->configure(static_cast<int>(*original), 6); }
    catch (const std::exception& e) { std::cerr << "Restore failed: " << e.what() << '\n'; return 1; }
    std::cout << "Restored power=" << *original << " dBm; rate=6\n";
    return ok ? 0 : 1;
}
