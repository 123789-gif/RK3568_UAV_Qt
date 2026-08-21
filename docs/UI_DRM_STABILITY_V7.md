# Qt紧凑布局与HDMI恢复 v7 验收

## 修改边界

- 车牌识别模式区只保留四个固定高度模式按钮；输入源由视频显示页统一管理，运行状态由右侧状态框和按钮高亮表示。
- FPGA配置页删除静态输入说明和重复的瞬态应用文本；保留FPGA实际模式、硬件能力、网口实际状态和输出信息。
- 车牌页面默认目标帧率为30 fps；45/60 fps只作为手动压力测试值。
- 行人和车辆逆行任务固定使用通用15 fps默认值，不继承车牌页面设置。
- DRM page flip超时时先核对当前scanout FB，再做同一会话内的受限modeset恢复；连续恢复失败后才退出会话。
- 异常自动重启期间不主动恢复启动前的X11 CRTC，避免HDMI短暂显示Linaro桌面；用户主动停止时仍正常恢复。

## HDMI恢复日志

正常运行不应持续出现恢复日志。偶发事件允许出现：

```text
drm pageflip timeout recovered: ... method=confirmed-current-fb
drm pageflip timeout recovered: ... method=forced-modeset
drm pageflip busy recovered by modeset: ...
```

以下日志表示同一会话内恢复已经失败，控制层将执行最多两次完整重建：

```text
drm pageflip timeout unrecovered: ...
[HDMI-RECOVERY] scheduled ...
```

若出现 `RGA/DRM exited`，先观察DSI：

- DSI上的Qt仍在：仅HDMI显示会话退出，检查上述DRM日志。
- DSI上的Qt也消失：属于进程崩溃，不应归类为page flip问题，需要同时保存应用日志、`journalctl`和`dmesg`。

## 上板验收

1. 进入车牌识别页，确认四个模式按钮两行文字完整显示，切换模式时按钮和页面高度不变化。
2. 进入FPGA配置页，确认四个算法按钮、当前模式、硬件能力、网口状态均在一屏内完整显示。
3. 首次升级启动后，车牌目标帧率应为30 fps；行人和逆行任务运行状态中的目标帧率应为15 fps。
4. 启动车牌标准模式，以30 fps持续运行10分钟，反复切换八个页面并点击不影响视频链路的配置按钮。
5. 依次测试四种车牌模式，每种运行5分钟，记录实际NPU fps、单帧耗时、dropped、NPU温度和HDMI flip fps。
6. 手动设置45 fps做压力测试。若实际fps低于40、单帧耗时持续高于22.2 ms、dropped快速增加或出现DRM恢复日志，不得把45 fps作为正式默认值。
7. 连续执行FPGA模式切换和网口开关各20次，确认ACK后按钮才转为当前状态，HDMI不得回到桌面。
8. 正常点击“停止HDMI”时允许恢复桌面；运行中异常不应主动恢复桌面。

建议实时观察：

```bash
APP_DIR=$(cat /userdata/Apps/RK3568_PTZ_Qt_test_current.path 2>/dev/null)
tail -f "$APP_DIR/logs/rk_dual_screen_video.log" |
grep -E 'DRM|drm pageflip|HDMI-RECOVERY|RGA/DRM exited|NPU'
```

若Qt进程退出，再采集：

```bash
journalctl -u rk3568-ptz-qt.service -n 300 --no-pager
dmesg -T | tail -n 300
```

构建部署标记：`BUILD-CHECK-07`。
