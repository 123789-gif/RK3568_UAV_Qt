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
- AI单任务选择、目标FPS、置信度和NMS阈值配置；
- AI运行状态与最近50条有效识别结果展示；
- AI配置写入部署目录 `config/ai_panel.ini`；
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

- `NpuTaskConfig` 统一保存启用状态、任务类型、目标FPS、置信度和NMS阈值；
- 单槽最新帧邮箱，输入固定来自PCIe主视频；
- 行人检测任务仅装载并执行 `ped_det.rknn`；
- 车牌级联任务仅装载并执行 `plate_det.rknn → plate_reco.rknn`；
- 任务切换时由推理线程释放旧任务模型并装载新任务模型；
- 统一结果快照与64帧有界结果历史，供HDMI OSD和Qt页面读取；
- RGB565整帧复制仍保持一份，不新增UI侧图像复制。

首版运行模式为单任务，同一时刻不并发行人和车牌任务。

### `src/ptz`

- UVC 视频采集；
- VISCA 串口控制；
- PTZ 状态和命令。

### `src/system`

- 风扇 PWM；
- 板级管理。

## 4. 当前已知问题

1. 首版仅支持单任务，不支持行人检测与车牌级联并发；
2. 车牌检测接口尚未返回检测分数，Qt结果表中车牌置信度显示为 `--`；
3. NPU输入仍为1920×1080 RGB565整帧复制，后续可评估裁剪或缩小输入邮箱；
4. HDMI 左上角系统 OSD 与 AI OSD 的统一排版尚未完成；
5. DRM/RGA、OSD 和 NPU 帧提交仍集中在 `DrmRgaDisplay`；
6. PCIe 驱动由部署脚本条件自动加载，用户态接收逻辑已剥离到 `src/pcie`。

## 5. 推荐后续顺序

```text
已完成：部署基线与PCIe驱动条件自动加载
已完成：PcieFrameReceiver 与 DrmRgaDisplay 解耦
已完成：NpuTaskConfig、单任务调度、结果快照和AI页面首版
下一步：板端性能与任务切换回归
下一步：OverlayComposer统一OSD，解决左上角文字重叠
后续：按实测瓶颈评估ROI、低分辨率邮箱或多任务调度
```

## 6. 当前 NPU 数据流

```text
PCIe RGB565 DMA帧
        ↓
按目标FPS抽帧并复制到单槽最新帧邮箱
        ↓
读取一次完整NpuTaskConfig快照
        ↓
RGB565 → RGB预处理
        ↓
当前单一任务
  ├─ 行人检测：ped_det
  └─ 车牌级联：plate_det → plate_reco
        ↓
统一NpuFrameResult
        ├─ HDMI OSD读取最新结果
        ├─ Qt页面5 Hz读取最新状态
        └─ Qt页面读取64帧有界结果历史
```

推理线程仍为单线程。任务切换不重启Qt、不改变PCIe/RGA/DRM链路，也不新增整帧复制。
