# ARM64构建说明

## 1. 已验证环境

```text
Host: Ubuntu 20.04.6 LTS x86_64
Project: /home/simba/Projects/RK3568_PTZ_Qt
qmake: /home/simba/Qt-5.12.9-arm64/bin/qmake
CC: /usr/bin/aarch64-linux-gnu-gcc
CXX: /usr/bin/aarch64-linux-gnu-g++
Dependencies: /home/simba/rk3568_deps
```

依赖目录：

```text
/home/simba/rk3568_deps/
├── jpeg_deps/
├── rga_deps/
├── rknn_api/
└── opencv/
```

可通过环境变量覆盖：

```text
RK3568_DEPS_ROOT
RKNN_API_ROOT
QMAKE_BIN
```

## 2. Qt Creator构建

选择RK3568 ARM64 Kit，启用Shadow Build，构建目录固定为：

```text
/home/simba/Projects/RK3568_PTZ_Qt/out/arm64-release
```

修改源码后执行：

```text
构建 → 重新构建所有项目
```

修改`.pro`、移动源码、切换Kit或更换依赖后执行：

```text
清理所有项目 → 执行qmake → 重新构建所有项目
```

## 3. 命令行构建

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
./tools/build_arm64.sh
```

脚本默认：

- 输出到`out/arm64-release`；
- 删除并重建该构建目录；
- 拒绝删除工程`out/`之外的路径；
- 使用`/home/simba/Qt-5.12.9-arm64/bin/qmake`。

增量构建：

```bash
CLEAN_BUILD=0 ./tools/build_arm64.sh
```

覆盖并行任务数：

```bash
JOBS=4 ./tools/build_arm64.sh
```

## 4. 必做检查

```bash
BIN=/home/simba/Projects/RK3568_PTZ_Qt/out/arm64-release/rk_dual_screen_video

file "$BIN"
readelf -d "$BIN" |
  grep -E 'libdrm|librga|librknnrt|libopencv_core|libopencv_imgproc|libjpeg|libQt5'

(
  cd /home/simba/Projects/RK3568_PTZ_Qt/out/arm64-release/models
  sha256sum -c SHA256SUMS
)
```

`file`必须包含：

```text
ARM aarch64
```

## 5. 模型状态

| 文件 | 当前状态 |
| --- | --- |
| `ped_det.rknn` | 行人检测和行人违法规则使用 |
| `plate_det.rknn` | 车牌检测使用 |
| `plate_reco.rknn` | 车牌识别使用 |
| `yolov8_car.rknn` | 保留的实验资产，当前代码不加载 |

模型和驱动不嵌入ELF，由`deploy/copy_runtime_assets.sh`复制到构建输出目录。

## 6. 链接约束

工程当前包含：

```qmake
QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
```

这是对板端OpenCV传递依赖的工程折中。构建成功不代表板端动态库完整，每次发布必须由`deploy_test.sh`在板端执行`check_runtime.sh`和`ldd`检查。

## 7. 禁止事项

- 不部署x86_64 ELF；
- 不复用来源不明的旧构建目录；
- 不从`dist/`反向复制文件覆盖源码；
- 不跳过模型SHA256检查；
- 不把`BUILD_DIR`指向工程源码、用户主目录或其他需要保留的目录。
