#include "gate_server.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>

void GateServer::start() {
    if (thread_.joinable()) return;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("Cannot open airtime gate socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string error = std::strerror(errno);
        close(fd_); fd_ = -1;
        throw std::runtime_error("Cannot bind airtime gate to 127.0.0.1:" + std::to_string(port_) + ": " + error);
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        close(fd_); fd_ = -1;
        throw std::runtime_error("Cannot retrieve airtime gate port");
    }
    port_ = ntohs(address.sin_port);
    stopping_ = false;
    try { thread_ = std::thread(&GateServer::run, this); }
    catch (...) { close(fd_); fd_ = -1; throw; }
}

void GateServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

void GateServer::run() {
    while (!stopping_) {
        pollfd descriptor{fd_, POLLIN, 0};
        if (poll(&descriptor, 1, 100) <= 0 || !(descriptor.revents & POLLIN)) continue;
        char buffer[256];
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const auto count = recvfrom(fd_, buffer, sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&peer), &size);
        if (count <= 0) continue;
        const auto reply = static_cast<std::size_t>(count) == sizeof(buffer) ? "ERROR size" :
            gate_.handle(std::string(buffer, static_cast<std::size_t>(count)), AirtimeGate::Clock::now());
        sendto(fd_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&peer), size);
    }
}
