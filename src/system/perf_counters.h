#pragma once

#include <cstdint>

namespace rkqt {
namespace perf {

struct AverageCounter
{
    uint64_t totalNs = 0;
    uint64_t count = 0;

    void addNs(uint64_t ns) {
        totalNs += ns;
        ++count;
    }

    double averageUs() const {
        return count ? static_cast<double>(totalNs) / static_cast<double>(count) / 1000.0 : 0.0;
    }

    void reset() {
        totalNs = 0;
        count = 0;
    }
};

} // namespace perf
} // namespace rkqt
