#ifndef CBRREADER_H
#define CBRREADER_H

#include <string>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>
#include <iostream>
#include <net/if.h>
#include <errno.h>
#include <netlink/genl/family.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <thread>
#include <mutex> // Incluso per std::mutex
#include <optional>
#include "common/utils.h"

// Forward declaration della classe per il cbr_handler (se necessario)
class CBRReader;
int cbr_handler(struct nl_msg *msg, void *arg);

class CBRReader {
    private:
        typedef struct CBRData {
            bool firstTime = true;
            bool verbose = false;
            unsigned long long startActiveTime;
            unsigned long long startBusyTime;
            unsigned long long startReceiveTime;
            unsigned long long startTransmitTime;
            bool haveRxTime = false, haveTxTime = false;
            std::optional<unsigned> frequency;
            float currentCBR = -1.0f;
            float currentTxTime = -1.0f;
            float currentRxTime = -1.0f;
            float currentBusyTime = -1.0f;
        } CBRData;

        void start_reading_cbr(nl_sock_info_t m_nl_sock_info);
        void read_cbr_from_netlink(nl_sock_info_t nl_sock_info);

        std::string m_dissemination_interface;

        // Spostata in private per impedire accessi diretti non sincronizzati dall'esterno
        CBRData m_cbr_data;

        // Permette alla funzione callback globale di accedere al mutex e ai dati privati
        friend int cbr_handler(struct nl_msg *msg, void *arg);

    public:
        CBRReader();
        ~CBRReader();

        void setupCBRReader(bool verbose, std::string dissemination_interface);
        float get_current_cbr();
        float get_current_busy_time();
        float get_current_tx_time();
        float get_current_rx_time();
        void wrapper_start_reading_cbr();

        // mutable permette il lock_guard anche all'interno di eventuali metodi const (es. i getter)
        mutable std::mutex m_mutex; 
};

#endif // CBRREADER_H
