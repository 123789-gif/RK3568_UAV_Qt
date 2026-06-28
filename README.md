# RK3568_PTZ_Qt

RK3568 双屏 Qt 控制、FPGA PCIe 视频显示、PTZ 采集与 VISCA 控制、RKNN 推理集成工程。

本版本完成第一阶段重构：

- 源码按 `ui/video/npu/ptz/system/app` 模块归档；
- 删除源码包内的构建产物、旧 Makefile、`.pro.user` 和日志；
- 三个已验证 RKNN 模型纳入 `assets/models` 并记录 SHA256；
- 构建完成后自动把模型复制到构建输出的 `models/`；
- 提供 `/userdata/Apps/RK3568_PTZ_Qt` 单命令部署脚本；
- 保持现有推理任务、UI、PCIe/DRM/RGA、PTZ、OSD 行为不变。

## 目录

```text
src/app       程序入口
src/ui        Qt 控制界面
src/video     PCIe/DRM/RGA 显示与 OSD（当前仍在同一模块，后续再解耦）
src/npu       RKNN 模型加载、推理 worker 和后处理
src/ptz       UVC 采集、VISCA 控制和串口
src/system    风扇及板级管理
assets/models 已验证 RKNN 模型
assets/config 部署显示参数
deploy        运行脚本、部署打包和可选 systemd 模板
docs          构建、部署、架构说明
tools         ARM64 构建脚本
```

详细操作见 `docs/BUILD.md` 和 `docs/DEPLOY.md`。
