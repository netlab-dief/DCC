#include "console_log.h"
#include "TxPowerManager.h"
#include <iostream>
#include <net/if.h> // Per if_nametoindex
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cstdint>

TxPowerManager::TxPowerManager() = default;

TxPowerManager::~TxPowerManager() = default;

bool TxPowerManager::set_tx_power(const std::string& interface_name, int power_dbm, bool bounded_io) {
    // 1. Inizializzazione del socket Netlink
    struct nl_sock *nl_sock = nl_socket_alloc();
    if (!nl_sock) {
        ConsoleLine(std::cerr) << "Errore: impossibile allocare il socket Netlink.\n";
        return false;
    }

    if (genl_connect(nl_sock) < 0) {
        ConsoleLine(std::cerr) << "Errore: impossibile connettersi al socket Generic Netlink.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    if (bounded_io) {
        timeval timeout{0, 80000};
        setsockopt(nl_socket_get_fd(nl_sock), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(nl_socket_get_fd(nl_sock), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    // 2. Risoluzione dell'ID della famiglia nl80211
    int nl80211_id = genl_ctrl_resolve(nl_sock, "nl80211");
    if (nl80211_id < 0) {
        ConsoleLine(std::cerr) << "Errore: famiglia nl80211 non trovata nel kernel.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 3. Ottenimento dell'indice dell'interfaccia di rete
    unsigned int ifindex = if_nametoindex(interface_name.c_str());
    if (ifindex == 0) {
        ConsoleLine(std::cerr) << "Errore: interfaccia '" << interface_name << "' non trovata.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 4. Creazione del messaggio di configurazione
    struct nl_msg *msg = nlmsg_alloc();
    if (!msg) {
        ConsoleLine(std::cerr) << "Errore: impossibile allocare il messaggio Netlink.\n";
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
        ConsoleLine(std::cerr) << "Errore nell'invio del messaggio Netlink.\n";
        nl_socket_free(nl_sock);
        return false;
    }

    // 7. Lettura della risposta (Aspettiamo un ACK o un errore dal kernel)
    err = nl_recvmsgs_default(nl_sock);
    
    // Pulizia
    nl_socket_free(nl_sock);

    if (err < 0) {
        ConsoleLine(std::cerr) << "Operazione fallita dal Kernel: " << nl_geterror(err) << "\n";
        return false;
    }

    return true;
}

std::optional<double> TxPowerManager::get_tx_power(const std::string& interface_name) {
    auto* socket = nl_socket_alloc();
    if (!socket) return {};
    std::optional<double> result;
    auto cleanup = [&] { nl_socket_free(socket); return result; };
    if (genl_connect(socket) < 0) return cleanup();
    timeval timeout{0, 80000};
    setsockopt(nl_socket_get_fd(socket), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(nl_socket_get_fd(socket), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const auto family = genl_ctrl_resolve(socket, "nl80211");
    const auto index = if_nametoindex(interface_name.c_str());
    if (family < 0 || !index) return cleanup();
    auto* message = nlmsg_alloc();
    if (!message) return cleanup();
    if (!genlmsg_put(message, NL_AUTO_PORT, NL_AUTO_SEQ, family, 0, 0, NL80211_CMD_GET_INTERFACE, 0) ||
        nla_put_u32(message, NL80211_ATTR_IFINDEX, index) < 0) {
        nlmsg_free(message);
        return cleanup();
    }
    nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, [](nl_msg* reply, void* context) -> int {
        auto* header = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(reply)));
        nlattr* attributes[NL80211_ATTR_MAX + 1]{};
        if (nla_parse(attributes, NL80211_ATTR_MAX, genlmsg_attrdata(header, 0), genlmsg_attrlen(header, 0), nullptr) == 0 &&
            attributes[NL80211_ATTR_WIPHY_TX_POWER_LEVEL] &&
            nla_len(attributes[NL80211_ATTR_WIPHY_TX_POWER_LEVEL]) >= 4) {
            *static_cast<std::optional<double>*>(context) =
                static_cast<std::int32_t>(nla_get_u32(attributes[NL80211_ATTR_WIPHY_TX_POWER_LEVEL])) / 100.0;
        }
        return NL_OK;
    }, &result);
    const auto sent = nl_send_auto_complete(socket, message);
    nlmsg_free(message);
    if (sent < 0 || nl_recvmsgs_default(socket) < 0) result.reset();
    return cleanup();
}
