# 板端部署、启动与回滚

## 1. 当前部署策略

板端正式目录：

```text
/userdata/Apps/RK3568_PTZ_Qt
```

运行用户：`root`

当前阶段：

- 支持单命令前台或后台启动；
- 不默认启用 systemd；
- PCIe 驱动仍需人工条件加载；
- 不依赖 `/userdata/Projects/icore_npu_rk/model`；
- 不应手工设置 `LD_LIBRARY_PATH`。

## 2. 生成发布包

虚拟机：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt

./deploy/make_bundle.sh   "$PWD/out/arm64-release/rk_dual_screen_video"   "$PWD/dist"
```

输出为版本化文件：

```text
dist/RK3568_PTZ_Qt_<UTC时间戳>.tar.gz
dist/RK3568_PTZ_Qt_<UTC时间戳>.tar.gz.sha256
```

发布包包含：

```text
RK3568_PTZ_Qt/
├── bin/rk_dual_screen_video
├── models/
├── config/runtime.env
├── scripts/
├── logs/
├── run/
├── VERSION
└── MANIFEST.sha256
```

## 3. 上传

```bash
PACKAGE="$(ls -1t dist/RK3568_PTZ_Qt_*.tar.gz | head -n 1)"

scp "$PACKAGE" "$PACKAGE.sha256" root@192.168.1.100:/tmp/
```

## 4. 板端压缩包校验

```bash
ssh root@192.168.1.100
cd /tmp

PACKAGE="$(ls -1t /tmp/RK3568_PTZ_Qt_*.tar.gz | head -n 1)"
PACKAGE_NAME="$(basename "$PACKAGE")"

sha256sum -c "$PACKAGE_NAME.sha256"
gzip -t "$PACKAGE"
```

必须全部成功。

## 5. 解压到干净测试目录

```bash
TEST_ROOT=/userdata/Apps/RK3568_PTZ_Qt_test_clean

rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT"

tar -xzf "$PACKAGE"   -C "$TEST_ROOT"   --strip-components=1

sync

cd "$TEST_ROOT"
sha256sum -c MANIFEST.sha256
```

`MANIFEST.sha256` 任一文件失败时禁止启动。

## 6. PCIe 驱动

已验证信息：

```text
驱动文件：/userdata/Projects/driver_1080/pango_pci_driver.ko
模块名称：pango_pci_driver
设备节点：/dev/pango_pci_driver
驱动 SHA256：e00ef92e4bcb3bfe1fbb3711bbba88fdab54c266c3b6fcc51ae6cf9a4942200f
vermagic：6.1.99 SMP mod_unload aarch64
```

当前人工幂等加载：

```bash
DRIVER_KO=/userdata/Projects/driver_1080/pango_pci_driver.ko

if [ -e /dev/pango_pci_driver ]; then
  echo "PCIe driver device already exists"
elif lsmod | grep -q '^pango_pci_driver '; then
  echo "ERROR: module is loaded but /dev/pango_pci_driver is missing" >&2
  exit 1
else
  insmod "$DRIVER_KO"
fi

lsmod | grep '^pango_pci_driver '
ls -l /dev/pango_pci_driver
```

不要每次无条件 `insmod`，也不要在普通 `stop.sh` 中 `rmmod`。

## 7. 完整运行检查

```bash
"$TEST_ROOT/scripts/check_runtime.sh"
```

检查内容包括：

- 发布包 `MANIFEST.sha256`；
- AArch64 ELF；
- DRM、RGA、RKNN、OpenCV 关键依赖；
- 三个模型；
- `/dev/pango_pci_driver`；
- `/dev/dri/card0`；
- `/dev/video0`；
- `/dev/ttyS0`；
- `/dev/mem`；
- 板端动态库。

最后必须出现：

```text
Runtime check passed.
```

## 8. 前台启动

```bash
"$TEST_ROOT/scripts/start.sh" --foreground
```

脚本执行：

```text
check_runtime
→ 启动或等待 LightDM
→ 检查/设置 DSI 和 HDMI 布局
→ Goodix 映射到 DSI
→ 使用干净 env 启动 Qt
```

当前 PCIe 驱动加载发生在脚本之外。

已验证显示布局：

```text
DSI-1   原始 720×1280，right 旋转后逻辑 1280×720，位置 0x0，primary
HDMI-1  1920×1080@60，位置 1280x0
桌面    3200×1080
触摸    goodix-ts → DSI-1
```

## 9. 后台启动、日志和停止

```bash
"$TEST_ROOT/scripts/start.sh"
```

查看：

```bash
cat "$TEST_ROOT/run/rk_dual_screen_video.pid"
ps -fp "$(cat "$TEST_ROOT/run/rk_dual_screen_video.pid")"
tail -n 100 "$TEST_ROOT/logs/rk_dual_screen_video.log"
```

停止：

```bash
"$TEST_ROOT/scripts/stop.sh"
pgrep -af rk_dual_screen_video
```

## 10. 切换正式目录

```bash
APP_ROOT=/userdata/Apps/RK3568_PTZ_Qt
BACKUP_ROOT=/userdata/Apps/RK3568_PTZ_Qt_backup_$(date +%Y%m%d_%H%M%S)

"$TEST_ROOT/scripts/stop.sh" 2>/dev/null || true

if [ -d "$APP_ROOT" ]; then
  "$APP_ROOT/scripts/stop.sh" 2>/dev/null || true
  mv "$APP_ROOT" "$BACKUP_ROOT"
fi

mv "$TEST_ROOT" "$APP_ROOT"

"$APP_ROOT/scripts/check_runtime.sh"
"$APP_ROOT/scripts/start.sh"
```

## 11. 回滚

```bash
"$APP_ROOT/scripts/stop.sh" 2>/dev/null || true

mv "$APP_ROOT"   "/userdata/Apps/RK3568_PTZ_Qt_failed_$(date +%Y%m%d_%H%M%S)"

mv "$BACKUP_ROOT" "$APP_ROOT"

"$APP_ROOT/scripts/check_runtime.sh"
"$APP_ROOT/scripts/start.sh"
```

## 12. systemd

`deploy/systemd/rk3568-ptz-qt.service` 当前只是模板，不要直接 `enable`。

启用前必须先完成：

- PCIe 驱动自动加载；
- 开发板冷启动回归；
- LightDM、USB采集、PCIe设备和应用启动顺序验证；
- systemd 服务退出和重启行为验证。
