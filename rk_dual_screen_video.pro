QT += core gui widgets

CONFIG += c++17
QMAKE_LFLAGS += -Wl,--allow-shlib-undefined

TARGET = rk_dual_screen_video
TEMPLATE = app

SOURCES += \
    src/app/main.cpp \
    src/ui/control_panel_window.cpp \
    src/ui/control_panel_window_video.cpp \
    src/ui/control_panel_window_fpga.cpp \
    src/ui/control_panel_window_ai.cpp \
    src/ui/control_panel_window_violation.cpp \
    src/ui/control_panel_window_wrong_way.cpp \
    src/ui/control_panel_window_ptz.cpp \
    src/ui/control_panel_window_system.cpp \
    src/ui/control_panel_window_style.cpp \
    src/ui/violation_preview_widget.cpp \
    src/ui/wrong_way_preview_widget.cpp \
    src/video/drm_rga_display.cpp \
    src/pcie/pcie_frame_receiver.cpp \
    src/npu/npu_inference_worker.cpp \
    src/npu/pedestrian_violation_engine.cpp \
    src/npu/violation_rule_config.cpp \
    src/npu/vehicle_direction_config.cpp \
    src/npu/vehicle_wrong_way_engine.cpp \
    src/npu/plate_v291.cpp \
    src/npu/vision.cpp \
    src/ptz/ptz_camera_profile.cpp \
    src/ptz/v4l2_device_detector.cpp \
    src/ptz/ptz_capture_worker.cpp \
    src/ptz/ptz_control_worker.cpp \
    src/ptz/visca_serial.cpp \
    src/system/process_signal_bridge.cpp \
    src/system/fan_controller.cpp \
    src/system/system_monitor.cpp \
    src/system/perf_counters.cpp

HEADERS += \
    src/ui/control_panel_window.h \
    src/ui/control_panel_window_internal.h \
    src/ui/violation_preview_widget.h \
    src/ui/wrong_way_preview_widget.h \
    src/video/drm_rga_display.h \
    src/video/video_route_config.h \
    src/pcie/pcie_frame_receiver.h \
    src/pcie/pango_pci_uapi.h \
    src/npu/npu_inference_worker.h \
    src/npu/pedestrian_violation_engine.h \
    src/npu/violation_rule_config.h \
    src/npu/vehicle_direction_config.h \
    src/npu/vehicle_wrong_way_engine.h \
    src/npu/npu_task_config.h \
    src/npu/npu_result_types.h \
    src/npu/demo_types.h \
    src/npu/plate_type_ids.h \
    src/npu/plate_v291.h \
    src/npu/vision.h \
    src/ptz/ptz_camera_profile.h \
    src/ptz/v4l2_device_detector.h \
    src/ptz/ptz_capture_worker.h \
    src/ptz/ptz_control_worker.h \
    src/ptz/visca_serial.hpp \
    src/system/process_signal_bridge.h \
    src/system/fan_controller.h \
    src/system/system_monitor.h \
    src/system/perf_counters.h

# 模块间统一使用 "module/file.h"，避免根目录平铺和同名头文件冲突。
INCLUDEPATH += $$PWD/src

# 交叉编译依赖根目录。可在 Qt Creator 构建环境中覆盖 RK3568_DEPS_ROOT。
RK3568_DEPS_ROOT = $$(RK3568_DEPS_ROOT)
isEmpty(RK3568_DEPS_ROOT) {
    RK3568_DEPS_ROOT = /home/simba/rk3568_deps
}

RGA_ROOT = $$RK3568_DEPS_ROOT/rga_deps
JPEG_ROOT = $$RK3568_DEPS_ROOT/jpeg_deps
OPENCV_ROOT = $$RK3568_DEPS_ROOT/opencv

RKNN_API_ROOT = $$(RKNN_API_ROOT)
isEmpty(RKNN_API_ROOT) {
    RKNN_API_ROOT = $$RK3568_DEPS_ROOT/rknn_api
}

# 不要 INCLUDEPATH += /usr/include，避免污染 aarch64 交叉编译标准库搜索顺序。
QMAKE_CXXFLAGS += -idirafter /usr/include
QMAKE_CXXFLAGS += -idirafter /usr/include/drm
QMAKE_CXXFLAGS += -idirafter /usr/include/libdrm
QMAKE_CXXFLAGS += -idirafter /usr/aarch64-linux-gnu/include/drm
QMAKE_CXXFLAGS += -idirafter $$RGA_ROOT/include/rga

INCLUDEPATH += $$JPEG_ROOT/include
INCLUDEPATH += $$RKNN_API_ROOT/include
INCLUDEPATH += $$OPENCV_ROOT/include/opencv4

LIBS += -L$$JPEG_ROOT/lib
LIBS += -L/usr/lib/aarch64-linux-gnu
LIBS += -L$$RGA_ROOT/lib
LIBS += -L$$RKNN_API_ROOT/aarch64
LIBS += -L$$OPENCV_ROOT/lib
LIBS += -ldrm -lrga -lrknnrt -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -ljpeg -lpthread -ldl

DEFINES += USE_RGA

# 即使误用源码内构建，也把中间文件收拢到构建目录，避免污染源码树。
OBJECTS_DIR = $$OUT_PWD/.obj
MOC_DIR = $$OUT_PWD/.moc
UI_DIR = $$OUT_PWD/.ui
RCC_DIR = $$OUT_PWD/.rcc

# 模型不嵌入可执行文件；链接完成后复制到可执行文件旁的 models/ 目录。
QMAKE_POST_LINK += bash $$PWD/deploy/copy_runtime_assets.sh $$OUT_PWD

DISTFILES += \
    README.md \
    assets/models/ped_det.rknn \
    assets/models/plate_det.rknn \
    assets/models/plate_reco.rknn \
    assets/models/yolov8_car_640x320_mmse.rknn \
    assets/models/car_classes.txt \
    assets/models/SHA256SUMS \
    assets/models/v291_candidate/plate_det.rknn \
    assets/models/v291_candidate/plate_reco.rknn \
    assets/models/v291_candidate/SHA256SUMS \
    assets/config/runtime.env \
    assets/config/pedestrian_violation.ini \
    assets/driver/rk3568-kernel-6.1.99/pango_pci_driver.ko \
    assets/driver/rk3568-kernel-6.1.99/SHA256SUMS \
    assets/driver/rk3568-kernel-6.1.99/DRIVER_INFO.txt \
    deploy/copy_runtime_assets.sh \
    deploy/scripts/load_pcie_driver.sh \
    deploy/scripts/setup_display.sh \
    deploy/scripts/check_runtime.sh \
    deploy/scripts/start.sh \
    deploy/scripts/stop.sh \
    deploy/systemd/rk3568-ptz-qt.service \
    docs/BUILD.md \
    docs/DEPLOY.md \
    docs/ARCHITECTURE.md \
    docs/UI_REFACTOR.md \
    docs/PLATE_ORIGINAL_ROI_EXPERIMENT.md \
    docs/VEHICLE_WRONG_WAY_INTEGRATION.md \
    docs/SOURCE_VALIDATION_20260725.txt

OTHER_FILES += \
    tests/plate_mode_config_test.cpp \
    tests/vehicle_wrong_way_engine_test.cpp \
    tools/test_plate_modes_host.sh \
    tools/test_vehicle_wrong_way_host.sh
