#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include <rknn_api.h>

namespace demo {

// 跨模块统一使用的 OpenCV 类型别名，简化函数签名。
using cv::Mat;
using cv::Point;
using cv::Point2f;
using cv::Rect;
using cv::Rect2f;
using cv::Scalar;
using cv::Size;

// 来自 PCIe 的原始采集分辨率（1080p60）。
constexpr int IMAGE_WIDTH = 1920;
constexpr int IMAGE_HEIGHT = 1080;

// 检测模型输入尺寸（采用 letterbox 填充）。
// constexpr int DET_INPUT_W = 640;
// constexpr int DET_INPUT_H = 640;
constexpr int DET_INPUT_W = 768;
constexpr int DET_INPUT_H = 448;
constexpr int DET_PAD_VALUE = 255;

// 默认运行阈值（车牌与行人分支独立）。
constexpr float DEFAULT_PLATE_CONF = 0.45f;
constexpr float DEFAULT_PLATE_IOU = 0.6f;
constexpr float DEFAULT_PED_CONF = 0.4f;
constexpr float DEFAULT_PED_IOU = 0.25f;
constexpr int DEFAULT_PED_HOLD = 3;
constexpr int DEFAULT_PED_TOPK_DECODE = 256;
constexpr int DEFAULT_PED_TOPK_NMS = 128;
constexpr int DEFAULT_PED_ROUTE_BASE = 5;
constexpr int DEFAULT_PED_ROUTE_MID = 6;
constexpr int DEFAULT_PED_ROUTE_MAX = 8;
constexpr int DEFAULT_PED_CACHE_TTL_MS = 1000;
constexpr int DEFAULT_PLATE_ROUTE_FPS = 30;
constexpr int DEFAULT_PLATE_OCR_BUDGET = 4;
constexpr int DEFAULT_PLATE_PIPELINE_INFLIGHT = 3;
constexpr int DEFAULT_PLATE_CACHE_TTL_MS = 250;
constexpr float DEFAULT_PLATE_BOX_EMA = 0.45f;
constexpr int DEFAULT_PLATE_TEXT_VOTE_N = 5;
constexpr int DEFAULT_PLATE_COLOR_HOLD = 2;
constexpr int DEFAULT_LIGHT_VOTE_WINDOW = 5;
constexpr int DEFAULT_LIGHT_VOTE_MIN = 3;
constexpr int DEFAULT_LIGHT_UNKNOWN_HOLD_MS = 300;
constexpr int DEFAULT_PED_TRACK_TTL_MS = 250;
constexpr float DEFAULT_PED_BOX_EMA = 0.5f;
constexpr bool DEFAULT_SHOW_LIGHT_ROI = false;
constexpr bool DEFAULT_SHOW_CROSSWALK = false;
constexpr bool DEFAULT_SHOW_PED_RADAR = false;
constexpr bool DEFAULT_SHOW_METRICS = true;
constexpr bool DEFAULT_DISPLAY_GUARD = true;
constexpr float DEFAULT_DISPLAY_GUARD_MIN_FPS = 58.0f;
constexpr float DEFAULT_DISPLAY_GUARD_LATENCY_MS = 15.5f;
constexpr int DEFAULT_VIDEO_TILE = 0;
constexpr int USB_VIDEO_TILE = 10;
constexpr int DEFAULT_VIDEO_STREAM_PORT = 8090;
constexpr int DEFAULT_VIDEO_STREAM_FPS = 10;
constexpr int DEFAULT_VIDEO_JPEG_QUALITY = 75;
constexpr int DEFAULT_USB_VIDEO_WIDTH = 1920;
constexpr int DEFAULT_USB_VIDEO_HEIGHT = 1080;
constexpr int DEFAULT_USB_VIDEO_FPS = 30;
constexpr bool DEFAULT_CLOUD_REPORT = false;
constexpr int DEFAULT_CLOUD_STREAM_PORT = 8091;
constexpr int DEFAULT_CLOUD_STREAM_FPS = 5;
constexpr int DEFAULT_CLOUD_JPEG_QUALITY = 70;
constexpr int DEFAULT_CLOUD_SWITCH_MS = 3000;

// 车牌颜色类别顺序必须与训练/导出的检测模型保持一致。
constexpr int PLATE_CLASS_BLUE = 0;
constexpr int PLATE_CLASS_GREEN = 1;
constexpr int PLATE_CLASS_YELLOW = 2;
constexpr int PLATE_CLASS_POLICE = 3;
constexpr int PLATE_CLASS_CONSULATE = 4;
constexpr int PLATE_CLASS_COUNT = 5;

// 同帧三模型（车牌检测+行人检测+车牌识别）端到端硬时限（毫秒）。
constexpr double SAME_FRAME_DEADLINE_MS = 60.0;

// 单个已加载模型的 RKNN 运行时句柄与张量属性缓存。
struct RknnModel {
    rknn_context ctx = 0;
    int num_in = 0;
    int num_out = 0;
    rknn_tensor_attr* in_attrs = nullptr;
    rknn_tensor_attr* out_attrs = nullptr;
};

// 启动时一次性解析的命令行运行参数。
struct RuntimeOptions {
    std::string plate_det_model;
    std::string plate_reco_model;
    std::string ped_model;
    std::string pcie_dev = "/dev/pango_pci_driver";
    std::string drm_card = "/dev/dri/card0";
    std::string cfg_file;
    std::string ctrl_fifo = "/tmp/app_pcie_ctrl";
    std::string ped_roi_file;
    std::string osd_font_path;
    int drm_mode_w = 0;
    int drm_mode_h = 0;
    int drm_refresh_hz = 60;
    int ped_classes = 1;
    float plate_conf = DEFAULT_PLATE_CONF;
    float plate_iou = DEFAULT_PLATE_IOU;
    float ped_conf = DEFAULT_PED_CONF;
    float ped_iou = DEFAULT_PED_IOU;
    int ped_hold = DEFAULT_PED_HOLD;
    int ped_topk_decode = DEFAULT_PED_TOPK_DECODE;
    int ped_topk_nms = DEFAULT_PED_TOPK_NMS;
    int ped_route_base = DEFAULT_PED_ROUTE_BASE;
    int ped_route_mid = DEFAULT_PED_ROUTE_MID;
    int ped_route_max = DEFAULT_PED_ROUTE_MAX;
    int ped_cache_ttl_ms = DEFAULT_PED_CACHE_TTL_MS;
    int plate_route_fps = DEFAULT_PLATE_ROUTE_FPS;
    int plate_ocr_budget = DEFAULT_PLATE_OCR_BUDGET;
    int plate_pipeline_inflight = DEFAULT_PLATE_PIPELINE_INFLIGHT;
    int plate_cache_ttl_ms = DEFAULT_PLATE_CACHE_TTL_MS;
    float plate_box_ema = DEFAULT_PLATE_BOX_EMA;
    int plate_text_vote_n = DEFAULT_PLATE_TEXT_VOTE_N;
    int plate_color_hold = DEFAULT_PLATE_COLOR_HOLD;
    int light_vote_window = DEFAULT_LIGHT_VOTE_WINDOW;
    int light_vote_min = DEFAULT_LIGHT_VOTE_MIN;
    int ped_track_ttl_ms = DEFAULT_PED_TRACK_TTL_MS;
    float ped_box_ema = DEFAULT_PED_BOX_EMA;
    bool ped_on_start = false;
    bool osd_on = true;
    bool pio_no_start = false;
    bool preproc_force_cpu = false;
    bool display_guard_on = DEFAULT_DISPLAY_GUARD;
    float display_guard_min_fps = DEFAULT_DISPLAY_GUARD_MIN_FPS;
    float display_guard_latency_ms = DEFAULT_DISPLAY_GUARD_LATENCY_MS;
    bool plate_stable_on = true;
    bool light_stable_on = true;
    bool ped_track_stable_on = true;
    bool show_light_roi = DEFAULT_SHOW_LIGHT_ROI;
    bool show_crosswalk = DEFAULT_SHOW_CROSSWALK;
    bool show_ped_radar = DEFAULT_SHOW_PED_RADAR;
    bool show_metrics = DEFAULT_SHOW_METRICS;
    int video_tile = DEFAULT_VIDEO_TILE;
    int video_stream_port = DEFAULT_VIDEO_STREAM_PORT;
    int video_stream_fps = DEFAULT_VIDEO_STREAM_FPS;
    int video_jpeg_quality = DEFAULT_VIDEO_JPEG_QUALITY;
    std::string usb_video_dev = "/dev/video0";
    int usb_video_width = DEFAULT_USB_VIDEO_WIDTH;
    int usb_video_height = DEFAULT_USB_VIDEO_HEIGHT;
    int usb_video_fps = DEFAULT_USB_VIDEO_FPS;
    bool cloud_report = DEFAULT_CLOUD_REPORT;
    int cloud_stream_port = DEFAULT_CLOUD_STREAM_PORT;
    int cloud_stream_fps = DEFAULT_CLOUD_STREAM_FPS;
    int cloud_jpeg_quality = DEFAULT_CLOUD_JPEG_QUALITY;
    int cloud_switch_ms = DEFAULT_CLOUD_SWITCH_MS;
};

// 运行时可通过键盘动态更新的阈值。
// 使用原子变量，工作线程读取时无需额外加锁。
struct Thresholds {
    std::atomic<float> plate_conf{DEFAULT_PLATE_CONF};
    std::atomic<float> plate_iou{DEFAULT_PLATE_IOU};
    std::atomic<float> ped_conf{DEFAULT_PED_CONF};
    std::atomic<float> ped_iou{DEFAULT_PED_IOU};
};

// 行人规则引擎使用的信号灯状态。
enum class TrafficLightState { Unknown, Red, Green };

// 行人状态分类结果，用于画面叠加与控制台输出。
enum class PedStatus { Unknown, Normal, Outside, RedAccum, RedViolation };

// 行人分支所需的用户配置区域。
struct PedRegions {
    Rect light_roi;
    std::vector<Point> crosswalk_quad;
    bool valid = false;
};

// 原图坐标系下的单个行人候选框。
struct PedBox {
    Rect2f box;
    float score = 0.0f;
    bool intersects = false;
    float overlap_ratio = 0.0f;
    PedStatus status = PedStatus::Unknown;
};

// 行人分支最近一次结果快照。
struct PedResult {
    uint64_t frame_id = 0;
    bool valid = false;
    TrafficLightState light = TrafficLightState::Unknown;
    int red_streak = 0;
    double red_overlap_ms = 0.0;
    Rect light_roi;
    std::vector<Point> crosswalk_quad;
    std::vector<PedBox> boxes;
};

// 发送给行人工作线程的任务负载。
struct PedTask {
    uint64_t frame_id = 0;
    std::shared_ptr<std::vector<uint8_t>> frame_rgb565;
    std::shared_ptr<std::vector<uint8_t>> input_rgb;
    int frame_w = IMAGE_WIDTH;
    int frame_h = IMAGE_HEIGHT;
    int input_w = DET_INPUT_W;
    int input_h = DET_INPUT_H;
    bool pre_used_rga = false;
    double pre_ms = 0.0;
    PedRegions regions;
    uint64_t epoch = 0;
    int orig_w = IMAGE_WIDTH;
    int orig_h = IMAGE_HEIGHT;
    int64_t source_ts_ms = 0;
    double hold_ms = 0.0;
};

// 展示层复用的行人结果缓存。
struct PedCacheState {
    PedResult result;
    bool has_result = false;
    uint64_t source_frame_id = 0;
    uint64_t source_epoch = 0;
    int64_t source_ts_ms = 0;
    double age_ms = 0.0;
};

// 行人调度与线程统计。
struct PedSchedulerStats {
    uint64_t enqueue_count = 0;
    uint64_t drop_count = 0;
    uint64_t processed_count = 0;
    double ped_ms_sum = 0.0;
    double e2e_ms_sum = 0.0;
    double npu_ms_sum = 0.0;
    double input_ms_sum = 0.0;
    double output_ms_sum = 0.0;
    double post_ms_sum = 0.0;
    uint64_t npu_run_count = 0;
    uint64_t result_publish_count = 0;
    uint64_t pre_count = 0;
    uint64_t pre_rga_count = 0;
    uint64_t pre_cpu_fallback_count = 0;
    double pre_ms_sum = 0.0;
    int current_route_n = DEFAULT_PED_ROUTE_BASE;
};

// Shared preprocessing statistics. One 1080p->768x448 input can feed both plate
// and pedestrian workers, so this stage is counted once instead of per branch.
struct InferPreprocessStats {
    uint64_t processed_count = 0;
    uint64_t rga_count = 0;
    uint64_t cpu_fallback_count = 0;
    double pre_ms_sum = 0.0;
};

// 车牌分支的推理任务，使用 latest-only 调度以减少显示链路阻塞。
struct PlateTask {
    uint64_t frame_id = 0;
    uint64_t epoch = 0;
    std::shared_ptr<std::vector<uint8_t>> frame_rgb565;
    std::shared_ptr<std::vector<uint8_t>> input_rgb;
    int frame_w = IMAGE_WIDTH;
    int frame_h = IMAGE_HEIGHT;
    int input_w = DET_INPUT_W;
    int input_h = DET_INPUT_H;
    bool pre_used_rga = false;
    double pre_ms = 0.0;
    uint64_t source_ts_ms = 0;
};

// 车牌分支的最近一次有效结果，用于主画面持久化叠加。
struct PlateCacheState {
    uint64_t frame_id = 0;
    uint64_t source_ts_ms = 0;
    bool valid = false;
    std::vector<std::string> plates;
    std::vector<std::string> pinyins;
    std::vector<Rect2f> boxes;
    // 原图坐标框由车牌 worker 更新，显示线程直接缩放绘制，避免每帧重复反算 letterbox。
    std::vector<Rect2f> orig_boxes;
    std::vector<int> color_ids;
};

// 车牌 worker 的运行统计。
struct PlateWorkerStats {
    uint64_t enqueue_count = 0;
    uint64_t drop_count = 0;
    uint64_t processed_count = 0;
    double proc_ms_sum = 0.0;
    double e2e_ms_sum = 0.0;
    double npu_ms_sum = 0.0;
    double input_ms_sum = 0.0;
    double output_ms_sum = 0.0;
    double post_ms_sum = 0.0;
    uint64_t npu_run_count = 0;
    uint64_t result_publish_count = 0;
    uint64_t pre_count = 0;
    uint64_t pre_rga_count = 0;
    uint64_t pre_cpu_fallback_count = 0;
    double pre_ms_sum = 0.0;
    uint64_t ocr_run_count = 0;
    uint64_t ocr_reuse_count = 0;
    uint64_t ocr_batch_count = 0;
    double ocr_ms_sum = 0.0;
};

// 车牌 CTC 类解码所用字符表。
inline const std::vector<std::string>& chars() {
    static const std::vector<std::string> v = {
        "京","沪","津","渝","冀","晋","蒙","辽","吉","黑",
        "苏","浙","皖","闽","赣","鲁","豫","鄂","湘","粤",
        "桂","琼","川","贵","云","藏","陕","甘","青","宁",
        "新","警","领",
        "0","1","2","3","4","5","6","7","8","9",
        "A","B","C","D","E","F","G","H","J","K",
        "L","M","N","P","Q","R","S","T","U","V",
        "W","X","Y","Z","-"
    };
    return v;
}

// 当前实现中拼音表与 chars() 保持一致。
inline const std::vector<std::string>& pinyin_chars() {
    static const std::vector<std::string> p = {
        "JING","HU","JIN","YU","JI","JIN2","MENG","LIAO","JI2","HEI",
        "SU","ZHE","WAN","MIN","GAN","LU","YU2","E","XIANG","YUE",
        "GUI","QIONG","CHUAN","GUI2","YUN","ZANG","SHAN","GAN2","QING","NING",
        "XIN","JINGPOL","LINGCON",
        "0","1","2","3","4","5","6","7","8","9",
        "A","B","C","D","E","F","G","H","J","K",
        "L","M","N","P","Q","R","S","T","U","V",
        "W","X","Y","Z","-"
    };
    return p;
}

inline const std::vector<std::string>& class_names() {
    static const std::vector<std::string> v = {"Blue", "Green", "Yellow", "Police", "Consulate"};
    return v;
}

// 各类别对应的绘制颜色（与 class_names() 顺序一致）。
inline const std::vector<Scalar>& class_colors() {
    static const std::vector<Scalar> v = {
        Scalar(255, 0, 0), Scalar(0, 255, 0), Scalar(0, 255, 255), Scalar(0, 0, 255), Scalar(0, 0, 0)
    };
    return v;
}

}  // namespace demo
