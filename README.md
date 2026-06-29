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
- `DrmRgaDisplay` 仍包含 PCIe、RGA、DRM、OSD 和 NPU 帧提交；
- HDMI OSD 文字重叠尚未重构；
- PCIe 驱动尚未随发布包自动加载。

后续功能修改必须从独立 `feature/*` 分支开始，并完成板端回归后再合并到 `main`。
