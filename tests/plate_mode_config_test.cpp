#include "npu/npu_task_config.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "plate_mode_config_test: FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void verifyMode(NpuPlateModelProfile profile,
                NpuPlateCropSource cropSource,
                const char* name)
{
    NpuTaskConfig config;
    config.task = NpuTaskType::PlateCascade;
    config.plateModelProfile = profile;
    config.plateCropSource = cropSource;
    const NpuTaskConfig sanitized = sanitizeNpuTaskConfig(config);
    require(sanitized.plateModelProfile == profile, name);
    require(sanitized.plateCropSource == cropSource, name);
}

} // namespace

int main()
{
    require(NPU_PLATE_TARGET_FPS_DEFAULT == 30,
            "plate UI default fps must remain 30");
    require(NPU_TARGET_FPS_DEFAULT == 15,
            "pedestrian/vehicle default fps must remain 15");

    NpuTaskConfig pedestrianConfig;
    pedestrianConfig.enabled = true;
    pedestrianConfig.task = NpuTaskType::Pedestrian;
    pedestrianConfig.pedestrianViolationRuleEnabled = true;
    const NpuTaskConfig pedestrianRuleConfig =
            sanitizeNpuTaskConfig(pedestrianConfig);
    require(pedestrianRuleConfig.enabled &&
            pedestrianRuleConfig.pedestrianViolationRuleEnabled,
            "pedestrian violation rule flag was not preserved");
    pedestrianConfig.pedestrianViolationRuleEnabled = false;
    require(sanitizeNpuTaskConfig(pedestrianConfig).enabled,
            "disabling pedestrian rule unexpectedly stopped the model");

    NpuTaskConfig vehicleConfig;
    vehicleConfig.enabled = true;
    vehicleConfig.task = NpuTaskType::VehicleWrongWay;
    vehicleConfig.wrongWayRuleEnabled = true;
    const NpuTaskConfig vehicleRuleConfig = sanitizeNpuTaskConfig(vehicleConfig);
    require(vehicleRuleConfig.enabled && vehicleRuleConfig.wrongWayRuleEnabled,
            "vehicle rule enable flag was not preserved");
    vehicleConfig.wrongWayRuleEnabled = false;
    require(sanitizeNpuTaskConfig(vehicleConfig).enabled,
            "disabling wrong-way rule unexpectedly stopped the model");

    NpuTaskConfig plateConfig;
    plateConfig.task = NpuTaskType::PlateCascade;
    plateConfig.pedestrianViolationRuleEnabled = true;
    plateConfig.wrongWayRuleEnabled = true;
    const NpuTaskConfig sanitizedPlate = sanitizeNpuTaskConfig(plateConfig);
    require(!sanitizedPlate.pedestrianViolationRuleEnabled,
            "pedestrian rule leaked into plate task");
    require(!sanitizedPlate.wrongWayRuleEnabled,
            "vehicle rule leaked into plate task");
    verifyMode(NpuPlateModelProfile::Legacy,
               NpuPlateCropSource::DetectorInput,
               "legacy detector ROI was not preserved");
    verifyMode(NpuPlateModelProfile::Legacy,
               NpuPlateCropSource::OriginalSource,
               "legacy original ROI was not preserved");
    verifyMode(NpuPlateModelProfile::V291Candidate,
               NpuPlateCropSource::DetectorInput,
               "v291 detector ROI was not preserved");
    verifyMode(NpuPlateModelProfile::V291Candidate,
               NpuPlateCropSource::OriginalSource,
               "v291 original ROI was not preserved");
    std::cout << "plate_mode_config_test: PASS" << std::endl;
    return 0;
}
