# 板端部署与启动

首版策略：部署到 `/userdata/Apps/RK3568_PTZ_Qt`，由 root 单命令启动，暂不开机自启。

## 生成部署包

在虚拟机中完成 ARM64 编译后：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
./deploy/make_bundle.sh \
  /home/simba/Projects/build-RK3568_PTZ_Qt-arm64-Release/rk_dual_screen_video
```

输出：

```text
dist/RK3568_PTZ_Qt/
dist/RK3568_PTZ_Qt_deploy.tar.gz
```

## 复制到开发板

```bash
scp dist/RK3568_PTZ_Qt_deploy.tar.gz root@192.168.1.100:/tmp/
ssh root@192.168.1.100
rm -rf /userdata/Apps/RK3568_PTZ_Qt
mkdir -p /userdata/Apps
tar -xzf /tmp/RK3568_PTZ_Qt_deploy.tar.gz -C /userdata/Apps
```

## 启动与停止

```bash
/userdata/Apps/RK3568_PTZ_Qt/scripts/start.sh
/userdata/Apps/RK3568_PTZ_Qt/scripts/stop.sh
```

前台调试：

```bash
/userdata/Apps/RK3568_PTZ_Qt/scripts/start.sh --foreground
```

日志：

```text
/userdata/Apps/RK3568_PTZ_Qt/logs/rk_dual_screen_video.log
```

启动脚本会自动：启动/等待 LightDM、配置 DSI+HDMI、映射 Goodix 触摸、检查模型/设备/动态库、设置模型路径并启动程序。

`deploy/systemd/rk3568-ptz-qt.service` 仅作为后续开机自启模板，本阶段不要 enable。
