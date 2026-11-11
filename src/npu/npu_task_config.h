#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
enum class NpuTaskType : std::uint8_t
{
    DroneDetection = 0
};
inline constexpr int NPU_TARGET_FPS_MIN = 1;
inline constexpr int NPU_TARGET_FPS_DEFAULT = 5;
inline constexpr int NPU_TARGET_FPS_MAX = 30;
struct NpuTaskConfig
{
    bool enabled = false;
    NpuTaskType task = NpuTaskType::DroneDetection;
    int targetFps = NPU_TARGET_FPS_DEFAULT;
    float confidence = 0.35f, nmsThreshold = 0.45f;
    std::uint64_t revision = 1;
};
inline NpuTaskConfig sanitizeNpuTaskConfig(NpuTaskConfig c)
{
    c.task = NpuTaskType::DroneDetection;
    c.targetFps = std::clamp(c.targetFps, NPU_TARGET_FPS_MIN, NPU_TARGET_FPS_MAX);
    c.confidence = std::isfinite(c.confidence) ? std::clamp(c.confidence, 0.01f, 0.99f) : 0.35f;
    c.nmsThreshold =
        std::isfinite(c.nmsThreshold) ? std::clamp(c.nmsThreshold, 0.01f, 0.99f) : 0.45f;
    return c;
}
inline const char *npuTaskKey(NpuTaskType)
{
    return "DRONE";
}
