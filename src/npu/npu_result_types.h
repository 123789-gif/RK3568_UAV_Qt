#pragma once

#include "npu/npu_task_config.h"
#include "npu/violation_rule_config.h"
#include "npu/vehicle_direction_config.h"

#include <cstdint>
#include <string>
#include <vector>

enum class NpuTrafficLightState : std::uint8_t
{
    Unknown = 0,
    Red = 1,
    Green = 2
};

enum class NpuPedestrianState : std::uint8_t
{
    Uncertain = 0,
    Legal = 1,
    IllegalAccumulating = 2,
    Illegal = 3
};

enum class NpuPedestrianViolationReason : std::uint8_t
{
    None = 0,
    RedLight = 1,
    OutsideCrosswalk = 2,
    RedAndOutside = 3
};

enum class NpuVehicleDirectionState : std::uint8_t
{
    Unknown = 0,
    Stopped = 1,
    Normal = 2,
    WrongWay = 3
};

struct NpuWrongWayEvent
{
    std::uint64_t eventId = 0;
    std::uint64_t frameId = 0;
    std::uint64_t wallClockMs = 0;
    int trackId = -1;
    int classId = -1;
    int laneId = -1;
    float normalizedMotion = 0.0f;
    float directionCosine = 0.0f;
};

struct NpuViolationEvent
{
    std::uint64_t eventId = 0;
    std::uint64_t frameId = 0;
    std::uint64_t wallClockMs = 0;
    int trackId = -1;
    double accumulatedMs = 0.0;
    NpuPedestrianViolationReason reason =
            NpuPedestrianViolationReason::None;
};

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
    int trackId = -1;
    NpuPedestrianState pedestrianState = NpuPedestrianState::Uncertain;
    NpuPedestrianViolationReason pedestrianViolationReason =
            NpuPedestrianViolationReason::None;
    NpuVehicleDirectionState vehicleDirectionState = NpuVehicleDirectionState::Unknown;
    int laneId = -1;
    float normalizedMotion = 0.0f;
    float directionCosine = 0.0f;
    double stateDurationMs = 0.0;
};

struct NpuFrameResult
{
    uint64_t frameId = 0;
    uint64_t configRevision = 0;
    uint64_t sourceRevision = 0;
    int sourceChannel = 0;
    NpuTaskType task = NpuTaskType::PlateCascade;
    NpuPlateModelProfile requestedPlateModelProfile = NpuPlateModelProfile::Legacy;
    NpuPlateModelProfile activePlateModelProfile = NpuPlateModelProfile::Legacy;
    NpuPlatePostprocess platePostprocess = NpuPlatePostprocess::Stable;
    NpuPlateCropSource requestedPlateCropSource = NpuPlateCropSource::DetectorInput;
    NpuPlateCropSource actualPlateCropSource = NpuPlateCropSource::DetectorInput;
    bool activePlateModelProfileValid = false;
    bool modelSwitching = false;
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
    double copyMs = 0.0;
    double originalFrameCopyMs = 0.0;
    double originalRoiConvertMs = 0.0;
    double originalRoiWarpMs = 0.0;
    double rknnInputMs = 0.0;
    double rknnRunMs = 0.0;
    double rknnOutputMs = 0.0;
    double rknnPostMs = 0.0;
    uint64_t rknnRunCount = 0;
    double plateMs = 0.0;
    double pedMs = 0.0;
    double vehicleMs = 0.0;
    int processed = 0;
    int dropped = 0;
    int originalSourceWidth = 0;
    int originalSourceHeight = 0;
    int originalRoiCropCount = 0;
    int originalRoiFallbackCount = 0;
    std::string originalRoiFallbackReason;
    std::string status;
    std::string error;
    std::vector<NpuBox> plates;
    std::vector<NpuBox> pedestrians;
    // 行人违法规则开启时，pedestrians和violationPedestrians都只包含
    // 经过全画面几何、置信度和时序确认的可信框。关闭规则时pedestrians
    // 保留模型原始输出，便于独立验证模型效果。
    std::vector<NpuBox> violationPedestrians;
    int rawPedestrianCount = 0;
    int pedestrianGeometryRejectedCount = 0;
    int pedestrianConfidenceRejectedCount = 0;
    int pedestrianUnconfirmedCount = 0;
    std::vector<NpuBox> vehicles;

    bool wrongWayRegionsConfigured = false;
    bool wrongWayRuleEnabled = false;
    VehicleDirectionConfig wrongWayConfig;
    int activeVehicleTrackCount = 0;
    int activeWrongWayCount = 0;
    std::uint64_t wrongWayEventCount = 0;
    bool hasRecentWrongWay = false;
    NpuWrongWayEvent recentWrongWay;

    bool violationRegionsConfigured = false;
    bool pedestrianViolationRuleEnabled = false;
    ViolationRuleConfig violationConfig;
    NpuTrafficLightState rawTrafficLight = NpuTrafficLightState::Unknown;
    NpuTrafficLightState trafficLight = NpuTrafficLightState::Unknown;
    float trafficLightRedScore = 0.0f;
    float trafficLightGreenScore = 0.0f;
    int activeTrackCount = 0;
    int activeViolationCount = 0;
    std::uint64_t violationEventCount = 0;
    bool hasRecentViolation = false;
    NpuViolationEvent recentViolation;
};
