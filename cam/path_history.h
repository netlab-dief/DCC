#pragma once

#include "cam_message.h"
#include <algorithm>
#include <cmath>
#include <deque>

namespace cam {
// Geometric concise history: VSC-A, DOT HS 811 492B, Appendix B-2,
// method three. Calibration defaults are not universal ETSI requirements.
class PathHistory {
public:
    static constexpr double allowable_error_m = 1.0;
    static constexpr double maximum_chord_m = 310.0;
    static constexpr double target_distance_m = 300.0;
    static constexpr std::size_t maximum_points = 23;
    static constexpr std::size_t maximum_open_samples = 2048;
    static constexpr std::uint64_t maximum_gap_ms = 2000;

    void clear() { concise_.clear(); open_.clear(); }

    void observe(const Position& position, std::uint64_t timestamp_ms) {
        if (position.latitude < -900000000 || position.latitude > 900000000 ||
            position.longitude < -1800000000 || position.longitude > 1800000000) {
            clear();
            return;
        }
        Observation current{{position, timestamp_ms}, ecef(position)};
        if (!open_.empty()) {
            const auto previous_time = open_.back().sample.timestamp_ms;
            if (timestamp_ms <= previous_time) return;
            // Do not invent a path through missing fixes or an unsampled jump.
            if (timestamp_ms - previous_time > maximum_gap_ms ||
                distance(open_.back().point, current.point) > maximum_chord_m)
                clear();
        }
        if (open_.empty()) {
            concise_.push_back(current);
            open_.push_back(current);
            return;
        }
        if (position.latitude == open_.back().sample.position.latitude &&
            position.longitude == open_.back().sample.position.longitude) {
            open_.back() = current;
            if (open_.size() == 1) concise_.back() = current;
        } else {
            bool split = distance(open_.front().point, current.point) > maximum_chord_m ||
                         open_.size() >= maximum_open_samples;
            if (!split) {
                const auto segment = project(open_.front(), current.point);
                const auto squared = segment.north * segment.north + segment.east * segment.east;
                for (const auto& observation : open_) {
                    const auto p = project(open_.front(), observation.point);
                    const double fraction = squared == 0 ? 0 : std::clamp(
                        (p.north * segment.north + p.east * segment.east) / squared, 0.0, 1.0);
                    if (std::hypot(p.north - fraction * segment.north,
                                   p.east - fraction * segment.east) > allowable_error_m) {
                        split = true;
                        break;
                    }
                }
            }
            if (split) {
                const auto previous = open_.back();
                // The previous candidate is the last endpoint whose segment
                // passed every sampled-point error check.
                if (previous.sample.timestamp_ms != concise_.back().sample.timestamp_ms)
                    concise_.push_back(previous);
                open_.clear();
                open_.push_back(previous);
            }
            open_.push_back(current);
        }
        prune(current);
    }

    // CAM offsets are encoded separately, newest first. No synthetic samples,
    // current-time points, or future points are returned.
    std::vector<PathSample> points() const {
        std::vector<PathSample> result;
        if (open_.empty()) return result;
        const auto& current = open_.back();
        if (std::all_of(concise_.begin(), concise_.end(), [&](const Observation& observation) {
                return distance(current.point, observation.point) <= allowable_error_m;
            }))
            return result;
        auto previous_time = current.sample.timestamp_ms;
        auto previous_position = current.sample.position;
        for (auto it = concise_.rbegin(); it != concise_.rend(); ++it) {
            if (it->sample.timestamp_ms >= previous_time) continue;
            const auto age = previous_time - it->sample.timestamp_ms;
            if (age < 10) continue;
            auto longitude = static_cast<std::int64_t>(it->sample.position.longitude) - previous_position.longitude;
            if (longitude > 1800000000LL) longitude -= 3600000000LL;
            if (longitude < -1800000000LL) longitude += 3600000000LL;
            if (age / 10 > 65535 ||
                std::abs(static_cast<std::int64_t>(it->sample.position.latitude) - previous_position.latitude) > 131071 ||
                std::abs(longitude) > 131071) break;
            result.push_back(it->sample);
            previous_time = it->sample.timestamp_ms;
            previous_position = it->sample.position;
        }
        return result;
    }

private:
    struct Point { double x, y, z; };
    struct Observation { PathSample sample; Point point; };
    struct NorthEast { double north, east; };
    static constexpr double radians = 3.14159265358979323846 / 180.0;

    // WGS84 at zero altitude: RMC supplies no measured altitude. Project ECEF
    // differences into the anchor's tangent plane, including at the dateline.
    static Point ecef(const Position& p) {
        const double latitude = p.latitude * 1e-7 * radians;
        const double longitude = p.longitude * 1e-7 * radians;
        const double sine = std::sin(latitude);
        const double radius = 6378137.0 / std::sqrt(1.0 - 0.0066943799901413165 * sine * sine);
        return {radius * std::cos(latitude) * std::cos(longitude),
                radius * std::cos(latitude) * std::sin(longitude),
                radius * (1.0 - 0.0066943799901413165) * sine};
    }
    static double distance(const Point& a, const Point& b) {
        return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
    }
    static NorthEast project(const Observation& origin, const Point& p) {
        const double latitude = origin.sample.position.latitude * 1e-7 * radians;
        const double longitude = origin.sample.position.longitude * 1e-7 * radians;
        const double x = p.x - origin.point.x, y = p.y - origin.point.y, z = p.z - origin.point.z;
        return {-std::sin(latitude) * (std::cos(longitude) * x + std::sin(longitude) * y) + std::cos(latitude) * z,
                -std::sin(longitude) * x + std::cos(longitude) * y};
    }
    void prune(const Observation& current) {
        double length = 0;
        Point previous = current.point;
        std::size_t keep = 0;
        for (auto it = concise_.rbegin(); it != concise_.rend(); ++it) {
            length += distance(previous, it->point);
            previous = it->point;
            ++keep;
            if (length >= target_distance_m || keep == maximum_points) break;
        }
        while (concise_.size() > keep) concise_.pop_front();
    }
    std::deque<Observation> concise_, open_;
};
} // namespace cam
