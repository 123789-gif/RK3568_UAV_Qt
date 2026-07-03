#pragma once

#include "npu/npu_task_config.h"

#include <cstdint>
#include <string>
#include <vector>

struct NpuBox
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float score = 0.0f;
    int classId = 0;
    std::string text;
    std::string label;
};

struct NpuFrameResult
{
    uint64_t frameId = 0;
    uint64_t configRevision = 0;
    uint64_t sourceRevision = 0;
    int sourceChannel = 0;
    NpuTaskType task = NpuTaskType::Pedestrian;
    bool enabled = false;
    bool ready = false;
    bool modelsReady = false;
    bool usedRga = false;
    int targetFps = NPU_TARGET_FPS_DEFAULT;
    float confidence = 0.50f;
    float nmsThreshold = 0.45f;
    double fps = 0.0;
    double lastMs = 0.0;
    double preMs = 0.0;
    double plateMs = 0.0;
    double pedMs = 0.0;
    int processed = 0;
    int dropped = 0;
    std::string status;
    std::string error;
    std::vector<NpuBox> plates;
    std::vector<NpuBox> pedestrians;
};
