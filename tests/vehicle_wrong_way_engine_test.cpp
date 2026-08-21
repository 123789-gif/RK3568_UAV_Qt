#include "npu/vehicle_wrong_way_engine.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

VehicleDirectionConfig singleLaneConfig()
{
    VehicleDirectionConfig config;
    VehicleLaneDirectionConfig lane;
    lane.quad = {{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
    lane.direction = {0.0f, -1.0f};
    config.lanes.push_back(lane);
    config.directionWindowMs = 300;
    config.wrongWayHoldMs = 300;
    config.wrongWayClearMs = 500;
    config.trackTtlMs = 800;
    config.stationaryHoldMs = 300;
    config.motionThreshold = 0.02f;
    config.directionCosine = 0.50f;
    return config;
}

NpuFrameResult step(VehicleWrongWayEngine& engine,
                    const VehicleDirectionConfig& config,
                    std::uint64_t frame,
                    std::uint64_t timeMs,
                    float y,
                    int sourceChannel = 1,
                    std::uint64_t sourceRevision = 1)
{
    std::vector<NpuBox> boxes(1);
    boxes[0].x = 260.0f;
    boxes[0].y = y;
    boxes[0].w = 100.0f;
    boxes[0].h = 60.0f;
    boxes[0].score = 0.9f;
    boxes[0].classId = 0;
    NpuFrameResult result;
    engine.setConfig(config);
    engine.process(frame, timeMs, 1000000 + timeMs,
                   640, 360, sourceChannel, sourceRevision, &boxes, &result);
    result.vehicles = boxes;
    return result;
}

void testWrongWayAndReset()
{
    VehicleWrongWayEngine engine;
    VehicleDirectionConfig config = singleLaneConfig();
    NpuFrameResult result;
    for (int i = 0; i < 10; ++i) {
        result = step(engine, config, static_cast<std::uint64_t>(i + 1),
                      static_cast<std::uint64_t>(i * 100), 40.0f + i * 8.0f);
    }
    assert(result.wrongWayEventCount == 1);
    assert(result.hasRecentWrongWay);
    assert(!result.vehicles.empty());
    assert(result.vehicles[0].vehicleDirectionState == NpuVehicleDirectionState::WrongWay);

    result = step(engine, config, 11, 1000, 120.0f, 2, 2);
    assert(result.wrongWayEventCount == 0);
    assert(!result.hasRecentWrongWay);
    assert(result.vehicles[0].trackId == 1);
}

void testNormalDirectionDoesNotTrigger()
{
    VehicleWrongWayEngine engine;
    VehicleDirectionConfig config = singleLaneConfig();
    NpuFrameResult result;
    for (int i = 0; i < 12; ++i) {
        result = step(engine, config, static_cast<std::uint64_t>(i + 1),
                      static_cast<std::uint64_t>(i * 100), 200.0f - i * 8.0f);
    }
    assert(result.wrongWayEventCount == 0);
    assert(!result.vehicles.empty());
    assert(result.vehicles[0].vehicleDirectionState == NpuVehicleDirectionState::Normal);
}

void testOverviewTilesNeverShareTrack()
{
    VehicleWrongWayEngine engine;
    VehicleDirectionConfig config = singleLaneConfig();

    std::vector<NpuBox> boxes(1);
    boxes[0].x = 205.0f;
    boxes[0].y = 30.0f;
    boxes[0].w = 10.0f;
    boxes[0].h = 20.0f;
    boxes[0].score = 0.9f;
    boxes[0].classId = 0;
    NpuFrameResult first;
    engine.setConfig(config);
    engine.process(1, 0, 1000000, 640, 360, 0, 1, &boxes, &first);
    assert(boxes.size() == 1);
    const int firstTrack = boxes[0].trackId;

    // 两个框只相差6像素，但底边中心分别位于CH1和CH2 tile。
    boxes[0].x = 211.0f;
    NpuFrameResult second;
    engine.process(2, 100, 1000100, 640, 360, 0, 1, &boxes, &second);
    assert(boxes.size() == 1);
    assert(boxes[0].trackId != firstTrack);
}

} // namespace

int main()
{
    testWrongWayAndReset();
    testNormalDirectionDoesNotTrigger();
    testOverviewTilesNeverShareTrack();
    std::cout << "vehicle_wrong_way_engine_test: PASS\n";
    return 0;
}
