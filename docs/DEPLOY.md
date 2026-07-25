# 板端测试部署说明

## 1. 当前部署策略

```text
板端地址：root@192.168.1.100
正式目录：/userdata/Apps/RK3568_PTZ_Qt
测试目录：/userdata/Apps/RK3568_PTZ_Qt_test_<UTC时间戳>
当前状态：/userdata/Apps/RK3568_PTZ_Qt_test_current.path
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
cd /home/simba/Projects/RK3568_PTZ_Qt
./tools/deploy_test.sh
```

脚本自动完成：

1. 检查ELF为ARM AArch64；
2. 拒绝部署比源码更旧的ELF；
3. 生成发布目录、压缩包和SHA256；
4. 上传到板端`/tmp`；
5. 校验压缩包和发布目录；
6. 停止旧测试程序和正式程序；
7. 加载/检查PCIe驱动；
8. 检查板端动态库、模型、设备和显示环境；
9. 启动新程序并确认实际运行的是新目录中的ELF；
10. 更新当前测试版本状态；
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
CURRENT="$(head -n 1 /userdata/Apps/RK3568_PTZ_Qt_test_current.path)"

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

## 7. 回滚

默认保留最近3个测试版本。选择需要回滚的目录后：

```bash
OLD=/userdata/Apps/RK3568_PTZ_Qt_test_<目标版本>
CURRENT="$(head -n 1 /userdata/Apps/RK3568_PTZ_Qt_test_current.path)"

"$CURRENT/scripts/stop.sh" 2>/dev/null || true
"$OLD/scripts/check_runtime.sh"
"$OLD/scripts/start.sh"
printf '%s\n' "$OLD" > /userdata/Apps/RK3568_PTZ_Qt_test_current.path
```

回滚前必须确认`OLD`是完整时间戳测试目录，不允许将变量设置为`/userdata/Apps`或其他上级路径。

## 8. 正式版本

当前脚本只部署版本化测试目录，不自动覆盖：

```text
/userdata/Apps/RK3568_PTZ_Qt
```

正式版本切换应在完成DSI、HDMI、PCIe、NPU、PTZ、FPGA控制、网口输出、启停和冷启动回归后单独执行。
