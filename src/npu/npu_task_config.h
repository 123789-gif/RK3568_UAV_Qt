#pragma once

#include <algorithm>
#include <cstdint>

enum class NpuTaskType : std::uint8_t
{
    Pedestrian = 0,
    PlateCascade = 1
};

inline constexpr int NPU_TARGET_FPS_MIN = 1;
inline constexpr int NPU_TARGET_FPS_DEFAULT = 15;
inline constexpr int NPU_TARGET_FPS_MAX = 30;

struct NpuTaskConfig
{
    bool enabled = false;
    NpuTaskType task = NpuTaskType::Pedestrian;
    int targetFps = NPU_TARGET_FPS_DEFAULT;
    float confidence = 0.50f;
    float nmsThreshold = 0.45f;
    int resultHistoryDepth = 50;
    std::uint64_t revision = 0;
};

inline NpuTaskConfig sanitizeNpuTaskConfig(NpuTaskConfig config)
{
    config.targetFps = std::max(NPU_TARGET_FPS_MIN,
                                std::min(NPU_TARGET_FPS_MAX, config.targetFps));
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
