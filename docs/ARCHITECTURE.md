# 当前架构与数据流

## 1. 系统数据流

```text
FPGA
  └─ PCIe DMA / 四缓冲
       └─ PcieFrameReceiver
            └─ 借用 DMA mmap 帧指针
                 └─ DrmRgaDisplay
                      ├─ RGA 缩放/格式处理
                      ├─ DRM/KMS → HDMI 1920×1080
                      ├─ OSD 系统状态与 AI 结果
                      └─ 最新帧拷贝 → NpuInferenceWorker

PTZ USB 采集
  └─ PtzCaptureWorker
       └─ Qt DSI 预览

Qt 控制界面
  └─ PtzControlWorker
       └─ VISCA /dev/ttyS0

FanController
  └─ PWM 与板级温度/状态
```

## 2. 显示拓扑

```text
DSI-1:
  面板原始模式 720×1280
  右旋后逻辑尺寸 1280×720
  位置 0x0
  primary

HDMI-1:
  1920×1080@60
  位置 1280x0

X11 虚拟桌面:
  3200×1080
```

Goodix 触摸仅映射到 `DSI-1`。

## 3. 当前模块职责

### `src/ui`

- 控制页面；
- NPU 总开关、总 FPS、总阈值；
- PTZ 控制；
- 系统状态显示；
- 模块信号连接的一部分。

### `src/pcie`

`PcieFrameReceiver` 负责：

- `/dev/pango_pci_driver` 打开与关闭；
- PCIe ioctl ABI 定义；
- DMA 四缓冲配置、mmap 与 unmap；
- BAR0 `/dev/mem` 映射；
- FPGA 启停寄存器写入；
- 阻塞等待下一帧并返回借用帧视图；
- PCIe 链路信息和错误状态。

该模块不创建新线程、不复制整帧，仍由显示工作线程同步调用。

### `src/video`

`DrmRgaDisplay` 负责：

- 调用 `PcieFrameReceiver` 获取最新 DMA 帧；
- RGA；
- DRM/KMS；
- OSD；
- NPU 帧提交；
- 状态统计。

### `src/npu`

- 三个 RKNN 模型；
- RGB 输入预处理；
- 车牌检测；
- 车牌识别；
- 行人检测；
- 结果发布。

当前每次推理仍固定执行既有任务序列。

### `src/ptz`

- UVC 视频采集；
- VISCA 串口控制；
- PTZ 状态和命令。

### `src/system`

- 风扇 PWM；
- 板级管理。

## 4. 当前已知问题

1. NPU 任务不能独立启停；
2. 不同模型共用 FPS 和置信度；
3. 全帧预处理和数据复制尚未按多任务调度复用；
4. HDMI 左上角系统 OSD 与 AI OSD 重叠；
5. DRM/RGA、OSD 和 NPU 帧提交仍集中在 `DrmRgaDisplay`；
6. PCIe 驱动由部署脚本条件自动加载，用户态接收逻辑已剥离到 `src/pcie`。

## 5. 推荐后续顺序

```text
1. 固化文档与部署基线
2. PCIe 驱动条件自动加载
3. PcieFrameReceiver 与 DrmRgaDisplay 解耦
4. NPU 任务配置结构
5. 单线程多任务调度与单帧邮箱
6. 统一 NPU 结果快照和结果过期机制
7. AI 推理页面
8. OverlayComposer 统一 OSD
```

## 6. NPU 推荐目标数据流

```text
PCIe RGB565 最新帧
        ↓
最多一帧的最新帧邮箱
        ↓
判断本周期到期任务
        ↓
RGB565 → RGB 只转换一次
        ↓
行人检测 / 车牌检测 / 车牌 OCR
        ↓
统一不可变结果快照
        ├─ Qt AI 页面
        ├─ HDMI OSD
        └─ 外部结果回调
```

首版应保持单调度线程。只有测得串行模型执行无法满足目标 FPS 后，才评估 RKNN 多上下文和 NPU 核并发。
