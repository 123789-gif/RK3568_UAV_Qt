# 第一阶段重构变更摘要

- 仅进行目录、构建、模型路径和部署工程化整理；未改动推理算法、UI 行为、显示数据通路、PTZ 协议和 OSD 内容。
- 删除未被工程引用的空占位文件 `fpga_control.cpp/.h`。
- 删除 `build/`、`build_manual/`、`.pro.user`、历史 `.pro` 备份和运行日志。
- 模型查找新增可执行文件相对路径，仍保留旧板端路径作为兼容回退。
- 首版运行继续使用 root，正式部署目录为 `/userdata/Apps/RK3568_PTZ_Qt`，暂不开机自启。

# 控制窗口职责拆分（2026-07-25）

- 将`ControlPanelWindow`的72个成员函数拆分为窗口核心、视频、FPGA、AI、
  违法规则、PTZ、系统状态和样式8个实现文件；
- 公共类声明和Qt信号槽接口保持不变；
- 删除未承载实际控件的空`control_panel_window.ui`及对应生成类依赖；
- 全部成员函数实现与拆分前逐函数比对一致，仅移除上述空UI对象的创建和释放；
- 未修改PCIe接收、RGA/DRM显示热路径、NPU调度/后处理、模型文件、
  PTZ工作线程、驱动和部署脚本。

# 第二阶段：PCIe 用户态模块剥离

- 新增 `src/pcie/pango_pci_uapi.h`，集中保存当前驱动 ioctl 和结构体 ABI；字段类型、顺序和 ioctl 编号保持不变。
- 新增 `src/pcie/pcie_frame_receiver.h/.cpp`，封装驱动打开、链路信息读取、DMA 四缓冲配置与 mmap、BAR0 PIO、阻塞等帧和资源释放。
- `DrmRgaDisplay` 不再直接持有 PCIe fd、DMA mmap 指针和 BAR0 资源，只消费 `FrameView` 并继续完成 NPU 提交、RGA 与 DRM 显示。
- 线程模型保持不变：PCIe 接收仍在原显示工作线程中同步执行。
- 数据搬运保持不变：RGA 直接读取驱动 mmap 缓冲，不新增整帧复制或 IPC。
- NPU、PTZ、OSD、显示时序、驱动 ABI 和部署脚本行为均未修改。

# 第三阶段：AI推理页面与单任务调度首版

- 新增 `src/npu/npu_task_config.h`，统一管理启用状态、任务类型、目标FPS、置信度、NMS阈值和结果历史深度。
- 推理模式改为单任务：行人检测仅执行 `ped_det`；车牌识别按 `plate_det → plate_reco` 级联执行。
- 任务切换由推理线程完成旧模型释放和新模型加载，不重启Qt，不修改PCIe主视频输入。
- 置信度和NMS阈值按一次完整配置快照传入推理线程，避免多个setter形成配置中间态。
- 新增64帧有界结果历史；Qt以5 Hz轮询批量结果，避免高频跨线程信号和结果遗漏。
- AI页面增加任务选择、目标FPS、置信度、NMS、运行状态和最近50条有效识别结果表。
- AI配置保存到部署目录 `config/ai_panel.ini`；输入源固定为PCIe主视频。
- HDMI OSD显示当前任务缩写；PCIe、RGA、DRM、PTZ和部署脚本未修改。
- 车牌检测接口当前未提供检测分数，结果表明确显示 `--`，不伪造置信度。


## 视频选路与单路 NPU 输入

- 新增 `video_route_config.h`，固定标准 3×3、640×360 的 CH1～CH9 ROI 映射；
- 视频显示页新增总览和 CH1～CH9 触控选路；
- 单路模式下 HDMI 和 NPU 绑定同一通道；
- 总览模式暂停 NPU，不再把完整九宫格送入模型；
- NPU 输入搬运由约 4.15 MB/推理帧降为约 0.46 MB/推理帧；
- 增加 source revision，防止切换通道后旧检测结果叠加到新画面。

# 第五阶段：PTZ 双相机 Profile 与动态 V4L2 选路

- 新增 `ptz_camera_profile.h/.cpp`，集中保存当前 HD USB PTZ 和 Sony EVI-D100 + MS210x 的 USB、视频、显示比例及 VISCA 配置；
- 新增 `v4l2_device_detector.h/.cpp`，枚举 `/dev/videoN`、过滤非采集节点、读取 sysfs USB VID/PID，并验证目标格式能力；
- `PtzCaptureWorker` 不再写死 `/dev/video0`、960×540@20，改为消费已解析的 `PtzResolvedDevice`；
- `PtzControlWorker` 不再内部写死串口路径，改为从相机 Profile 获取 `/dev/ttyS0` 和 VISCA 配置；
- PTZ 页面新增“自动识别 / 当前 HD USB PTZ / Sony EVI-D100 + MS210x”选择及应用按钮；
- 配置切换先解析新设备，再停止并重启原本处于运行状态的 PTZ 服务；解析失败时保留现有运行链路；
- EVI-D100 预览按 4:3 显示，HD USB PTZ 按 16:9 显示；
- PTZ Profile 选择保存到 `config/ptz_camera.ini`；
- 部署运行检查由固定 `/dev/video0` 改为接受任意 `/dev/videoN` 候选节点；
- PCIe、九宫格选路、NPU、RGA/DRM、FPGA 协议和模型均未修改。


# 第六阶段：PTZ Profile 修正与运行时热切换

- 修正 HD USB PTZ 的实测 USB ID：`33f1:0003` 改为 `33f1:1003`；保留已验证的 MJPEG 960×540@20、16:9 配置；
- 增加 1 秒周期 PTZ 设备监测，Qt 运行期间可拔出当前视频设备并插入另一套 PTZ，无需重启应用；
- 自动模式保持仍在线的当前源；当前源消失后按上次成功 Profile 或唯一在线设备自动恢复；
- 手动选择目标设备未在线时保留当前有效链路并等待，目标设备出现后自动切换；
- 采集线程增加 V4L2 设备丢失检测，覆盖 `POLLERR/POLLHUP/POLLNVAL`、节点消失及 `ENODEV/EIO/EPIPE`；
- 切换流程固定为 VISCA 停止 → 采集停止 → STREAMOFF/munmap/close → 新设备解析与配置 → 预览/控制按用户期望状态恢复；
- 切换和设备断开时清空最新帧与截图源，避免旧相机画面残留；
- PTZ 配置文件新增 `last_successful_profile`，用于自动模式下两台设备同时在线时保持确定性；
- `check_runtime.sh` 在没有 `/dev/videoN` 时改为警告而非阻止启动，使应用可先启动再等待相机热插入；
- 未修改 PCIe、FPGA、九宫格选路、NPU、RGA/DRM 主显示链路。

# 第七阶段：OSD 排版与 NPU 吞吐上限优化

- 系统状态 OSD 不再与 AI 状态 OSD 分别使用独立硬编码纵坐标；系统状态块返回下一可用 Y 坐标，AI 状态从该位置继续绘制；
- 保留原有 PCIe/RGA/DRM 状态、NPU 状态、检测框和目标标签，仅修正左上角文字重叠，不改变检测框坐标映射；
- 新增统一的 `NPU_TARGET_FPS_MIN/DEFAULT/MAX` 常量，目标推理帧率范围调整为 1～30 FPS，默认值调整为 15 FPS；
- Qt 控件、配置钳位、INI 读取和结果默认值统一使用相同帧率常量，避免界面值与后端实际值不一致；
- 保留单槽 latest-only 调度；NPU 忙时新帧覆盖旧等待帧，不引入任务队列和延迟堆积；
- 未修改 PCIe、PTZ、模型文件、推理后处理、RGA 裁剪和部署脚本。

# 第八阶段：NPU 绝对截止时间调度修正

- 将目标推理帧率节流从“整数毫秒间隔 + 以上次实际提交时刻重新计时”改为纳秒级绝对截止时间调度；
- 使用 `nextSubmitNs` 保留理想时间轴相位，源帧率与目标帧率不可整除时允许相邻源帧间隔交替，长期平均值接近目标帧率；
- `wantsFrame()` 仅在 RGA 裁剪前做低成本到期预检查，`submit()` 在来源、尺寸和数据长度验证通过后原子占用时间槽；
- 增加 1 ms 截止时间容差，避免 60 Hz 帧边界抖动导致 15/20/25/30 FPS 分别退化到较低离散档位；
- 长时间无源帧或处理停顿后跳过已错过的截止点，不突发补交历史任务；
- 配置、任务、启停或输入通道变化时重置调度时间轴；保留现有 latest-only 单槽覆盖机制；
- 未修改模型、预处理、后处理、PCIe、RGA、DRM、OSD、PTZ和部署脚本。

# 第九阶段：NPU 60 FPS 性能探测与 AI 参数布局优化

- 将统一目标推理帧率上限从 30 FPS 扩展到 60 FPS，最小值仍为 1 FPS，默认值仍为 15 FPS；
- Qt 控件范围、配置钳位和 INI 读取继续共用 `NPU_TARGET_FPS_MIN/DEFAULT/MAX`，避免界面与后端范围不一致；
- “目标推理帧率”标签不再附带范围说明，范围改由 SpinBox 约束和工具提示表达；
- AI 参数网格固定紧凑标签列，三个数值框统一扩大并允许横向伸展，`− / +` 按钮和间距适度收紧，解决 DSI 页面数值显示不完整；
- 60 FPS 仅作为吞吐上限测试范围，默认配置不变；未修改绝对截止时间调度、latest-only 邮箱、模型、PCIe、RGA、DRM、OSD 和 PTZ。

# 第十阶段：九宫格总览全图 AI 与界面标题更新

- Qt 主窗口标题和页面顶部标题统一改为“智能交通感知系统控制台”；
- 九宫格总览模式不再强制暂停 NPU，AI 开启时支持总览全图推理；
- CH1～CH9 单路模式仍保持原有 640×360 ROI 裁剪和全屏显示路径；
- `NpuInferenceWorker` 允许 sourceChannel=0 表示“总览全图”这一有效输入源，保留 source revision 过滤旧结果；
- HDMI OSD 支持总览模式和单路模式下的检测框绘制；
- Qt AI 页面和底部状态栏不再把总览显示为暂停，改为“总览全图 / ALL”；
- 未修改 NPU 模型、后处理算法、目标 FPS 调度、PCIe 协议、PTZ 和部署脚本。

# 第十一阶段：九宫格总览 AI 轻量输入与显示优先修复

## 修改目标

上一阶段的总览全图 AI 直接将完整 1920×1080 RGB565 帧同步提交给 NPU worker。该路径会在显示线程中产生大块内存复制和预处理压力，九宫格 AI 低帧率运行时也可能拖低 RX/RGA/FLIP 统计并引入偶发撕裂。

## 修改内容

- 九宫格总览仍支持 AI 推理，但不再把完整 1920×1080 帧提交给 NPU；
- 总览模式下先使用 RGA 将完整九宫格缩小为 640×360 RGB565 轻量输入，再提交给 NPU worker；
- CH1～CH9 单路模式继续沿用原有 640×360 ROI 裁剪路径；
- 总览和单路模式的检测框均按 640×360 NPU 输入坐标映射回 HDMI 输出；
- 该修改不改变 NPU 调度器、模型、PCIe 协议、DRM 输出时序和 PTZ 逻辑。

## 预期效果

- 九宫格总览 + NPU 低帧率推理时，RX/RGA/FLIP 不应再跟随 AI FPS 明显下降；
- HDMI 九宫格显示应恢复“显示优先”，不应再因完整 1080P AI 输入复制而出现明显背压或偶发撕裂；
- 总览 AI 的小目标效果可能弱于单路 ROI 推理，这是 640×360 全局缩小输入的预期代价。

### 本轮显示优先与车牌OSD修正

- 1080P 全图/九宫格总览 AI 仍通过 RGA 缩小为 640×360 后送入 NPU，适用于 PCIe 输入为单路 1080P 高清画面的测试。
- NPU 输入准备调整为 DRM flip 之后执行，当前 HDMI 帧优先提交，降低 AI 输入缩放对显示关键路径的影响。
- 补全 RGB565 OSD 5×7 字库中的 B/H/J/O/Q/V/W/Y/Z，修复车牌识别拼音标签中部分字母显示成占位符或错误形状的问题。


# 第十二阶段：车牌 OSD 颜色、中文格式和稳定显示

- 车牌检测框颜色改为按车牌类型显示：蓝牌、绿牌、黄牌、警牌和领馆牌分别使用对应颜色风格；未知类型使用安全 fallback。
- 车牌识别字符不再使用拼音标签直接绘制，显示层优先使用识别输出的中文车牌文本，并将 fallback 拼音省份映射为中文简称。
- 车牌 OSD 统一格式化为“浙A 14FH3”形式，省份简称与城市字母紧邻，后续号码与前缀之间增加空格。
- 车牌字符结果改为绘制在车牌框上方，字号放大，并使用 Qt 字体渲染以支持中文省份简称。
- 增加最多 4 个车牌 OSD 稳定槽，使用 IoU/中心距离匹配、坐标 EMA 平滑、连续 3 次确认后更新稳定字符，减少识别字符抖动和视觉闪烁。
- 车牌稳定槽在 source channel、source revision、任务变化或超时后清理，避免通道切换后旧标签残留。
- 未修改模型文件、车牌后处理算法、PCIe协议、RGA/DRM输出时序、PTZ逻辑和部署脚本。

# 第十三阶段：性能分阶段打点与车牌 OSD 缓存优化

- 新增 `src/system/system_monitor.*`，将 NPU load 与 SoC 温度读取从 `DrmRgaDisplay` 中拆出，作为后续系统性能监测模块的统一入口。
- 新增 `src/system/perf_counters.*`，提供轻量平均耗时计数器，避免显示线程中继续散落临时统计变量。
- HDMI 左上角 OSD 新增 `OSD/PL/NP` 微秒级平均耗时：分别用于观察整体 OSD 绘制、车牌稳定标签绘制和 NPU 输入准备耗时。
- AI 页面新增拷贝、RKNN input/run/output/post 与 run 次数显示，便于区分帧复制、预处理、纯 NPU 运行、输出获取和后处理瓶颈。
- 车牌中文稳定标签不再每帧重新通过 Qt 字体渲染；稳定文本、车牌类型或输出宽度变化时才重建 QImage 缓存，普通帧只做 RGB565 blit。
- 保留现有连续确认、位置平滑、中文省份格式和按车牌类型着色逻辑，不修改模型、后处理算法、PCIe、PTZ 和部署脚本。

## perf_hud_split_plate_osd_opt_v1

- HDMI 左上角性能 HUD 与 AI 结果 OSD 语义分离：性能 HUD 默认保留，`AI结果OSD` 开关只控制检测框、车牌稳定标签等业务叠加。
- HDMI 性能 HUD 收敛为三类摘要：`SYS`、`FPS`、`LAT`，用于现场快速判断 CPU/NPU/温度、PCIe/AI/显示帧率和 RGA/NP/OSD/PL 耗时。
- 车牌稳定标签缓存从 ARGB QImage 每帧 alpha 混合，改为预渲染不透明 RGB565 标签块并按行 memcpy/blit，降低多车牌时 PL 线性增长开销。
- 系统监测增加 `/proc/stat` CPU 使用率采样，并继续保留 NPU load 和 SoC 温度读取。
- Qt“系统状态/详细状态”增加性能 HUD、AI结果OSD、OSD/PL/NP 耗时和 CPU load 信息，便于区分显示叠加、NPU输入准备和 RKNN 本体耗时。

## perf_hud_readability_v1

- 新增 HUD 专用 8x12 ASCII 点阵绘制路径，补齐 `/` 等性能指标必要符号。
- HDMI 左上角 HUD 改为深色信息卡片、左侧青色状态条和 SYS/FPS/LAT 三行固定布局。
- HUD 数字、指标名和分组标签采用不同颜色层级，异常状态使用黄色/红色提示。
- HUD 每 250ms 更新 RGB565 缓存图层，显示帧只执行 blit，保持较低 OSD 开销。

### Perf HUD Polish v2

- HDMI 左上角性能 HUD 改为更紧凑的三行网格布局，缩小背景卡片、内边距和左侧状态条。
- 指标统一采用 `KEY: VALUE` 形式，CPU/RX/RGA 等首列字段、冒号和数值列固定坐标对齐。
- FPS 行右侧统一显示 `(FPS)`，LAT 行右侧统一显示 `(MS)`，减少每个字段重复单位造成的视觉噪声。
- HUD 继续使用轻量字库缓存路径，但原生 8x12 绘制改为软边缘像素混合，降低 `/`、`%`、`:`、数字等字符的锯齿感。
- 继续保持 250 ms HUD 缓存刷新和每帧 RGB565 blit，避免重新引入明显 OSD 开销。

### Perf HUD Layout Trim v3

- HDMI 左上角默认性能 HUD 隐藏 `PL`，保留 `RGA/NP/OSD` 三个 LAT 行指标。
- `SYS/FPS/LAT` 三行改为严格三列对齐：第一列 `CPU/RX/RGA`，第二列 `NPU/AI/NP`，第三列 `TMP/DSP/OSD`。
- HUD 背景卡片宽度缩小，`PL` 继续保留在内部 `OsdStats::plateOsdAvgUs` 统计中，便于后续放到系统详细状态或调试 HUD。

### Perf HUD Compact RX v4

- HUD 字体从 8x12 原生显示提升到约 12x18 的轻量软边缘显示，增强 HDMI 现场可读性。
- FPS 行去掉末尾 `(FPS)`，LAT 行改为 `LAT MS`，HUD 横向布局更紧凑。
- `TMP` 温度值增加 `C` 前的空格，例如 `62.0 C`。
- 保留 `RX` 标签，不改动 PCIe 驱动；后续驱动提供 producer FPS 后可切换数据源。

### Perf HUD Grid Balance v5

- HDMI 左上角 HUD 进行小范围回调：去掉 `LAT MS` 中的 `MS`，LAT 行默认以毫秒为隐含单位，避免左侧行标题过长。
- FPS 行的 `AI` 字段只显示实时推理帧率，不再显示目标帧率；目标帧率仍保留在 Qt 的 AI 推理页面。
- 字体保持上一版约 12x18 的 HUD 尺寸，但三行行距扩大约 1.2 倍，改善字体放大后的拥挤感。
- 三列横向坐标重新收紧，继续保持 `CPU/RX/RGA`、`NPU/AI/NP`、`TMP/DSP/OSD` 纵向对齐。


## HUD / Plate OSD Fix v6
- Fixed unequal visual row blocks in the HDMI HUD by using explicit equal row geometry.
- Reduced stale multi-plate label persistence after scene switches by expiring unmatched stable slots quickly.
- Added per-character suffix coloring for police and consulate plate labels.

## Traffic Violation Vehicle Detect v1

- Added `yolov8_car.rknn` as a fourth RKNN model for vehicle detection.
- Added AI task `TrafficViolation` / “违法检测（行人+车辆）”. In this first stage it runs pedestrian detection and vehicle detection on the same preprocessed frame, then publishes both result sets.
- Added vehicle result vectors and OSD drawing for `car / bus / van / truck` boxes.
- Kept PCIe, RGA/DRM display, PTZ control, plate stable OSD and driver logic unchanged.
- This stage does not yet enable crosswalk ROI, traffic-light state, or violation-rule judgment; those are reserved for a later rules patch.
