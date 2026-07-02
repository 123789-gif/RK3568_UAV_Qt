#pragma once

#include "npu/npu_result_types.h"
#include "npu/npu_task_config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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

    void setInputSource(int sourceChannel, uint64_t sourceRevision);
    bool wantsFrame() const;
    void submitFrame(uint64_t frameId,
                     const uint8_t* rgb565,
                     std::size_t bytes,
                     int width,
                     int height,
                     int sourceChannel,
                     uint64_t sourceRevision);
    NpuFrameResult latestResult() const;
    std::vector<NpuFrameResult> resultsAfter(uint64_t frameId, std::size_t maxResults = 64) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
