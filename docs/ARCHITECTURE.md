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
- AI单任务选择、1～60 FPS目标帧率、置信度和NMS阈值配置；
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
- OSD：系统状态块计算下一可用 Y 坐标，AI 状态块顺序排版，检测框单独绘制；
- NPU 帧提交；
- 状态统计。

### `src/npu`

- `NpuTaskConfig` 统一保存启用状态、任务类型、目标FPS、置信度和NMS阈值；目标帧率范围为 1～60 FPS，默认 15 FPS；
- 纳秒级绝对截止时间调度，目标帧率不再因源帧离散边界退化到较低档位；
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
3. 九宫格总览模式下 NPU 输入改为 1920×1080 全图经 RGA 缩小后的 640×360 轻量帧，不再同步复制完整 1080P 帧；
4. DRM/RGA、OSD 和 NPU 帧提交仍集中在 `DrmRgaDisplay`；
5. PCIe 驱动由部署脚本条件自动加载，用户态接收逻辑已剥离到 `src/pcie`。

## 5. 推荐后续顺序

```text
已完成：部署基线与PCIe驱动条件自动加载
已完成：PcieFrameReceiver 与 DrmRgaDisplay 解耦
已完成：NpuTaskConfig、单任务调度、结果快照和AI页面首版
下一步：板端性能与任务切换回归
已完成：系统 OSD 与 AI 状态 OSD 使用共享纵向布局，消除左上角重叠
后续：按实测瓶颈评估ROI、低分辨率邮箱或多任务调度
```

## 6. 当前 NPU 数据流

```text
PCIe RGB565 DMA帧
        ↓
按 1～60 FPS 纳秒级绝对截止时间抽帧并复制到单槽最新帧邮箱
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

推理线程仍为单线程。帧提交使用绝对截止时间保持长期平均目标帧率；若实际处理耗时超过目标周期，latest-only邮箱仍以新帧覆盖旧等待帧，避免延迟堆积。任务切换不重启Qt、不改变PCIe/RGA/DRM链路，也不新增整帧复制。

AI 参数区使用紧凑标签列与统一宽度的触控数值框；帧率范围通过控件约束和提示信息表达，不再占用行标签宽度。60 FPS 是性能测试上限，默认值仍保持 15 FPS。


## 九宫格软件选路与全图/单路推理

PCIe 输入仍保持固定的 1920×1080 RGB565 九宫格帧，不修改 FPGA、PCIe 协议或驱动 ABI。视频显示页提供“九宫格总览”和 CH1～CH9 十种路由：

- 总览模式直接显示完整 1920×1080 九宫格，并在 NPU 开启时将全图缩小为 640×360 后用于推理；
- 单路模式按照标准 3×3 映射裁出对应 640×360 ROI，RGA 放大到 HDMI 全屏，并仅将该 ROI 送入 NPU；
- NPU 在目标帧率到期时按当前路由提交输入：总览为 1920×1080 全图缩小后的 640×360 轻量图，CH1～CH9 为 640×360 单路 ROI；
- 通道切换使用 source revision 丢弃旧通道在途结果，清空旧 OSD 与结果历史；
- 检测框按实际 NPU 输入尺寸映射回 HDMI 输出坐标：总览和单路均以 640×360 NPU 输入坐标映射到当前 HDMI 输出区域。

程序启动默认进入九宫格总览。若 AI 开关开启，则总览模式会执行缩小后的全图推理，便于在保持显示优先的前提下评估全局低成本感知效果；完整 1080P 帧不再同步复制进 NPU 任务，避免显示链路被 AI 输入准备反压。

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

### 本轮显示优先与车牌OSD修正

- 1080P 全图/九宫格总览 AI 仍通过 RGA 缩小为 640×360 后送入 NPU，适用于 PCIe 输入为单路 1080P 高清画面的测试。
- NPU 输入准备调整为 DRM flip 之后执行，当前 HDMI 帧优先提交，降低 AI 输入缩放对显示关键路径的影响。
- 补全 RGB565 OSD 5×7 字库中的 B/H/J/O/Q/V/W/Y/Z，修复车牌识别拼音标签中部分字母显示成占位符或错误形状的问题。

## 车牌 OSD 稳定显示

车牌识别结果在显示层分为检测框和字符显示两部分。检测框按当前检测结果实时绘制，并根据车牌颜色类别选择蓝、绿、黄、白或黑色风格；字符显示使用稳定缓存，先将模型输出的中文车牌或拼音标签统一格式化为“浙A 14FH3”形式，再经过连续确认后更新到 HDMI OSD。

字符标签不再跟随每一帧检测框坐标直接跳动，而是使用平滑后的车牌框中心作为锚点，绘制在车牌框上方。标签采用 Qt 字体渲染到 RGB565 framebuffer，支持中文省份简称、英文字母和数字混合显示；缓存超时、任务切换、通道切换或 source revision 变化时会自动清除旧标签。

## 性能监测与阶段打点

系统性能监测相关代码逐步收敛到 `src/system`。当前新增 `SystemMonitor` 用于读取 NPU load 与 SoC 温度，`AverageCounter` 用于显示线程内的轻量平均耗时统计。显示主循环左上角 OSD 中的 `OSD/PL/NP` 分别表示整体 OSD 绘制、车牌稳定标签绘制和 NPU 输入准备的平均耗时，AI 页面中的 `copy/pre/RKNN input/run/output/post` 用于区分帧复制、预处理、NPU运行、输出获取和后处理开销。

车牌稳定标签采用缓存式渲染：只有稳定车牌字符串、车牌类型或输出宽度变化时才通过 Qt 字体生成 ARGB 标签图，普通显示帧只执行 RGB565 blit。该结构用于降低多车牌场景下中文 OSD 对 RX/RGA/FLIP 和 AI 实际帧率的影响。

### 性能 HUD 与 AI 结果 OSD 分层

HDMI 输出叠加分为两层：性能 HUD 和 AI 结果 OSD。性能 HUD 用于现场调试，默认显示 SYS/FPS/LAT 三类轻量 ASCII 信息；AI 结果 OSD 只负责检测框、车牌稳定大字标签和测试框叠加。这样关闭 AI 结果 OSD 时仍能观察 RX、AI、DISP、OSD、NP 等关键指标；PL/车牌标签耗时保留在详细统计中，便于定位车牌文字绘制、NPU 输入准备和显示链路之间的瓶颈。

车牌稳定标签采用两级缓存：字符内容、车牌类型或输出宽度变化时才使用 Qt 字体重新渲染；稳定状态下保存为 RGB565 不透明块，每帧只进行行级拷贝，避免多车牌场景下反复执行 ARGB alpha 混合。

### HDMI Perf HUD 可读性优化

HDMI 左上角性能 HUD 采用独立轻量渲染路径，不依赖 Qt 字体。HUD 被组织为 `SYS / FPS / LAT` 三行摘要，使用深色背景卡片、青色分组标签、浅灰指标名和白色实时数值；异常温度、负载、帧率或延迟会以黄色/红色提示。HUD 先渲染到 RGB565 缓存块，再由显示线程按行 blit 到 framebuffer。

### Perf HUD Polish v2

- HDMI 左上角性能 HUD 改为更紧凑的三行网格布局，缩小背景卡片、内边距和左侧状态条。
- 指标统一采用 `KEY: VALUE` 形式，CPU/RX/RGA 等首列字段、冒号和数值列固定坐标对齐。
- FPS 行右侧统一显示 `(FPS)`，LAT 行右侧统一显示 `(MS)`，减少每个字段重复单位造成的视觉噪声。
- HUD 继续使用轻量字库缓存路径，但原生 8x12 绘制改为软边缘像素混合，降低 `/`、`%`、`:`、数字等字符的锯齿感。
- 继续保持 250 ms HUD 缓存刷新和每帧 RGB565 blit，避免重新引入明显 OSD 开销。

### Perf HUD Layout Trim v3

默认 HDMI 性能 HUD 不再直接显示 `PL`，而是固定为三行三列：`CPU/NPU/TMP`、`RX/AI/DSP`、`RGA/NP/OSD`。这样 `TMP/DSP/OSD` 第三列可严格纵向对齐，`PL` 仍作为车牌稳定标签绘制耗时保留在内部性能统计和详细状态中，用于需要时进一步诊断多车牌 OSD 开销。

### Perf HUD Compact RX v4

HDMI 性能 HUD 在保持 `SYS / FPS / LAT` 三行三列结构的基础上做轻量微调：`FPS` 行不再重复显示 `(FPS)`，`LAT` 行直接以 `LAT MS` 标明延迟单位，`TMP` 温度显示为 `xx.x C`。本轮继续保留 `RX` 命名，表示 PCIe 接收链路帧率语义；由于驱动 producer 统计接口本轮暂不修改，后续可在驱动暴露独立 producer counter 后将 HUD 中 RX 的数据源切换为真正硬件接收帧率。

### Perf HUD Grid Balance v5

- HDMI 左上角 HUD 进行小范围回调：去掉 `LAT MS` 中的 `MS`，LAT 行默认以毫秒为隐含单位，避免左侧行标题过长。
- FPS 行的 `AI` 字段只显示实时推理帧率，不再显示目标帧率；目标帧率仍保留在 Qt 的 AI 推理页面。
- 字体保持上一版约 12x18 的 HUD 尺寸，但三行行距扩大约 1.2 倍，改善字体放大后的拥挤感。
- 三列横向坐标重新收紧，继续保持 `CPU/RX/RGA`、`NPU/AI/NP`、`TMP/DSP/OSD` 纵向对齐。


### HUD and Plate Label Rendering v6
The HDMI performance HUD keeps the compact three-row layout, but row blocks are now equal-height. Plate label rendering remains cached RGB565, with fast stale-slot eviction on unmatched detections and per-suffix red rendering for police/consulate plates.

### Traffic violation task v1

The AI page now exposes a “违法检测（行人+车辆）” task. The first implementation reuses the existing 768x448 letterbox preprocessing path and serially runs the pedestrian detector plus the new four-class vehicle detector (`car`, `bus`, `van`, `truck`). The output is merged into one `NpuFrameResult` as `pedestrians` and `vehicles`, so the AI table and HDMI OSD can verify model loading, coordinate mapping and runtime performance before the rule engine is enabled.

The rule layer is intentionally not enabled in this patch. Crosswalk ROI configuration, red/green-light state, and event definitions such as pedestrian red-light crossing or vehicle occupying a crosswalk should be added after this dual-model detection path is stable on the board.
