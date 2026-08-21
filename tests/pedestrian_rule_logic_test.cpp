#include "npu/pedestrian_rule_logic.h"

#include <cassert>
#include <iostream>

namespace {

NpuBox makeBox(float x, float y, float w, float h)
{
    NpuBox box;
    box.x = x;
    box.y = y;
    box.w = w;
    box.h = h;
    box.score = 0.9f;
    return box;
}

void testFullFrameGeometryFilter()
{
    assert(pedestrianBoxPassesGeometry(
               makeBox(100.0f, 40.0f, 40.0f, 120.0f),
               640, 360, 24.0f, 1.8f, 5.0f));
    assert(!pedestrianBoxPassesGeometry(
                makeBox(100.0f, 40.0f, 100.0f, 80.0f),
                640, 360, 24.0f, 1.8f, 5.0f));
    assert(!pedestrianBoxPassesGeometry(
                makeBox(100.0f, 40.0f, 10.0f, 80.0f),
                640, 360, 24.0f, 1.8f, 5.0f));
    assert(!pedestrianBoxPassesGeometry(
                makeBox(100.0f, 40.0f, 20.0f, 20.0f),
                640, 360, 24.0f, 1.8f, 5.0f));
    assert(!pedestrianBoxPassesGeometry(
                makeBox(700.0f, 40.0f, 40.0f, 120.0f),
                640, 360, 24.0f, 1.8f, 5.0f));
}

void testLegalTruthTable()
{
    assert(pedestrianMovementLegal(NpuTrafficLightState::Green, true));
    assert(!pedestrianMovementLegal(NpuTrafficLightState::Green, false));
    assert(!pedestrianMovementLegal(NpuTrafficLightState::Red, true));
    assert(!pedestrianMovementLegal(NpuTrafficLightState::Red, false));
    assert(!pedestrianMovementLegal(NpuTrafficLightState::Unknown, true));

    assert(pedestrianViolationReason(NpuTrafficLightState::Green, true) ==
           NpuPedestrianViolationReason::None);
    assert(pedestrianViolationReason(NpuTrafficLightState::Green, false) ==
           NpuPedestrianViolationReason::OutsideCrosswalk);
    assert(pedestrianViolationReason(NpuTrafficLightState::Red, true) ==
           NpuPedestrianViolationReason::RedLight);
    assert(pedestrianViolationReason(NpuTrafficLightState::Red, false) ==
           NpuPedestrianViolationReason::RedAndOutside);
    assert(pedestrianViolationReason(NpuTrafficLightState::Unknown, false) ==
           NpuPedestrianViolationReason::None);
}

} // namespace

int main()
{
    testFullFrameGeometryFilter();
    testLegalTruthTable();
    std::cout << "pedestrian_rule_logic_test: PASS\n";
    return 0;
}
