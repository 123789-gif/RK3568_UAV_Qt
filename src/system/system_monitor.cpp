#include "system/system_monitor.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rkqt {
namespace system {
namespace {

std::string read_file_string(const char *path)
{
    FILE *fp = std::fopen(path, "r");
    if (!fp) {
        return std::string();
    }

    char buf[256];
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    buf[n] = '\0';
    return std::string(buf);
}

bool scan_first_int(const std::string &s, int *out)
{
    if (!out) {
        return false;
    }
    const char *p = s.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '-') {
            int v = 0;
            if (std::sscanf(p, "%d", &v) == 1) {
                *out = v;
                return true;
            }
        }
        ++p;
    }
    return false;
}


struct CpuStatSample
{
    uint64_t idle = 0;
    uint64_t total = 0;
};

bool read_cpu_stat_sample(CpuStatSample *sample)
{
    if (!sample) {
        return false;
    }

    FILE *fp = std::fopen("/proc/stat", "r");
    if (!fp) {
        return false;
    }

    char cpu[8] = {};
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
    const int n = std::fscanf(fp,
                              "%7s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                              cpu,
                              &user,
                              &nice,
                              &system,
                              &idle,
                              &iowait,
                              &irq,
                              &softirq,
                              &steal);
    std::fclose(fp);
    if (n < 5) {
        return false;
    }

    sample->idle = idle + iowait;
    sample->total = user + nice + system + idle + iowait + irq + softirq + steal;
    return sample->total > 0;
}

} // namespace

int SystemMonitor::readCpuLoadPercent()
{
    static CpuStatSample last;
    static bool hasLast = false;

    CpuStatSample cur;
    if (!read_cpu_stat_sample(&cur)) {
        return -1;
    }

    if (!hasLast) {
        last = cur;
        hasLast = true;
        return -1;
    }

    const uint64_t totalDiff = cur.total - last.total;
    const uint64_t idleDiff = cur.idle - last.idle;
    last = cur;

    if (totalDiff == 0 || idleDiff > totalDiff) {
        return -1;
    }

    const int load = static_cast<int>(((totalDiff - idleDiff) * 100ULL + totalDiff / 2ULL) / totalDiff);
    if (load < 0) return 0;
    if (load > 100) return 100;
    return load;
}

int SystemMonitor::readNpuLoadPercent()
{
    int v = -1;

    std::string s = read_file_string("/sys/kernel/debug/rknpu/load");
    if (std::sscanf(s.c_str(), "NPU load: %d%%", &v) == 1) {
        return v;
    }
    if (scan_first_int(s, &v)) {
        return v;
    }

    s = read_file_string("/sys/class/devfreq/fde40000.npu/load");
    if (std::sscanf(s.c_str(), "%d", &v) == 1) {
        return v;
    }
    if (scan_first_int(s, &v)) {
        return v;
    }

    return -1;
}

double SystemMonitor::readSocTempC()
{
    const char *paths[] = {
        "/sys/devices/virtual/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone0/temp"
    };

    for (const char *path : paths) {
        const std::string s = read_file_string(path);
        if (!s.empty()) {
            long v = 0;
            if (std::sscanf(s.c_str(), "%ld", &v) == 1) {
                return static_cast<double>(v) / 1000.0;
            }
        }
    }
    return -1.0;
}

SystemSnapshot SystemMonitor::readSnapshot()
{
    SystemSnapshot snapshot;
    snapshot.cpuLoadPercent = readCpuLoadPercent();
    snapshot.npuLoadPercent = readNpuLoadPercent();
    snapshot.socTempC = readSocTempC();
    return snapshot;
}

} // namespace system
} // namespace rkqt
