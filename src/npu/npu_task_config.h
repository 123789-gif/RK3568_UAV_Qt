#pragma once

#include <algorithm>
#include <cstdint>

enum class NpuTaskType : std::uint8_t
{
    Pedestrian = 0,
    PlateCascade = 1,
    TrafficViolation = 2
};

enum class NpuPlateModelProfile : std::uint8_t
{
    Legacy = 0,
    V291Candidate = 1
};

enum class NpuPlatePostprocess : std::uint8_t
{
    Stable = 0,
    Raw = 1
};

// OCR车牌图的像素来源。检测模型始终使用现有低分辨率输入；
// OriginalSource只改变四点透视裁剪的数据源，不改变RKNN模型输入契约。
enum class NpuPlateCropSource : std::uint8_t
{
    DetectorInput = 0,
    OriginalSource = 1
};

inline constexpr int NPU_TARGET_FPS_MIN = 1;
inline constexpr int NPU_TARGET_FPS_DEFAULT = 15;
inline constexpr int NPU_TARGET_FPS_MAX = 60;

struct NpuTaskConfig
{
    bool enabled = false;
    NpuTaskType task = NpuTaskType::PlateCascade;
    NpuPlateModelProfile plateModelProfile = NpuPlateModelProfile::Legacy;
    NpuPlatePostprocess platePostprocess = NpuPlatePostprocess::Stable;
    NpuPlateCropSource plateCropSource = NpuPlateCropSource::DetectorInput;
    int targetFps = NPU_TARGET_FPS_DEFAULT;
    float confidence = 0.50f;
    float nmsThreshold = 0.45f;
    int resultHistoryDepth = 50;
    std::uint64_t revision = 0;
};

inline NpuTaskConfig sanitizeNpuTaskConfig(NpuTaskConfig config)
{
    if (config.plateModelProfile != NpuPlateModelProfile::Legacy &&
        config.plateModelProfile != NpuPlateModelProfile::V291Candidate) {
        config.plateModelProfile = NpuPlateModelProfile::V291Candidate;
    }
    if (config.platePostprocess != NpuPlatePostprocess::Stable &&
        config.platePostprocess != NpuPlatePostprocess::Raw) {
        config.platePostprocess = NpuPlatePostprocess::Stable;
    }
    if (config.plateCropSource != NpuPlateCropSource::DetectorInput &&
        config.plateCropSource != NpuPlateCropSource::OriginalSource) {
        config.plateCropSource = NpuPlateCropSource::DetectorInput;
    }
    // 第一轮原图ROI实验仅接入具备四点诊断输出的v2.9.1后端。
    // legacy仍保持已验证链路，避免“标准模式”被实验代码影响。
    if (config.plateModelProfile == NpuPlateModelProfile::Legacy) {
        config.plateCropSource = NpuPlateCropSource::DetectorInput;
    }
    config.targetFps = std::max(NPU_TARGET_FPS_MIN,
                                std::min(NPU_TARGET_FPS_MAX, config.targetFps));
    config.confidence = std::max(0.10f, std::min(0.95f, config.confidence));
    config.nmsThreshold = std::max(0.10f, std::min(0.90f, config.nmsThreshold));
    config.resultHistoryDepth = std::max(10, std::min(200, config.resultHistoryDepth));
    return config;
}

inline const char* npuPlateModelProfileKey(NpuPlateModelProfile profile)
{
    return profile == NpuPlateModelProfile::Legacy ? "legacy" : "v291_candidate";
}

inline const char* npuPlatePostprocessKey(NpuPlatePostprocess mode)
{
    return mode == NpuPlatePostprocess::Raw ? "raw" : "stable";
}

inline const char* npuPlateCropSourceKey(NpuPlateCropSource source)
{
    return source == NpuPlateCropSource::OriginalSource
            ? "original" : "detector";
}

inline const char* npuTaskKey(NpuTaskType task)
{
    switch (task) {
    case NpuTaskType::TrafficViolation:
        return "traffic_violation";
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
    case NpuTaskType::TrafficViolation:
        return "VIOL";
    case NpuTaskType::PlateCascade:
        return "PLATE";
    case NpuTaskType::Pedestrian:
    default:
        return "PED";
    }
}
