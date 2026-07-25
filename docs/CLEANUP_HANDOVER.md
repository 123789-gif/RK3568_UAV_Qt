# 保守清理版交接说明

## 1. 来源

本目录最初基于2026-07-24上传的完整工程快照整理；本轮又以
2026-07-25上传的最新完整工程压缩包为唯一输入，开始控制窗口源码拆分。

## 2. 已移除

- 12个根目录`.backup_*`历史补丁备份；
- `out/`构建产物；
- `dist/`发布目录和压缩包；
- `rk_dual_screen_video.pro.user`本机Qt Creator配置。

这些内容均不属于源码真值。原始上传压缩包仍可作为离线备份。

## 3. 已保留

- 完整`.git`历史、分支和标签；
- 全部`src/`业务源码；
- 全部模型、默认配置和已验证PCIe驱动；
- 发布、启动、停止和板端检查脚本；
- 最新`tools/deploy_test.sh`；
- `BUILD-CHECK-01`标题修改。

## 4. 保守清理阶段修改

- `.gitignore`增加`.backup_*`规则；
- `tools/build_arm64.sh`默认输出统一为`out/arm64-release`；
- 构建脚本增加删除路径保护；
- README、构建、部署和开发流程文档更新为当前实际状态；
- 明确`yolov8_car.rknn`当前未接入运行链路。

该阶段未修改`src/`下任何业务逻辑。

## 5. 控制窗口拆分阶段修改

- 新建分支`refactor/control-panel-window-split-v1`；
- 将`ControlPanelWindow`的72个成员函数按视频、FPGA、AI、违法规则、PTZ、
  系统状态、样式和窗口核心拆分到8个`.cpp`；
- 保持全部成员函数实现逐字一致，仅删除空`.ui`产生的`ui`对象初始化、
  `setupUi()`和析构释放；
- 删除未承载控件和布局的`control_panel_window.ui`；
- qmake工程改为直接编译拆分后的实现文件；
- PCIe、RGA/DRM、NPU调度与后处理、模型、PTZ工作线程及部署脚本均未改动。

## 6. 解压后的预期Git状态

交付压缩包位于`refactor/control-panel-window-split-v1`分支，源码拆分保持为未提交修改。
解压后`git status`不是clean是预期行为。

必须先完成一次重新构建和板端回归，再将这些明确修改提交为新的验证基线。不要为了让`git status`变干净而丢弃修改。

## 7. 当前功能事实

- 当前运行模型：`ped_det.rknn`、`plate_det.rknn`、`plate_reco.rknn`；
- 当前违法任务：行人检测+交通灯/斑马线规则；
- `yolov8_car.rknn`仅保留为实验资产；
- 车辆检测结果链路当前未启用。

## 8. 下一阶段

控制窗口拆分通过板端回归并提交新基线后，再处理：

1. 将运行配置移到版本目录之外的持久目录；
2. 自动生成Git提交号/构建版本；
3. 评估是否继续拆分`control_panel_window_internal.h`中的私有辅助函数；
4. 分离OSD、NPU输入准备和NPU任务执行器；
5. 决定恢复车辆模型链路或移除实验模型。
