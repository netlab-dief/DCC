#include "cam/path_history.h"
#include <cstdlib>
#include <iostream>
#include <random>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
cam::Position position(double east, double north = 0) {
    cam::Position p;
    p.latitude = std::lround(north / 110574.2758 * 1e7);
    p.longitude = std::lround(east / 111319.4908 * 1e7);
    return p;
}
struct Fix { cam::Position p; std::uint64_t time; double east, north; };
void verify_geometry(const std::vector<Fix>& fixes, const std::vector<cam::PathSample>& points) {
    require(!points.empty() && points.size() <= 23, "concise output has one to 23 points");
    auto endpoint = fixes.back();
    for (const auto& point : points) {
        const auto found = std::find_if(fixes.begin(), fixes.end(), [&](const Fix& f) { return f.time == point.timestamp_ms; });
        require(found != fixes.end() && point.timestamp_ms < endpoint.time, "output contains original samples in reverse time order");
        require(found->p.latitude == point.position.latitude && found->p.longitude == point.position.longitude,
                "selection does not synthesize coordinates");
        const double dx = endpoint.east - found->east, dy = endpoint.north - found->north;
        const double length = std::hypot(dx, dy);
        require(length <= 310.03, "selected chords remain within calibrated length");
        for (auto it = found; it != fixes.end() && it->time <= endpoint.time; ++it) {
            const double ratio = length == 0 ? 0 : std::clamp(
                ((it->east - found->east) * dx + (it->north - found->north) * dy) / (length * length), 0.0, 1.0);
            require(std::hypot(it->east - found->east - ratio * dx, it->north - found->north - ratio * dy) <= 1.03,
                    "every represented raw sample lies within one metre, allowing coordinate quantization");
        }
        endpoint = *found;
    }
}
void test_stationary_and_ordering() {
    cam::PathHistory history;
    for (unsigned i = 0; i < 10000; ++i)
        history.observe(position(0.1 * (i % 3), 0.1 * (i % 2)), i * 100ULL);
    require(history.points().empty(), "sub-metre stationary jitter does not fill transmitted history");
    history.clear();
    history.observe(position(0), 1000);
    history.observe(position(10), 1100);
    const auto previous = history.points();
    history.observe(position(50), 1100);
    history.observe(position(100), 900);
    require(history.points().size() == previous.size() && history.points()[0].timestamp_ms == previous[0].timestamp_ms,
            "duplicate and out-of-order timestamps cannot rewrite history");
    history.observe(position(20), 4000);
    require(history.points().empty(), "missing fixes break the path rather than bridging a gap");
    history.observe(position(30), 4100);
    history.observe(position(1000), 4200);
    require(history.points().empty(), "unsampled jump beyond maximum chord resets history");
    auto invalid = position(0);
    invalid.latitude = 900000001;
    history.observe(invalid, 4300);
    require(history.points().empty(), "invalid coordinates clear history");
}
void test_straight() {
    cam::PathHistory history;
    std::vector<Fix> fixes;
    for (unsigned i = 0; i <= 1000; ++i) {
        fixes.push_back({position(i), i * 100ULL, static_cast<double>(i), 0});
        history.observe(fixes.back().p, fixes.back().time);
    }
    const auto points = history.points();
    require(points.size() <= 3, "one thousand straight-road fixes compress to a few points");
    require(1000 - points.back().position.longitude * 1e-7 * 111319.4908 >= 299.98,
            "history retains the boundary point needed to cover 300 metres");
    verify_geometry(fixes, points);
}
void test_curves_and_reversal() {
    constexpr double pi = 3.14159265358979323846;
    cam::PathHistory history;
    std::vector<Fix> fixes;
    for (unsigned i = 0; i <= 360; ++i) {
        const double angle = i * pi / 180;
        const double east = 40 * std::cos(angle), north = 40 * std::sin(angle);
        fixes.push_back({position(east, north), i * 100ULL, east, north});
        history.observe(fixes.back().p, fixes.back().time);
    }
    verify_geometry(fixes, history.points());
    require(history.points().size() > 8, "curved road retains geometric detail");
    history.clear(); fixes.clear();
    for (unsigned i = 0; i <= 200; ++i) {
        const double east = i <= 100 ? i : 200 - i;
        fixes.push_back({position(east), i * 100ULL, east, 0});
        history.observe(fixes.back().p, fixes.back().time);
    }
    const auto reversal = history.points();
    verify_geometry(fixes, reversal);
    require(std::any_of(reversal.begin(), reversal.end(), [](const cam::PathSample& sample) {
                return sample.position.longitude * 1e-7 * 111319.4908 > 98;
            }),
            "reversal must not collapse to a zero-length straight segment");
}
void test_limits_and_dateline() {
    cam::PathHistory history;
    for (unsigned i = 0; i < 1000; ++i)
        history.observe(position(i * 2, (i % 2) * 3), i * 100ULL);
    require(history.points().size() == 23, "highly curved path obeys CAM point limit");
    history.clear();
    auto p = position(0);
    p.longitude = 1799999900;
    history.observe(p, 1000);
    p.longitude = -1799999900;
    history.observe(p, 1100);
    require(history.points().size() == 1, "dateline crossing is a short continuous path");
    history.clear();
    p.latitude = 900000000; p.longitude = 0;
    history.observe(p, 1000);
    p.latitude -= 1000;
    history.observe(p, 1100);
    require(history.points().size() == 1, "polar tangent projection is finite");
    history.clear();
    p = position(0);
    history.observe(p, 0);
    history.observe(position(10), 100);
    for (unsigned i = 1; i < 7000; ++i) history.observe(position(10), 100 + i * 100ULL);
    require(history.points().empty(), "unrepresentably old path times are not transmitted");
}
void test_sampled_error_over_random_paths() {
    for (unsigned seed = 0; seed < 16; ++seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<double> turn(-0.65, 0.65), step(0.2, 8);
        cam::PathHistory history;
        std::vector<Fix> fixes;
        double east = 0, north = 0, heading = 0;
        for (unsigned i = 0; i < 400; ++i) {
            heading += turn(random);
            const auto length = step(random);
            if (i % 17 != 0) {
                east += length * std::sin(heading);
                north += length * std::cos(heading);
            }
            fixes.push_back({position(east, north), i * 100ULL, east, north});
            history.observe(fixes.back().p, fixes.back().time);
            const auto selected = history.points();
            if (!selected.empty()) verify_geometry(fixes, selected);
        }
    }
}
} // namespace
int main() {
    test_stationary_and_ordering();
    test_straight();
    test_curves_and_reversal();
    test_limits_and_dateline();
    test_sampled_error_over_random_paths();
    std::cout << "Concise path-history geometry tests passed\n";
}
