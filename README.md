# RK3568_PTZ_Qt

RK3568 双屏 Qt 应用，集成 FPGA PCIe 视频接收与 HDMI 显示、PTZ 视频采集与 VISCA 控制、RKNN NPU 推理、风扇和板级状态管理。

## 当前稳定基线

当前已验证基线标签：

```text
baseline_deploy_v1
```

已经验证：

- Ubuntu 20.04 虚拟机使用 ARM64 Qt Kit 完成交叉编译；
- 生成的程序为 AArch64 ELF；
- 三个 RKNN 模型随工程管理并进行 SHA256 校验；
- 发布包具有版本号、压缩包 SHA256 和完整 `MANIFEST.sha256`；
- 板端 DSI、HDMI、Goodix 触控、PCIe、DRM/RGA、PTZ 和 NPU 主链路可运行；
- `start.sh` 支持前台调试和后台启动；
- 当前 PCIe 驱动仍需在启动 Qt 前人工加载，自动加载尚未实现。

## 目录结构

```text
src/app       程序入口
src/ui        Qt 控制界面
src/video     PCIe、RGA、DRM、OSD 与 NPU 帧提交
src/npu       RKNN 模型加载、推理线程和后处理
src/ptz       UVC 采集、VISCA 控制与串口
src/system    风扇及板级管理
assets/models 已验证的 RKNN 模型及校验文件
assets/config 板端运行参数
deploy        发布、显示配置、运行检查、启停和 systemd 模板
docs          构建、部署、架构、故障排查和验证证据
tools         ARM64 命令行构建脚本
out           本机构建产物，不进入 Git
dist          本地发布包，不进入 Git
```

## 标准入口

- 完整开发流程：[`docs/DEVELOPMENT_WORKFLOW.md`](docs/DEVELOPMENT_WORKFLOW.md)
- 虚拟机构建：[`docs/BUILD.md`](docs/BUILD.md)
- 板端部署：[`docs/DEPLOY.md`](docs/DEPLOY.md)
- 架构与数据流：[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- 故障排查：[`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)

## 当前工程边界

当前版本没有改动以下行为：

- NPU 仍按既有顺序执行车牌检测、车牌识别和行人检测；
- AI 页面仍使用总开关、总 FPS 和总置信度；
- Qt 主窗口标题为“智能交通感知系统控制台”；
- `DrmRgaDisplay` 仍包含 PCIe、RGA、DRM、OSD 和 NPU 帧提交；
- 九宫格总览模式可将完整画面缩小为 640×360 轻量输入送入 AI，CH1～CH9 单路模式仍将对应 640×360 ROI 送入 AI；
- HDMI 左上角系统状态与 AI 状态使用共享纵向布局，检测框仍独立绘制；
- AI 目标推理帧率可设置为 1～60 FPS，默认仍为 15 FPS，并继续使用 latest-only 防积压调度；
- PCIe 驱动尚未随发布包自动加载。

后续功能修改必须从独立 `feature/*` 分支开始，并完成板端回归后再合并到 `main`。

### 本轮显示优先与车牌OSD修正

- 1080P 全图/九宫格总览 AI 仍通过 RGA 缩小为 640×360 后送入 NPU，适用于 PCIe 输入为单路 1080P 高清画面的测试。
- NPU 输入准备调整为 DRM flip 之后执行，当前 HDMI 帧优先提交，降低 AI 输入缩放对显示关键路径的影响。
- 补全 RGB565 OSD 5×7 字库中的 B/H/J/O/Q/V/W/Y/Z，修复车牌识别拼音标签中部分字母显示成占位符或错误形状的问题。

### 本轮车牌 OSD 稳定显示优化

- 车牌识别 OSD 使用真实中文省份简称显示，格式化为“浙A 14FH3”形式，省份汉字与城市字母紧邻，后续号码单独分隔。
- 车牌检测框颜色根据车牌类型显示：蓝牌、绿牌、新能源/黄牌、警牌和领馆牌分别使用对应颜色风格。
- 车牌字符结果不再作为小号框旁标签实时闪烁，而是在车牌框上方以更大的 Qt 字体绘制。
- 车牌字符显示增加连续确认和位置平滑，只有连续多次识别一致才更新稳定 OSD，减少字符抖动和视觉闪烁。

### 本轮性能分阶段打点与车牌OSD缓存优化

- 新增 `src/system/system_monitor.*` 和 `src/system/perf_counters.*`，把 NPU load、温度和轻量耗时计数从显示主循环中拆出，后续系统性能监测模块统一放在 `src/system` 下扩展。
- AI 页面增加 `copy/pre/RKNN input/run/output/post` 等阶段耗时，便于判断瓶颈来自帧复制、预处理、NPU运行、输出获取、后处理还是OSD显示。
- HDMI OSD 增加 `OSD/PL/NP` 平均耗时字段，分别观察整体 OSD 绘制、车牌稳定标签绘制和 NPU 输入准备开销。
- 车牌中文稳定标签改为缓存式渲染：稳定字符串和颜色不变时不再每帧创建 QImage/QPainter，仅把缓存图层叠加到 RGB565 framebuffer，降低多车牌场景下的 OSD 开销。

### perf_hud_split_plate_osd_opt_v1 说明

本版本将 HDMI 左上角性能 HUD 与 AI 结果 OSD 分离。`AI结果OSD 开/关` 只控制检测框和车牌大字标签，性能 HUD 默认保留，便于在关闭业务叠加时继续观察 `SYS / FPS / LAT` 指标。车牌稳定标签改为 RGB565 预渲染缓存，减少多车牌时 PL 绘制耗时。

## HUD Readability v1

本轮将 HDMI 左上角性能 HUD 从普通小号调试文字调整为深色信息卡片：使用 HUD 专用 8x12 ASCII 点阵、左侧青色状态条、SYS/FPS/LAT 三行固定布局，并补齐 `/`、`.`、`%`、`+` 等符号。HUD 文本每 250ms 更新到 RGB565 缓存图层，显示帧仅执行缓存 blit，避免重新引入 OSD 绘制开销。

### Perf HUD Polish v2

- HDMI 左上角性能 HUD 改为更紧凑的三行网格布局，缩小背景卡片、内边距和左侧状态条。
- 指标统一采用 `KEY: VALUE` 形式，CPU/RX/RGA 等首列字段、冒号和数值列固定坐标对齐。
- FPS 行右侧统一显示 `(FPS)`，LAT 行右侧统一显示 `(MS)`，减少每个字段重复单位造成的视觉噪声。
- HUD 继续使用轻量字库缓存路径，但原生 8x12 绘制改为软边缘像素混合，降低 `/`、`%`、`:`、数字等字符的锯齿感。
- 继续保持 250 ms HUD 缓存刷新和每帧 RGB565 blit，避免重新引入明显 OSD 开销。

### Perf HUD Layout Trim v3

- HDMI 左上角默认 HUD 进一步收敛为严格三列：`SYS: CPU/NPU/TMP`、`FPS: RX/AI/DSP`、`LAT: RGA/NP/OSD`。
- 默认不再显示 `PL`，车牌稳定标签耗时仍保留在内部统计与详细状态中，避免破坏 `TMP/DSP/OSD` 第三列对齐。
- HUD 背景卡片宽度同步缩小，现场画面遮挡更少，默认展示更接近正式系统状态栏。

### Perf HUD Compact RX v4

- HDMI 左上角 HUD 继续保留 `RX` 命名，用于表达 PCIe 接收链路帧率；本轮不修改 PCIe 驱动，后续可通过驱动 producer 统计接口把该数值升级为独立硬件接收帧率。
- HUD 字体从原生 8x12 提升为约 12x18 的轻量软边缘字形，改善上一版偏小的问题，同时避免回到 scale=2 的笨重效果。
- 去掉 FPS 行尾的 `(FPS)`，LAT 行改为 `LAT MS`，去掉行尾 `(MS)`，三列内容更紧凑。
- `TMP` 温度值改为 `62.0 C` 形式，数字与 `C` 之间保留小间隔。
- 三列间距收紧，继续保持 `CPU/RX/RGA`、`NPU/AI/NP`、`TMP/DSP/OSD` 纵向对齐。

### Perf HUD Grid Balance v5

- HDMI 左上角 HUD 进行小范围回调：去掉 `LAT MS` 中的 `MS`，LAT 行默认以毫秒为隐含单位，避免左侧行标题过长。
- FPS 行的 `AI` 字段只显示实时推理帧率，不再显示目标帧率；目标帧率仍保留在 Qt 的 AI 推理页面。
- 字体保持上一版约 12x18 的 HUD 尺寸，但三行行距扩大约 1.2 倍，改善字体放大后的拥挤感。
- 三列横向坐标重新收紧，继续保持 `CPU/RX/RGA`、`NPU/AI/NP`、`TMP/DSP/OSD` 纵向对齐。


### HUD / Plate OSD Fix v6
- HDMI HUD rows now use explicit equal-height SYS/FPS/LAT row geometry.
- Stable plate labels now expire after short consecutive misses instead of lingering for about one second after scene changes.
- Police and consulate plate labels use per-character color rendering: police prefixes are black with a red suffix, and consulate labels keep a white prefix with a red suffix.

### Traffic violation vehicle detection v1

This version adds a first-stage “违法检测（行人+车辆）” AI task. It loads the existing pedestrian model and the new `yolov8_car.rknn` model, then displays `person / car / bus / van / truck` detection results in the AI page and HDMI OSD. It is a model-integration baseline only; crosswalk ROI, signal-light state, and violation rule judgment are reserved for the next patch.
