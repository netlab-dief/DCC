#include "common/console_log.h"
#include "radio/rate_manager.h"
#include "common/bounded_io.h"
#include <memory>
#include <iostream>

RateManager::RateManager() : is_initialized_(false) {}

RateManager::~RateManager() {
    cleanup();
}

bool RateManager::setup_rate_manager(const std::string& interface) {
    interface_ = interface;
    is_initialized_ = true;
    //ConsoleLine() << interface_ << " [RateManager] Inizializzazione completata." << std::endl;
    return true;
}

int RateManager::map_rate_index(int rate_idx) const {
    // Mappa l'indice passato dal DCC sul numero Mbps effettivo da passare a "iw"
    switch (rate_idx) {
        case 24:  return 24; 
        case 12:  return 12; 
        case 6: return 6; 
        default: return rate_idx; // Fallback: usa il numero grezzo passato
    }
}

void RateManager::set_fixed_rate(int rate_idx) {
    set_fixed_rate_netlink(rate_idx);
}

bool RateManager::set_fixed_rate_netlink(int rate_idx) {
    if (!is_initialized_) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Errore: Istanza non inizializzata!\033[0m" << std::endl;
        return false;
    }

    int bitrate_num = map_rate_index(rate_idx);
    switch (bitrate_num) {
        case 6: case 9: case 12: case 18: case 24: case 36: case 48: case 54: break;
        default: return false;
    }

    // 1. Risoluzione dell'indice dell'interfaccia di rete (es. "wlan1" -> 3)
    unsigned int if_index = if_nametoindex(interface_.c_str());
    if (if_index == 0) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Interfaccia non trovata: " << interface_ << "\033[0m" << std::endl;
        return false;
    }

    // 2. Allocazione e connessione del socket Netlink
    std::unique_ptr<nl_sock, decltype(&nl_socket_free)> owned_socket(nl_socket_alloc(), nl_socket_free);
    auto* socket = owned_socket.get();
    if (!socket) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Errore allocazione socket Netlink\033[0m" << std::endl;
        return false;
    }

    if (genl_connect(socket) < 0) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Connessione Generic Netlink fallita\033[0m" << std::endl;
        return false;
    }
    if (!set_io_timeout(nl_socket_get_fd(socket))) return false;

    // 3. Risoluzione dinamica dell'ID del driver 'nl80211'
    int driver_id = genl_ctrl_resolve(socket, "nl80211");
    if (driver_id < 0) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Driver nl80211 non trovato nel kernel\033[0m" << std::endl;
        return false;
    }

    // OCB broadcast traffic uses the multicast rate, not the unicast mask.
    int interface_type = -1;
    if (nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM,
        [](nl_msg* reply, void* data) -> int {
            nlattr* attrs[NL80211_ATTR_MAX + 1] = {};
            auto* header = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(reply)));
            if (nla_parse(attrs, NL80211_ATTR_MAX, genlmsg_attrdata(header, 0),
                          genlmsg_attrlen(header, 0), nullptr) < 0) return NL_SKIP;
            if (attrs[NL80211_ATTR_IFTYPE] &&
                nla_len(attrs[NL80211_ATTR_IFTYPE]) == sizeof(uint32_t))
                *static_cast<int*>(data) = nla_get_u32(attrs[NL80211_ATTR_IFTYPE]);
            return NL_OK;
        }, &interface_type) < 0) return false;
    std::unique_ptr<nl_msg, decltype(&nlmsg_free)> query(nlmsg_alloc(), nlmsg_free);
    if (!query || !genlmsg_put(query.get(), NL_AUTO_PORT, NL_AUTO_SEQ, driver_id,
        0, NLM_F_REQUEST, NL80211_CMD_GET_INTERFACE, 0) ||
        nla_put_u32(query.get(), NL80211_ATTR_IFINDEX, if_index) < 0) return false;
    if (nl_send_sync(socket, query.release()) < 0 || interface_type < 0) return false;
    const bool ocb = interface_type == NL80211_IFTYPE_OCB;

    // 4. Allocazione del messaggio Netlink principale
    std::unique_ptr<nl_msg, decltype(&nlmsg_free)> owned_msg(nlmsg_alloc(), nlmsg_free);
    auto* msg = owned_msg.get();
    if (!msg) {
        return false;
    }

    // 5. Configurazione dell'intestazione del messaggio (NL80211_CMD_SET_TX_BITRATE_MASK)
    if (!genlmsg_put(msg,
                NL_AUTO_PORT, 
                NL_AUTO_SEQ, 
                driver_id, 
                0, 
                NLM_F_REQUEST, 
                ocb ? NL80211_CMD_SET_MCAST_RATE : NL80211_CMD_SET_TX_BITRATE_MASK,
                0)) return false;

    // Aggiunta dell'indice dell'interfaccia target
    if (nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_index) < 0) return false;

    // 6. Costruzione della struttura nidificata presa dall'esempio di 'iw'
    bool serialization_success = false;
    
    // nl_rates = nla_nest_start(msg, NL80211_ATTR_TX_RATES);
    struct nlattr* nl_rates = ocb ? nullptr : nla_nest_start(msg, NL80211_ATTR_TX_RATES);
    if (ocb) {
        // nl80211 multicast rates are expressed in units of 100 kbit/s.
        serialization_success = nla_put_u32(msg, NL80211_ATTR_MCAST_RATE, bitrate_num * 10) >= 0;
    } else if (nl_rates) {
        // nl_band = nla_nest_start(msg, NL80211_BAND_5GHZ);
        struct nlattr* nl_band = nla_nest_start(msg, NL80211_BAND_5GHZ);
        if (nl_band) {
            // Calcolo del valore secondo i sorgenti di iw: Mbps * 2 (es. 24 Mbps * 2 = 48)
            uint8_t legacy_5 = static_cast<uint8_t>(bitrate_num * 2);
            
            // nla_put(msg, NL80211_TXRATE_LEGACY, n_legacy_5, legacy_5);
            // Passiamo la dimensione dell'array (1 byte nel nostro caso singolo) e il puntatore al dato
            if (nla_put(msg, NL80211_TXRATE_LEGACY, sizeof(legacy_5), &legacy_5) >= 0) {
                nla_nest_end(msg, nl_band);
                nla_nest_end(msg, nl_rates);
                serialization_success = true;
            }
        }
    }

    if (!serialization_success) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Errore di serializzazione dei parametri Netlink!\033[0m" << std::endl;
        return false;
    }

    // 7. Invio sincrono al kernel e attesa dell'ACK di successo
    int ret = nl_send_sync(socket, owned_msg.release()); // libnl consumes the message.

    // 8. Gestione del risultato ritornato dal kernel
    if (ret != 0) {
        ConsoleLine(std::cerr) << "\033[1;31m[RateManager][Netlink] Errore nell'impostare il bitrate: "
                  << interface_ << (ocb ? " (OCB multicast " : " (legacy-5 ") << bitrate_num << " Mbps): " << nl_geterror(ret) << "\033[0m" << std::endl;
        return false;
    } else {
        ConsoleLine() << "\033[1;32m[RateManager][Netlink] Bitrate impostato: " << interface_
                  << (ocb ? " -> OCB multicast " : " -> legacy-5 ") << bitrate_num << " Mbps\033[0m" << std::endl;
    }
    return true;
}

void RateManager::cleanup() {
    is_initialized_ = false;
}
