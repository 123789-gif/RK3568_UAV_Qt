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


## 九宫格软件选路与单路推理

PCIe 输入仍保持固定的 1920×1080 RGB565 九宫格帧，不修改 FPGA、PCIe 协议或驱动 ABI。视频显示页提供“九宫格总览”和 CH1～CH9 十种路由：

- 总览模式直接显示完整 1920×1080 九宫格，并暂停 NPU 帧提交；
- 单路模式按照标准 3×3 映射裁出对应 640×360 ROI，RGA 放大到 HDMI 全屏；
- NPU 仅在目标帧率到期时由 RGA 复制所选 640×360 ROI，避免继续复制整幅 1080P 九宫格；
- 通道切换使用 source revision 丢弃旧通道在途结果，清空旧 OSD 与结果历史；
- NPU 检测框以 640×360 单路坐标映射到 HDMI 输出坐标。

首版不持久化通道选择，程序启动默认进入九宫格总览，避免启动后自动对未知通道推理。

## PTZ 双相机 Profile 与自动检测

PTZ 视频采集不再写死 `/dev/video0` 和单一分辨率。Qt 启动或用户点击“应用配置”时，先枚举 `/dev/videoN`，过滤不支持 `VIDEO_CAPTURE + STREAMING` 的辅助节点，再沿 sysfs 读取所属 USB 设备的 VID/PID，并验证目标 MJPEG 分辨率和帧率能力。

当前内置 Profile：

| Profile | USB VID:PID | 首选采集格式 | 预览比例 | 控制链路 |
|---|---|---|---|---|
| 当前 HD USB PTZ | `33f1:1003` | MJPEG 960×540@20 | 16:9 | VISCA `/dev/ttyS0`, 9600 8N1 |
| Sony EVI-D100 + MS210x | `534d:0021` | MJPEG 720×480@25 | 4:3 | VISCA `/dev/ttyS0`, 9600 8N1 |

工作模式：

```text
自动识别 / 手动选择 Profile
        ↓
枚举 /dev/videoN + 查询 V4L2 能力
        ↓
读取 sysfs USB VID/PID
        ↓
验证 Profile 目标格式与帧率
        ↓
生成统一 PtzResolvedDevice
        ├─ PtzCaptureWorker：动态视频节点、格式、分辨率和帧率
        ├─ PtzControlWorker：Profile 对应串口配置
        └─ Qt 预览：按 Profile 的显示比例缩放
```

EVI-D100 的 720×480 模拟视频采用非方形像素，Qt 预览按 4:3 显示；保存的截图仍保留原始 720×480 数据。Profile 选择保存在部署目录 `config/ptz_camera.ini`，同时记录上次成功连接的相机类型。

### PTZ 运行时热切换

Qt 主线程以 1 秒周期重新枚举 `/dev/videoN`，只在设备集合或服务状态变化时执行重配置：

- 自动模式下，当前设备仍在线时保持当前源；拔出后释放旧 V4L2 fd、MMAP 和 JPEG 帧，再选择唯一在线设备或上次成功设备；
- 手动模式下，目标设备未连接时保留仍然有效的当前源，并进入等待；目标接入后自动完成切换；
- 切换前先停止 VISCA 运动与变焦，再停止采集，随后设置新 Profile 并恢复用户原本希望运行的预览/控制服务；
- V4L2 线程检测到 `POLLHUP/POLLERR/ENODEV/EIO` 时上报设备丢失，UI 清除旧帧并触发重新枚举；
- “启动预览/连接控制”分别保存为期望状态，设备暂时不存在时按钮显示“取消等待”，插入后自动恢复；
- 自动模式下两台设备同时在线且当前源仍在线时不会无故跳转；首次启动两台同时在线时优先使用上次成功 Profile，否则要求手动选择。

热切换会经历 USB 枚举、V4L2 建流和 JPEG 解码初始化，允许出现约 1～3 秒无画面状态，但不需要重启 Qt。
