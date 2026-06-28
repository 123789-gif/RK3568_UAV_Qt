# 构建说明

## 依赖布局

默认依赖根目录：

```text
/home/simba/rk3568_deps/
├── jpeg_deps/
├── rga_deps/
├── rknn_api/
└── opencv/
```

也可在 Qt Creator 的构建环境中设置：

```bash
RK3568_DEPS_ROOT=/其他路径/rk3568_deps
```

`RKNN_API_ROOT` 可单独覆盖。

## Qt Creator

选择原有 ARM64 Qt Kit，使用 shadow build，然后执行：

1. 清理所有项目；
2. 执行 qmake；
3. 重新构建所有项目。

链接成功后，`deploy/copy_runtime_assets.sh` 会在构建目录生成：

```text
rk_dual_screen_video
models/*.rknn
config/runtime.env
```

## 命令行

```bash
cd /home/simba/Projects/RK3568_PTZ_Qt
QMAKE_BIN=/home/simba/Qt-5.12.9-arm64/bin/qmake ./tools/build_arm64.sh
```
