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


## 视频选路与单路 NPU 输入

- 新增 `video_route_config.h`，固定标准 3×3、640×360 的 CH1～CH9 ROI 映射；
- 视频显示页新增总览和 CH1～CH9 触控选路；
- 单路模式下 HDMI 和 NPU 绑定同一通道；
- 总览模式暂停 NPU，不再把完整九宫格送入模型；
- NPU 输入搬运由约 4.15 MB/推理帧降为约 0.46 MB/推理帧；
- 增加 source revision，防止切换通道后旧检测结果叠加到新画面。

# 第五阶段：PTZ 双相机 Profile 与动态 V4L2 选路

- 新增 `ptz_camera_profile.h/.cpp`，集中保存当前 HD USB PTZ 和 Sony EVI-D100 + MS210x 的 USB、视频、显示比例及 VISCA 配置；
- 新增 `v4l2_device_detector.h/.cpp`，枚举 `/dev/videoN`、过滤非采集节点、读取 sysfs USB VID/PID，并验证目标格式能力；
- `PtzCaptureWorker` 不再写死 `/dev/video0`、960×540@20，改为消费已解析的 `PtzResolvedDevice`；
- `PtzControlWorker` 不再内部写死串口路径，改为从相机 Profile 获取 `/dev/ttyS0` 和 VISCA 配置；
- PTZ 页面新增“自动识别 / 当前 HD USB PTZ / Sony EVI-D100 + MS210x”选择及应用按钮；
- 配置切换先解析新设备，再停止并重启原本处于运行状态的 PTZ 服务；解析失败时保留现有运行链路；
- EVI-D100 预览按 4:3 显示，HD USB PTZ 按 16:9 显示；
- PTZ Profile 选择保存到 `config/ptz_camera.ini`；
- 部署运行检查由固定 `/dev/video0` 改为接受任意 `/dev/videoN` 候选节点；
- PCIe、九宫格选路、NPU、RGA/DRM、FPGA 协议和模型均未修改。


# 第六阶段：PTZ Profile 修正与运行时热切换

- 修正 HD USB PTZ 的实测 USB ID：`33f1:0003` 改为 `33f1:1003`；保留已验证的 MJPEG 960×540@20、16:9 配置；
- 增加 1 秒周期 PTZ 设备监测，Qt 运行期间可拔出当前视频设备并插入另一套 PTZ，无需重启应用；
- 自动模式保持仍在线的当前源；当前源消失后按上次成功 Profile 或唯一在线设备自动恢复；
- 手动选择目标设备未在线时保留当前有效链路并等待，目标设备出现后自动切换；
- 采集线程增加 V4L2 设备丢失检测，覆盖 `POLLERR/POLLHUP/POLLNVAL`、节点消失及 `ENODEV/EIO/EPIPE`；
- 切换流程固定为 VISCA 停止 → 采集停止 → STREAMOFF/munmap/close → 新设备解析与配置 → 预览/控制按用户期望状态恢复；
- 切换和设备断开时清空最新帧与截图源，避免旧相机画面残留；
- PTZ 配置文件新增 `last_successful_profile`，用于自动模式下两台设备同时在线时保持确定性；
- `check_runtime.sh` 在没有 `/dev/videoN` 时改为警告而非阻止启动，使应用可先启动再等待相机热插入；
- 未修改 PCIe、FPGA、九宫格选路、NPU、RGA/DRM 主显示链路。

# 第七阶段：OSD 排版与 NPU 吞吐上限优化

- 系统状态 OSD 不再与 AI 状态 OSD 分别使用独立硬编码纵坐标；系统状态块返回下一可用 Y 坐标，AI 状态从该位置继续绘制；
- 保留原有 PCIe/RGA/DRM 状态、NPU 状态、检测框和目标标签，仅修正左上角文字重叠，不改变检测框坐标映射；
- 新增统一的 `NPU_TARGET_FPS_MIN/DEFAULT/MAX` 常量，目标推理帧率范围调整为 1～30 FPS，默认值调整为 15 FPS；
- Qt 控件、配置钳位、INI 读取和结果默认值统一使用相同帧率常量，避免界面值与后端实际值不一致；
- 保留单槽 latest-only 调度；NPU 忙时新帧覆盖旧等待帧，不引入任务队列和延迟堆积；
- 未修改 PCIe、PTZ、模型文件、推理后处理、RGA 裁剪和部署脚本。

# 第八阶段：NPU 绝对截止时间调度修正

- 将目标推理帧率节流从“整数毫秒间隔 + 以上次实际提交时刻重新计时”改为纳秒级绝对截止时间调度；
- 使用 `nextSubmitNs` 保留理想时间轴相位，源帧率与目标帧率不可整除时允许相邻源帧间隔交替，长期平均值接近目标帧率；
- `wantsFrame()` 仅在 RGA 裁剪前做低成本到期预检查，`submit()` 在来源、尺寸和数据长度验证通过后原子占用时间槽；
- 增加 1 ms 截止时间容差，避免 60 Hz 帧边界抖动导致 15/20/25/30 FPS 分别退化到较低离散档位；
- 长时间无源帧或处理停顿后跳过已错过的截止点，不突发补交历史任务；
- 配置、任务、启停或输入通道变化时重置调度时间轴；保留现有 latest-only 单槽覆盖机制；
- 未修改模型、预处理、后处理、PCIe、RGA、DRM、OSD、PTZ和部署脚本。
