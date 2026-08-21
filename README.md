# RK3568_PTZ_Qt

RK3568双屏Qt应用，集成FPGA PCIe视频接收、RGA/DRM HDMI显示、FPGA预处理与网口输出控制、PTZ采集与VISCA控制、RKNN推理以及板级风扇管理。

## 当前工程状态

- Qt：5.12.9 ARM64交叉编译；
- 目标平台：RK3568，Linux 6.1.99，AArch64；
- 主程序：`rk_dual_screen_video`；
- 标准构建目录：`out/arm64-release`；
- 标准测试部署入口：`./tools/deploy_test.sh`；
- 板端正式目录：`/userdata/Apps/RK3568_PTZ_Qt`；
- 板端测试目录：`/userdata/Apps/RK3568_PTZ_Qt_test_<UTC时间戳>`；
- 测试部署默认保留最新3版，并保护当前运行版本。

当前窗口标题包含 `BUILD-CHECK-01`，用于确认修改后的ELF确实完成了重新构建和上板替换。完成基线确认后，应改为自动生成的构建版本信息，不再手工修改标题。

## 当前功能边界

### 视频与显示

- FPGA经PCIe DMA向RK3568提供RGB565视频帧；
- `PcieFrameReceiver`负责设备、DMA缓冲和FPGA控制接口；
- `DrmRgaDisplay`负责RGA处理、DRM/KMS显示、OSD和NPU帧提交；
- 支持九宫格总览及CH1～CH9软件选路；
- FPGA预处理与网口视频输出可由Qt界面控制。

### NPU任务

当前实际代码包含四个互斥任务：

1. 行人检测：`ped_det.rknn`；
2. 车牌级联：`plate_det.rknn → plate_reco.rknn`；
3. 行人违法规则：复用`ped_det.rknn`，结合交通灯ROI、斑马线ROI和行人轨迹进行规则判断；
4. 车辆逆行：`yolov8_car_640x320_mmse.rknn`完成一次车辆检测，CPU完成车道跟踪和方向判定，支持总览及单路输入。

车牌页面提供Legacy/v2.9.1与检测图ROI/原图ROI的四种组合；逐帧/稳帧仍作为独立结果策略。

### PTZ与系统

- 自动枚举UVC采集节点并支持双相机Profile；
- VISCA串口控制；
- PTZ热插拔恢复；
- 截图保存；
- 风扇PWM和系统状态监测。

## 目录结构

```text
src/            C++/Qt业务源码
assets/         模型、默认配置和已验证PCIe驱动
deploy/         发布包生成、板端检查、启停和显示配置
tools/          构建、测试部署和驱动同步工具
docs/           构建、部署、架构和故障排查文档
out/            本地构建产物，不进入Git
dist/           本地发布包，不进入Git
```

`.backup_*`、`out/`、`dist/`和`*.pro.user`均不属于源码，不应进入源码压缩包。

`src/ui/control_panel_window_*.cpp`按视频、FPGA、AI、违法规则、PTZ、系统状态和样式拆分；
`control_panel_window_internal.h`仅供这些实现文件共享私有辅助函数，不属于模块公共接口。

## 标准流程

Qt Creator重新构建后：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
./tools/deploy_test.sh
```

命令行全量构建：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
./tools/build_arm64.sh
```

首次配置SSH密钥以及完整部署说明见：

- [构建说明](docs/BUILD.md)
- [部署说明](docs/DEPLOY.md)
- [开发流程](docs/DEVELOPMENT_WORKFLOW.md)
- [架构说明](docs/ARCHITECTURE.md)
- [故障排查](docs/TROUBLESHOOTING.md)
- [清理版交接说明](docs/CLEANUP_HANDOVER.md)
- [控制窗口拆分说明](docs/UI_REFACTOR.md)

## 当前已知工程债务

- `ControlPanelWindow`成员实现已按职责拆分，但共享私有辅助函数仍集中在
  `control_panel_window_internal.h`；后续只应在板端回归后继续缩小该私有头；
- `DrmRgaDisplay`和`NpuInferenceWorker`文件仍较大，后续应分阶段拆分；
- 运行配置仍写在版本化发布目录中，版本切换时不能自动继承；
- 总览车辆检测受单次640x320模型输入限制，小目标召回率低于单路模式；
- `QMAKE_LFLAGS`使用`--allow-shlib-undefined`，每次发布必须依赖板端`ldd`和运行检查兜底。

本轮控制窗口拆分必须先完成Qt Creator全量重新构建和板端回归，再提交为新基线；
在此之前不要同时迁移构建系统或修改PCIe、RGA/DRM、NPU和PTZ工作线程。
