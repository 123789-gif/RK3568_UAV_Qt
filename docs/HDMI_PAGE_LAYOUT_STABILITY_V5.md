# HDMI与页面布局稳定性 v5 验收

## 本轮边界

本轮修复基于 `RK3568_PTZ_Qt-821-2` 完整工程，基线源码树为
`d6e450f2dbc791771426a74efa4023217300fe33`。

- 不改变 PCIe 取流、RGA、DRM page flip 和 NPU 输入坐标约定。
- 不把“切换到车牌页”与 HDMI 启停绑定；导航仍只切换 Qt 页面。
- 修复顶层窗口和子页面 size hint 相互影响的问题，并减少隐藏页面重绘。
- DRM 会话异常退出后最多自动重建两次；连续三个统计周期正常后才恢复重试额度。

## 布局修复

- 控制窗口固定在包含桌面原点的 DSI `availableGeometry`，页面最小尺寸不再把窗口推入右侧 HDMI 桌面。
- 八个页面统一放入忽略内容 size hint 的 viewport；车牌页横、竖滚动条均关闭，其余页面仅在低于设计分辨率时兜底滚动。
- 车牌四模式区域从 170 px 调整为 190 px，四个模式按钮固定为 60 px，满足文字、布局边距和样式边距的实际高度。
- 行人页的区域按钮、模型按钮、阈值框和灯态按钮与 QSS 最小高度统一，消除“固定高度小于样式最小高度”的冲突。
- 页面切换会输出 `[UI-GEOMETRY]` 日志，包含窗口、页面内容、最小尺寸和滚动条状态。

## 刷新与 HDMI 恢复

- 5 Hz AI 状态轮询只刷新当前可见的车牌、行人或逆行页面。
- 预览帧只写入当前可见的行人/逆行预览控件；隐藏页面不再触发无效更新。
- 行人、车辆模型运行按钮仅在状态属性真正改变时重新 polish，避免每 200 ms 重算样式。
- 用户启动 HDMI 后记录“期望运行”状态。若 worker 异常退出，等待 400 ms、1000 ms 分别尝试重建；两次均失败即停止重试并保留错误。
- 恢复等待期间再次点击 HDMI 启停按钮会取消恢复；手动停止不会被自动拉起。
- 自动恢复日志以 `[HDMI-RECOVERY]` 开头，系统状态页显示累计恢复次数。

## 虚拟机操作

```bash
cd /home/simba/Downloads
unzip RK3568_PTZ_Qt_hdmi_page_layout_stability_v5_patch.zip

cd /home/simba/Projects/RK3568_PTZ_Qt
bash /home/simba/Downloads/RK3568_PTZ_Qt_hdmi_page_layout_stability_v5/apply_patch.sh "$PWD"

./tools/build_arm64.sh
EXPECTED_MARKER=BUILD-CHECK-05 ./tools/deploy_test.sh --keep-releases 5
```

补丁 ZIP 不配套外置 SHA256 文件；`apply_patch.sh` 自身会检查干净工作区、允许的基线源码树以及补丁应用结果。

## 上板验收顺序

1. 启动 HDMI，确认全屏总览持续输出，底栏 `FLIP` 稳定。
2. 依次点击全部八个页面，每页停留至少 5 秒；确认 HDMI 不黑屏、不闪烁、进程不退出。
3. 在车牌页连续切换四种模式和两种结果策略，确认四个按钮完整显示、页面无横竖滚动条、下方参数与结果区域高度不跳变。
4. 行人页开启/关闭行人模型和违法规则；逆行页开启/关闭车辆模型和逆行规则，观察隐藏页面不应产生额外闪动。
5. 收集日志：

   ```bash
   journalctl -f | grep -E 'UI-GEOMETRY|HDMI-RECOVERY|drm pageflip|RGA/DRM'
   ```

6. 每次切页的车牌页日志应为 `h_scroll=false v_scroll=false`；窗口矩形应完全位于 DSI screen 矩形内。
7. 若制造一次 DRM 瞬态退出，应看到有上限的恢复日志；若连续两次重建失败，应停止重试而不是循环重启。

若进程仍退出，保留退出前 100 行日志和 core/backtrace；本轮日志可区分“Qt 页面几何越界”“DRM page flip 超时”和“NPU任务切换异常”三类路径。
