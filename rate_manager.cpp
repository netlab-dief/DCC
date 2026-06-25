#include "rate_manager.h"
#include <iostream>
#include <cstdlib> // Necessario per std::system
#include <sstream>

RateManager::RateManager() : is_initialized_(false) {}

RateManager::~RateManager() {
    cleanup();
}

bool RateManager::setup_rate_manager(const std::string& interface) {
    interface_ = interface;
    is_initialized_ = true;
    //std::cout << interface_ << " [RateManager] Inizializzazione completata." << std::endl;
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
    if (!is_initialized_) {
        std::cerr << "\033[1;31m[RateManager] Errore: Istanza non inizializzata!\033[0m" << std::endl;
        return;
    }

    // Otteniamo il numero Mbps (6, 12 o 24)
    int bitrate_num = map_rate_index(rate_idx);

    // Componiamo la stringa del comando esatto
    std::stringstream ss;
    ss << "sudo iw dev " << interface_ << " set bitrates legacy-5 " << bitrate_num << " > /dev/null 2>&1";
    std::string command = ss.str();

    // Eseguiamo il comando a livello di sistema operativo
    int ret = std::system(command.c_str());

    if (ret != 0) {
        std::cerr << "\033[1;31m[RateManager] Errore nell'esecuzione del comando: " << command << "\033[0m" << std::endl;
    } else {
        std::cout << "\033[1;32m[RateManager] Comando eseguito: iw dev " << interface_ 
                  << " set bitrates legacy-5 " << bitrate_num << "\033[0m" << std::endl;
    }
}

void RateManager::set_fixed_rate_netlink(int rate_idx) {
    if (!is_initialized_) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Errore: Istanza non inizializzata!\033[0m" << std::endl;
        return;
    }

    int bitrate_num = map_rate_index(rate_idx);

    // 1. Risoluzione dell'indice dell'interfaccia di rete
    unsigned int if_index = if_nametoindex(interface_.c_str());
    if (if_index == 0) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Interfaccia non trovata: " << interface_ << "\033[0m" << std::endl;
        return;
    }

    // 2. Allocazione e connessione del socket Netlink
    struct nl_sock* socket = nl_socket_alloc();
    if (!socket) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Errore allocazione socket Netlink\033[0m" << std::endl;
        return;
    }

    if (genl_connect(socket) < 0) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Connessione Generic Netlink fallita\033[0m" << std::endl;
        nl_socket_free(socket);
        return;
    }

    // 3. Risoluzione dinamica dell'ID del driver 'nl80211'
    int driver_id = genl_ctrl_resolve(socket, "nl80211");
    if (driver_id < 0) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Driver nl80211 non trovato nel kernel\033[0m" << std::endl;
        nl_socket_free(socket);
        return;
    }

    // 4. Allocazione del messaggio Netlink principale
    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) {
        nl_socket_free(socket);
        return;
    }

    // 5. Configurazione dell'intestazione del messaggio
    genlmsg_put(msg, 
                NL_AUTO_PORT, 
                NL_AUTO_SEQ, 
                driver_id, 
                0, 
                NLM_F_REQUEST, 
                NL80211_CMD_SET_TX_BITRATE_MASK, 
                0);

    // Impostiamo l'interfaccia
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_index);

    // 6. Costruzione della struttura nidificata dei bitrate tramite nest pointers
    bool serialization_success = false;
    struct nlattr* tx_rates_nest = nla_nest_start(msg, NL80211_ATTR_TX_RATES);
    
    if (tx_rates_nest) {
        struct nlattr* band_nest = nla_nest_start(msg, NL80211_BAND_5GHZ);
        if (band_nest) {
            uint8_t rate_value = static_cast<uint8_t>(bitrate_num * 10);
            if (nla_put(msg, NL80211_TXRATE_LEGACY, sizeof(rate_value), &rate_value) >= 0) {
                nla_nest_end(msg, band_nest);
                nla_nest_end(msg, tx_rates_nest);
                serialization_success = true;
            }
        }
    }

    if (!serialization_success) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Errore di serializzazione dei parametri Netlink!\033[0m" << std::endl;
        nlmsg_free(msg);
        nl_socket_free(socket);
        return;
    }

    // 7. Invio sincrono al kernel e attesa dell'ACK di successo
    int ret = nl_send_sync(socket, msg);

    // Pulizia del socket principale (nl_send_sync si occupa già di liberare il 'msg')
    nl_socket_free(socket);

    // 8. Gestione del risultato
    if (ret != 0) {
        std::cerr << "\033[1;31m[RateManager][Netlink] Errore nell'impostare il bitrate: " 
                  << nl_geterror(ret) << "\033[0m" << std::endl;
    } else {
        std::cout << "\033[1;32m[RateManager][Netlink] Bitrate impostato: " << interface_ 
                  << " -> legacy-5 " << bitrate_num << " Mbps\033[0m" << std::endl;
    }
}

void RateManager::cleanup() {
    is_initialized_ = false;
}