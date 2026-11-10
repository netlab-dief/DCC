#ifndef TX_POWER_MANAGER_H
#define TX_POWER_MANAGER_H

#include <string>
#include <optional>

class TxPowerManager {
public:
    TxPowerManager();
    ~TxPowerManager();

    /**
     * Imposta la potenza di trasmissione dell'interfaccia specificata.
     * * @param interface_name Nome dell'interfaccia (es. "wlan0")
     * @param power_dbm Potenza desiderata in dBm
     * @return true se il comando ha successo, false in caso di errore
     */
    std::optional<double> get_tx_power(const std::string& interface_name);
    bool set_tx_power(const std::string& interface_name, int power_dbm, bool bounded_io = false);
};

#endif // TX_POWER_MANAGER_H