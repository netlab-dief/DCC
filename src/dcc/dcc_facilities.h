#pragma once
#include "dcc/dcc_contracts.h"
#include <algorithm>

// Initial facilities adapter for the existing single CAM service. This is not
// a multi-service DCC_FAC allocator or a Release 2 implementation.
class DccFacilities {
public:
    static int cam_generation_interval_ms(const dcc::ChannelResources& resources) {
        if (!resources.valid) return 1000;
        const auto ms = (resources.suggested_admission_interval.count() + 999) / 1000;
        return static_cast<int>(std::clamp<std::int64_t>(ms, 100, 1000));
    }
};
