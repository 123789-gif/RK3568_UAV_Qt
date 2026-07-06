#pragma once

namespace rkqt {
namespace system {

struct SystemSnapshot
{
    int cpuLoadPercent = -1;
    int npuLoadPercent = -1;
    double socTempC = -1.0;
};

class SystemMonitor
{
public:
    static int readCpuLoadPercent();
    static int readNpuLoadPercent();
    static double readSocTempC();
    static SystemSnapshot readSnapshot();
};

} // namespace system
} // namespace rkqt
