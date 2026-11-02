#include "dcc_adaptive.h"
#include "airtime_gate.h"
#include "gate_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void close_to(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-12, message);
}

void test_adaptive() {
    DccAdaptive controller;
    require(!controller.observe(0.68, 1), "one sample cannot update delta");
    require(controller.observe(0.68, 2), "UTC even slot updates delta");
    close_to(controller.filtered_cbr(), 0.34, "filter starts at zero and averages two samples");
    close_to(controller.delta(), 0.0009984, "reference first update");
    require(!controller.observe(0.0, 2), "duplicate boundary cannot update twice");
    controller.observe(1.0, 3);
    controller.observe(1.0, 4);
    close_to(controller.filtered_cbr(), 0.67, "recursive two-sample filter");
    close_to(controller.delta(), 0.0009944256, "reference second update");
    const double before = controller.delta();
    controller.observe(0.0, 5);
    require(!controller.observe(std::numeric_limits<double>::quiet_NaN(), 6), "NaN rejected");
    require(!controller.observe(0.0, 8), "gap discards incomplete pair");
    close_to(controller.delta(), before, "invalid or missing samples preserve budget");

    DccAdaptive positive;
    positive.observe(0.0, 1); positive.observe(0.0, 2);
    close_to(positive.delta(), 0.0010904, "positive correction capped at 0.0005");
    for (int slot = 3; slot < 2000; ++slot) positive.observe(0.0, slot);
    close_to(positive.delta(), DccAdaptive::delta_max, "upper saturation");
    const double old = positive.delta();
    positive.observe(1.0, 2000); // filtered=0.25: previous sample was zero
    positive.observe(1.0, 2001); positive.observe(1.0, 2002);
    positive.observe(1.0, 2003); positive.observe(1.0, 2004);
    const double before_negative = positive.delta();
    positive.observe(1.0, 2005); positive.observe(1.0, 2006);
    close_to(positive.delta(), 0.984 * before_negative - 0.00025, "negative correction capped");
    require(old > 0, "upper budget positive");
    for (int slot = 2007; slot < 5000; ++slot) positive.observe(1.0, slot);
    close_to(positive.delta(), DccAdaptive::delta_min, "lower saturation");
}

void test_gate() {
    AirtimeGate gate;
    const auto t0 = AirtimeGate::Clock::time_point{};
    require(gate.handle("ADMIT 1 500", t0).find("WAIT") == 0, "closed until valid CBR");
    gate.configure(0.01, 25ms, t0);
    require(gate.handle("ADMIT 1 500", t0) == "GRANT 1", "first packet granted");
    require(gate.handle("ADMIT 1 500", t0) == "ERROR stale_id", "duplicate grants never replayed");
    require(gate.handle("ADMIT 2 500", t0 + 1ms).find("WAIT") == 0, "one outstanding TX across clients");
    require(gate.handle("REPORT 2 500", t0) == "ERROR reservation", "mismatched reports rejected");
    require(gate.handle("REPORT 1 1000", t0 + 2ms) == "DONE 1", "actual airtime corrects underestimate");
    require(gate.handle("REPORT 1 1000", t0 + 3ms) == "DONE 1", "report retries idempotent");
    require(gate.handle("ADMIT 2 200", t0 + 101ms) == "WAIT 2 1000", "1000us at one percent needs 100ms");
    require(gate.handle("ADMIT 2 200", t0 + 102ms) == "GRANT 2", "exact reopening boundary");
    gate.configure(0.001, 25ms, t0 + 103ms);
    gate.configure(0.03, 25ms, t0 + 104ms);
    require(gate.handle("REPORT 2 100", t0 + 105ms) == "DONE 2", "smaller actual duration cannot refund reservation");
    require(gate.handle("ADMIT 3 100", t0 + 304ms) == "WAIT 3 1000", "tightest pending budget preserved");
    gate.configure(0.03, 25ms, t0 + 305ms);
    require(gate.handle("ADMIT 3 100", t0 + 305ms) == "GRANT 3", "budget updates retain debt");
    gate.handle("REPORT 3 100", t0 + 306ms);
    require(gate.handle("ADMIT 4 100", t0 + 330ms) == "WAIT 4 1000", "25ms minimum spacing");
    require(gate.handle("ADMIT 4 100", t0 + 900ms).find("WAIT") == 0, "expired CBR blocks admissions");
    gate.configure(0.03, 100ms, t0 + 900ms);
    require(gate.handle("ADMIT 4 100", t0 + 900ms) == "GRANT 4", "reactive profile usable");
    gate.handle("REPORT 4 100", t0 + 901ms);
    require(gate.handle("ADMIT 5 100", t0 + 1000ms) == "WAIT 5 1000", "reactive minimum interval enforced");
    gate.disable();
    require(gate.handle("ADMIT 5 100", t0 + 1002ms).find("WAIT") == 0, "invalid CBR disables gate");
    for (const auto* request : {"ADMIT -1 500", "ADMIT 1 nan", "ADMIT 0 500", "ADMIT 8 0",
         "ADMIT 8 1000001", "REPORT 8 500 extra", "ADMIT 999999999999999999999999 500"})
        require(gate.handle(request, t0).find("ERROR") == 0, "malformed request rejected");
}

void test_server() {
    AirtimeGate gate;
    gate.configure(0.01, 25ms, AirtimeGate::Clock::now());
    GateServer server(gate, 0);
    server.start();
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0, "client socket");
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto exchange = [&](const std::string& request) {
        sendto(fd, request.data(), request.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        char reply[256];
        const auto count = recv(fd, reply, sizeof(reply), 0);
        require(count > 0, "gate server replies over UDP");
        return std::string(reply, static_cast<std::size_t>(count));
    };
    require(exchange("ADMIT 1 500") == "GRANT 1", "live admission");
    require(exchange("REPORT 1 700") == "DONE 1", "live completion feedback");
    close(fd);
    server.stop();
    server.start();
    server.stop();
}
}

int main() {
    test_adaptive();
    test_gate();
    test_server();
    std::cout << "All adaptive DCC, airtime gate and UDP integration tests passed\n";
}
