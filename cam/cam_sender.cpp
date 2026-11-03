#include "cam_sender.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cam {
Sender::Sender(AirtimeGate& gate, std::string interface, Options options,
               std::function<int()> dcc_interval)
    : gate_(gate), interface_(std::move(interface)), options_(std::move(options)),
      dcc_interval_(std::move(dcc_interval)),
      thread_([this] {
          try { run(); }
          catch (...) { std::lock_guard<std::mutex> lock(mutex_); failure_ = std::current_exception(); }
      }) {}
Sender::~Sender() { stop(); }
void Sender::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}
void Sender::check() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
}

bool Sender::transmit(AirtimeGate& gate, std::uint64_t id, std::uint64_t estimate,
    const std::function<std::uint64_t()>& send_and_wait, const std::atomic<bool>& stopping,
    const std::function<bool()>& ready, const std::function<std::uint64_t()>& current_estimate) {
    const auto suffix = " " + std::to_string(id);
    while (!stopping) {
        if (ready && !ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const auto reservation = current_estimate ? current_estimate() : estimate;
        const auto reply = gate.handle("ADMIT" + suffix + " " + std::to_string(reservation), AirtimeGate::Clock::now());
        if (reply == "GRANT" + suffix) {
            // Once reserved, complete/report even if shutdown was requested.
            const auto actual = send_and_wait();
            if (actual == 0 || actual > 1000000) throw std::runtime_error("Invalid CAM PHY airtime feedback");
            if (gate.handle("REPORT" + suffix + " " + std::to_string(actual), AirtimeGate::Clock::now()) != "DONE" + suffix)
                throw std::runtime_error("CAM TX report rejected");
            return true;
        }
        if (reply.rfind("WAIT" + suffix + " ", 0) != 0)
            throw std::runtime_error("CAM admission rejected: " + reply);
        const auto delay = std::stoull(reply.substr(reply.find_last_of(' ') + 1));
        std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::uint64_t>(delay, 10000)));
    }
    return false;
}

#ifdef __linux__
namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {
        if (fd < 0) throw std::runtime_error(std::strerror(errno));
    }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

void checked(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
} // namespace
#endif

void Sender::run() {
#ifndef __linux__
    (void)gate_;
    throw std::runtime_error("CAM radio transmission requires Linux AF_PACKET");
#else
    Fd radio(socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)));
    ifreq request{};
    if (interface_.size() >= IFNAMSIZ) throw std::runtime_error("CAM interface name too long");
    std::memcpy(request.ifr_name, interface_.c_str(), interface_.size() + 1);
    checked(ioctl(radio.value, SIOCGIFINDEX, &request) == 0, "CAM interface index");
    sockaddr_ll destination{};
    destination.sll_family = AF_PACKET;
    destination.sll_ifindex = request.ifr_ifindex;
    destination.sll_protocol = htons(ETH_P_ALL);
    destination.sll_halen = 6;
    std::memset(destination.sll_addr, 255, 6);
    checked(ioctl(radio.value, SIOCGIFHWADDR, &request) == 0, "CAM interface MAC");
    std::array<std::uint8_t, 6> mac{};
    std::memcpy(mac.data(), request.ifr_hwaddr.sa_data, mac.size());

    // A driver bridge supplies real TX completion/airtime. sendto() alone only
    // confirms queueing; it must never be reported as completed PHY transmission.
    Fd feedback(socket(AF_INET, SOCK_DGRAM, 0));
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = htons(options_.feedback_port);
    checked(connect(feedback.value, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0, "CAM feedback connect");

    std::unique_ptr<Fd> serial;
    if (!options_.gps_device.empty()) {
        serial = std::make_unique<Fd>(open(options_.gps_device.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK));
        termios config{};
        checked(tcgetattr(serial->value, &config) == 0, "GPS configuration");
        cfmakeraw(&config);
        cfsetispeed(&config, B9600); cfsetospeed(&config, B9600);
        config.c_cflag |= CLOCAL | CREAD;
        config.c_cc[VMIN] = 0; config.c_cc[VTIME] = 0;
        checked(tcsetattr(serial->value, TCSANOW, &config) == 0, "GPS configuration");
    }
    GenerationBuffer generation(options_.station_id, mac, options_.priority, options_.force_100ms);
    std::exception_ptr tx_failure;
    std::cout << "[CAM] " << (serial ? "GPS" : "Static") << " sender on " << interface_
              << "; priority " << static_cast<unsigned>(options_.priority) << " (" << edca_queue(options_.priority) << ")"
              << (options_.force_100ms ? "; generation forced every 100 ms" : "; EN 302 637-2 generation")
              << "; checks every " << GenerationPolicy::check_ms << " ms"
              << "; requires driver TX feedback on 127.0.0.1:" << options_.feedback_port << std::endl;

    std::thread transmitter([&] {
        try {
            std::uint64_t id = 0;
            std::uint16_t sequence = 0;
            while (!stopping_) {
                id = std::max(id + 1, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
                std::optional<GeneratedCam> selected;
                const bool sent = transmit(gate_, id, 1, [&]() -> std::uint64_t {
                    if (!selected || !selected->prepare_for_tx(AirtimeGate::Clock::now(), sequence))
                        throw std::runtime_error("CAM expired after admission; reservation remains closed");
                    checked(sendto(radio.value, selected->frame.data(), selected->frame.size(), 0,
                        reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == static_cast<ssize_t>(selected->frame.size()), "CAM send");
                    generation.submitted(selected->version);
                    const auto notice = "TX " + std::to_string(id) + " " + std::to_string(sequence & 4095) + " " + std::to_string(selected->frame.size());
                    checked(send(feedback.value, notice.data(), notice.size(), 0) == static_cast<ssize_t>(notice.size()), "CAM feedback request");
                    const auto deadline = AirtimeGate::Clock::now() + std::chrono::seconds(2);
                    while (AirtimeGate::Clock::now() < deadline) {
                        pollfd descriptor{feedback.value, POLLIN, 0};
                        const int ready = poll(&descriptor, 1, 10);
                        if (ready < 0 && errno == EINTR) continue;
                        checked(ready >= 0 && !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)), "CAM feedback poll");
                        if (!(descriptor.revents & POLLIN)) continue;
                        char buffer[256];
                        const auto count = recv(feedback.value, buffer, sizeof(buffer), 0);
                        checked(count > 0, "CAM feedback receive");
                        if (static_cast<std::size_t>(count) == sizeof(buffer)) throw std::runtime_error("Oversized CAM TX feedback");
                        std::istringstream input(std::string(buffer, count));
                        std::string command, extra;
                        std::uint64_t reply_id = 0, actual = 0;
                        if (!(input >> command >> reply_id >> actual) || input >> extra || command != "DONE" || reply_id != id)
                            throw std::runtime_error("Invalid CAM TX feedback");
                        ++sequence;
                        return actual;
                    }
                    throw std::runtime_error("CAM TX completion feedback timed out; admission remains closed");
                }, stopping_, [&] {
                    selected = generation.latest(AirtimeGate::Clock::now());
                    return selected && selected->prepare_for_tx(AirtimeGate::Clock::now(), sequence);
                }, [&] {
                    // The selected snapshot is stable for this ADMIT attempt.
                    // Reserve its full size, including LF/path history, at
                    // 3 Mb/s OFDM 10 MHz with FCS; REPORT charges actual airtime.
                    const auto bytes = selected->frame.size();
                    return std::uint64_t(40 + 8 * ((16 + 8 * (bytes - 8 + 4) + 6 + 23) / 24));
                });
                if (!sent) break;
                generation.transmitted(*selected);
            }
        } catch (...) {
            tx_failure = std::current_exception();
            stopping_ = true;
        }
    });

    std::exception_ptr generation_failure;
    try {
        std::string pending;
        Position position;
        std::optional<AirtimeGate::Clock::time_point> last_fix;
        while (!stopping_) {
            const auto tick = AirtimeGate::Clock::now();
            if (serial) {
                pollfd descriptor{serial->value, POLLIN, 0};
                const int ready = poll(&descriptor, 1, 0);
                if (ready < 0 && errno != EINTR) throw std::runtime_error("GPS poll failed");
                if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("GPS disconnected");
                if (descriptor.revents & POLLIN) {
                    char buffer[512];
                    const auto count = read(serial->value, buffer, sizeof(buffer));
                    if (count < 0 && errno != EAGAIN && errno != EINTR) throw std::runtime_error("GPS read failed");
                    if (count > 0) pending.append(buffer, count);
                    std::size_t end;
                    while ((end = pending.find('\n')) != std::string::npos) {
                        const auto line = pending.substr(0, end);
                        pending.erase(0, end + 1);
                        Position next;
                        if (parse_rmc(line, next)) {
                            // Duplicate/out-of-order NMEA fixes must not refresh
                            // the source lifetime or reverse vehicle dynamics.
                            if (!last_fix || !position.sample_timestamp_ms ||
                                next.sample_timestamp_ms > position.sample_timestamp_ms) {
                                position = next;
                                last_fix = AirtimeGate::Clock::now();
                            }
                        }
                        else if (line.rfind("$GPRMC,", 0) == 0 || line.rfind("$GNRMC,", 0) == 0) {
                            last_fix.reset();
                            generation.invalidate_source();
                        }
                    }
                    if (pending.size() > 4096) pending.clear();
                }
            }
            const auto now = AirtimeGate::Clock::now();
            if (!serial || (last_fix && now - *last_fix < std::chrono::seconds(2))) {
                const auto valid_until = serial ? *last_fix + std::chrono::seconds(2) : AirtimeGate::Clock::time_point::max();
                generation.generate(position, now, timestamp_ms(), dcc_interval_ ? dcc_interval_() : 0, valid_until);
            } else generation.invalidate_source();
            // No catch-up iterations after a scheduler delay.
            std::this_thread::sleep_until(std::max(tick + std::chrono::milliseconds(GenerationPolicy::check_ms), AirtimeGate::Clock::now()));
        }
    } catch (...) { generation_failure = std::current_exception(); }
    stopping_ = true;
    transmitter.join();
    if (generation_failure) std::rethrow_exception(generation_failure);
    if (tx_failure) std::rethrow_exception(tx_failure);
#endif
}
} // namespace cam
