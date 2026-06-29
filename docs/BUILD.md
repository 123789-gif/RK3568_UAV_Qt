# ARM64 构建说明

## 1. 已验证环境

虚拟机：

```text
Ubuntu 20.04.6 LTS
x86_64
```

工程目录：

```text
/home/simba/Projects/RK3568_PTZ_Qt
```

Qt ARM64：

```text
qmake: /home/simba/Qt-5.12.9-arm64/bin/qmake
```

交叉编译器：

```text
CC:  /usr/bin/aarch64-linux-gnu-gcc
CXX: /usr/bin/aarch64-linux-gnu-g++
```

依赖目录：

```text
/home/simba/rk3568_deps/
├── jpeg_deps/
├── rga_deps/
├── rknn_api/
└── opencv/
```

可通过 Qt Creator 构建环境覆盖：

```text
RK3568_DEPS_ROOT
RKNN_API_ROOT
```

## 2. Qt Creator 构建

选择已配置的 `RK3568` ARM64 Kit。

启用 Shadow Build，构建目录设置为：

```text
/home/simba/Projects/RK3568_PTZ_Qt/out/arm64-release
```

首次构建、移动源码、修改 `.pro` 或切换依赖后：

```text
构建 → 清理所有项目
构建 → 执行 qmake
构建 → 重新构建所有项目
```

不要复用历史 `build_manual`。

## 3. 命令行构建

当前 `tools/build_arm64.sh` 默认使用源码目录同级的外部构建目录。为了与 Qt Creator 保持一致，明确覆盖 `BUILD_DIR`：

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt

BUILD_DIR="$PWD/out/arm64-release" QMAKE_BIN=/home/simba/Qt-5.12.9-arm64/bin/qmake ./tools/build_arm64.sh
```

注意：该脚本会删除并重建 `BUILD_DIR`，不要将其指向源码或其他需要保留的目录。

## 4. 构建后产物

链接后 `deploy/copy_runtime_assets.sh` 会生成：

```text
out/arm64-release/
├── rk_dual_screen_video
├── models/
│   ├── ped_det.rknn
│   ├── plate_det.rknn
│   ├── plate_reco.rknn
│   └── SHA256SUMS
└── config/
    └── runtime.env
```

模型不嵌入 ELF。

## 5. 必做检查

架构：

```bash
file out/arm64-release/rk_dual_screen_video
```

应包含：

```text
ARM aarch64
```

关键依赖：

```bash
readelf -d out/arm64-release/rk_dual_screen_video   | grep -E 'libdrm|librga|librknnrt|libopencv_core|libopencv_imgproc'
```

应包含：

```text
libdrm.so.2
librga.so.2
librknnrt.so
libopencv_core.so.406
libopencv_imgproc.so.406
```

模型：

```bash
(
  cd out/arm64-release/models
  sha256sum -c SHA256SUMS
)
```

当前模型哈希：

```text
ped_det.rknn    1f4cf6821682a01984af93de74240f206d756e0d980f3601f24751098889dc98
plate_det.rknn  c8f7abd3882539f25fc29589e08127a4c1f9b8241203c78ced1fe1d97a2e1700
plate_reco.rknn cb0f5c4d5609fff3fd70bf8d6360e22c80584c5b494fb7bb326c49b9106ff6c6
```

## 6. 当前链接约束

工程使用：

```qmake
QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
```

原因是当前导入的板端 OpenCV ARM64 动态库包含 TBB、BLAS、LAPACK 等传递依赖，虚拟机链接阶段不完整解析这些板端共享库符号。

这是一项当前已验证但需要严格约束的工程折中：

- 构建成功不代表板端依赖完整；
- 每个发布包必须在板端执行 `check_runtime.sh`；
- 不得省略 `ldd` 检查；
- 后续更换 OpenCV 或 rootfs 时必须重新验证 ABI。

## 7. 常见错误

### `No rule to make target npu/vision.cpp`

源码路径与 `.pro` 不一致。当前文件应位于：

```text
src/npu/vision.cpp
src/npu/vision.h
src/npu/demo_types.h
```

修改后重新执行 qmake。

### `rknn_api.h` 不存在

检查：

```text
/home/simba/rk3568_deps/rknn_api/include/rknn_api.h
```

以及 `RKNN_API_ROOT`。

### `opencv2/opencv.hpp` 不存在

检查：

```text
/home/simba/rk3568_deps/opencv/include/opencv4/opencv2/opencv.hpp
```

### 误编译为 x86_64

必须检查 `file` 输出。Kit 或 qmake 指向桌面 Qt 时，不允许部署。
