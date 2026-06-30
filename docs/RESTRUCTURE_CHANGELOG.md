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
