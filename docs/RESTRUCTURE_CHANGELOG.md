# 第一阶段重构变更摘要

- 仅进行目录、构建、模型路径和部署工程化整理；未改动推理算法、UI 行为、显示数据通路、PTZ 协议和 OSD 内容。
- 删除未被工程引用的空占位文件 `fpga_control.cpp/.h`。
- 删除 `build/`、`build_manual/`、`.pro.user`、历史 `.pro` 备份和运行日志。
- 模型查找新增可执行文件相对路径，仍保留旧板端路径作为兼容回退。
- 首版运行继续使用 root，正式部署目录为 `/userdata/Apps/RK3568_PTZ_Qt`，暂不开机自启。

# 第二阶段：PCIe 用户态模块剥离

- 新增 `src/pcie/pango_pci_uapi.h`，集中保存当前驱动 ioctl 和结构体 ABI；字段类型、顺序和 ioctl 编号保持不变。
- 新增 `src/pcie/pcie_frame_receiver.h/.cpp`，封装驱动打开、链路信息读取、DMA 四缓冲配置与 mmap、BAR0 PIO、阻塞等帧和资源释放。
- `DrmRgaDisplay` 不再直接持有 PCIe fd、DMA mmap 指针和 BAR0 资源，只消费 `FrameView` 并继续完成 NPU 提交、RGA 与 DRM 显示。
- 线程模型保持不变：PCIe 接收仍在原显示工作线程中同步执行。
- 数据搬运保持不变：RGA 直接读取驱动 mmap 缓冲，不新增整帧复制或 IPC。
- NPU、PTZ、OSD、显示时序、驱动 ABI 和部署脚本行为均未修改。

# 第三阶段：AI推理页面与单任务调度首版

- 新增 `src/npu/npu_task_config.h`，统一管理启用状态、任务类型、目标FPS、置信度、NMS阈值和结果历史深度。
- 推理模式改为单任务：行人检测仅执行 `ped_det`；车牌识别按 `plate_det → plate_reco` 级联执行。
- 任务切换由推理线程完成旧模型释放和新模型加载，不重启Qt，不修改PCIe主视频输入。
- 置信度和NMS阈值按一次完整配置快照传入推理线程，避免多个setter形成配置中间态。
- 新增64帧有界结果历史；Qt以5 Hz轮询批量结果，避免高频跨线程信号和结果遗漏。
- AI页面增加任务选择、目标FPS、置信度、NMS、运行状态和最近50条有效识别结果表。
- AI配置保存到部署目录 `config/ai_panel.ini`；输入源固定为PCIe主视频。
- HDMI OSD显示当前任务缩写；PCIe、RGA、DRM、PTZ和部署脚本未修改。
- 车牌检测接口当前未提供检测分数，结果表明确显示 `--`，不伪造置信度。
