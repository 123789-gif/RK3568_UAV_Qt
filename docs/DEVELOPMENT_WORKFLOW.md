# 标准开发流程

## 1. 唯一推荐流程

```text
确认Git基线
→ 创建feature/fix分支
→ 修改源码
→ ARM64重新构建
→ deploy_test.sh测试部署
→ 板端功能回归
→ 提交和标记验证版本
```

## 2. 修改前

```bash
cd /home/simba/Projects/RK3568_UAV_Qt
git status --short --branch
git branch --show-current
```

要求：

- 明确当前基线和本次修改范围；
- 不从`.backup_*`、`out/`或`dist/`复制旧代码覆盖源码；
- 一个功能修改使用一个独立分支；
- 未完成板端回归前不合并到`main`。

## 3. 分支约定

```text
main        板端完整验证通过的稳定版本
feature/*   新功能
fix/*       缺陷修复
refactor/*  不改变行为的结构调整
docs/*      纯文档修改
```

建议：

```bash
git switch main
git pull --ff-only
git switch -c fix/<task-name>
```

## 4. 构建

Qt Creator构建目录和命令行构建目录统一为：

```text
out/arm64-release
```

Qt Creator重新构建，或者：

```bash
./tools/build_arm64.sh
```

## 5. 测试部署

```bash
./tools/deploy_test.sh
```

禁止把单个ELF手工复制到正式目录作为正式部署流程。

## 6. 最低板端回归项

- DSI控制界面正常，Goodix触摸映射正确；
- HDMI 1920×1080输出正常；
- PCIe视频持续接收，无异常进程残留；
- FPGA预处理模式切换；
- FPGA网口输出开关；
- 行人检测；
- 车牌检测和识别；
- 行人违法规则与ROI配置；
- PTZ预览、热插拔和VISCA控制；
- 风扇控制；
- 后台启动、停止和日志；
- 至少一次冷启动检查。

当前代码不运行`yolov8_car.rknn`，车辆检测不能作为已通过功能记录。

## 7. 提交

板端回归通过后：

```bash
git status
git diff
git add <本次明确修改的文件>
git commit -m "<type>(<scope>): <summary>"
git push
```

推荐类型：

```text
feat fix refactor build docs test chore
```

提交或标签说明至少记录：

- Git提交号；
- 发布包VERSION；
- 板端内核版本；
- 驱动SHA256；
- 模型SHA256；
- 回归结果；
- 已知问题。

## 8. 生成物边界

以下目录只允许作为本地生成物：

```text
out/
dist/
```

以下内容不得回到工程根目录：

```text
.backup_*
*.pro.user
日志
PID文件
板端运行时快照
```

## 9. 后续重构约束

代码拆分必须分阶段进行。优先顺序：

1. UI页面与配置读写；
2. OSD渲染；
3. NPU输入准备；
4. NPU任务执行器；
5. 模型后处理。

不要同时迁移构建系统、调整PCIe链路和重构NPU任务。
