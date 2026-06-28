# 当前架构与重构边界

## 当前数据流

```text
FPGA PCIe DMA -> DrmRgaDisplay -> RGA/DRM HDMI
                         \-> latest-frame copy -> NpuInferenceWorker
PTZ UVC -> PtzCaptureWorker -> Qt DSI preview
Qt controls -> PtzControlWorker -> VISCA serial
```

## 第一阶段保持不变

- `DrmRgaDisplay` 仍同时包含 PCIe、RGA、DRM、OSD 和 NPU 帧提交；
- NPU 仍固定执行车牌检测、车牌识别、行人检测；
- AI 页面仍使用总开关、总 FPS 和总置信度；
- OSD 布局未修改。

这样可以先验证目录重构没有改变运行行为。

## 后续重构顺序

1. 部署基线回归；
2. 拆分 NPU 任务配置与调度器；
3. 完善 AI 页面；
4. 引入统一 OverlayComposer，解决 OSD 重叠；
5. 最后再从 `DrmRgaDisplay` 中拆出独立 PCIe 接收模块。
