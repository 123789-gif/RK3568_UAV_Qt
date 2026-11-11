#pragma once
#include "npu/npu_task_config.h"
#include <string>
#include <vector>
struct NpuBox
{
    float x = 0, y = 0, w = 0, h = 0, score = 0;
    int classId = 0;
};
struct NpuFrameResult
{
    std::uint64_t frameId = 0, configRevision = 0, sourceRevision = 0;
    int sourceChannel = 0;
    NpuTaskType task = NpuTaskType::DroneDetection;
    bool enabled = false, ready = false, modelsReady = false;
    int targetFps = NPU_TARGET_FPS_DEFAULT;
    float confidence = 0.35f, nmsThreshold = 0.45f;
    double fps = 0, lastMs = 0, preMs = 0, rknnRunMs = 0, postMs = 0, copyMs = 0;
    int processed = 0, dropped = 0;
    std::string status = "off", error;
    // Coordinates normalized to selected-source 640x360 for existing display mapping.
    std::vector<NpuBox> drones;
};
