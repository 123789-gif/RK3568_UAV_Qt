# RK3568_UAV_Qt

UAV-DETECT-02：基于已验证UAV-FLOW-01工程派生的无人机检测版本。移除车牌/OCR、行人、车辆、交通违章和逆行规则源码及模型；保留PCIe视频、RGA/DRM、选路、FPGA控制、PTZ预览与VISCA手动控制、风扇与状态。

构建：`./tools/build_arm64.sh`。部署：`./tools/deploy_test.sh --no-start` 后 `./tools/deploy_test.sh`。标题标记UAV-DETECT-02。板端新版本目录为`/userdata/Apps/RK3568_UAV_Qt_test_<UTC时间戳>`，默认SSH为root@192.168.1.100。

模型说明见`assets/models/drone.json`，边界与数据流见`docs/UAV_DETECT_V2.md`。
