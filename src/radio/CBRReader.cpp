#include "common/console_log.h"
#include "radio/CBRReader.h"
#include <stdexcept>
#include <mutex> // Incluso per std::mutex e std::lock_guard

CBRReader::CBRReader () = default;

CBRReader::~CBRReader () = default;

void CBRReader::setupCBRReader(bool verbose, std::string dissemination_interface)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cbr_data = CBRData{};
    m_cbr_data.verbose = verbose;
    m_dissemination_interface = dissemination_interface;
}

float CBRReader::get_current_cbr()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cbr_data.currentCBR;
}

float CBRReader::get_current_busy_time()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cbr_data.currentBusyTime;
}

float CBRReader::get_current_tx_time()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cbr_data.currentTxTime;
}

float CBRReader::get_current_rx_time()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cbr_data.currentRxTime;
}

void CBRReader::start_reading_cbr(nl_sock_info_t m_nl_sock_info)
{
    pthread_setname_np(pthread_self(), "CBR_reader_thr");

    if (m_nl_sock_info.sock_valid==true)
    {
        read_cbr_from_netlink(m_nl_sock_info);
    }
    else
    {
        throw std::runtime_error("Socket is invalid. Cannot read from netlink.");
    }
}

void CBRReader::read_cbr_from_netlink(nl_sock_info_t nl_sock_info)
{
    struct nl_msg *nl_msg = nlmsg_alloc();
    if (!nl_msg) {
        free_nl_socket(nl_sock_info);
        return;
    }

    if (!genlmsg_put(nl_msg,NL_AUTO_PORT,NL_AUTO_SEQ,nl_sock_info.nl80211_id,0,NLM_F_DUMP | NLM_F_REQUEST,NL80211_CMD_GET_SURVEY,0) ||
        nla_put_u32(nl_msg,NL80211_ATTR_IFINDEX,nl_sock_info.ifindex) < 0) {
        nlmsg_free(nl_msg);
        free_nl_socket(nl_sock_info);
        return;
    }

    nl_socket_modify_cb(nl_sock_info.nl_sock,NL_CB_VALID,NL_CB_CUSTOM,cbr_handler,this);

    int err = nl_send_auto_complete(nl_sock_info.nl_sock,nl_msg);
    if (err >= 0) err = nl_recvmsgs_default(nl_sock_info.nl_sock);
    if (err < 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cbr_data.currentCBR = -1.0f;
    }

    nlmsg_free(nl_msg);
    free_nl_socket(nl_sock_info);
}

void CBRReader::wrapper_start_reading_cbr()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cbr_data.currentCBR = -1.0f;
    }
    nl_sock_info_t nl_sock_info = open_nl_socket(m_dissemination_interface, 80);
    if (!nl_sock_info.sock_valid) return;
    // Synchronous acquisition: errors no longer escape an uncaught child thread.
    start_reading_cbr(nl_sock_info);
    return;
}

int cbr_handler(struct nl_msg *msg, void *arg)
{
    auto* reader = reinterpret_cast<CBRReader*>(arg);
    
    // Acquisiamo il lock sull'istanza di CBRReader poiché modificheremo/leggeremo m_cbr_data
    // Il lock verrà rilasciato automaticamente a fine funzione (RAII)
    std::lock_guard<std::mutex> lock(reader->m_mutex);

    struct nlattr *tb[NL80211_ATTR_MAX + 1]{};
    struct genlmsghdr *gnlh = (struct genlmsghdr *) nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *sinfo[NL80211_SURVEY_INFO_MAX + 1]{};

    struct nla_policy survey_policy[NL80211_SURVEY_INFO_MAX + 1] = {};
    survey_policy[NL80211_SURVEY_INFO_FREQUENCY].type = NLA_U32;
    survey_policy[NL80211_SURVEY_INFO_NOISE].type = NLA_U8;
    for (int attribute : {NL80211_SURVEY_INFO_CHANNEL_TIME, NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY,
            NL80211_SURVEY_INFO_CHANNEL_TIME_RX, NL80211_SURVEY_INFO_CHANNEL_TIME_TX})
        survey_policy[attribute].type = NLA_U64;

    auto invalidate = [&] {
        reader->m_cbr_data.firstTime = true;
        reader->m_cbr_data.currentCBR = -1.0f;
        reader->m_cbr_data.currentRxTime = -1.0f;
        reader->m_cbr_data.currentTxTime = -1.0f;
        reader->m_cbr_data.currentBusyTime = -1.0f;
        return NL_SKIP;
    };
    if (nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL) < 0)
        return invalidate();

    if (!tb[NL80211_ATTR_SURVEY_INFO]) {
        fprintf(stderr, "Survey data missing!\n");
        return invalidate();
    }

    if (nla_parse_nested(sinfo, NL80211_SURVEY_INFO_MAX, tb[NL80211_ATTR_SURVEY_INFO], survey_policy)) {
        fprintf(stderr, "Failed to parse nested survey attributes!\n");
        return invalidate();
    }

    unsigned long long activeTime = 0, busyTime = 0, rxTime = 0, txTime = 0;

    if (!sinfo[NL80211_SURVEY_INFO_IN_USE])
    {
        return NL_SKIP;  // Not the active channel
    }
    if (!sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME] || !sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY])
        return invalidate();
    if (sinfo[NL80211_SURVEY_INFO_FREQUENCY]) {
        const auto frequency = nla_get_u32(sinfo[NL80211_SURVEY_INFO_FREQUENCY]);
        if (reader->m_cbr_data.frequency && reader->m_cbr_data.frequency != frequency)
            invalidate();
        reader->m_cbr_data.frequency = frequency;
    }

    if (sinfo[NL80211_SURVEY_INFO_FREQUENCY] && reader->m_cbr_data.verbose && reader->m_cbr_data.firstTime)
    {
        printf("Frequency:\t\t\t%u MHz%s\n", nla_get_u32(sinfo[NL80211_SURVEY_INFO_FREQUENCY]),
               sinfo[NL80211_SURVEY_INFO_IN_USE] ? " [in use]" : "");
    }

    if (sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME])
        activeTime = nla_get_u64(sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME]);

    if (sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY])
        busyTime = nla_get_u64(sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY]);

    if (sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_RX])
        rxTime = nla_get_u64(sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_RX]);

    if (sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_TX])
        txTime = nla_get_u64(sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_TX]);
    if (busyTime > activeTime) return invalidate();

    bool firstTime = reader->m_cbr_data.firstTime;

    if (firstTime) {
        if (reader->m_cbr_data.verbose)
        {
            ConsoleLine() << "First Time for CBR Computing" << std::endl;
        }
        reader->m_cbr_data.firstTime = false;
        reader->m_cbr_data.startActiveTime = activeTime;
        reader->m_cbr_data.startBusyTime = busyTime;
        reader->m_cbr_data.startReceiveTime = rxTime;
        reader->m_cbr_data.startTransmitTime = txTime;
        reader->m_cbr_data.haveRxTime = sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_RX] != nullptr;
        reader->m_cbr_data.haveTxTime = sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_TX] != nullptr;
        reader->m_cbr_data.currentCBR = -1.0f;
        return NL_SKIP;
    }

    // Check for counter reset or wraparound
    if (activeTime < reader->m_cbr_data.startActiveTime || busyTime < reader->m_cbr_data.startBusyTime) {
        if (reader->m_cbr_data.verbose)
        {
            ConsoleLine(std::cerr) << "Warning: time counters reset or wrapped. Skipping...\n";
        }
        reader->m_cbr_data.startActiveTime = activeTime;
        reader->m_cbr_data.startBusyTime = busyTime;
        reader->m_cbr_data.startReceiveTime = rxTime;
        reader->m_cbr_data.startTransmitTime = txTime;
        return invalidate();
    }

    unsigned long long deltaActiveTime = activeTime - reader->m_cbr_data.startActiveTime;
    unsigned long long deltaBusyTime = busyTime - reader->m_cbr_data.startBusyTime;
    const bool rx_valid = sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_RX] && reader->m_cbr_data.haveRxTime &&
        rxTime >= reader->m_cbr_data.startReceiveTime;
    const bool tx_valid = sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_TX] && reader->m_cbr_data.haveTxTime &&
        txTime >= reader->m_cbr_data.startTransmitTime;
    unsigned long long deltaReceiveTime = rx_valid ? rxTime - reader->m_cbr_data.startReceiveTime : 0;
    unsigned long long deltaTransmitTime = tx_valid ? txTime - reader->m_cbr_data.startTransmitTime : 0;
    if (!deltaActiveTime || deltaBusyTime > deltaActiveTime) return invalidate();

    if (reader->m_cbr_data.verbose)
    {
        ConsoleLine() << "Active-time: " << deltaActiveTime
                  << " Busy-time: " << deltaBusyTime
                  << " RX-time: " << deltaReceiveTime
                  << " TX-time: " << deltaTransmitTime << std::endl;
    }

    float cbr = -1.0f;

    if (deltaActiveTime > 0) {
        cbr = static_cast<float>(deltaBusyTime) / deltaActiveTime;
    }

    if (reader->m_cbr_data.verbose && cbr >= 0.0f)
        ConsoleLine() << "Current Channel Busy Ratio: " << cbr << std::endl;

    // Save new base values for next delta
    reader->m_cbr_data.currentCBR = cbr;
    reader->m_cbr_data.startActiveTime = activeTime;
    reader->m_cbr_data.startBusyTime = busyTime;
    reader->m_cbr_data.startReceiveTime = rxTime;
    reader->m_cbr_data.startTransmitTime = txTime;
    reader->m_cbr_data.currentBusyTime = deltaBusyTime;
    reader->m_cbr_data.currentRxTime = rx_valid ? static_cast<float>(deltaReceiveTime) : -1.0f;
    reader->m_cbr_data.currentTxTime = tx_valid ? static_cast<float>(deltaTransmitTime) : -1.0f;
    reader->m_cbr_data.haveRxTime = sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_RX] != nullptr;
    reader->m_cbr_data.haveTxTime = sinfo[NL80211_SURVEY_INFO_CHANNEL_TIME_TX] != nullptr;

    return NL_SKIP;
}
