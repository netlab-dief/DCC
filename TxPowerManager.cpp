#include "TxPowerManager.h"
#include <iostream>
#include <net/if.h> // Per if_nametoindex
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>

TxPowerManager::TxPowerManager() = default;

TxPowerManager::~TxPowerManager() = default;

bool TxPowerManager::set_tx_power(const std::string& interface_name, int power_dbm) {
    // 1. Inizializzazione del socket Netlink
    struct nl_sock *nl_sock = nl_socket_alloc();
    if (!nl_sock) {
        std::cerr << "Errore: impossibile allocare il socket Netlink.\n";
        return false;
    }

    if (genl_connect(nl_sock) < 0) {
        std::cerr << "Errore: impossibile connettersi al socket Generic Netlink.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 2. Risoluzione dell'ID della famiglia nl80211
    int nl80211_id = genl_ctrl_resolve(nl_sock, "nl80211");
    if (nl80211_id < 0) {
        std::cerr << "Errore: famiglia nl80211 non trovata nel kernel.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 3. Ottenimento dell'indice dell'interfaccia di rete
    unsigned int ifindex = if_nametoindex(interface_name.c_str());
    if (ifindex == 0) {
        std::cerr << "Errore: interfaccia '" << interface_name << "' non trovata.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 4. Creazione del messaggio di configurazione
    struct nl_msg *msg = nlmsg_alloc();
    if (!msg) {
        std::cerr << "Errore: impossibile allocare il messaggio Netlink.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // Specifica il comando SET_WIPHY per modificare i parametri hardware
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, 0, NL80211_CMD_SET_WIPHY, 0);

    // 5. Inserimento degli attributi (Payload)
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
    
    // Imposta la modalità su "Fissa"
    nla_put_u32(msg, NL80211_ATTR_WIPHY_TX_POWER_SETTING, NL80211_TX_POWER_FIXED);
    
    // Il kernel Linux si aspetta il valore in mBm (milli-Bel-milliwatts). 1 dBm = 100 mBm.
    nla_put_u32(msg, NL80211_ATTR_WIPHY_TX_POWER_LEVEL, power_dbm * 100);

    // 6. Invio del messaggio
    int err = nl_send_auto_complete(nl_sock, msg);
    nlmsg_free(msg); // Il messaggio può essere liberato dopo l'invio

    if (err < 0) {
        std::cerr << "Errore nell'invio del messaggio Netlink.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 7. Lettura della risposta (Aspettiamo un ACK o un errore dal kernel)
    err = nl_recvmsgs_default(nl_sock);
    
    // Pulizia
    nl_socket_free(nl_sock);

    if (err < 0) {
        std::cerr << "Operazione fallita dal Kernel: " << nl_geterror(err) << "\n";
        return false;
    }

    return true;
}