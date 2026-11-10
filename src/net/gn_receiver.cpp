#include "net/gn_receiver.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>
#include <stdexcept>
#ifdef __linux__
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#endif

void GnReceiver::start(const std::string& radio, const std::string& survey,
                       std::function<std::optional<CbrSample>()> local) {
    if (thread_.joinable()) return;
#ifndef __linux__
    (void)radio; (void)survey; (void)local;
    throw std::runtime_error("--dcc-net requires Linux and an ITS-G5 radiotap monitor");
#else
    if (radio.size() >= IFNAMSIZ || survey.size() >= IFNAMSIZ ||
        radio.find('/') != std::string::npos || survey.find('/') != std::string::npos)
        throw std::invalid_argument("Invalid DCC_NET interface name");
    // GN_ADDR is keyed per this fixed radio/channel. Refuse unrelated PHYs.
    char radio_phy[4096], survey_phy[4096];
    if (!realpath(("/sys/class/net/" + radio + "/phy80211").c_str(), radio_phy) ||
        !realpath(("/sys/class/net/" + survey + "/phy80211").c_str(), survey_phy) ||
        std::strcmp(radio_phy, survey_phy) != 0)
        throw std::runtime_error("--dcc-net requires survey and monitor interfaces on the same PHY");
    fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd_ < 0) throw std::runtime_error("DCC_NET raw socket: " + std::string(std::strerror(errno)));
    try {
        ifreq request{};
        std::memcpy(request.ifr_name, radio.c_str(), radio.size() + 1);
        if (ioctl(fd_, SIOCGIFHWADDR, &request) < 0 || request.ifr_hwaddr.sa_family != ARPHRD_IEEE80211_RADIOTAP)
            throw std::runtime_error("--dcc-net requires a radiotap monitor interface");
        std::copy_n(reinterpret_cast<const std::uint8_t*>(request.ifr_hwaddr.sa_data), 6, own_mac_.begin());
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = static_cast<int>(if_nametoindex(radio.c_str()));
        if (!address.sll_ifindex || bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw std::runtime_error("DCC_NET monitor bind failed");
        state_.reset();
        stopping_ = false;
        { std::lock_guard<std::mutex> lock(mutex_); failure_ = {}; }
        local_ = std::move(local);
        thread_ = std::thread([this] {
            try { run(); }
            catch (...) {
                { std::lock_guard<std::mutex> lock(mutex_); failure_ = std::current_exception(); }
                stopping_ = true;
                state_.stop();
            }
        });
    } catch (...) { close(fd_); fd_ = -1; throw; }
#endif
}
void GnReceiver::stop() {
    stopping_ = true;
    state_.stop();
    if (thread_.joinable()) thread_.join();
#ifdef __linux__
    if (fd_ >= 0) close(fd_);
#endif
    fd_ = -1;
}
void GnReceiver::check() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
}
void GnReceiver::run() {
#ifdef __linux__
    std::mt19937 random(std::random_device{}());
    auto next = dccnet::Clock::now() + std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 99)(random));
    while (!stopping_) {
        const auto now = dccnet::Clock::now();
        if (now >= next) {
            const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() / 100;
            state_.trigger(local_(), now, utc);
            // Preserve the random phase, skip missed intervals without catch-up.
            do { next += dccnet::State::interval; } while (next <= now);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next - dccnet::Clock::now()).count();
        pollfd descriptor{fd_, POLLIN, 0};
        const auto ready = poll(&descriptor, 1, static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 50)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("DCC_NET monitor poll failed");
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("DCC_NET monitor disconnected");
        if (!(descriptor.revents & POLLIN)) continue;
        std::vector<std::uint8_t> buffer(65536);
        sockaddr_ll source{};
        socklen_t length = sizeof(source);
        const auto count = recvfrom(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC,
                                    reinterpret_cast<sockaddr*>(&source), &length);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::runtime_error("DCC_NET monitor receive failed");
        }
        if (static_cast<std::size_t>(count) > buffer.size() || source.sll_pkttype == PACKET_OUTGOING) continue;
        buffer.resize(static_cast<std::size_t>(count));
        const auto observation = dccnet::parse(buffer);
        if (!observation || std::equal(own_mac_.begin(), own_mac_.end(), observation->address.begin() + 2)) continue;
        state_.receive(*observation, dccnet::Clock::now());
    }
#endif
}
