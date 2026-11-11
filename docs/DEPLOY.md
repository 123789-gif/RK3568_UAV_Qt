# 板端测试部署说明

## 1. 当前部署策略

```text
板端地址：root@192.168.1.100
正式目录：/userdata/Apps/RK3568_UAV_Qt
测试目录：/userdata/Apps/RK3568_UAV_Qt_test_<UTC时间戳>
当前状态：/userdata/Apps/RK3568_UAV_Qt_test_current.path
```

测试部署不会覆盖正式目录。默认保留最新3个时间戳测试版本，并保护当前版本和正在运行的版本。

## 2. 一次性配置SSH密钥

不要把板端密码写入脚本。

在虚拟机`simba`用户下执行：

```bash
mkdir -p ~/.ssh
chmod 700 ~/.ssh

[ -f ~/.ssh/id_ed25519 ] || \
ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519 -N ''

ssh-copy-id -i ~/.ssh/id_ed25519.pub root@192.168.1.100
```

验证：

```bash
ssh -o BatchMode=yes root@192.168.1.100 'echo SSH_KEY_OK'
```

必须无密码提示并输出`SSH_KEY_OK`。

## 3. 标准测试部署

在Qt Creator完成重新构建后：

```bash
cd /home/simba/Projects/RK3568_UAV_Qt
./tools/deploy_test.sh
```

脚本自动完成：

1. 检查ELF为ARM AArch64；
2. 拒绝部署比源码更旧的ELF；
3. 生成发布目录、压缩包和SHA256；
4. 上传到板端`/tmp`；
5. 校验压缩包和发布目录；
6. 识别并停止切换前实际运行的正式/测试程序；
7. 加载/检查PCIe驱动；
8. 检查板端动态库、模型、设备和显示环境；
9. 启动新程序并确认实际运行的是新目录中的ELF；
10. 更新当前测试版本状态；若第6～9步失败，自动停止新版本并恢复原运行版本；
11. 保留最新3个测试版本；
12. 删除板端`/tmp`中的本次发布包。

## 4. BUILD-CHECK验证

当前标题验证：

```bash
EXPECTED_MARKER=BUILD-CHECK-01 ./tools/deploy_test.sh
```

该检查只用于确认ELF确实来自当前源码。后续应使用自动构建版本信息替代手工标题标记。

## 5. 常用选项

仅上传和解压，不切换运行程序：

```bash
./tools/deploy_test.sh --no-start
```

保留最新5个测试版本：

```bash
./tools/deploy_test.sh --keep-releases 5
```

指定板端：

```bash
./tools/deploy_test.sh --board root@192.168.1.100
```

## 6. 板端检查

```bash
CURRENT="$(head -n 1 /userdata/Apps/RK3568_UAV_Qt_test_current.path)"

echo "$CURRENT"
cat "$CURRENT/run/rk_dual_screen_video.pid"
tail -n 100 "$CURRENT/logs/rk_dual_screen_video.log"
"$CURRENT/scripts/check_runtime.sh"
```

停止当前测试程序：

```bash
"$CURRENT/scripts/stop.sh"
```

前台调试：

```bash
"$CURRENT/scripts/start.sh" --foreground
```

## 7. 自动回滚与手动回滚

标准测试部署现在是事务式切换。脚本在停止程序前通过`/proc/<PID>/exe`记录实际运行版本；新版本启动失败、启动后10秒内退出或检测到并行视频进程时，会自动：

1. 精确停止新目录中的ELF；
2. 重新启动切换前实际运行的正式版或测试版；
3. 校验恢复进程的ELF路径；
4. 恢复`RK3568_UAV_Qt_test_current.path`，部署命令仍返回失败，便于CI/人工识别本次新版本未通过。

若切换前没有运行中的受管版本，脚本不会猜测一个版本启动，而是保留失败现场和日志。

以下命令仅用于需要人工选择历史版本时的手动回滚。

默认保留最近3个测试版本。选择需要回滚的目录后：

```bash
OLD=/userdata/Apps/RK3568_UAV_Qt_test_<目标版本>
CURRENT="$(head -n 1 /userdata/Apps/RK3568_UAV_Qt_test_current.path)"

"$CURRENT/scripts/stop.sh" 2>/dev/null || true
"$OLD/scripts/check_runtime.sh"
"$OLD/scripts/start.sh"
printf '%s\n' "$OLD" > /userdata/Apps/RK3568_UAV_Qt_test_current.path
```

回滚前必须确认`OLD`是完整时间戳测试目录，不允许将变量设置为`/userdata/Apps`或其他上级路径。

## 8. 正式版本

当前脚本只部署版本化测试目录，不自动覆盖：

```text
/userdata/Apps/RK3568_UAV_Qt
```

正式版本切换应在完成DSI、HDMI、PCIe、NPU、PTZ、FPGA控制、网口输出、启停和冷启动回归后单独执行。

## 9. 首轮可靠性回归

补丁应用并重新构建后，至少执行：

1. 连续20次`start.sh`/`stop.sh`，日志中不应出现`terminate called`，停止脚本不应使用SIGKILL；
2. 云台运动中执行`stop.sh`，确认云台和变焦停止；
3. NPU运行中执行`stop.sh`，确认进程在10秒内优雅退出；
4. HDMI未连接时启动失败，再连接HDMI并重新启动，应用不得因旧`std::thread`对象崩溃；
5. PCIe设备缺失/恢复后重复启停，永久`read()`错误必须出现在日志中；
6. 制造新版本启动失败（例如临时移走测试目录中的模型），确认脚本自动恢复原运行版本；
7. 在PID文件中写入一个无关存活PID，执行`stop.sh`，确认无关进程未被终止；
8. 正常视频下确认1080p60、RGA/DRM flip、NPU和PTZ性能相对基线变化不超过5%。
