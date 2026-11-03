#include "cam_message.h"
#include "UtilsEncode/utils.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <ctime>

namespace cam {
namespace {
double number(const std::string& text) {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) throw std::invalid_argument("NMEA number");
    return value;
}
int coordinate(const std::string& text, const std::string& direction, bool latitude) {
    const double value = number(text);
    if (value < 0 || value > (latitude ? 9000 : 18000))
        throw std::invalid_argument("NMEA coordinate range");
    const int degrees = static_cast<int>(value / 100);
    const double minutes = value - degrees * 100;
    const double decimal = degrees + minutes / 60;
    if (value < 0 || minutes >= 60 || decimal > (latitude ? 90 : 180) ||
        (latitude ? direction != "N" && direction != "S" : direction != "E" && direction != "W"))
        throw std::invalid_argument("NMEA coordinate");
    const int sign = direction == "S" || direction == "W" ? -1 : 1;
    return static_cast<int>(std::llround(sign * decimal * 1e7));
}
void be16(std::vector<std::uint8_t>& out, unsigned value) {
    out.push_back((value >> 8) & 255); out.push_back(value & 255);
}
void be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    be16(out, value >> 16); be16(out, value & 65535);
}
std::uint64_t rmc_timestamp(const std::string& time, const std::string& date) {
    if (date.size() != 6 || date.find_first_not_of("0123456789") != std::string::npos ||
        time.size() < 6 || time.substr(0, 6).find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("NMEA UTC date/time");
    std::tm utc{};
    utc.tm_year = 100 + std::stoi(date.substr(4, 2));
    utc.tm_mon = std::stoi(date.substr(2, 2)) - 1;
    utc.tm_mday = std::stoi(date.substr(0, 2));
    utc.tm_hour = std::stoi(time.substr(0, 2));
    utc.tm_min = std::stoi(time.substr(2, 2));
    utc.tm_sec = std::stoi(time.substr(4, 2));
    const auto original = utc;
    const auto seconds = timegm(&utc);
    if (utc.tm_year != original.tm_year || utc.tm_mon != original.tm_mon || utc.tm_mday != original.tm_mday ||
        utc.tm_hour != original.tm_hour || utc.tm_min != original.tm_min || utc.tm_sec != original.tm_sec || seconds < 1072915200)
        throw std::invalid_argument("NMEA UTC range");
    unsigned fraction = 0;
    if (time.size() > 6) {
        if (time[6] != '.' || time.size() == 7 || time.substr(7).find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("NMEA UTC fraction");
        const auto digits = time.substr(7, 3);
        fraction = std::stoul(digits + std::string(3 - digits.size(), '0'));
    }
    return static_cast<std::uint64_t>(seconds - 1072915200) * 1000 + fraction;
}
template<class T> T* allocate() {
    auto* value = static_cast<T*>(std::calloc(1, sizeof(T)));
    if (!value) throw std::bad_alloc();
    return value;
}
} // namespace

bool parse_rmc(const std::string& sentence, Position& position) {
    try {
        const auto star = sentence.find('*');
        if (sentence.empty() || sentence[0] != '$' || star == std::string::npos || star + 3 > sentence.size()) return false;
        unsigned checksum = 0;
        for (std::size_t i = 1; i < star; ++i) checksum ^= static_cast<unsigned char>(sentence[i]);
        const auto tail = sentence.substr(star + 3);
        if (tail.find_first_not_of("\r\n") != std::string::npos) return false;
        const auto digits = sentence.substr(star + 1, 2);
        if (digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
            checksum != std::stoul(digits, nullptr, 16)) return false;
        std::vector<std::string> fields;
        std::istringstream input(sentence.substr(0, star));
        std::string field;
        while (std::getline(input, field, ',')) fields.push_back(field);
        if (fields.size() < 10 || (fields[0] != "$GPRMC" && fields[0] != "$GNRMC") || fields[2] != "A") return false;
        Position next;
        next.sample_timestamp_ms = rmc_timestamp(fields[1], fields[9]);
        next.latitude = coordinate(fields[3], fields[4], true);
        next.longitude = coordinate(fields[5], fields[6], false);
        if (!fields[7].empty()) {
            const double speed = number(fields[7]) * (1852.0 / 3600.0) * 100;
            if (speed < 0 || speed > 16382) return false;
            next.speed = static_cast<int>(std::lround(speed));
        } else next.speed = 16383;
        if (!fields[8].empty()) {
            const double heading = number(fields[8]);
            if (heading < 0 || heading >= 360) return false;
            next.heading = static_cast<int>(std::lround(heading * 10)) % 3600;
        }
        position = next;
        return true;
    } catch (const std::exception&) { return false; }
}

std::uint64_t timestamp_ms() {
    const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(unix_ms - 1072915200000LL);
}

std::vector<std::uint8_t> encode(const Position& position, std::uint32_t station_id,
                               std::uint64_t timestamp, const LowFrequencyData* low_frequency) {
    auto release = [](CAM_t* value) { ASN_STRUCT_FREE(asn_DEF_CAM, value); };
    std::unique_ptr<CAM_t, decltype(release)> message(create_cam_message(position.latitude,
        position.longitude, position.sample_timestamp_ms.value_or(timestamp) & 65535, position.speed, position.heading), release);
    if (!message) throw std::runtime_error("Cannot allocate CAM");
    message->header.stationID = station_id;
    if (low_frequency) {
        auto*& container = message->cam.camParameters.lowFrequencyContainer;
        container = allocate<LowFrequencyContainer_t>();
        container->present = LowFrequencyContainer_PR_basicVehicleContainerLowFrequency;
        auto& lf = container->choice.basicVehicleContainerLowFrequency;
        lf.vehicleRole = VehicleRole_default;
        lf.exteriorLights.buf = allocate<std::uint8_t>();
        lf.exteriorLights.size = 1;
        lf.exteriorLights.buf[0] = low_frequency->exterior_lights;
        Position previous = position;
        auto previous_time = position.sample_timestamp_ms.value_or(timestamp);
        for (const auto& sample : low_frequency->history) {
            if (lf.pathHistory.list.count == 23) break;
            if (sample.timestamp_ms >= previous_time || previous_time - sample.timestamp_ms < 10) continue;
            const auto age = (previous_time - sample.timestamp_ms) / 10;
            const auto dlat = static_cast<std::int64_t>(sample.position.latitude) - previous.latitude;
            auto dlon = static_cast<std::int64_t>(sample.position.longitude) - previous.longitude;
            if (dlon > 1800000000LL) dlon -= 3600000000LL;
            if (dlon < -1800000000LL) dlon += 3600000000LL;
            if (age > 65535 || std::abs(dlat) > 131071 || std::abs(dlon) > 131071) break;
            auto free_point = [](PathPoint_t* point) { ASN_STRUCT_FREE(asn_DEF_PathPoint, point); };
            std::unique_ptr<PathPoint_t, decltype(free_point)> point(allocate<PathPoint_t>(), free_point);
            point->pathPosition.deltaLatitude = dlat;
            point->pathPosition.deltaLongitude = dlon;
            point->pathPosition.deltaAltitude = DeltaAltitude_unavailable;
            point->pathDeltaTime = allocate<PathDeltaTime_t>();
            *point->pathDeltaTime = age;
            if (ASN_SEQUENCE_ADD(&lf.pathHistory.list, point.get()) != 0) throw std::bad_alloc();
            point.release();
            previous = sample.position;
            previous_time = sample.timestamp_ms;
        }
    }
    const auto result = encode_cam_message(message.get());
    std::unique_ptr<void, decltype(&std::free)> buffer(result.r.buffer, &std::free);
    if (!result.b || !buffer || result.r.result.encoded <= 0) throw std::runtime_error("CAM UPER encoding failed");
    const auto* bytes = static_cast<const std::uint8_t*>(buffer.get());
    return {bytes, bytes + result.r.result.encoded};
}

const char* edca_queue(std::uint8_t priority) {
    if (priority > 7) throw std::invalid_argument("CAM priority must be in [0, 7]");
    constexpr const char* queues[]{"AC_BE", "AC_BK", "AC_BK", "AC_BE", "AC_VI", "AC_VI", "AC_VO", "AC_VO"};
    return queues[priority];
}

std::vector<std::uint8_t> make_frame(const Position& p, std::uint32_t station_id,
    const std::array<std::uint8_t, 6>& mac, std::uint16_t sequence, std::uint64_t timestamp,
    std::uint8_t priority, const LowFrequencyData* low_frequency) {
    if (priority > 7) throw std::invalid_argument("CAM priority must be in [0, 7]");
    const auto payload = encode(p, station_id, timestamp, low_frequency);
    // Minimal radiotap header; rate selection remains under the DCC radio policy.
    std::vector<std::uint8_t> out{0, 0, 8, 0, 0, 0, 0, 0, 0x88, 0, 0, 0};
    out.insert(out.end(), 6, 255); // Broadcast destination.
    out.insert(out.end(), mac.begin(), mac.end());
    out.insert(out.end(), 6, 255); // BSSID.
    const unsigned seq = (sequence & 4095) << 4;
    out.push_back(seq & 255); out.push_back(seq >> 8);
    // mac80211 selects the EDCA queue from the injected QoS frame's TID.
    out.insert(out.end(), {priority, 0, 0xaa, 0xaa, 3, 0, 0, 0, 0x89, 0x47});
    // GeoNetworking single-hop broadcast, BTP-B; explicit network byte order.
    // CAM maximum GN lifetime: 1000 ms = 20 units of 50 ms.
    out.insert(out.end(), {0x11, 0, 0x50, 1, 0x20, 0x50, traffic_class, 0});
    be16(out, payload.size() + 4); // Includes BTP-B header.
    out.insert(out.end(), {1, 0});
    be16(out, 5U << 10); // Passenger car GN address.
    out.insert(out.end(), mac.begin(), mac.end());
    be32(out, static_cast<std::uint32_t>(timestamp));
    be32(out, static_cast<std::uint32_t>(p.latitude));
    be32(out, static_cast<std::uint32_t>(p.longitude));
    be16(out, p.speed == 16383 ? 32767 : p.speed);
    be16(out, p.heading);
    out.insert(out.end(), {0, 0, 0, 0}); // SHB media dependent extension.
    be16(out, 2001); be16(out, 0); // CAM BTP destination port.
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
} // namespace cam
