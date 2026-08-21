#pragma once

#include <array>
#include <cstdint>
#include <vector>

class QString;

struct VehicleDirectionPointF
{
    float x = 0.0f;
    float y = 0.0f;
};

struct VehicleLaneDirectionConfig
{
    std::array<VehicleDirectionPointF, 4> quad {};
    VehicleDirectionPointF direction {0.0f, -1.0f};
};

struct VehicleDirectionConfig
{
    std::vector<VehicleLaneDirectionConfig> lanes;

    // 全部时序参数均使用毫秒，避免NPU目标帧率和显示frameId改变规则语义。
    int directionWindowMs = 600;
    int wrongWayHoldMs = 350;
    int wrongWayClearMs = 800;
    int trackTtlMs = 800;
    int stationaryHoldMs = 450;
    float motionThreshold = 0.02f;
    float directionCosine = 0.50f;
    float boxEma = 0.30f;
    std::uint64_t revision = 1;

    bool regionsValid() const { return !lanes.empty(); }
};

VehicleDirectionConfig sanitizeVehicleDirectionConfig(VehicleDirectionConfig config);
bool validateVehicleDirectionConfig(const VehicleDirectionConfig& config,
                                    QString* errorMessage = nullptr);
bool loadVehicleDirectionConfig(const QString& path,
                                VehicleDirectionConfig* config,
                                QString* errorMessage = nullptr);
bool saveVehicleDirectionConfig(const QString& path,
                                const VehicleDirectionConfig& config,
                                QString* errorMessage = nullptr);
