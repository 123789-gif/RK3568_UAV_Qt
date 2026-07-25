# ControlPanelWindow拆分说明

## 目标

降低`control_panel_window.cpp`单文件规模，让接手人员可以按功能定位界面代码，
同时保持板端运行行为、线程模型和数据通路不变。

## 文件职责

| 文件 | 职责 |
|---|---|
| `control_panel_window.cpp` | 构造、析构、窗口显示、导航和页面装配 |
| `control_panel_window_video.cpp` | 视频页面、启停、OSD、显示配置与显示统计 |
| `control_panel_window_fpga.cpp` | FPGA预处理模式、控制状态和网口输出 |
| `control_panel_window_ai.cpp` | AI任务配置、运行状态、结果缓存与展示 |
| `control_panel_window_violation.cpp` | 违法规则配置、启停和结果展示 |
| `control_panel_window_ptz.cpp` | PTZ页面、Profile、热插拔、预览、控制和截图 |
| `control_panel_window_system.cpp` | CPU/状态页面、风扇和周期状态更新 |
| `control_panel_window_style.cpp` | Qt样式表 |
| `control_panel_window_internal.h` | 仅供上述实现文件使用的私有依赖和辅助函数 |

## 本轮明确未改

- `PcieFrameReceiver`及驱动ABI；
- `DrmRgaDisplay`、RGA/DRM显示时序和OSD行为；
- NPU线程、任务调度、模型加载、前后处理和结果语义；
- PTZ采集/控制工作线程及VISCA协议；
- 模型、驱动、运行配置和部署脚本。

原`.ui`文件只是一个空`QMainWindow`，实际控件一直由C++代码创建。
因此本轮删除`.ui`、`Ui::ControlPanelWindow`成员和`setupUi()`调用，不会删除实际页面。

## 验证要求

由于交付环境不包含项目使用的Qt 5.12.9 ARM64 Kit，本包完成的是源码级校验，
最终验收必须在原虚拟机完成：

1. Qt Creator执行“清理项目”后“重新构建项目”；
2. 确认可执行文件时间和ARM64架构；
3. 执行`./tools/deploy_test.sh`；
4. 板端依次回归主视频、通道切换、FPGA模式、网口输出、三类AI任务、
   违法规则、PTZ预览/控制/热插拔/截图、风扇控制和退出重启；
5. 回归通过后再提交并打验证标签。

若构建出现旧`.ui`缓存相关错误，应删除`out/arm64-release`后重新运行qmake，
不要恢复已删除的空`.ui`文件。
