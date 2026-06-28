#pragma once

#include "npu/npu_result_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

class NpuInferenceWorker
{
public:
    NpuInferenceWorker();
    ~NpuInferenceWorker();

    NpuInferenceWorker(const NpuInferenceWorker&) = delete;
    NpuInferenceWorker& operator=(const NpuInferenceWorker&) = delete;

    void start();
    void stop();

    void setEnabled(bool enabled);
    bool enabled() const;

    void setTargetFps(int fps);
    int targetFps() const;

    void setConfidence(float confidence);
    float confidence() const;

    void submitFrame(uint64_t frameId, const uint8_t* rgb565, std::size_t bytes, int width, int height);
    NpuFrameResult latestResult() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
