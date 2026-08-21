# 演示稳定性与独立模型页面 v3 上板验收

## 目标

- HDMI 只走 DRM page-flip；失败或超时后安全结束显示会话，不在热路径执行 `drmModeSetCrtc`。
- 左侧只保留八个一级页面按钮，并按可用高度等分。
- 原“闯红灯检测”页改为独立“行人检测”页，只验证 `ped_det.rknn` 的置信度与 NMS 输出。
- “逆行检测”页可单独开启车辆模型；CPU 轨迹与逆行规则由第二个开关控制。
- 页面内容超出 DSI 可用高度时出现滚动条，不能再从窗口下边缘截断。

## NPU 任务互斥

工程仍使用一个 RKNN worker，同一时刻只能运行一个任务：车牌、行人或车辆。切换页面本身不切换模型；只有点击对应的“应用/开启”按钮才切换。任务切换期间相关按钮临时锁定，直到目标模型返回首帧或明确错误。

逆行页的两个开关含义如下：

| 操作 | RKNN | CPU 后处理 | HDMI 叠加 |
|---|---|---|---|
| 开启车辆模型 | 一次车辆检测 | 关闭 | 类别、置信度、车辆框 |
| 逆行判定：开启 | 不重载模型 | 车道跟踪、方向与稳帧 | 车道、轨迹状态、逆行结果 |

## 构建与一键部署

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
./tools/build_arm64.sh 2>&1 | tee /tmp/build_demo_stability_v3.log
file out/arm64-release/rk_dual_screen_video
LC_ALL=C readelf -h out/arm64-release/rk_dual_screen_video | grep Machine
EXPECTED_MARKER=BUILD-CHECK-01 ./tools/deploy_test.sh --keep-releases 5
```

部署脚本会拒绝比源码旧的 ELF，并自动完成打包、SHA256 校验、上传、版本化解压、启动检查和失败回滚。不要在重新构建之前直接部署旧 ELF。

## 上板测试顺序

### 1. 布局

依次进入视频显示、PTZ控制、FPGA配置、CPU配置、车牌识别、行人检测、逆行检测、系统状态。确认八个导航按钮等高，标题和底部状态栏完整；低分辨率下允许页面内部纵向滚动，但任何操作按钮都不得被窗口边缘截断。

### 2. HDMI 稳定性

先关闭全部 NPU 任务，只运行 PCIe/HDMI 10 分钟；再分别运行车牌、行人、车辆模型各 10 分钟。期间每 2 秒切换一次总览与 CH1～CH9，至少 50 次。记录黑屏、闪烁、程序退出和日志中的 page-flip 错误。

若 page flip 失败，预期行为是当前显示会话停止并在 Qt 显示明确错误；不应连续闪烁或继续提交 framebuffer。用户点击重新启动 PCIe 视频后应创建干净的新 DRM 会话。

### 3. 行人模型页

点击“开启行人模型”，确认日志加载 `ped_det.rknn`，界面显示行人数量、置信度、框尺寸和推理耗时。画面中横向、蹲姿或局部行人不应再被旧站立姿态规则提前过滤。改变置信度/NMS后重新点击开启，使新参数生效。

### 4. 逆行页

在没有车道配置时点击“开启车辆模型”，必须能看到车辆框、类别和置信度，且有效轨迹与逆行规则不运行。完成车道和方向配置后开启“逆行判定”，确认不重新加载 RKNN 模型，仅开始 CPU 规则。关闭判定后车辆框继续输出；关闭车辆模型后 NPU 停止。

清除全部车道并保存，然后切换输入源再切回，旧车道不能恢复。

### 5. 快速切换与崩溃回归

按“车牌应用 → 行人开启 → 车辆开启 → 车牌应用”的顺序循环 30 次。每次等待界面解除“切换中”再进行下一步；不得在模型释放/装载期间重复点击。记录进程 PID、`dmesg`、Qt 日志和 RKNN 错误。

## 日志采集

```bash
CURRENT="$(head -n 1 /userdata/Apps/RK3568_PTZ_Qt_test_current.path)"
PID="$(pidof rk_dual_screen_video)"
echo "CURRENT=$CURRENT PID=$PID"
tail -n 200 "$CURRENT/logs/rk_dual_screen_video.log"
dmesg | tail -n 200
```

发现退出时额外记录：

```bash
ulimit -c unlimited
find /userdata /tmp -maxdepth 3 -type f -name 'core*' -ls 2>/dev/null
```

## 通过标准

- 空载与三个独立模型阶段均无持续闪烁、无崩溃；快速切换 30 轮 PID 不变。
- 八个导航按钮等高，八页底部功能均可到达。
- 行人页不运行交通灯/斑马线/违法规则。
- 车辆模型可脱离车道配置独立运行；逆行规则开关不触发 RKNN 重载。
- 总览与 CH 配置隔离，清空配置可以持久化。
