#include "radio/CBRReader.h"
#include "radio/rate_manager.h"
#include "common/its_time.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void survey(CBRReader& reader, std::optional<std::uint64_t> active, std::optional<std::uint64_t> busy,
    bool truncated_busy = false, unsigned frequency = 5900) {
    std::unique_ptr<nl_msg, decltype(&nlmsg_free)> message(nlmsg_alloc(), nlmsg_free);
    require(bool(message), "allocate synthetic survey");
    require(genlmsg_put(message.get(), 0, 0, 1, 0, 0, NL80211_CMD_NEW_SURVEY_RESULTS, 0), "survey generic header");
    auto* nested = nla_nest_start(message.get(), NL80211_ATTR_SURVEY_INFO);
    require(nested && nla_put_flag(message.get(), NL80211_SURVEY_INFO_IN_USE) == 0 &&
        nla_put_u32(message.get(), NL80211_SURVEY_INFO_FREQUENCY, frequency) == 0, "survey metadata");
    if (active) require(nla_put_u64(message.get(), NL80211_SURVEY_INFO_CHANNEL_TIME, *active) == 0, "active counter");
    if (truncated_busy) {
        require(nla_put_u8(message.get(), NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY, 0) == 0, "malformed short counter");
    } else if (busy) require(nla_put_u64(message.get(), NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY, *busy) == 0, "busy counter");
    nla_nest_end(message.get(), nested);
    cbr_handler(message.get(), &reader);
}

void test_survey_validation() {
    CBRReader reader;
    reader.setupCBRReader(false, "unused");
    survey(reader, 100, 50);
    require(reader.get_current_cbr() < 0, "first counters only prime baseline");
    survey(reader, 200, 100);
    require(std::abs(reader.get_current_cbr() - 0.5f) < 1e-6, "CBR independent of optional RX/TX counters");
    survey(reader, 300, {});
    require(reader.get_current_cbr() < 0, "missing busy time never becomes CBR zero");
    survey(reader, 400, 200);
    require(reader.get_current_cbr() < 0, "first complete survey after missing field reprimes");
    survey(reader, 500, 250);
    require(reader.get_current_cbr() == 0.5f, "complete consecutive surveys recover");
    survey(reader, {}, 300);
    require(reader.get_current_cbr() < 0, "missing active time invalidates");
    survey(reader, 600, 300);
    survey(reader, 700, {}, true);
    require(reader.get_current_cbr() < 0, "truncated busy attribute rejected without out-of-bounds read");
    survey(reader, 800, 400);
    survey(reader, 900, 550);
    require(reader.get_current_cbr() < 0, "busy delta exceeding active delta rejected instead of clamped");
    survey(reader, 1000, 600);
    survey(reader, 1100, 650);
    require(reader.get_current_cbr() == 0.5f, "valid data recovers after impossible delta");
    survey(reader, 100, 50);
    require(reader.get_current_cbr() < 0, "counter reset invalidates");
    survey(reader, 200, 100);
    survey(reader, 300, 150);
    require(reader.get_current_cbr() == 0.5f, "counter reset recovery");
    survey(reader, 400, 200, false, 5890);
    require(reader.get_current_cbr() < 0, "channel change starts a new measurement baseline");
}

void test_rate_failure() {
    RateManager rate;
    require(!rate.set_fixed_rate_netlink(12), "uninitialized rate manager reports failure");
    rate.setup_rate_manager("dcc-test-absent");
    require(!rate.set_fixed_rate_netlink(12), "nonexistent interface cannot appear successfully configured");
    require(!rate.set_fixed_rate_netlink(999), "invalid rate rejected");
}

void test_common_clocks() {
    const auto before = its::now_ms();
    const auto gn = get_timestamp_ms_gn();
    const auto cam = get_timestamp_ms_cam();
    const auto cpm = get_timestamp_ms_cpm();
    const auto after = its::now_ms();
    const auto near = [&](std::uint64_t value, std::uint64_t mask) {
        return ((value - (before & mask)) & mask) <= after - before;
    };
    require(near(gn, 0xffffffffULL) && near(cam, 0xffffULL) && near(cpm, (1ULL << 42) - 1),
        "legacy GN/CAM/CPM clocks use the same ITS epoch and leap second conversion");
}
} // namespace

int main() {
    test_survey_validation(); test_rate_failure(); test_common_clocks();
    std::cout << "Linux Netlink validation tests passed\n";
}
