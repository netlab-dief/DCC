#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// ETSI TS 102 687 V1.2.1, 5.4 / Table 3. CBR and delta are fractions.
class DccAdaptive {
public:
    static constexpr double delta_min = 0.0006;
    static constexpr double delta_max = 0.03;

    // slot = floor(UTC milliseconds / 100). Only complete, consecutive
    // 100 ms pairs ending on a UTC 200 ms boundary update the controller.
    bool observe(double cbr, std::int64_t slot) {
        if (!std::isfinite(cbr) || cbr < 0.0 || cbr > 1.0) {
            invalidate();
            return false;
        }
        if (have_previous_ && slot <= previous_slot_) return false;
        const bool ready = have_previous_ && slot == previous_slot_ + 1 && slot % 2 == 0;
        if (ready) {
            filtered_ = 0.5 * filtered_ + 0.5 * ((previous_ + cbr) / 2.0);
            const double offset = std::clamp(0.0012 * (0.68 - filtered_), -0.00025, 0.0005);
            delta_ = std::clamp((1.0 - 0.016) * delta_ + offset, delta_min, delta_max);
        }
        previous_ = cbr;
        previous_slot_ = slot;
        have_previous_ = true;
        return ready;
    }
    void invalidate() { have_previous_ = false; }
    double delta() const { return delta_; }
    double filtered_cbr() const { return filtered_; }

private:
    double delta_ = delta_min;
    double filtered_ = 0.0;
    double previous_ = 0.0;
    std::int64_t previous_slot_ = 0;
    bool have_previous_ = false;
};
