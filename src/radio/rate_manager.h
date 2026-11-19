#ifndef RATE_MANAGER_H
#define RATE_MANAGER_H

#include <string>
#include <net/if.h>               // Per if_nametoindex
#include <netlink/netlink.h>      // Core libnl
#include <netlink/genl/genl.h>    // Generic Netlink
#include <netlink/genl/ctrl.h>    // Controllo famiglia nl80211
#include <linux/nl80211.h>        // Costanti kernel nl80211

class RateManager {
public:
    RateManager();
    ~RateManager();

    // Inizializza il manager passando l'interfaccia (es. "wlan1")
    bool setup_rate_manager(const std::string& interface);

    // Esegue direttamente il comando iw con il numero corrispondente all'indice
    void set_fixed_rate(int rate_idx);
    bool set_fixed_rate_netlink(int rate_idx);

    void cleanup();

private:
    std::string interface_;
    bool is_initialized_;

    // Mappa l'indice DCC (3, 6, 12) sul numero di bitrate desiderato (24, 12, 6)
    int map_rate_index(int rate_idx) const;
};

#endif // RATE_MANAGER_H
