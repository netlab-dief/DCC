#include "net/dcc_net.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dccnet {
namespace {
unsigned le16(const std::vector<std::uint8_t>& b, std::size_t p) {
    return unsigned(b[p]) | (unsigned(b[p + 1]) << 8);
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t p) {
    return std::uint32_t(le16(b, p)) | (std::uint32_t(le16(b, p + 2)) << 16);
}
unsigned be16(const std::vector<std::uint8_t>& b, std::size_t p) {
    return (unsigned(b[p]) << 8) | b[p + 1];
}
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t p) {
    return (std::uint32_t(be16(b, p)) << 16) | be16(b, p + 2);
}
struct Layout {
    std::size_t gn;
    std::optional<int> rssi;
    std::optional<unsigned> frequency;
};
std::optional<Layout> layout(const std::vector<std::uint8_t>& b) {
    if (b.size() < 8 || b[0] != 0) return {};
    const std::size_t rt = le16(b, 2);
    if (rt < 8 || rt > b.size() || b.size() - rt < 24) return {};
    const auto present = le32(b, 4);
    std::size_t fields = 8;
    auto bitmap = present;
    while (bitmap & 0x80000000U) {
        if (fields + 4 > rt) return {};
        bitmap = le32(b, fields);
        fields += 4;
    }
    // Only fields through dBm antenna signal are needed. Later/vendor fields
    // cannot change their offsets. Alignment is relative to radiotap start.
    constexpr unsigned alignment[]{8, 1, 1, 2, 2, 1};
    constexpr unsigned length[]{8, 1, 1, 4, 2, 1};
    unsigned flags = 0;
    std::optional<int> rssi;
    std::optional<unsigned> frequency;
    for (unsigned bit = 0; bit <= 5; ++bit) {
        if (!(present & (1U << bit))) continue;
        fields = (fields + alignment[bit] - 1) & ~(std::size_t(alignment[bit]) - 1);
        if (fields + length[bit] > rt) return {};
        if (bit == 1) flags = b[fields];
        if (bit == 3) frequency = le16(b, fields);
        if (bit == 5) rssi = b[fields] < 128 ? int(b[fields]) : int(b[fields]) - 256;
        fields += length[bit];
    }
    if (flags & 0x40U) return {}; // bad FCS
    const unsigned fc = le16(b, rt);
    // Ordinary or QoS ad-hoc data only; no fragments, encryption, DS or null data.
    if ((fc & 0x00ffU) != 0x0008U && (fc & 0x00ffU) != 0x0088U) return {};
    if ((fc & 0x4700U) || (le16(b, rt + 22) & 15U)) return {};
    std::size_t mac_length = 24;
    if (fc & 0x0080U) {
        if (rt + 26 > b.size() || (b[rt + 24] & 0x80U)) return {}; // A-MSDU
        mac_length += 2;
        if (fc & 0x8000U) mac_length += 4; // HT control
    }
    if (flags & 0x20U) mac_length = (mac_length + 3) & ~std::size_t(3);
    const auto llc = rt + mac_length;
    constexpr std::uint8_t snap[]{0xaa, 0xaa, 3, 0, 0, 0, 0x89, 0x47};
    if (llc + sizeof(snap) > b.size() || !std::equal(std::begin(snap), std::end(snap), b.begin() + llc)) return {};
    const auto gn = llc + sizeof(snap);
    const auto end = b.size() - ((flags & 0x10U) ? 4 : 0);
    if (end < gn || end - gn < 40) return {};
    // Basic NH=Common Header: secured packets require a security decapsulator.
    if (b[gn] != 0x11 || (b[gn + 2] >> 2) == 0 || b[gn + 3] != 1 ||
        b[gn + 5] != 0x50 || b[gn + 10] != 1 ||
        (b[gn + 4] >> 4) > 3 || be16(b, gn + 8) > end - gn - 40) return {};
    return Layout{gn, rssi, frequency};
}
double select(const std::vector<double>& values) {
    if (values.empty()) return 0;
    double sum = 0, largest = 0, second = 0;
    for (double value : values) {
        sum += value;
        if (value >= largest) { second = largest; largest = value; }
        else if (value > second) second = value;
    }
    // With fewer than two entries there is no second-largest: use zero.
    return sum / values.size() > State::target ? largest : second;
}
bool cbr(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }
}
std::array<std::uint8_t, 4> encode(double local, double one_hop, double eirp_dbm) {
    if (!cbr(local) || !cbr(one_hop) || !std::isfinite(eirp_dbm))
        throw std::invalid_argument("Invalid DCC-MCO value");
    return {static_cast<std::uint8_t>(std::floor(local * 255)),
            static_cast<std::uint8_t>(std::floor(one_hop * 255)),
            static_cast<std::uint8_t>(static_cast<unsigned>(std::clamp(std::floor(eirp_dbm), 0.0, 31.0)) << 3), 0};
}
std::optional<Observation> parse(const std::vector<std::uint8_t>& frame) {
    const auto l = layout(frame);
    if (!l) return {};
    Observation o;
    std::copy_n(frame.begin() + l->gn + 12, 8, o.address.begin());
    o.source_timestamp = be32(frame, l->gn + 20);
    // Replace SHB's media-dependent field after its 24-byte SO PV:
    // EN 302 636-4-1 table 13 gives GN octets 36..39. TS 102 636-4-2
    // table 3 labels them 40..43; no extra bytes are inserted into SHB.
    o.local = frame[l->gn + 36] / 255.0;
    o.one_hop = frame[l->gn + 37] / 255.0;
    o.tx_eirp_dbm = frame[l->gn + 38] >> 3;
    o.rssi_dbm = l->rssi;
    o.frequency_mhz = l->frequency;
    // Zero is a legitimate CBR/power value, not an absence marker.
    return o;
}
void write_extension(std::vector<std::uint8_t>& frame, double local, double one_hop, double eirp_dbm) {
    const auto l = layout(frame);
    if (!l) throw std::invalid_argument("Invalid outgoing GN SHB frame");
    const auto bytes = encode(local, one_hop, eirp_dbm);
    std::copy(bytes.begin(), bytes.end(), frame.begin() + l->gn + 36);
}
void State::expire(Clock::time_point now) {
    for (auto it = neighbors_.begin(); it != neighbors_.end();) {
        if (now - it->second.received_at >= lifetime) it = neighbors_.erase(it);
        else ++it;
    }
}
bool State::receive(const Observation& o, Clock::time_point now) {
    if (!cbr(o.local) || !cbr(o.one_hop) || o.tx_eirp_dbm < 0 || o.tx_eirp_dbm > 31) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    expire(now);
    auto it = neighbors_.find(o.address);
    if (it != neighbors_.end()) {
        const auto advance = o.source_timestamp - it->second.observation.source_timestamp;
        if (advance == 0 || advance >= 0x80000000U) return false;
    }
    // Bound memory on an untrusted broadcast medium. Drop new entries when full.
    if (it == neighbors_.end() && neighbors_.size() >= 4096) return false;
    neighbors_[o.address] = Neighbor{o, now};
    return true;
}
Snapshot State::trigger(const std::optional<CbrSample>& local, Clock::time_point now, std::int64_t slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire(now);
    std::vector<double> zero, one;
    for (const auto& entry : neighbors_) {
        zero.push_back(entry.second.observation.local);
        one.push_back(entry.second.observation.one_hop);
    }
    Snapshot next;
    next.neighbors = neighbors_.size();
    next.one_hop = select(zero);
    next.two_hop = select(one);
    const bool fresh = local && local->fresh(now) && cbr(local->value);
    next.local = fresh ? local->value : 0;
    // Equation (5): the local term belongs to the preceding trigger interval.
    const bool previous_fresh = previous_local_ &&
        now >= snapshot_.control.measured_at && now - snapshot_.control.measured_at <= std::chrono::milliseconds(150) &&
        now >= previous_local_->measured_at && now - previous_local_->measured_at <= std::chrono::milliseconds(250) &&
        fresh && local->sequence == previous_local_->sequence + 1 &&
        local->utc_slot == previous_local_->utc_slot + 1;
    next.global = std::max({previous_fresh ? double(previous_local_->value) : 0.0, next.one_hop, next.two_hop});
    next.control.sequence = snapshot_.control.sequence + 1;
    next.control.utc_slot = slot;
    next.control.value = static_cast<float>(next.global);
    next.control.measured_at = now;
    next.control.valid = fresh && previous_fresh;
    previous_local_ = fresh ? local : std::nullopt;
    snapshot_ = next;
    changed_.notify_all();
    return next;
}
Snapshot State::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}
std::optional<Snapshot> State::wait_next(std::uint64_t& cursor) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(150), [&] { return stopped_ || snapshot_.control.sequence > cursor; });
    if (stopped_ || snapshot_.control.sequence <= cursor) return {};
    cursor = snapshot_.control.sequence;
    return snapshot_;
}
void State::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    changed_.notify_all();
}
void State::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = false;
    neighbors_.clear();
    previous_local_.reset();
    snapshot_ = {};
}
std::vector<Neighbor> State::neighbors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Neighbor> result;
    for (const auto& entry : neighbors_) result.push_back(entry.second);
    return result;
}
} // namespace dccnet
