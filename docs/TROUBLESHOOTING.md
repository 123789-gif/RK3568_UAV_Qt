# 故障排查

## 1. 构建找不到 `npu/vision.cpp`

### 现象

```text
No rule to make target 'npu/vision.cpp'
npu/vision.h: No such file or directory
```

### 根因

`.pro` 路径与重构后的文件目录不一致。

### 处理

确认：

```text
src/npu/vision.cpp
src/npu/vision.h
src/npu/demo_types.h
```

更新 `.pro` 后执行 qmake 和全量重构建。

---

## 2. 找不到 RKNN API

### 现象

```text
rknn_api.h: No such file or directory
cannot find -lrknnrt
```

### 检查

```bash
ls -l /home/simba/rk3568_deps/rknn_api/include/rknn_api.h
ls -l /home/simba/rk3568_deps/rknn_api/aarch64/librknnrt.so
```

### 处理

设置正确的 `RK3568_DEPS_ROOT` 或 `RKNN_API_ROOT`，重新 qmake。

---

## 3. 找不到 OpenCV 头文件

### 现象

```text
opencv2/opencv.hpp: No such file or directory
```

### 检查

```bash
ls -l /home/simba/rk3568_deps/opencv/include/opencv4/opencv2/opencv.hpp
```

### 处理

确认 `.pro` 中：

```text
INCLUDEPATH += $$OPENCV_ROOT/include/opencv4
```

---

## 4. OpenCV 链接出现 TBB、BLAS、GLIBC 未定义引用

### 根因

导入的板端 OpenCV 动态库具有传递依赖，虚拟机链接环境与板端 rootfs ABI 不完全相同。

### 当前方案

工程允许共享库内部未解析符号：

```text
-Wl,--allow-shlib-undefined
```

### 风险控制

每次发布必须在板端执行：

```bash
scripts/check_runtime.sh
```

不能只根据虚拟机链接成功判断可运行。

---

## 5. Qt 无法连接 `DISPLAY=:0`

### 现象

```text
qt.qpa.xcb: could not connect to display :0
```

### 检查

```bash
systemctl is-active lightdm
ls -l /tmp/.X11-unix/X0
ls -l /home/linaro/.Xauthority
```

### 处理

使用 `start.sh`。脚本会启动或等待 LightDM，并设置：

```text
DISPLAY=:0
XAUTHORITY=/home/linaro/.Xauthority
QT_QPA_PLATFORM=xcb
```

---

## 6. `runuser` 导致 `free(): invalid pointer`

### 现象

```text
free(): invalid pointer
setup_display.sh: ... Aborted runuser ...
```

### 根因

板端 `runuser` 调用 X11 工具不稳定。

### 处理

当前脚本改用：

```bash
sudo -u linaro env DISPLAY=:0 XAUTHORITY=... <command>
```

并在布局已正确时跳过重复 `xrandr` modeset。

---

## 7. 触摸光标跑到 HDMI

### 根因

Goodix 坐标映射到整个 3200×1080 虚拟桌面，或 `xrandr` 后映射失效。

### 处理

```bash
sudo -u linaro env DISPLAY=:0 XAUTHORITY=/home/linaro/.Xauthority xinput map-to-output "goodix-ts" DSI-1
```

必须在显示布局配置之后执行。

---

## 8. ELF 出现乱码、空 `NEEDED` 或 Verneed 错误

### 现象

```text
unsupported version 65535 of Verneed record
Shared library: []
error while loading shared libraries: <乱码>
```

### 根因

板端运行目录中的 ELF 与虚拟机产物不一致，曾出现旧文件残留或异常覆盖。

### 检查

虚拟机和板端分别执行：

```bash
sha256sum rk_dual_screen_video
readelf -d rk_dual_screen_video | grep NEEDED
```

### 处理

- 使用版本化发布包；
- 上传 `.tar.gz` 和 `.sha256`；
- 解压到全新目录；
- 启动前执行 `sha256sum -c MANIFEST.sha256`；
- 不直接覆盖正式目录中的单个文件作为正式部署。

---

## 9. `check_runtime` 报 `/dev/pango_pci_driver` 缺失

### 根因

开发板重启后 PCIe 驱动未加载。

### 处理

```bash
CURRENT="$(head -n 1 /userdata/Apps/RK3568_PTZ_Qt_test_current.path)"
"$CURRENT/scripts/load_pcie_driver.sh"

lsmod | grep '^pango_pci_driver '
ls -l /dev/pango_pci_driver
```

若模块已加载但节点缺失，不要重复 `insmod`，检查：

```bash
dmesg -T | grep -Ei 'pango|pci|dma|BAR|error|fail' | tail -n 120
```

---

## 10. `/dev/video0` 缺失

### 检查

```bash
lsusb
lsmod | grep uvcvideo
dmesg -T | grep -Ei 'usb|uvc|video4linux' | tail -n 100
```

### 处理

```bash
modprobe uvcvideo
sleep 2
ls -l /dev/video*
```

若 `lsusb` 中没有采集卡，需要重新插拔、检查供电和 USB 接口。

---

## 11. 模型校验失败

### 处理

虚拟机：

```bash
(
  cd assets/models
  sha256sum -c SHA256SUMS
)
```

板端：

```bash
(
  cd /userdata/Apps/RK3568_PTZ_Qt/models
  sha256sum -c SHA256SUMS
)
```

不要用同名但来源不明的模型替换。

---

## 12. 虚拟机磁盘不足

### 现象

根分区接近 100%，Qt 构建或 Git 操作异常。

### 安全清理

```bash
rm -rf ~/.local/share/Trash/files/*
rm -rf ~/.local/share/Trash/info/*
rm -rf ~/.cache/vmware/drag_and_drop/*
sudo apt clean
sudo journalctl --vacuum-size=100M
df -h /
```

不要删除当前 Qt ARM64 工具链、`rk3568_deps`、源码或 `.git`。

---

## 13. Git 分支重命名失败

空仓库在首次提交前没有真实分支引用，旧 Git 可能报：

```text
refname refs/heads/master not found
```

处理：

```bash
git symbolic-ref HEAD refs/heads/main
```

首次提交后再正常使用分支命令。

---

## 14. `stop.sh` 提示使用了 SIGKILL

### 含义

应用收到SIGTERM后10秒内没有完成Qt析构和工作线程回收。脚本为释放DRM/PCIe资源进行了最终强制终止；这不是正常停止结果。

### 检查

```bash
CURRENT="$(head -n 1 /userdata/Apps/RK3568_PTZ_Qt_test_current.path)"
tail -n 200 "$CURRENT/logs/rk_dual_screen_video.log"
dmesg -T | grep -Ei 'pango|pci|dma|drm|hang|error|fail' | tail -n 120
```

日志应包含`Received signal ... requesting graceful Qt shutdown`。若存在该日志但仍超时，重点检查PCIe驱动`read()`是否不可中断地阻塞，以及PTZ/NPU停止过程是否卡住。

---

## 15. 新测试版本失败后自动回滚

### 正常日志特征

```text
WARNING: 新版本切换失败，开始自动回滚。
恢复：/userdata/Apps/RK3568_PTZ_Qt...
回滚成功，PID：...
```

即使回滚成功，`deploy_test.sh`仍返回非零，表示本次新发布未通过。通过以下命令确认当前实际进程和状态文件一致：

```bash
CURRENT="$(head -n 1 /userdata/Apps/RK3568_PTZ_Qt_test_current.path 2>/dev/null || true)"
PID="$(cat "$CURRENT/run/rk_dual_screen_video.pid")"
readlink -f "/proc/$PID/exe"
echo "$CURRENT"
```

若提示“仍有视频进程，无法安全恢复旧版本”，脚本不会并行启动旧版本。先人工确认残留进程的`/proc/<PID>/exe`，不得使用宽泛`pkill -f`。
