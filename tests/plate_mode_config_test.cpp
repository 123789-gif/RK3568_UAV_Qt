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
    NpuTaskConfig pedestrianConfig;
    pedestrianConfig.task = NpuTaskType::Pedestrian;
    pedestrianConfig.pedestrianViolationRuleEnabled = true;
    require(sanitizeNpuTaskConfig(pedestrianConfig)
            .pedestrianViolationRuleEnabled,
            "pedestrian violation rule flag was not preserved");

    NpuTaskConfig vehicleConfig;
    vehicleConfig.task = NpuTaskType::VehicleWrongWay;
    vehicleConfig.wrongWayRuleEnabled = true;
    require(sanitizeNpuTaskConfig(vehicleConfig).wrongWayRuleEnabled,
            "vehicle rule enable flag was not preserved");

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
