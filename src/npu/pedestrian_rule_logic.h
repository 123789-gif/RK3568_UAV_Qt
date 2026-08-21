#pragma once

#include "npu/npu_result_types.h"

#include <algorithm>
#include <cmath>

inline bool pedestrianBoxPassesGeometry(const NpuBox& box,
                                        int frameWidth,
                                        int frameHeight,
                                        float minHeightPx,
                                        float minHeightWidthRatio,
                                        float maxHeightWidthRatio)
{
    if (frameWidth <= 0 || frameHeight <= 0 ||
        !std::isfinite(box.x) || !std::isfinite(box.y) ||
        !std::isfinite(box.w) || !std::isfinite(box.h) ||
        box.w <= 1.0f || box.h < minHeightPx) {
        return false;
    }

    const float ratio = box.h / box.w;
    if (!std::isfinite(ratio) || ratio < minHeightWidthRatio ||
        ratio > maxHeightWidthRatio) {
        return false;
    }

    // 允许检测框在边界外有少量回映射误差，但拒绝主体完全落在画面外。
    const float left = std::max(0.0f, box.x);
    const float top = std::max(0.0f, box.y);
    const float right = std::min(static_cast<float>(frameWidth), box.x + box.w);
    const float bottom = std::min(static_cast<float>(frameHeight), box.y + box.h);
    return right - left > 1.0f && bottom - top > 1.0f;
}

inline bool pedestrianLightKnown(NpuTrafficLightState light)
{
    return light == NpuTrafficLightState::Red ||
            light == NpuTrafficLightState::Green;
}

inline bool pedestrianMovementLegal(NpuTrafficLightState light,
                                    bool footInsideCrosswalk)
{
    return light == NpuTrafficLightState::Green && footInsideCrosswalk;
}

inline NpuPedestrianViolationReason pedestrianViolationReason(
        NpuTrafficLightState light,
        bool footInsideCrosswalk)
{
    if (!pedestrianLightKnown(light) ||
        pedestrianMovementLegal(light, footInsideCrosswalk)) {
        return NpuPedestrianViolationReason::None;
    }
    if (light == NpuTrafficLightState::Red && !footInsideCrosswalk) {
        return NpuPedestrianViolationReason::RedAndOutside;
    }
    return light == NpuTrafficLightState::Red
            ? NpuPedestrianViolationReason::RedLight
            : NpuPedestrianViolationReason::OutsideCrosswalk;
}
