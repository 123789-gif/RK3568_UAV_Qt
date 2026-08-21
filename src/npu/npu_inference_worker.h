#pragma once

#include "npu/npu_result_types.h"
#include "npu/npu_task_config.h"
#include "npu/violation_rule_config.h"
#include "npu/vehicle_direction_config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// 捕获线程在submitFrame()调用期间有效的只读原始帧视图。
// worker只在任一车牌原图ROI模式下同步复制选路区域，不跨线程保存data裸指针。
struct NpuOriginalFrameView
{
    const uint8_t *data = nullptr;
    std::size_t bytes = 0;
    int width = 0;
    int height = 0;
    int stridePixels = 0;
    int roiX = 0;
    int roiY = 0;
    int roiWidth = 0;
    int roiHeight = 0;
};

class NpuInferenceWorker
{
public:
    NpuInferenceWorker();
    ~NpuInferenceWorker();

    NpuInferenceWorker(const NpuInferenceWorker&) = delete;
    NpuInferenceWorker& operator=(const NpuInferenceWorker&) = delete;

    void start();
    void stop();

    void setConfig(const NpuTaskConfig& config);
    NpuTaskConfig config() const;

    // 兼容旧调用接口，内部统一更新一份完整配置快照。
    void setEnabled(bool enabled);
    bool enabled() const;
    void setTask(NpuTaskType task);
    NpuTaskType task() const;
    void setTargetFps(int fps);
    int targetFps() const;
    void setConfidence(float confidence);
    float confidence() const;
    void setNmsThreshold(float threshold);
    float nmsThreshold() const;

    void setViolationConfig(const ViolationRuleConfig& config);
    ViolationRuleConfig violationConfig() const;

    void setWrongWayConfig(const VehicleDirectionConfig& config);
    VehicleDirectionConfig wrongWayConfig() const;

    void setInputSource(int sourceChannel, uint64_t sourceRevision);
    bool wantsFrame() const;
    void submitFrame(uint64_t frameId,
                     const uint8_t* rgb565,
                     std::size_t bytes,
                     int width,
                     int height,
                     int sourceChannel,
                     uint64_t sourceRevision,
                     const NpuOriginalFrameView *originalView = nullptr);
    NpuFrameResult latestResult() const;
    std::vector<NpuFrameResult> resultsAfter(uint64_t frameId, std::size_t maxResults = 64) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
