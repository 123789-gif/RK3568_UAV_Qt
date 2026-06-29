# 标准开发流程

## 1. 目标

本文件规定本工程唯一推荐的开发路径，避免出现旧构建产物、错误模型、重复部署目录和板端文件被部分覆盖等问题。

```text
创建功能分支
→ 修改源码
→ ARM64 构建
→ ELF 与模型校验
→ 生成版本化发布包
→ 上传并校验压缩包
→ 解压到全新测试目录
→ 加载 PCIe 驱动
→ check_runtime
→ 前台回归
→ 后台启停测试
→ 切换正式版本
→ Git 提交与 Pull Request
```

## 2. 分支约定

```text
main        只保存板端完整验证通过的稳定版本
feature/*   功能开发
fix/*       缺陷修复
docs/*      文档修改
```

开始新任务：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
git switch main
git pull --ff-only
git switch -c feature/<task-name>
git push -u origin feature/<task-name>
```

禁止直接在 `main` 上进行多文件功能重构。

## 3. 修改前检查

```bash
git status
git branch --show-current
df -h /
```

要求：

- 工作区不存在与本任务无关的修改；
- 根分区建议至少保留 4 GB，可进行全量 Qt 构建时建议保留 6 GB 以上；
- 不从旧的 `build_manual` 或其他历史目录复制对象文件和可执行文件。

## 4. 构建

Qt Creator 标准构建目录：

```text
/home/simba/Projects/RK3568_PTZ_Qt/out/arm64-release
```

首次构建或 `.pro`、目录结构变化后执行：

```text
清理所有项目
→ 执行 qmake
→ 重新构建所有项目
```

构建后必须检查：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt

file out/arm64-release/rk_dual_screen_video

readelf -d out/arm64-release/rk_dual_screen_video   | grep -E 'libdrm|librga|librknnrt|libopencv_core|libopencv_imgproc'

cd out/arm64-release/models
sha256sum -c SHA256SUMS
```

只有 AArch64 ELF、依赖和模型校验全部正确，才能生成发布包。

## 5. 生成发布包

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt

rm -rf dist

./deploy/make_bundle.sh   "$PWD/out/arm64-release/rk_dual_screen_video"   "$PWD/dist"
```

脚本生成：

```text
dist/RK3568_PTZ_Qt/
dist/RK3568_PTZ_Qt_<UTC时间戳>.tar.gz
dist/RK3568_PTZ_Qt_<UTC时间戳>.tar.gz.sha256
```

本地验证：

```bash
PACKAGE="$(ls -1t dist/RK3568_PTZ_Qt_*.tar.gz | head -n 1)"

gzip -t "$PACKAGE"

(
  cd dist/RK3568_PTZ_Qt
  sha256sum -c MANIFEST.sha256
)
```

## 6. 上传和板端验证

上传：

```bash
scp "$PACKAGE" "$PACKAGE.sha256" root@192.168.1.100:/tmp/
```

板端使用实际文件名，不要把文档中的占位符原样执行：

```bash
cd /tmp

PACKAGE="$(ls -1t /tmp/RK3568_PTZ_Qt_*.tar.gz | head -n 1)"
PACKAGE_NAME="$(basename "$PACKAGE")"

sha256sum -c "$PACKAGE_NAME.sha256"
gzip -t "$PACKAGE"
```

解压到全新测试目录：

```bash
TEST_ROOT=/userdata/Apps/RK3568_PTZ_Qt_test_clean

rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT"

tar -xzf "$PACKAGE"   -C "$TEST_ROOT"   --strip-components=1

sync

cd "$TEST_ROOT"
sha256sum -c MANIFEST.sha256
```

禁止直接覆盖当前正式目录中的单个文件作为正式发布流程。

## 7. 驱动和运行检查

当前已验证 PCIe 驱动：

```text
文件：/userdata/Projects/driver_1080/pango_pci_driver.ko
模块：pango_pci_driver
节点：/dev/pango_pci_driver
内核：6.1.99 SMP mod_unload aarch64
```

在设备节点不存在时人工加载：

```bash
if ! lsmod | grep -q '^pango_pci_driver '; then
  insmod /userdata/Projects/driver_1080/pango_pci_driver.ko
fi

lsmod | grep '^pango_pci_driver '
ls -l /dev/pango_pci_driver
```

随后：

```bash
"$TEST_ROOT/scripts/check_runtime.sh"
```

必须以 `Runtime check passed.` 结束。

## 8. 前台回归

```bash
"$TEST_ROOT/scripts/start.sh" --foreground
```

至少验证：

- DSI 控制界面和 Goodix 触控；
- HDMI 1920×1080 输出；
- PCIe 视频输入与 DRM/RGA；
- 三个 RKNN 模型加载和 NPU 推理；
- PTZ `/dev/video0` 预览；
- VISCA `/dev/ttyS0` 控制；
- 风扇控制；
- 截图和日志。

结束使用 `Ctrl+C`。

## 9. 后台启停

```bash
"$TEST_ROOT/scripts/start.sh"

ps -fp "$(cat "$TEST_ROOT/run/rk_dual_screen_video.pid")"

tail -n 100 "$TEST_ROOT/logs/rk_dual_screen_video.log"

"$TEST_ROOT/scripts/stop.sh"

pgrep -af rk_dual_screen_video
```

停止后不应残留应用进程。普通 `stop.sh` 不卸载 PCIe 驱动。

## 10. 切换正式版本

测试通过后：

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

旧备份至少保留到新版本完成一次冷启动回归。

## 11. Git 提交

```bash
git status
git diff
git add <本次明确修改的文件>
git commit -m "<type>(<scope>): <summary>"
git push
```

推荐提交类型：

```text
feat  新功能
fix   缺陷修复
docs  文档
refactor 不改变行为的重构
build 构建和依赖
```

在 GitHub 创建 Pull Request，板端回归结果应写入 PR 描述。合并后：

```bash
git switch main
git pull --ff-only
```

## 12. 禁止事项

- 不上传或部署整个 `out/`；
- 不提交 `out/`、`dist/`、对象文件、日志和发布压缩包；
- 不复用含未知旧文件的测试目录；
- 不跳过压缩包 SHA256 和 `MANIFEST.sha256`；
- 不手工设置长期生效的 `LD_LIBRARY_PATH`；
- 不在 `stop.sh` 中自动 `rmmod`；
- 不在未完成板端回归前合并到 `main`。
