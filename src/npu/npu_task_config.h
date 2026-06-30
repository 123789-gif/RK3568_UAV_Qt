#pragma once

#include <algorithm>
#include <cstdint>

enum class NpuTaskType : std::uint8_t
{
    Pedestrian = 0,
    PlateCascade = 1
};

struct NpuTaskConfig
{
    bool enabled = false;
    NpuTaskType task = NpuTaskType::Pedestrian;
    int targetFps = 5;
    float confidence = 0.50f;
    float nmsThreshold = 0.45f;
    int resultHistoryDepth = 50;
    std::uint64_t revision = 0;
};

inline NpuTaskConfig sanitizeNpuTaskConfig(NpuTaskConfig config)
{
    config.targetFps = std::max(1, std::min(15, config.targetFps));
    config.confidence = std::max(0.10f, std::min(0.95f, config.confidence));
    config.nmsThreshold = std::max(0.10f, std::min(0.90f, config.nmsThreshold));
    config.resultHistoryDepth = std::max(10, std::min(200, config.resultHistoryDepth));
    return config;
}

inline const char* npuTaskKey(NpuTaskType task)
{
    switch (task) {
    case NpuTaskType::PlateCascade:
        return "plate_cascade";
    case NpuTaskType::Pedestrian:
    default:
        return "pedestrian";
    }
}

inline const char* npuTaskShortName(NpuTaskType task)
{
    switch (task) {
    case NpuTaskType::PlateCascade:
        return "PLATE";
    case NpuTaskType::Pedestrian:
    default:
        return "PED";
    }
}
