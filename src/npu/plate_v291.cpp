#include "npu/plate_v291.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <utility>

#ifdef USE_RGA
#include "RgaUtils.h"
#include <RgaApi.h>
#include "im2d.h"
#include "rga.h"
#endif

namespace demo {
namespace plate_v291 {

using std::lock_guard;
using std::string;
using std::uint64_t;
using std::vector;
using std::memset;

static void get_scaling_info(int ow, int oh, int& nw, int& nh, int& sx, int& sy);

static double ms_since(const std::chrono::high_resolution_clock::time_point& start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::high_resolution_clock::now() - start)
        .count();
}

static double steady_ms_now() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static std::mutex g_rknn_run_interval_mutex;
static std::deque<RknnRunInterval> g_rknn_run_intervals;
static uint64_t g_rknn_run_seq = 0;
static std::atomic<bool> g_plate_quad_compensate{DEFAULT_PLATE_QUAD_COMPENSATE};

static void record_rknn_run_interval(int branch, double start_ms, double end_ms) {
    lock_guard<std::mutex> lk(g_rknn_run_interval_mutex);
    RknnRunInterval item;
    item.seq = ++g_rknn_run_seq;
    item.branch = branch;
    item.start_ms = start_ms;
    item.end_ms = end_ms;
    g_rknn_run_intervals.push_back(item);
    while (g_rknn_run_intervals.size() > 4096) {
        g_rknn_run_intervals.pop_front();
    }
}

void collect_rknn_run_intervals(uint64_t after_seq,
                                uint64_t* latest_seq,
                                std::vector<RknnRunInterval>* out) {
    lock_guard<std::mutex> lk(g_rknn_run_interval_mutex);
    if (latest_seq) *latest_seq = g_rknn_run_seq;
    if (!out) return;
    out->clear();
    for (const auto& item : g_rknn_run_intervals) {
        if (item.seq > after_seq) out->push_back(item);
    }
}

void set_plate_quad_compensation_enabled(bool enabled) {
    g_plate_quad_compensate.store(enabled, std::memory_order_release);
}

bool plate_quad_compensation_enabled() {
    return g_plate_quad_compensate.load(std::memory_order_acquire);
}

constexpr int PLATE_TOPK_DECODE = 256;
constexpr int PLATE_TOPK_NMS = 64;

#ifdef USE_RGA
// 使用 RGA 加速缩放，失败或不满足条件时回退 CPU。
static bool rga_resize(const Mat& src, Mat& dst, bool rgb = false) {
    if (src.empty() || dst.empty()) return false;
    if (src.cols % 4 != 0 || dst.cols % 4 != 0) {
        cv::resize(src, dst, dst.size());
        if (rgb) cv::cvtColor(dst, dst, cv::COLOR_BGR2RGB);
        return true;
    }
    int format = rgb ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888;
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(reinterpret_cast<void*>(const_cast<uchar*>(src.data)), src.cols, src.rows, format);
    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(reinterpret_cast<void*>(dst.data), dst.cols, dst.rows, format);
    return imresize(src_buf, dst_buf) == IM_STATUS_SUCCESS;
}

// 使用 RGA 加速 BGR->RGB，失败或不满足条件时回退 CPU。
static bool rga_cvt_bgr_to_rgb(const Mat& bgr, Mat& rgb) {
    if (bgr.empty()) return false;
    if (rgb.rows != bgr.rows || rgb.cols != bgr.cols || rgb.type() != CV_8UC3) rgb.create(bgr.rows, bgr.cols, CV_8UC3);
    if (bgr.cols % 4 != 0) {
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        return true;
    }
    rga_buffer_t src = wrapbuffer_virtualaddr(reinterpret_cast<void*>(const_cast<uchar*>(bgr.data)), bgr.cols, bgr.rows, RK_FORMAT_BGR_888);
    rga_buffer_t dst = wrapbuffer_virtualaddr(reinterpret_cast<void*>(rgb.data), rgb.cols, rgb.rows, RK_FORMAT_RGB_888);
    return imcvtcolor(src, dst, RK_FORMAT_BGR_888, RK_FORMAT_RGB_888) == IM_STATUS_SUCCESS;
}

enum class RgaRgb565ToRgbMode {
    None = 0,
    Blit,
    ImResize,
    BlitBgrOut,
    ImResizeBgrOut,
    Hybrid,
};

static const char* rga_preproc_mode_name(RgaRgb565ToRgbMode mode) {
    switch (mode) {
        case RgaRgb565ToRgbMode::Blit: return "rga_blit_565_to_888";
        case RgaRgb565ToRgbMode::ImResize: return "imresize_565_to_888";
        case RgaRgb565ToRgbMode::BlitBgrOut: return "rga_blit_565_to_bgr888";
        case RgaRgb565ToRgbMode::ImResizeBgrOut: return "imresize_565_to_bgr888";
        case RgaRgb565ToRgbMode::Hybrid: return "rga_resize_565_cpu_convert";
        default: return "cpu";
    }
}

// RGA 只负责 RGB565 同格式缩放，不做 RGB888 转色。
// 当板端 RGB565->RGB888 转色自检不通过时，使用这条安全路径减轻 CPU 缩放压力。
static bool rga_resize_rgb565_to_rgb565(const uint8_t* src_ptr,
                                        int src_w,
                                        int src_h,
                                        Mat& dst_565) {
    if (!src_ptr || src_w <= 0 || src_h <= 0 || dst_565.empty()) return false;
    if (dst_565.type() != CV_8UC2 || !dst_565.isContinuous()) return false;
    // RK3568 BSP 的 RGA 虚拟地址路径要求像素宽度按 4 对齐。未对齐时
    // 部分 librga 版本不会可靠拒绝请求，而可能按对齐 stride 写越界。
    if ((src_w & 3) != 0 || (dst_565.cols & 3) != 0) return false;
    rga_buffer_t src = wrapbuffer_virtualaddr(const_cast<uint8_t*>(src_ptr),
                                              src_w,
                                              src_h,
                                              RK_FORMAT_RGB_565);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_565.data,
                                              dst_565.cols,
                                              dst_565.rows,
                                              RK_FORMAT_RGB_565);
    return imresize(src, dst) == IM_STATUS_SUCCESS;
}

// 使用 librga 旧接口完成 RGB565 -> RGB888 缩放转色。
// 这条路径与显示侧 RGB565 blit 口径更接近，部分 RK3568 BSP 上比 imresize 更稳定。
static bool rga_blit_565_to_888(const uint8_t* src_ptr,
                                int src_w,
                                int src_h,
                                Mat& dst_888,
                                int dst_format = RK_FORMAT_RGB_888) {
    if (!src_ptr || src_w <= 0 || src_h <= 0 || dst_888.empty()) return false;
    if (dst_888.type() != CV_8UC3 || !dst_888.isContinuous()) return false;
    if ((src_w & 3) != 0 || (dst_888.cols & 3) != 0) return false;
    rga_info_t src {};
    rga_info_t dst {};
    src.fd = -1;
    src.mmuFlag = 1;
    src.virAddr = const_cast<uint8_t*>(src_ptr);
    src.rotation = 0;
    rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, RK_FORMAT_RGB_565);

    dst.fd = -1;
    dst.mmuFlag = 1;
    dst.virAddr = dst_888.data;
    dst.rotation = 0;
    rga_set_rect(&dst.rect, 0, 0, dst_888.cols, dst_888.rows, dst_888.cols, dst_888.rows, dst_format);
    return c_RkRgaBlit(&src, &dst, nullptr) == 0;
}

// 使用 IM2D imresize 完成 RGB565 -> RGB888 缩放转色，作为全 RGA 的第二候选。
static bool rga_imresize_565_to_888(const uint8_t* src_ptr,
                                    int src_w,
                                    int src_h,
                                    Mat& dst_888,
                                    int dst_format = RK_FORMAT_RGB_888) {
    if (!src_ptr || src_w <= 0 || src_h <= 0 || dst_888.empty()) return false;
    if (dst_888.type() != CV_8UC3 || !dst_888.isContinuous()) return false;
    if ((src_w & 3) != 0 || (dst_888.cols & 3) != 0) return false;
    rga_buffer_t src = wrapbuffer_virtualaddr(const_cast<uint8_t*>(src_ptr),
                                              src_w,
                                              src_h,
                                              RK_FORMAT_RGB_565);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_888.data,
                                              dst_888.cols,
                                              dst_888.rows,
                                              dst_format);
    return imresize(src, dst) == IM_STATUS_SUCCESS;
}

static bool rga_preproc_apply(RgaRgb565ToRgbMode mode,
                              const uint8_t* src_ptr,
                              int src_w,
                              int src_h,
                              Mat& dst_888) {
    switch (mode) {
        case RgaRgb565ToRgbMode::Blit:
            return rga_blit_565_to_888(src_ptr, src_w, src_h, dst_888);
        case RgaRgb565ToRgbMode::ImResize:
            return rga_imresize_565_to_888(src_ptr, src_w, src_h, dst_888);
        case RgaRgb565ToRgbMode::BlitBgrOut:
            return rga_blit_565_to_888(src_ptr, src_w, src_h, dst_888, RK_FORMAT_BGR_888);
        case RgaRgb565ToRgbMode::ImResizeBgrOut:
            return rga_imresize_565_to_888(src_ptr, src_w, src_h, dst_888, RK_FORMAT_BGR_888);
        default:
            return false;
    }
}
#endif

// 复刻旧主程序的手写 RGB565 -> RGB888 口径，作为 RGA 预处理的可靠对照路径。
static void cpu_rgb565_to_rgb888(const uint8_t* src_ptr, int src_w, int src_h, Mat& full_rgb) {
    full_rgb.create(src_h, src_w, CV_8UC3);
    uint8_t* out = full_rgb.data;
    size_t pixels = static_cast<size_t>(src_w) * static_cast<size_t>(src_h);
    for (size_t i = 0; i < pixels; ++i) {
        uint16_t px = static_cast<uint16_t>(src_ptr[i * 2]) |
                      static_cast<uint16_t>(src_ptr[i * 2 + 1] << 8);
        out[i * 3 + 0] = static_cast<uint8_t>(((px >> 11) & 0x1F) << 3);
        out[i * 3 + 1] = static_cast<uint8_t>(((px >> 5) & 0x3F) << 2);
        out[i * 3 + 2] = static_cast<uint8_t>((px & 0x1F) << 3);
    }
}

static void cpu_resize_rgb565_to_rgb(const uint8_t* src_ptr,
                                     int src_w,
                                     int src_h,
                                     int dst_w,
                                     int dst_h,
                                     Mat& dst_rgb) {
    static thread_local Mat full_rgb;
    cpu_rgb565_to_rgb888(src_ptr, src_w, src_h, full_rgb);
    if (dst_rgb.cols != dst_w || dst_rgb.rows != dst_h || dst_rgb.type() != CV_8UC3) {
        dst_rgb.create(dst_h, dst_w, CV_8UC3);
    }
    cv::resize(full_rgb, dst_rgb, Size(dst_w, dst_h));
}

static void cpu_convert_rgb565_to_rgb(const Mat& src_565, Mat& dst_rgb) {
    if (src_565.empty() || src_565.type() != CV_8UC2) {
        dst_rgb.release();
        return;
    }
    cpu_rgb565_to_rgb888(src_565.data, src_565.cols, src_565.rows, dst_rgb);
}

static void make_letterbox_rgb(const Mat& resized_rgb,
                               int sx,
                               int sy,
                               int nw,
                               int nh,
                               Mat& out_rgb) {
    out_rgb.create(DET_INPUT_H, DET_INPUT_W, CV_8UC3);
    out_rgb.setTo(Scalar(DET_PAD_VALUE, DET_PAD_VALUE, DET_PAD_VALUE));
    resized_rgb.copyTo(out_rgb(Rect(sx, sy, nw, nh)));
}

#ifdef USE_RGA
struct RgaPreprocSelfTest {
    RgaRgb565ToRgbMode mode = RgaRgb565ToRgbMode::None;
    bool done = false;
    double mean_abs_diff = 999.0;
};

static bool rga_hybrid_preproc(const uint8_t* src_ptr,
                               int src_w,
                               int src_h,
                               Mat& resized_rgb,
                               Mat& resized_565) {
    if (resized_565.cols != resized_rgb.cols ||
        resized_565.rows != resized_rgb.rows ||
        resized_565.type() != CV_8UC2) {
        resized_565.create(resized_rgb.rows, resized_rgb.cols, CV_8UC2);
    }
    if (!rga_resize_rgb565_to_rgb565(src_ptr, src_w, src_h, resized_565)) return false;
    cpu_convert_rgb565_to_rgb(resized_565, resized_rgb);
    return !resized_rgb.empty();
}

static double rgb565_preproc_mean_diff(const Mat& ref_rgb, const Mat& rga_rgb) {
    if (ref_rgb.empty() || rga_rgb.empty() || ref_rgb.size() != rga_rgb.size() ||
        ref_rgb.type() != CV_8UC3 || rga_rgb.type() != CV_8UC3) {
        return 999.0;
    }
    double sum = 0.0;
    size_t count = ref_rgb.total() * 3;
    const uint8_t* a = ref_rgb.ptr<uint8_t>(0);
    const uint8_t* b = rga_rgb.ptr<uint8_t>(0);
    for (size_t i = 0; i < count; ++i) {
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return count ? (sum / static_cast<double>(count)) : 999.0;
}

static bool rga_preproc_mode_works(RgaRgb565ToRgbMode mode,
                                   const uint8_t* src_ptr,
                                   int src_w,
                                   int src_h,
                                   int dst_w,
                                   int dst_h,
                                   const Mat& ref_rgb,
                                   double& diff) {
    Mat rga_rgb(dst_h, dst_w, CV_8UC3);
    bool ok = false;
    if (mode == RgaRgb565ToRgbMode::Hybrid) {
        Mat resized_565(dst_h, dst_w, CV_8UC2);
        ok = rga_hybrid_preproc(src_ptr, src_w, src_h, rga_rgb, resized_565);
    } else {
        ok = rga_preproc_apply(mode, src_ptr, src_w, src_h, rga_rgb);
    }
    diff = ok ? rgb565_preproc_mean_diff(ref_rgb, rga_rgb) : 999.0;
    return ok && diff < 8.0;
}

// 首次预处理时按真实 1080p->768x432 尺寸做轻量自检，并缓存可用路径。
static RgaRgb565ToRgbMode select_rga_preproc_mode() {
    static RgaPreprocSelfTest state;
    static std::mutex self_test_mutex;
    lock_guard<std::mutex> lk(self_test_mutex);
    if (state.done) return state.mode;

    const int sw = IMAGE_WIDTH;
    const int sh = IMAGE_HEIGHT;
    int dw = 0;
    int dh = 0;
    int sx = 0;
    int sy = 0;
    get_scaling_info(sw, sh, dw, dh, sx, sy);
    std::vector<uint8_t> pattern(static_cast<size_t>(sw) * sh * 2);
    auto put_rgb565 = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint16_t px = static_cast<uint16_t>(((r & 0xF8) << 8) |
                                            ((g & 0xFC) << 3) |
                                            (b >> 3));
        size_t off = (static_cast<size_t>(y) * sw + x) * 2;
        pattern[off] = static_cast<uint8_t>(px & 0xFF);
        pattern[off + 1] = static_cast<uint8_t>(px >> 8);
    };
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            int r = (x * 255) / std::max(1, sw - 1);
            int g = (y * 255) / std::max(1, sh - 1);
            int b = ((x + y) * 127) / std::max(1, sw + sh - 2);
            if (y > sh / 3 && y < sh * 2 / 3) {
                int band = (x * 5) / sw;
                switch (band) {
                    case 0: r = 32;  g = 96;  b = 220; break;  // 蓝牌近似色
                    case 1: r = 30;  g = 180; b = 60;  break;  // 绿牌近似色
                    case 2: r = 230; g = 205; b = 30;  break;  // 黄牌近似色
                    case 3: r = 220; g = 220; b = 220; break;  // 白/警牌近似色
                    default:r = 180; g = 70;  b = 40;  break;
                }
            }
            put_rgb565(x, y, static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
        }
    }

    Mat ref_rgb;
    cpu_resize_rgb565_to_rgb(pattern.data(), sw, sh, dw, dh, ref_rgb);

    const RgaRgb565ToRgbMode candidates[] = {
        RgaRgb565ToRgbMode::Blit,
        RgaRgb565ToRgbMode::ImResize,
        RgaRgb565ToRgbMode::BlitBgrOut,
        RgaRgb565ToRgbMode::ImResizeBgrOut,
        RgaRgb565ToRgbMode::Hybrid,
    };
    RgaRgb565ToRgbMode first_full_mode = RgaRgb565ToRgbMode::None;
    double first_full_diff = 999.0;
    RgaRgb565ToRgbMode first_safe_mode = RgaRgb565ToRgbMode::None;
    double first_safe_diff = 999.0;
    double min_diff = 999.0;
    for (RgaRgb565ToRgbMode mode : candidates) {
        double diff = 999.0;
        bool works = rga_preproc_mode_works(mode, pattern.data(), sw, sh, dw, dh, ref_rgb, diff);
        min_diff = std::min(min_diff, diff);
        if (!works) {
            continue;
        }
        if (mode == RgaRgb565ToRgbMode::Hybrid) {
            if (first_safe_mode == RgaRgb565ToRgbMode::None) {
                first_safe_mode = mode;
                first_safe_diff = diff;
            }
        } else if (first_full_mode == RgaRgb565ToRgbMode::None) {
            first_full_mode = mode;
            first_full_diff = diff;
        }
    }
    if (first_full_mode != RgaRgb565ToRgbMode::None) {
        state.mode = first_full_mode;
        state.mean_abs_diff = first_full_diff;
    } else {
        state.mode = first_safe_mode;
        state.mean_abs_diff = (first_safe_mode != RgaRgb565ToRgbMode::None) ? first_safe_diff : min_diff;
    }
    state.done = true;
    if (state.mode == RgaRgb565ToRgbMode::None) {
        std::cerr << "[RGA] preproc self-test failed, fallback to CPU"
                  << " min_mean_abs_diff=" << state.mean_abs_diff << std::endl;
    } else if (std::getenv("APP_PCIE_VERBOSE_INIT")) {
        std::cout << "[RGA] preproc mode=" << rga_preproc_mode_name(state.mode)
                  << " mean_abs_diff=" << state.mean_abs_diff << std::endl;
    }
    return state.mode;
}
#endif

// 计算从原图到检测输入的 letterbox 几何参数。
static void get_scaling_info(int ow, int oh, int& nw, int& nh, int& sx, int& sy) {
    if (ow <= 0 || oh <= 0) {
        nw = DET_INPUT_W;
        nh = DET_INPUT_H;
        sx = 0;
        sy = 0;
        return;
    }
    float scale = std::min(static_cast<float>(DET_INPUT_W) / ow, static_cast<float>(DET_INPUT_H) / oh);
    nw = std::max(1, std::min(DET_INPUT_W, static_cast<int>(std::round(ow * scale))));
    nh = std::max(1, std::min(DET_INPUT_H, static_cast<int>(std::round(oh * scale))));
    // 1920x1080 -> 640x320 按常规四舍五入会得到 569x320。569 不能安全
    // 作为该 BSP 上 RGA RGB888 虚拟缓冲区的 width stride，因此在保持
    // 长宽比的前提下向下对齐到 4 像素（实际为 568x320）。预处理与坐标
    // 反变换共用本函数，避免引入 1 像素几何口径不一致。
    if (nw >= 4 && nw < DET_INPUT_W) {
        nw &= ~3;
        nw = std::max(4, nw);
    }
    float pad_w = static_cast<float>(DET_INPUT_W - nw);
    float pad_h = static_cast<float>(DET_INPUT_H - nh);
    sx = static_cast<int>(std::round(pad_w * 0.5f - 0.1f));
    sy = static_cast<int>(std::round(pad_h * 0.5f - 0.1f));
}

// 用于 NMS 的轴对齐 IoU。
static float iou_rect(const Rect2f& a, const Rect2f& b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.width, b.x + b.width);
    float y2 = std::min(a.y + a.height, b.y + b.height);
    float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    float uni = std::max(0.0f, a.width) * std::max(0.0f, a.height) + std::max(0.0f, b.width) * std::max(0.0f, b.height) - inter;
    if (uni <= 1e-6f) return 0.0f;
    return inter / uni;
}

// 按置信度降序的贪心 NMS。
static std::vector<int> nms_indices(const std::vector<Rect2f>& boxes, const std::vector<float>& scores, float thres) {
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int l, int r){ return scores[l] > scores[r]; });
    std::vector<int> keep;
    while (!order.empty()) {
        int i = order.front();
        keep.push_back(i);
        std::vector<int> next;
        for (size_t j = 1; j < order.size(); ++j) {
            if (iou_rect(boxes[i], boxes[order[j]]) <= thres) next.push_back(order[j]);
        }
        order.swap(next);
    }
    return keep;
}

// 按分数保留 TopK 候选，避免 DFL 解码与 NMS 在高密场景下退化。
struct ScoreCandidate {
    int idx = -1;
    float score = 0.0f;
};

static void keep_topk_candidates(std::vector<ScoreCandidate>& cands, int topk) {
    if (topk <= 0 || static_cast<int>(cands.size()) <= topk) return;
    std::nth_element(cands.begin(),
                     cands.begin() + topk,
                     cands.end(),
                     [](const ScoreCandidate& a, const ScoreCandidate& b) { return a.score > b.score; });
    cands.resize(topk);
    std::sort(cands.begin(), cands.end(), [](const ScoreCandidate& a, const ScoreCandidate& b) { return a.score > b.score; });
}

struct YoloScoreCandidate {
    int idx = -1;
    float score = 0.0f;
    int class_id = -1;
};

static void keep_topk_yolo_candidates(std::vector<YoloScoreCandidate>& cands, int topk) {
    if (topk <= 0 || static_cast<int>(cands.size()) <= topk) return;
    std::nth_element(cands.begin(),
                     cands.begin() + topk,
                     cands.end(),
                     [](const YoloScoreCandidate& a, const YoloScoreCandidate& b) { return a.score > b.score; });
    cands.resize(topk);
    std::sort(cands.begin(), cands.end(), [](const YoloScoreCandidate& a, const YoloScoreCandidate& b) {
        return a.score > b.score;
    });
}

// 为动态输出形状构建中心点网格与步长表。
// 不同模型可能输出 {8,16,32} 或 {4,8,16} 等组合。
static bool build_grid(int num_preds, std::vector<Point2f>& grid, std::vector<int>& strides) {
    auto add = [&](int stride) {
        int fw = DET_INPUT_W / stride;
        int fh = DET_INPUT_H / stride;
        for (int y = 0; y < fh; ++y) {
            for (int x = 0; x < fw; ++x) {
                grid.emplace_back(x + 0.5f, y + 0.5f);
                strides.push_back(stride);
            }
        }
    };

    const std::vector<std::vector<int>> choices = {
        {8, 16, 32}, {4, 8, 16}, {8, 16}, {4, 8}, {16, 32}, {8}, {16}, {4}, {32}
    };
    for (const auto& cs : choices) {
        int cnt = 0;
        for (int s : cs) cnt += (DET_INPUT_W / s) * (DET_INPUT_H / s);
        if (cnt == num_preds) {
            grid.clear();
            strides.clear();
            for (int s : cs) add(s);
            return true;
        }
    }
    return false;
}

// 线程内缓存 grid/stride，避免每帧重复构建同形状网格。
static bool get_grid_cached(int num_preds,
                            const std::vector<Point2f>*& grid,
                            const std::vector<int>*& strides) {
    struct GridCache {
        int num_preds = -1;
        std::vector<Point2f> grid;
        std::vector<int> strides;
    };
    static thread_local GridCache cache;

    if (cache.num_preds != num_preds) {
        cache.grid.clear();
        cache.strides.clear();
        if (!build_grid(num_preds, cache.grid, cache.strides)) return false;
        cache.num_preds = num_preds;
    }
    grid = &cache.grid;
    strides = &cache.strides;
    return true;
}

// 将输出张量统一抽象为 [C, N] 访问接口：
// - channels_first=true  : 内存布局是 [C, N]
// - channels_first=false : 内存布局是 [N, C]
struct TensorCNView {
    const float* data = nullptr;
    int channels = 0;
    int num_preds = 0;
    bool channels_first = true;

    inline float at(int c, int i) const {
        return channels_first ? data[c * num_preds + i] : data[i * channels + c];
    }
};

// 判断某个维度值是否可能是检测头的候选点数量（由输入尺寸与 stride 组合决定）。
static bool is_grid_count(int num_preds) {
    std::vector<Point2f> grid;
    std::vector<int> strides;
    return build_grid(num_preds, grid, strides);
}

// 从 RKNN 输出属性中推断 [C, N] 映射关系，兼容 [1,C,N] 与 [1,N,C] 两类布局。
static bool infer_cn_view(const rknn_tensor_attr& attr, const float* data, TensorCNView& out) {
    if (!data) return false;

    std::vector<int> dims;
    for (int i = 0; i < attr.n_dims; ++i) {
        if (attr.dims[i] > 0) dims.push_back(attr.dims[i]);
    }
    if (dims.size() < 2) return false;

    int d1 = dims[dims.size() - 2];
    int d2 = dims[dims.size() - 1];

    bool d1_grid = is_grid_count(d1);
    bool d2_grid = is_grid_count(d2);

    out.data = data;
    if (d1_grid && !d2_grid) {
        out.num_preds = d1;
        out.channels = d2;
        out.channels_first = false;  // [N, C]
    } else if (d2_grid && !d1_grid) {
        out.num_preds = d2;
        out.channels = d1;
        out.channels_first = true;   // [C, N]
    } else {
        // 无法凭 grid 唯一判定时，优先把较大维度视为 N（更符合检测头常见形状）。
        if (d1 >= d2) {
            out.num_preds = d1;
            out.channels = d2;
            out.channels_first = false;
        } else {
            out.num_preds = d2;
            out.channels = d1;
            out.channels_first = true;
        }
    }
    return out.channels > 0 && out.num_preds > 0;
}

// 数值稳定的 Sigmoid，用于把 cls logits 转成概率分数。
static inline float sigmoidf(float x) {
    if (x >= 0.0f) {
        float z = expf(-x);
        return 1.0f / (1.0f + z);
    }
    float z = expf(x);
    return z / (1.0f + z);
}

// 判断分数分支是否更像 logits：
// 若数值明显超出 [0,1]，则按 logits 处理。
static bool cls_looks_like_logit(const TensorCNView& out, int cls_start, int cls_cnt) {
    if (cls_cnt <= 0 || cls_start < 0 || cls_start + cls_cnt > out.channels) return false;

    const int sample_points = std::min(out.num_preds, 128);
    const int step = std::max(1, out.num_preds / std::max(1, sample_points));
    float mn = std::numeric_limits<float>::max();
    float mx = -std::numeric_limits<float>::max();

    for (int i = 0; i < out.num_preds; i += step) {
        for (int c = 0; c < cls_cnt; ++c) {
            float v = out.at(cls_start + c, i);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
    }
    return (mn < -0.2f || mx > 1.2f);
}

// 判断前 64 通道是否像 DFL logits：
// 用于区分 “DFL 原始输出” 与 “Detect 已解码输出”。
static bool first64_looks_like_dfl(const TensorCNView& out) {
    if (out.channels < 64) return false;

    const int sample_points = std::min(out.num_preds, 32);
    const int step = std::max(1, out.num_preds / std::max(1, sample_points));
    float mn = std::numeric_limits<float>::max();
    float mx = -std::numeric_limits<float>::max();
    int prob_like = 0;
    int total = 0;

    for (int i = 0; i < out.num_preds; i += step) {
        for (int c = 0; c < 64; ++c) {
            float v = out.at(c, i);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            if (v >= 0.0f && v <= 1.0f) prob_like++;
            total++;
        }
    }
    if (mn < -0.05f) return true;  // DFL logits 常见为有正有负。
    if (mx > 30.0f) return false;  // 已解码坐标常出现较大正数。
    float ratio = (total > 0) ? static_cast<float>(prob_like) / total : 1.0f;
    if (mn >= 0.0f && ratio > 0.85f) return false;  // 多数落在 [0,1]，更像概率分布而非 logits。
    return true;
}

// 基于 TensorCNView 解码 DFL 框（通用布局）。
static Rect2f decode_dfl_box(const TensorCNView& out, int idx, const Point2f& g, int stride) {
    float d[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) {
        float max_v = -1e9f;
        for (int j = 0; j < 16; ++j) max_v = std::max(max_v, out.at(k * 16 + j, idx));
        float sum = 0.0f;
        float wsum = 0.0f;
        for (int j = 0; j < 16; ++j) {
            float e = expf(out.at(k * 16 + j, idx) - max_v);
            sum += e;
            wsum += e * j;
        }
        d[k] = (sum > 1e-6f) ? (wsum / sum) : 0.0f;
    }
    float x1 = (g.x - d[0]) * stride;
    float y1 = (g.y - d[1]) * stride;
    float x2 = (g.x + d[2]) * stride;
    float y2 = (g.y + d[3]) * stride;
    return Rect2f(x1, y1, std::max(0.0f, x2 - x1), std::max(0.0f, y2 - y1));
}

// 已解码 Detect 输出通常是 (cx, cy, w, h)，这里统一转为 (x, y, w, h)。
static Rect2f decode_xywh_box(const TensorCNView& out, int idx) {
    if (out.channels < 4) return Rect2f();
    float cx = out.at(0, idx);
    float cy = out.at(1, idx);
    float w = out.at(2, idx);
    float h = out.at(3, idx);
    if (w <= 0.0f || h <= 0.0f) return Rect2f();

    float x1 = cx - w * 0.5f;
    float y1 = cy - h * 0.5f;
    float x2 = cx + w * 0.5f;
    float y2 = cy + h * 0.5f;

    x1 = std::max(0.0f, std::min(x1, static_cast<float>(DET_INPUT_W - 1)));
    y1 = std::max(0.0f, std::min(y1, static_cast<float>(DET_INPUT_H - 1)));
    x2 = std::max(0.0f, std::min(x2, static_cast<float>(DET_INPUT_W - 1)));
    y2 = std::max(0.0f, std::min(y2, static_cast<float>(DET_INPUT_H - 1)));
    return Rect2f(x1, y1, std::max(0.0f, x2 - x1), std::max(0.0f, y2 - y1));
}

// 解码车牌 4 个关键点。
// DFL 原始头的关键点按 YOLO Pose 网格偏移解码；noDFL/已解码头优先使用绝对坐标。
static std::vector<Point2f> decode_plate_keypoints(const TensorCNView& out,
                                                   int idx,
                                                   int kpt_base,
                                                   int kpt_step,
                                                   const Point2f& g,
                                                   int stride,
                                                   bool force_grid_decode) {
    std::vector<Point2f> pts(4);
    bool absolute_like = !force_grid_decode;
    if (absolute_like) {
        for (int k = 0; k < 4; ++k) {
            float px = out.at(kpt_base + k * kpt_step, idx);
            float py = out.at(kpt_base + k * kpt_step + 1, idx);
            if (px < 0.0f || px > static_cast<float>(DET_INPUT_W) ||
                py < 0.0f || py > static_cast<float>(DET_INPUT_H)) {
                absolute_like = false;
                break;
            }
            if (std::max(std::fabs(px), std::fabs(py)) <= 4.0f) {
                absolute_like = false;
                break;
            }
        }
    }

    for (int k = 0; k < 4; ++k) {
        float px = out.at(kpt_base + k * kpt_step, idx);
        float py = out.at(kpt_base + k * kpt_step + 1, idx);
        if (absolute_like) {
            pts[k] = Point2f(px, py);
        } else {
            pts[k] = Point2f((px * 2.0f + (g.x - 0.5f)) * stride,
                             (py * 2.0f + (g.y - 0.5f)) * stride);
        }
    }
    return pts;
}

static float quad_area_abs(const std::vector<Point2f>& quad) {
    if (quad.size() < 4) return 0.0f;
    float area = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const Point2f& a = quad[i];
        const Point2f& b = quad[(i + 1) % 4];
        area += a.x * b.y - b.x * a.y;
    }
    return std::fabs(area) * 0.5f;
}

// 与 Python ONNX 参考链路 order_quad_clockwise() 保持一致。
static bool order_plate_points_for_warp(const std::vector<Point2f>& pts, std::vector<Point2f>& ordered) {
    if (pts.size() < 4) return false;
    int tl = 0;
    int tr = 0;
    int br = 0;
    int bl = 0;
    float min_sum = pts[0].x + pts[0].y;
    float max_sum = min_sum;
    float min_diff = pts[0].y - pts[0].x;
    float max_diff = min_diff;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y)) return false;
        float sum = pts[i].x + pts[i].y;
        float diff = pts[i].y - pts[i].x;
        if (sum < min_sum) {
            min_sum = sum;
            tl = i;
        }
        if (sum > max_sum) {
            max_sum = sum;
            br = i;
        }
        if (diff < min_diff) {
            min_diff = diff;
            tr = i;
        }
        if (diff > max_diff) {
            max_diff = diff;
            bl = i;
        }
    }
    if (tl == tr || tl == br || tl == bl || tr == br || tr == bl || br == bl) return false;
    ordered = {pts[tl], pts[tr], pts[br], pts[bl]};
    return quad_area_abs(ordered) > 1.0f;
}

static bool quad_looks_reliable(const std::vector<Point2f>& ordered,
                                const Rect2f& box,
                                Size image_size,
                                std::string& reason) {
    if (ordered.size() < 4) {
        reason = "not_enough_points";
        return false;
    }
    if (box.width <= 1.0f || box.height <= 1.0f) {
        reason = "bad_bbox";
        return false;
    }
    for (const Point2f& p : ordered) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            reason = "non_finite_point";
            return false;
        }
    }

    float pad_x = std::max(8.0f, box.width * 0.55f);
    float pad_y = std::max(8.0f, box.height * 0.80f);
    Rect2f expanded(box.x - pad_x,
                    box.y - pad_y,
                    box.width + pad_x * 2.0f,
                    box.height + pad_y * 2.0f);
    expanded.x = std::max(-16.0f, expanded.x);
    expanded.y = std::max(-16.0f, expanded.y);
    expanded.width = std::min(static_cast<float>(image_size.width) + 32.0f - expanded.x, expanded.width);
    expanded.height = std::min(static_cast<float>(image_size.height) + 32.0f - expanded.y, expanded.height);
    for (const Point2f& p : ordered) {
        if (p.x < expanded.x || p.x > expanded.x + expanded.width ||
            p.y < expanded.y || p.y > expanded.y + expanded.height) {
            reason = "point_outside_bbox_guard";
            return false;
        }
    }

    float area = quad_area_abs(ordered);
    float bbox_area = box.width * box.height;
    if (area < 8.0f) {
        reason = "quad_area_too_small";
        return false;
    }
    if (area > bbox_area * 2.8f) {
        reason = "quad_area_too_large";
        return false;
    }

    float top = static_cast<float>(cv::norm(ordered[1] - ordered[0]));
    float right = static_cast<float>(cv::norm(ordered[2] - ordered[1]));
    float bottom = static_cast<float>(cv::norm(ordered[2] - ordered[3]));
    float left = static_cast<float>(cv::norm(ordered[3] - ordered[0]));
    if (top < 5.0f || bottom < 5.0f || left < 2.0f || right < 2.0f) {
        reason = "edge_too_short";
        return false;
    }
    if (std::max(top, bottom) / std::max(1.0f, std::min(top, bottom)) > 4.0f ||
        std::max(left, right) / std::max(1.0f, std::min(left, right)) > 6.0f) {
        reason = "edge_ratio_unreasonable";
        return false;
    }
    float edge_aspect = ((top + bottom) * 0.5f) / std::max(1.0f, (left + right) * 0.5f);
    if (edge_aspect < 1.2f || edge_aspect > 12.0f) {
        reason = "quad_edge_aspect_unreasonable";
        return false;
    }

    float min_x = ordered[0].x;
    float max_x = ordered[0].x;
    float min_y = ordered[0].y;
    float max_y = ordered[0].y;
    for (const Point2f& p : ordered) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
    float q_w = max_x - min_x;
    float q_h = max_y - min_y;
    float aspect = q_h > 1.0f ? q_w / q_h : 0.0f;
    if (aspect < 1.0f || aspect > 12.0f) {
        reason = "quad_aspect_unreasonable";
        return false;
    }
    float span_w_ratio = q_w / std::max(1.0f, box.width);
    float span_h_ratio = q_h / std::max(1.0f, box.height);
    if (span_w_ratio < 0.18f || span_h_ratio < 0.15f) {
        reason = "quad_span_too_small";
        return false;
    }
    // A tilted plate can occupy much less area than its axis-aligned bbox.
    // Only reject low area when the projected span is also implausibly small.
    if (bbox_area > 1.0f) {
        float area_ratio = area / bbox_area;
        bool projected_span_ok = span_w_ratio >= 0.55f && span_h_ratio >= 0.40f;
        if (area_ratio < 0.08f) {
            reason = "quad_area_too_small";
            return false;
        }
        if (area_ratio < 0.12f && !projected_span_ok) {
            reason = "quad_area_too_small";
            return false;
        }
    }
    float quad_cx = 0.0f;
    float quad_cy = 0.0f;
    for (const Point2f& p : ordered) {
        quad_cx += p.x;
        quad_cy += p.y;
    }
    quad_cx *= 0.25f;
    quad_cy *= 0.25f;
    float box_cx = box.x + box.width * 0.5f;
    float box_cy = box.y + box.height * 0.5f;
    if (std::fabs(quad_cx - box_cx) > box.width * 0.35f ||
        std::fabs(quad_cy - box_cy) > box.height * 0.45f) {
        reason = "quad_center_far_from_bbox";
        return false;
    }

    float top_mid_y = (ordered[0].y + ordered[1].y) * 0.5f;
    float bottom_mid_y = (ordered[2].y + ordered[3].y) * 0.5f;
    if (top_mid_y > bottom_mid_y + std::max(2.0f, box.height * 0.20f)) {
        reason = "top_bottom_reversed";
        return false;
    }

    reason = "ok";
    return true;
}

static std::vector<Point2f> bbox_quad_for_warp(const Rect2f& box, Size image_size) {
    if (image_size.width <= 1 || image_size.height <= 1) {
        return {Point2f(0, 0), Point2f(1, 0), Point2f(1, 1), Point2f(0, 1)};
    }
    float pad_x = box.width * 0.02f;
    float pad_y = box.height * 0.08f;
    float max_x = static_cast<float>(image_size.width - 1);
    float max_y = static_cast<float>(image_size.height - 1);
    float x0 = std::max(0.0f, std::min(max_x - 1.0f, box.x - pad_x));
    float y0 = std::max(0.0f, std::min(max_y - 1.0f, box.y - pad_y));
    float x1 = std::max(x0 + 1.0f, std::min(max_x, box.x + box.width + pad_x));
    float y1 = std::max(y0 + 1.0f, std::min(max_y, box.y + box.height + pad_y));
    return {Point2f(x0, y0), Point2f(x1, y0), Point2f(x1, y1), Point2f(x0, y1)};
}

static Mat crop_from_quad(const Mat& img, const std::vector<Point2f>& src, Size shape) {
    std::vector<Point2f> dst = {
        Point2f(0, 0),
        Point2f(static_cast<float>(shape.width - 1), 0),
        Point2f(static_cast<float>(shape.width - 1), static_cast<float>(shape.height - 1)),
        Point2f(0, static_cast<float>(shape.height - 1))
    };
    Mat m = cv::getPerspectiveTransform(src, dst);
    Mat crop;
    cv::warpPerspective(img, crop, m, shape, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return crop;
}

struct QuadGeometryCompEstimate {
    float shear = 0.0f;
    float confidence = 0.0f;
    float target_shear = 0.0f;
    float current_shear = 0.0f;
    float current_left_shear = 0.0f;
    float current_right_shear = 0.0f;
    float raw_delta = 0.0f;
    float roll_shear = 0.0f;
    float roll_left_delta = 0.0f;
    float roll_right_delta = 0.0f;
    float view_shear = 0.0f;
    float perspective_weight = 0.0f;
    float delta_shear = 0.0f;
    float delta_direction = 0.0f;
    float left_dx = 0.0f;
    float right_dx = 0.0f;
    float dx_tl = 0.0f;
    float dx_tr = 0.0f;
    float dx_br = 0.0f;
    float dx_bl = 0.0f;
    float gain = 0.0f;
    float roll_score = 0.0f;
    float persp_score = 0.0f;
    float persp_apply = 0.0f;
    float pure_persp_score = 0.0f;
    float left_gain = 0.0f;
    float right_gain = 0.0f;
    float missing_shear = 0.0f;
    float missing_px_norm = 0.0f;
    float need_score = 0.0f;
    float geometry_quality = 0.0f;
    float gain_raw = 0.0f;
    float coverage = 0.0f;
    float coverage_scale = 0.0f;
    float over_risk = 0.0f;
    float effective_gain = 0.0f;
    float effective_max_point_dx = 0.0f;
    float under_planned_dx = 0.0f;
    float under_move_score = 0.0f;
    float under_evidence_score = 0.0f;
    float under_deficit_score = 0.0f;
    float under_target_dx = 0.0f;
    float under_boost = 1.0f;
    float deficit_ratio = 0.0f;
    float horizontal_angle_deg = 0.0f;
    float diag1 = 0.0f;
    float diag2 = 0.0f;
    float diag_ratio = 0.0f;
    float diag_rel = 0.0f;
    float signed_diag_rel = 0.0f;
    float tb_rel = 0.0f;
    float signed_tb_rel = 0.0f;
    float signed_lr_rel = 0.0f;
    float short_rel = 0.0f;
    float perspective_strength = 0.0f;
    float diag_delta = 0.0f;
    float base_delta = 0.0f;
    float final_delta = 0.0f;
    float base_left_delta = 0.0f;
    float base_right_delta = 0.0f;
    float fit_left_delta = 0.0f;
    float fit_right_delta = 0.0f;
    float fit_gain = 1.0f;
    float under_weight = 0.0f;
    float roll_floor_weight = 0.0f;
    float fit_quality = 0.0f;
    float projection_persp_lock_raw = 0.0f;
    float projection_persp_lock = 0.0f;
    float rotation_confidence = 0.0f;
    float roll_residual_evidence = 0.0f;
    float high_roll_unlock = 0.0f;
    float horizontal_reliability = 0.0f;
    float rotation_unlock_evidence = 0.0f;
    float diag_mid_norm = 0.0f;
    float affine_shape_score = 0.0f;
    float trapezoid_score = 0.0f;
    float affine_rotation_unlock = 0.0f;
    float affine_floor_weight = 0.0f;
    float tilt_share = 0.0f;
    float safety_scale = 0.0f;
    float move_norm = 0.0f;
    std::string fit_model_version = "none";
    std::string delta_source = "none";
    std::string view_case = "none";
    std::string selected_level = "raw";
    // Legacy JSON compatibility. The old raw/weak/medium/strong crop scoring
    // path is disabled; v2.9.1 selects one O(1) geometry result directly.
    int candidate_count = 0;
    float quality_raw = -1.0f;
    float quality_weak = -1.0f;
    float quality_medium = -1.0f;
    float quality_strong = -1.0f;
    float quality_margin = 0.0f;
    double geom_ms = 0.0;
    double select_ms = 0.0;
    std::vector<std::string> candidate_levels;
    std::vector<float> candidate_qualities;
    std::vector<std::vector<Point2f>> candidate_quads;
    std::string reason = "not_attempted";
};

// Plate x-compensation v2.9.1. Runtime remains O(1): use only bbox and ordered
// TL/TR/BR/BL geometry, move corner x coordinates, keep y coordinates unchanged,
// and fall back to the original quad when quality/width/safety checks fail.
constexpr const char* PLATE_QUAD_COMP_FORMULA_VERSION = "geometry_corner_dx_v2_9_1";
constexpr float PLATE_QUAD_COMP_MIN_GEOMETRY_QUALITY = 0.20f;
constexpr float PLATE_QUAD_COMP_MIN_POINT_MOVE_PX = 1.5f;
constexpr float PLATE_QUAD_COMP_TARGET_SHEAR_LIMIT = 0.46f;
constexpr float PLATE_QUAD_COMP_DELTA_SHEAR_LIMIT = 0.46f;
constexpr float PLATE_QUAD_COMP_BBOX_DX_RATIO = 0.14f;
constexpr float PLATE_QUAD_COMP_WIDTH_DX_RATIO = 0.22f;
constexpr float PLATE_QUAD_COMP_MAX_POINT_DX = 18.0f;
constexpr float PLATE_QUAD_COMP_PROJECTION_LOCK_SCALE = 0.78f;
constexpr float PLATE_QUAD_COMP_ROLL_FLOOR_K = 1.58f;
constexpr float PLATE_QUAD_COMP_WIDTH_KEEP_RATIO = 0.96f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_MIN = 0.045f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_MAX = 0.085f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_DELTA_START = 0.09f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_DELTA_RANGE = 0.07f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_DIAG_MIN = 0.075f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_DIAG_MAX = 0.090f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_TB_MAX = 0.012f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_CONF_MAX = 0.80f;
constexpr float PLATE_QUAD_COMP_RESIDUAL_ROLL_VIEW_DELTA_GAIN = 2.80f;
constexpr float PLATE_QUAD_COMP_BBOX_VIEW_SHIFT_START = 0.006f;
constexpr float PLATE_QUAD_COMP_BBOX_VIEW_SHIFT_RANGE = 0.018f;
constexpr float PLATE_QUAD_COMP_BBOX_VIEW_EXTRA_GAIN = 1.20f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_ROLL_START = 0.050f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_ROLL_RANGE = 0.180f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_SIDE_FULL = 0.045f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_SIDE_ZERO = 0.135f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_BBOX_FULL = 0.018f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_BBOX_ZERO = 0.060f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_DIAG_FULL = 0.190f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_DIAG_ZERO = 0.270f;
constexpr float PLATE_QUAD_COMP_ROT_SHARE_MIN_WHEN_ROLL = 0.18f;
constexpr float PLATE_QUAD_COMP_ROTATION_DX_LIMIT_BOOST = 0.40f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_TB_START = 0.004f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_TB_RANGE = 0.018f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_DELTA_START = 0.120f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_DELTA_RANGE = 0.240f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_LOCK_START = 0.600f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_LOCK_RANGE = 0.350f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_LOCK_SCALE = 0.650f;
constexpr float PLATE_QUAD_COMP_ROT_UNLOCK_CONF_MAX = 0.350f;
constexpr float PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_START = 0.350f;
constexpr float PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_RANGE = 0.160f;
constexpr float PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_DELTA_START = 0.220f;
constexpr float PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_DELTA_RANGE = 0.260f;
constexpr float PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_LOCK_SCALE = 0.750f;
constexpr float PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_CONF_MAX = 0.620f;
constexpr float PLATE_QUAD_COMP_AFFINE_DIAG_MID_START = 0.012f;
constexpr float PLATE_QUAD_COMP_AFFINE_DIAG_MID_RANGE = 0.020f;
constexpr float PLATE_QUAD_COMP_AFFINE_EDGE_BALANCE_START = 0.055f;
constexpr float PLATE_QUAD_COMP_AFFINE_EDGE_BALANCE_RANGE = 0.080f;
constexpr float PLATE_QUAD_COMP_AFFINE_TRAP_DIAG_START = 0.018f;
constexpr float PLATE_QUAD_COMP_AFFINE_TRAP_DIAG_RANGE = 0.035f;
constexpr float PLATE_QUAD_COMP_AFFINE_TRAP_EDGE_START = 0.070f;
constexpr float PLATE_QUAD_COMP_AFFINE_TRAP_EDGE_RANGE = 0.070f;
constexpr float PLATE_QUAD_COMP_AFFINE_ROLL_START = 0.260f;
constexpr float PLATE_QUAD_COMP_AFFINE_ROLL_RANGE = 0.160f;
constexpr float PLATE_QUAD_COMP_AFFINE_DELTA_START = 0.180f;
constexpr float PLATE_QUAD_COMP_AFFINE_DELTA_RANGE = 0.280f;
constexpr float PLATE_QUAD_COMP_AFFINE_UNLOCK_LOCK_SCALE = 0.600f;
constexpr float PLATE_QUAD_COMP_AFFINE_UNLOCK_CONF_SCALE = 0.750f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_TB_START = 0.004f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_TB_RANGE = 0.010f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_SHORT_START = 0.018f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_SHORT_RANGE = 0.020f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_ROLL_START = 0.400f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_ROLL_RANGE = 0.100f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_ASPECT_START = 1.420f;
constexpr float PLATE_QUAD_COMP_PURE_GUARD_ASPECT_RANGE = 0.240f;
constexpr float PLATE_QUAD_COMP_FLOOR_UNDER_START = 0.035f;
constexpr float PLATE_QUAD_COMP_FLOOR_UNDER_RANGE = 0.140f;
constexpr float PLATE_QUAD_COMP_MIXED_LOW_ROLL_MAX = 0.120f;
constexpr float PLATE_QUAD_COMP_MIXED_LOW_ROLL_RANGE = 0.040f;
constexpr float PLATE_QUAD_COMP_MIXED_VIEW_DIAG_START = 0.035f;
constexpr float PLATE_QUAD_COMP_MIXED_VIEW_DIAG_RANGE = 0.055f;
constexpr float PLATE_QUAD_COMP_MIXED_VIEW_DELTA_GAIN = 2.20f;
constexpr float PLATE_QUAD_COMP_MIXED_VIEW_TARGET_GAIN = 2.20f;
constexpr float PLATE_QUAD_COMP_MIXED_VIEW_BBOX_GAIN = 0.75f;
constexpr float PLATE_QUAD_COMP_MIXED_VIEW_ROLL_GAIN = 0.20f;
constexpr float PLATE_QUAD_COMP_MIXED_ROLL_DELTA_GAIN = 2.80f;
constexpr float PLATE_QUAD_COMP_MIXED_ROLL_GAP_START = 0.045f;
constexpr float PLATE_QUAD_COMP_MIXED_ROLL_GAP_RANGE = 0.080f;
constexpr float PLATE_QUAD_COMP_MIXED_ROLL_CONF_SCALE = 0.70f;
constexpr float PLATE_QUAD_COMP_MIXED_FLOOR_MAX_POINT_DX = 8.0f;
constexpr float PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_H_START = 0.98f;
constexpr float PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_H_RANGE = 0.06f;
constexpr float PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_AREA_START = 0.76f;
constexpr float PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_AREA_RANGE = 0.18f;
constexpr float PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_DAMP = 1.00f;
constexpr int PLATE_QUAD_COMP_FEATURE_COUNT = 11;
static const float PLATE_QUAD_COMP_FIT_LEFT_COEF[PLATE_QUAD_COMP_FEATURE_COUNT] = {
    -0.37497114f, -0.88371181f, 2.10684771f, -1.45988875f,
    3.21152567f,  -1.16581674f, -0.23988599f, 0.57335245f,
    -1.56319075f, -1.13075563f, -0.05430265f};
static const float PLATE_QUAD_COMP_FIT_RIGHT_COEF[PLATE_QUAD_COMP_FEATURE_COUNT] = {
    -0.58371257f, -1.28189377f, 2.53688399f, -1.76725965f,
    5.41300369f,  -2.16574303f, -0.17908812f, 0.51772957f,
    -1.42996464f, -1.15603104f, -0.04888783f};

static float angle_diff_abs_deg(float a, float b) {
    float d = std::fabs(a - b);
    constexpr float kPi = 3.14159265358979323846f;
    while (d > kPi) d = std::fabs(d - 2.0f * kPi);
    return d * 180.0f / kPi;
}

static float safe_ratio(float a, float b) {
    float mn = std::max(1e-6f, std::min(a, b));
    return std::max(a, b) / mn;
}

static float point_norm(const Point2f& p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

static float clamp_float(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static float smoothstep01(float t) {
    t = clamp_float(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static Rect2f quad_bounds_rect(const std::vector<Point2f>& quad) {
    if (quad.empty()) return Rect2f();
    float min_x = quad[0].x;
    float max_x = quad[0].x;
    float min_y = quad[0].y;
    float max_y = quad[0].y;
    for (const Point2f& p : quad) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
    return Rect2f(min_x, min_y, std::max(0.0f, max_x - min_x), std::max(0.0f, max_y - min_y));
}

static float bbox_fit_score(const std::vector<Point2f>& ordered, const Rect2f& box) {
    if (ordered.size() != 4 || box.width <= 1.0f || box.height <= 1.0f) return 0.0f;
    const Rect2f q = quad_bounds_rect(ordered);
    const float span_w_ratio = q.width / std::max(1.0f, box.width);
    const float span_h_ratio = q.height / std::max(1.0f, box.height);
    const float area_ratio = quad_area_abs(ordered) / std::max(1.0f, box.width * box.height);
    const float quad_cx = (ordered[0].x + ordered[1].x + ordered[2].x + ordered[3].x) * 0.25f;
    const float quad_cy = (ordered[0].y + ordered[1].y + ordered[2].y + ordered[3].y) * 0.25f;
    const float box_cx = box.x + box.width * 0.5f;
    const float box_cy = box.y + box.height * 0.5f;
    const float center_dx = std::fabs(quad_cx - box_cx) / std::max(1.0f, box.width);
    const float center_dy = std::fabs(quad_cy - box_cy) / std::max(1.0f, box.height);

    const float span_w_score = 1.0f - clamp_float(std::fabs(span_w_ratio - 0.92f) / 0.42f, 0.0f, 1.0f);
    const float span_h_score = 1.0f - clamp_float(std::fabs(span_h_ratio - 0.82f) / 0.45f, 0.0f, 1.0f);
    const float area_score = 1.0f - clamp_float(std::fabs(area_ratio - 0.62f) / 0.48f, 0.0f, 1.0f);
    const float center_score = 1.0f - clamp_float(std::max(center_dx / 0.18f, center_dy / 0.24f), 0.0f, 1.0f);
    return clamp_float(0.28f * span_w_score + 0.22f * span_h_score +
                       0.22f * area_score + 0.28f * center_score,
                       0.0f,
                       1.0f);
}

static float directional_bbox_slack_x(const Point2f& p, float direction, const Rect2f& box) {
    if (direction < 0.0f) return std::max(0.0f, p.x - box.x);
    if (direction > 0.0f) return std::max(0.0f, box.x + box.width - p.x);
    return std::max(1.0f, box.width * 0.5f);
}

static float dot_plate_quad_fit(const float* coef, const float* feature) {
    float out = 0.0f;
    for (int i = 0; i < PLATE_QUAD_COMP_FEATURE_COUNT; ++i) out += coef[i] * feature[i];
    return out;
}

struct SideCornerDx {
    float top_dx = 0.0f;
    float bottom_dx = 0.0f;
    float full_dx = 0.0f;
    float max_abs_dx = 0.0f;
};

struct SideDeltaV2 {
    float final_delta = 0.0f;
    float fit_delta = 0.0f;
    float locked_base = 0.0f;
    float floor_delta = 0.0f;
    float under_weight = 0.0f;
    float floor_weight = 0.0f;
};

static SideDeltaV2 estimate_side_delta_v2(float fit_delta,
                                          float floor_source_delta,
                                          float projection_persp_lock,
                                          float rotation_confidence,
                                          bool prefer_floor_on_conflict = false) {
    SideDeltaV2 out;
    out.fit_delta = clamp_float(fit_delta,
                                -PLATE_QUAD_COMP_DELTA_SHEAR_LIMIT,
                                PLATE_QUAD_COMP_DELTA_SHEAR_LIMIT);
    const float under = std::max(0.0f, std::fabs(floor_source_delta) - std::fabs(out.fit_delta));
    const float under_start = prefer_floor_on_conflict ? PLATE_QUAD_COMP_FLOOR_UNDER_START : 0.070f;
    const float under_range = prefer_floor_on_conflict ? PLATE_QUAD_COMP_FLOOR_UNDER_RANGE : 0.180f;
    out.under_weight = smoothstep01((under - under_start) / under_range);
    out.locked_base = out.fit_delta * (1.0f - PLATE_QUAD_COMP_PROJECTION_LOCK_SCALE * projection_persp_lock);
    out.floor_delta = floor_source_delta * PLATE_QUAD_COMP_ROLL_FLOOR_K * rotation_confidence * out.under_weight;
    out.floor_weight = 0.0f;

    const bool same_direction = out.locked_base * out.floor_delta > 0.0f;
    const float conflict_conf = prefer_floor_on_conflict ? 0.32f : 0.72f;
    const float conflict_base_abs = prefer_floor_on_conflict ? 0.075f : 0.045f;
    const bool trusted_floor_conflict =
        rotation_confidence > conflict_conf && std::fabs(out.locked_base) < conflict_base_abs &&
        std::fabs(out.floor_delta) > std::fabs(out.locked_base);
    if (same_direction) {
        out.final_delta = out.locked_base + out.floor_delta;
        out.floor_weight = rotation_confidence * out.under_weight;
    } else if (trusted_floor_conflict) {
        out.final_delta = out.floor_delta;
        out.floor_weight = rotation_confidence * out.under_weight;
    } else {
        out.final_delta = out.locked_base;
    }
    out.final_delta = clamp_float(out.final_delta,
                                  -PLATE_QUAD_COMP_DELTA_SHEAR_LIMIT,
                                  PLATE_QUAD_COMP_DELTA_SHEAR_LIMIT);
    return out;
}

static SideCornerDx split_side_dx_by_bbox_slack(const Point2f& top,
                                                const Point2f& bottom,
                                                float side_full_dx,
                                                const Rect2f& box,
                                                float max_point_dx) {
    SideCornerDx out;
    const float mag = std::fabs(side_full_dx);
    if (mag <= 1e-6f || max_point_dx <= 0.0f) return out;
    const float sign = (side_full_dx >= 0.0f) ? 1.0f : -1.0f;
    const float top_dir = -sign;
    const float bottom_dir = sign;
    const float top_slack = directional_bbox_slack_x(top, top_dir, box);
    const float bottom_slack = directional_bbox_slack_x(bottom, bottom_dir, box);
    const float share_denom = top_slack + bottom_slack;
    float top_share = (share_denom > 1e-6f) ? (top_slack / share_denom) : 0.5f;
    top_share = clamp_float(0.5f + (top_share - 0.5f) * 0.35f, 0.42f, 0.58f);
    const float bottom_share = 1.0f - top_share;
    const float safe_mag = std::min(mag, max_point_dx / std::max(top_share, bottom_share));

    out.top_dx = top_dir * safe_mag * top_share;
    out.bottom_dx = bottom_dir * safe_mag * bottom_share;
    out.full_dx = out.bottom_dx - out.top_dx;
    out.max_abs_dx = std::max(std::fabs(out.top_dx), std::fabs(out.bottom_dx));
    return out;
}

static std::string comp_level_from_move(float max_point_move, float mean_width) {
    if (max_point_move < PLATE_QUAD_COMP_MIN_POINT_MOVE_PX) return "raw";
    const float move_norm = max_point_move / std::max(1.0f, mean_width);
    if (move_norm < 0.018f) return "weak";
    if (move_norm < 0.040f) return "medium";
    return "strong";
}

static bool compensate_plate_quad_x_corner_geometry_bbox(const std::vector<Point2f>& ordered,
                                                         const Rect2f& box,
                                                         Size image_size,
                                                         QuadGeometryCompEstimate& est,
                                                         std::vector<Point2f>& final_quad,
                                                         std::string& reason) {
    // Shared by real-time and one-shot image tests. The fitted residual gives
    // the base direction, roll residual unlocks front/affine rotation cases,
    // and projection/trapezoid guards suppress pure-perspective overfix.
    est = QuadGeometryCompEstimate();
    final_quad = ordered;
    reason.clear();
    est.quality_raw = -1.0f;
    est.quality_weak = -1.0f;
    est.quality_medium = -1.0f;
    est.quality_strong = -1.0f;
    est.quality_margin = 0.0f;
    est.candidate_count = 0;
    est.candidate_levels.clear();
    est.candidate_qualities.clear();
    est.candidate_quads.clear();
    est.selected_level = "raw";
    est.delta_source = "fit_residual_rotation_lock";
    est.fit_model_version = "ccpd_50k_ridge_mixed_rotation_v2_9_1";

    if (ordered.size() != 4) {
        reason = "not_enough_points";
        est.reason = reason;
        return false;
    }
    if (box.width <= 1.0f || box.height <= 1.0f || image_size.width <= 1 || image_size.height <= 1) {
        reason = "bbox_invalid";
        est.reason = reason;
        return false;
    }

    const Point2f top_vec = ordered[1] - ordered[0];
    const Point2f bottom_vec = ordered[2] - ordered[3];
    const Point2f left_vec = ordered[3] - ordered[0];
    const Point2f right_vec = ordered[2] - ordered[1];
    const float top_len = point_norm(top_vec);
    const float bottom_len = point_norm(bottom_vec);
    const float left_len = point_norm(left_vec);
    const float right_len = point_norm(right_vec);
    const float left_h = left_vec.y;
    const float right_h = right_vec.y;
    const float mean_tb_len = std::max(1.0f, (top_len + bottom_len) * 0.5f);
    const float mean_lr_len = std::max(1.0f, (left_len + right_len) * 0.5f);
    if (top_len < 5.0f || bottom_len < 5.0f || left_len < 2.0f || right_len < 2.0f) {
        reason = "edge_too_short";
        est.reason = reason;
        return false;
    }
    if (left_h <= 1.0f || right_h <= 1.0f) {
        reason = "vertical_edge_degenerate";
        est.reason = reason;
        return false;
    }

    const float left_shear = left_vec.x / left_h;
    const float right_shear = right_vec.x / right_h;
    const float tb_ratio = safe_ratio(top_len, bottom_len);
    const float lr_ratio = safe_ratio(left_len, right_len);
    if (tb_ratio > 4.0f || lr_ratio > 6.0f) {
        reason = "perspective_ratio_too_large";
        est.reason = reason;
        return false;
    }

    const float top_angle = std::atan2(top_vec.y, top_vec.x);
    const float bottom_angle = std::atan2(bottom_vec.y, bottom_vec.x);
    const float top_bottom_angle_diff = angle_diff_abs_deg(top_angle, bottom_angle);
    const float side_score = clamp_float(1.0f - std::fabs(left_shear - right_shear) / 1.20f, 0.0f, 1.0f);
    const float angle_score = clamp_float(1.0f - top_bottom_angle_diff / 35.0f, 0.0f, 1.0f);
    est.horizontal_reliability =
        1.0f - smoothstep01((top_bottom_angle_diff - 2.0f) / 6.0f);
    const float ratio_score = clamp_float(1.0f - std::max(tb_ratio / 4.0f, lr_ratio / 6.0f) * 0.35f,
                                          0.0f,
                                          1.0f);
    est.geometry_quality = side_score * angle_score * ratio_score;
    if (est.geometry_quality < PLATE_QUAD_COMP_MIN_GEOMETRY_QUALITY) {
        reason = "geometry_low_quality";
        est.reason = reason;
        return false;
    }

    const Point2f horizontal_sum = top_vec + bottom_vec;
    if (std::fabs(horizontal_sum.x) < 1e-3f) {
        reason = "horizontal_edge_degenerate";
        est.reason = reason;
        return false;
    }
    const float theta_h = std::atan2(horizontal_sum.y, horizontal_sum.x);
    est.horizontal_angle_deg = theta_h * 180.0f / 3.14159265358979323846f;
    est.roll_shear = -std::tan(theta_h);
    est.roll_score = smoothstep01((std::fabs(est.roll_shear) - 0.055f) / 0.17f);
    est.current_left_shear = left_shear;
    est.current_right_shear = right_shear;
    est.current_shear = (left_shear * std::fabs(left_h) + right_shear * std::fabs(right_h)) /
                        std::max(1e-6f, std::fabs(left_h) + std::fabs(right_h));
    est.roll_left_delta = est.roll_shear - left_shear;
    est.roll_right_delta = est.roll_shear - right_shear;

    est.diag1 = point_norm(ordered[2] - ordered[0]);
    est.diag2 = point_norm(ordered[3] - ordered[1]);
    const float mean_diag = std::max(1.0f, (est.diag1 + est.diag2) * 0.5f);
    est.diag_ratio = safe_ratio(est.diag1, est.diag2);
    est.diag_rel = std::fabs(est.diag1 - est.diag2) / mean_diag;
    est.signed_diag_rel = (est.diag1 - est.diag2) / mean_diag;
    est.signed_tb_rel = (top_len - bottom_len) / mean_tb_len;
    est.signed_lr_rel = (left_len - right_len) / mean_lr_len;
    est.tb_rel = std::fabs(est.signed_tb_rel);
    est.short_rel = std::fabs(est.signed_lr_rel);
    const Point2f diag_mid_a((ordered[0].x + ordered[2].x) * 0.5f,
                             (ordered[0].y + ordered[2].y) * 0.5f);
    const Point2f diag_mid_b((ordered[1].x + ordered[3].x) * 0.5f,
                             (ordered[1].y + ordered[3].y) * 0.5f);
    est.diag_mid_norm = point_norm(diag_mid_a - diag_mid_b) / mean_tb_len;
    const float edge_balance = std::max(est.tb_rel, est.short_rel);
    const float diag_mid_score =
        1.0f - smoothstep01((est.diag_mid_norm - PLATE_QUAD_COMP_AFFINE_DIAG_MID_START) /
                            PLATE_QUAD_COMP_AFFINE_DIAG_MID_RANGE);
    const float edge_balance_score =
        1.0f - smoothstep01((edge_balance - PLATE_QUAD_COMP_AFFINE_EDGE_BALANCE_START) /
                            PLATE_QUAD_COMP_AFFINE_EDGE_BALANCE_RANGE);
    est.affine_shape_score =
        clamp_float(diag_mid_score * edge_balance_score * est.horizontal_reliability * side_score,
                    0.0f,
                    1.0f);
    est.trapezoid_score =
        clamp_float(std::max(std::max(smoothstep01((est.diag_mid_norm - PLATE_QUAD_COMP_AFFINE_TRAP_DIAG_START) /
                                                   PLATE_QUAD_COMP_AFFINE_TRAP_DIAG_RANGE),
                                      smoothstep01((edge_balance - PLATE_QUAD_COMP_AFFINE_TRAP_EDGE_START) /
                                                   PLATE_QUAD_COMP_AFFINE_TRAP_EDGE_RANGE)),
                             1.0f - est.affine_shape_score),
                    0.0f,
                    1.0f);
    est.perspective_strength = 0.65f * est.diag_rel + 0.35f * est.tb_rel;
    est.persp_score = smoothstep01((est.perspective_strength - 0.06f) / 0.18f);
    const bool left_roll_conflict = est.roll_shear * left_shear < -1e-4f;
    const bool right_roll_conflict = est.roll_shear * right_shear < -1e-4f;
    const float roll_delta_mean =
        (std::fabs(est.roll_left_delta) * std::fabs(left_h) +
         std::fabs(est.roll_right_delta) * std::fabs(right_h)) /
        std::max(1e-6f, std::fabs(left_h) + std::fabs(right_h));
    const float current_side_mean_abs =
        (std::fabs(left_shear) * std::fabs(left_h) +
         std::fabs(right_shear) * std::fabs(right_h)) /
        std::max(1e-6f, std::fabs(left_h) + std::fabs(right_h));
    const float top_center_x = 0.5f * (ordered[0].x + ordered[1].x);
    const float bottom_center_x = 0.5f * (ordered[3].x + ordered[2].x);
    const float bbox_view_shift = (bottom_center_x - top_center_x) / std::max(1.0f, box.width);
    const Rect2f ordered_bounds = quad_bounds_rect(ordered);
    const float quad_span_h_ratio = ordered_bounds.height / std::max(1.0f, box.height);
    const float quad_area_ratio = quad_area_abs(ordered) / std::max(1.0f, box.width * box.height);
    const float mean_side_h = std::max(1.0f, 0.5f * (std::fabs(left_h) + std::fabs(right_h)));
    est.tilt_share = (std::fabs(est.roll_shear) * mean_tb_len) /
                     std::max(1.0f, std::fabs(est.roll_shear) * mean_tb_len + mean_side_h);
    const float projection_persp_lock_raw =
        smoothstep01((est.diag_rel - 0.10f) / 0.10f) * smoothstep01((est.tilt_share - 0.30f) / 0.12f);
    est.projection_persp_lock_raw = projection_persp_lock_raw;
    const float rot_roll_score =
        smoothstep01((std::fabs(est.roll_shear) - PLATE_QUAD_COMP_ROT_SHARE_ROLL_START) /
                     PLATE_QUAD_COMP_ROT_SHARE_ROLL_RANGE);
    const float rot_side_score =
        1.0f - smoothstep01((current_side_mean_abs - PLATE_QUAD_COMP_ROT_SHARE_SIDE_FULL) /
                            (PLATE_QUAD_COMP_ROT_SHARE_SIDE_ZERO - PLATE_QUAD_COMP_ROT_SHARE_SIDE_FULL));
    const float rot_bbox_score =
        1.0f - smoothstep01((std::fabs(bbox_view_shift) - PLATE_QUAD_COMP_ROT_SHARE_BBOX_FULL) /
                            (PLATE_QUAD_COMP_ROT_SHARE_BBOX_ZERO - PLATE_QUAD_COMP_ROT_SHARE_BBOX_FULL));
    const float rot_diag_score =
        1.0f - smoothstep01((est.diag_rel - PLATE_QUAD_COMP_ROT_SHARE_DIAG_FULL) /
                            (PLATE_QUAD_COMP_ROT_SHARE_DIAG_ZERO - PLATE_QUAD_COMP_ROT_SHARE_DIAG_FULL));
    const float rot_pose_score = clamp_float(0.55f * rot_side_score + 0.45f * rot_bbox_score, 0.0f, 1.0f);
    float rotation_share = rot_roll_score * rot_pose_score * rot_diag_score;
    if (rot_roll_score > 0.70f &&
        rot_diag_score > 0.55f &&
        current_side_mean_abs < PLATE_QUAD_COMP_ROT_SHARE_SIDE_ZERO &&
        std::fabs(bbox_view_shift) < PLATE_QUAD_COMP_ROT_SHARE_BBOX_ZERO) {
        rotation_share = std::max(rotation_share,
                                  PLATE_QUAD_COMP_ROT_SHARE_MIN_WHEN_ROLL *
                                      rot_roll_score * rot_diag_score);
    }
    rotation_share = clamp_float(rotation_share, 0.0f, 1.0f);
    const float rot_unlock_tb_score =
        smoothstep01((est.tb_rel - PLATE_QUAD_COMP_ROT_UNLOCK_TB_START) /
                     PLATE_QUAD_COMP_ROT_UNLOCK_TB_RANGE);
    const float rot_unlock_delta_score =
        smoothstep01((roll_delta_mean - PLATE_QUAD_COMP_ROT_UNLOCK_DELTA_START) /
                     PLATE_QUAD_COMP_ROT_UNLOCK_DELTA_RANGE);
    const float rot_unlock_lock_score =
        smoothstep01((projection_persp_lock_raw - PLATE_QUAD_COMP_ROT_UNLOCK_LOCK_START) /
                     PLATE_QUAD_COMP_ROT_UNLOCK_LOCK_RANGE);
    const float bbox_aspect = box.width / std::max(1.0f, box.height);
    const float pure_persp_guard =
        clamp_float((1.0f - smoothstep01((est.tb_rel - PLATE_QUAD_COMP_PURE_GUARD_TB_START) /
                                         PLATE_QUAD_COMP_PURE_GUARD_TB_RANGE)) *
                        (1.0f - smoothstep01((est.short_rel - PLATE_QUAD_COMP_PURE_GUARD_SHORT_START) /
                                             PLATE_QUAD_COMP_PURE_GUARD_SHORT_RANGE)) *
                        (1.0f - smoothstep01((std::fabs(est.roll_shear) - PLATE_QUAD_COMP_PURE_GUARD_ROLL_START) /
                                             PLATE_QUAD_COMP_PURE_GUARD_ROLL_RANGE)) *
                        smoothstep01((bbox_aspect - PLATE_QUAD_COMP_PURE_GUARD_ASPECT_START) /
                                     PLATE_QUAD_COMP_PURE_GUARD_ASPECT_RANGE),
                    0.0f,
                    1.0f);
    const float pure_persp_release = (1.0f - pure_persp_guard) * (1.0f - pure_persp_guard);
    const float rotation_unlock_evidence =
        rot_roll_score * rot_unlock_tb_score * rot_unlock_delta_score * rot_unlock_lock_score;
    est.rotation_unlock_evidence = rotation_unlock_evidence;
    const float roll_conflict_score =
        (left_roll_conflict && right_roll_conflict) ? 1.0f : ((left_roll_conflict || right_roll_conflict) ? 0.65f : 0.0f);
    const float roll_residual_delta_score =
        smoothstep01((roll_delta_mean - PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_DELTA_START) /
                     PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_DELTA_RANGE);
    est.roll_residual_evidence =
        rot_roll_score * roll_residual_delta_score * roll_conflict_score * est.horizontal_reliability;
    const float high_roll_score =
        smoothstep01((std::fabs(est.roll_shear) - PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_START) /
                     PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_RANGE);
    // High-rotation plates can have inflated diagonal imbalance when the vertical
    // edges are under-corrected, so this unlock is driven by roll residuals instead.
    est.high_roll_unlock =
        high_roll_score * roll_residual_delta_score * roll_conflict_score *
        rot_side_score * est.horizontal_reliability * rot_unlock_lock_score * pure_persp_release;
    const float affine_roll_score =
        smoothstep01((std::fabs(est.roll_shear) - PLATE_QUAD_COMP_AFFINE_ROLL_START) /
                     PLATE_QUAD_COMP_AFFINE_ROLL_RANGE);
    const float affine_deficit_score =
        smoothstep01((roll_delta_mean - PLATE_QUAD_COMP_AFFINE_DELTA_START) /
                     PLATE_QUAD_COMP_AFFINE_DELTA_RANGE);
    est.affine_rotation_unlock =
        est.affine_shape_score * affine_roll_score * affine_deficit_score *
        rot_unlock_lock_score * pure_persp_release;
    const float rotation_unlock_scale =
        clamp_float(PLATE_QUAD_COMP_ROT_UNLOCK_LOCK_SCALE * rotation_unlock_evidence +
                        PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_LOCK_SCALE * est.high_roll_unlock +
                        PLATE_QUAD_COMP_AFFINE_UNLOCK_LOCK_SCALE * est.affine_rotation_unlock,
                    0.0f,
                    0.85f);
    const float rotation_unlock_confidence =
        std::max(std::max(PLATE_QUAD_COMP_ROT_UNLOCK_CONF_MAX * rotation_unlock_evidence,
                          PLATE_QUAD_COMP_HIGH_ROLL_UNLOCK_CONF_MAX * est.high_roll_unlock),
                 PLATE_QUAD_COMP_AFFINE_UNLOCK_CONF_SCALE * est.affine_rotation_unlock);
    // Most plates are a mix of rotation and perspective: the lock is softened
    // continuously instead of treating "front roll" and "pure perspective" as
    // mutually exclusive classes.
    est.projection_persp_lock =
        projection_persp_lock_raw * (1.0f - rotation_share) * (1.0f - rotation_unlock_scale);
    const float lock_free = (1.0f - est.projection_persp_lock) * (1.0f - est.projection_persp_lock);
    const float score_rotation_confidence =
        std::max(std::max(est.roll_score * lock_free, rotation_share), rotation_unlock_confidence);
    const bool bbox_view_aligned = bbox_view_shift * est.signed_diag_rel > 0.0f;
    const float bbox_view_conf =
        bbox_view_aligned
            ? smoothstep01((std::fabs(bbox_view_shift) - PLATE_QUAD_COMP_BBOX_VIEW_SHIFT_START) /
                           PLATE_QUAD_COMP_BBOX_VIEW_SHIFT_RANGE)
            : 0.0f;
    const float low_roll_weight =
        1.0f - smoothstep01((std::fabs(est.roll_shear) - PLATE_QUAD_COMP_MIXED_LOW_ROLL_MAX) /
                            PLATE_QUAD_COMP_MIXED_LOW_ROLL_RANGE);
    const float mixed_view_diag_score =
        smoothstep01((est.diag_rel - PLATE_QUAD_COMP_MIXED_VIEW_DIAG_START) /
                     PLATE_QUAD_COMP_MIXED_VIEW_DIAG_RANGE);
    const float weak_view_overfill_guard =
        low_roll_weight * mixed_view_diag_score *
        std::max(smoothstep01((quad_span_h_ratio - PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_H_START) /
                              PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_H_RANGE),
                 smoothstep01((quad_area_ratio - PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_AREA_START) /
                              PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_AREA_RANGE));
    const float weak_view_overfill_scale =
        clamp_float(1.0f - PLATE_QUAD_COMP_WEAK_VIEW_OVERFILL_DAMP * weak_view_overfill_guard,
                    0.0f,
                    1.0f);
    const float mixed_view_conf =
        bbox_view_conf * mixed_view_diag_score * low_roll_weight * (1.0f - est.projection_persp_lock);
    const float view_sign = (est.signed_diag_rel >= 0.0f) ? 1.0f : -1.0f;
    const float mixed_view_target =
        view_sign *
        clamp_float(PLATE_QUAD_COMP_MIXED_VIEW_TARGET_GAIN * est.diag_rel +
                        PLATE_QUAD_COMP_MIXED_VIEW_BBOX_GAIN * std::fabs(bbox_view_shift) +
                        PLATE_QUAD_COMP_MIXED_VIEW_ROLL_GAIN * std::fabs(est.roll_shear),
                    0.0f,
                    0.18f);
    const float mixed_roll_gap_conf =
        smoothstep01((roll_delta_mean - PLATE_QUAD_COMP_MIXED_ROLL_GAP_START) /
                     PLATE_QUAD_COMP_MIXED_ROLL_GAP_RANGE);
    const float mixed_roll_conf =
        PLATE_QUAD_COMP_MIXED_ROLL_CONF_SCALE * mixed_roll_gap_conf * low_roll_weight *
        (1.0f - est.projection_persp_lock) * (1.0f - 0.80f * mixed_view_conf);
    const bool residual_roll_view_window =
        est.diag_rel >= PLATE_QUAD_COMP_RESIDUAL_ROLL_DIAG_MIN &&
        est.diag_rel <= PLATE_QUAD_COMP_RESIDUAL_ROLL_DIAG_MAX &&
        est.tb_rel <= PLATE_QUAD_COMP_RESIDUAL_ROLL_TB_MAX;
    float residual_rotation_confidence = 0.0f;
    if (std::fabs(est.roll_shear) >= PLATE_QUAD_COMP_RESIDUAL_ROLL_MIN &&
        std::fabs(est.roll_shear) <= PLATE_QUAD_COMP_RESIDUAL_ROLL_MAX &&
        residual_roll_view_window &&
        est.persp_score <= 0.04f &&
        bbox_view_conf > 0.15f &&
        current_side_mean_abs <= 0.13f &&
        (left_roll_conflict || right_roll_conflict)) {
        // Low roll angles can still require x correction. In this weak-view window,
        // signed diagonal geometry is a safer direction source than horizontal roll.
        const float conflict_scale = (left_roll_conflict && right_roll_conflict) ? 1.0f : 0.65f;
        residual_rotation_confidence =
            PLATE_QUAD_COMP_RESIDUAL_ROLL_CONF_MAX *
            conflict_scale *
            smoothstep01((roll_delta_mean - PLATE_QUAD_COMP_RESIDUAL_ROLL_DELTA_START) /
                         PLATE_QUAD_COMP_RESIDUAL_ROLL_DELTA_RANGE) *
            (0.60f + 0.40f * bbox_view_conf) * lock_free;
    }
    est.rotation_confidence = std::max(std::max(score_rotation_confidence, residual_rotation_confidence),
                                       std::max(mixed_view_conf, mixed_roll_conf));
    const bool residual_view_floor =
        residual_rotation_confidence > score_rotation_confidence + 1e-4f &&
        std::fabs(est.signed_diag_rel) > 1e-4f;
    est.under_evidence_score =
        std::max(std::max(mixed_view_conf, est.affine_rotation_unlock),
                 std::max(rotation_unlock_evidence, est.high_roll_unlock));
    est.under_deficit_score =
        std::max(std::max(mixed_roll_conf, affine_deficit_score), rot_unlock_delta_score);
    est.under_target_dx = mixed_view_target;
    est.under_boost = PLATE_QUAD_COMP_MIXED_ROLL_DELTA_GAIN;
    est.pure_persp_score = est.projection_persp_lock;
    est.persp_apply = est.rotation_confidence;
    est.perspective_weight = est.rotation_confidence;
    if (est.signed_diag_rel > 1e-4f) {
        est.view_case = (est.persp_score > 0.5f) ? "right_top_view" : "right_top_weak";
    } else if (est.signed_diag_rel < -1e-4f) {
        est.view_case = (est.persp_score > 0.5f) ? "left_top_view" : "left_top_weak";
    } else {
        est.view_case = "front_roll";
    }

    float feature[PLATE_QUAD_COMP_FEATURE_COUNT] = {
        est.roll_shear,
        left_shear,
        right_shear,
        est.signed_diag_rel,
        est.signed_tb_rel,
        est.signed_lr_rel,
        std::fabs(est.roll_shear),
        est.diag_rel,
        est.roll_shear * est.diag_rel,
        est.signed_diag_rel * std::fabs(est.roll_shear),
        1.0f};
    const float fit_left_raw = dot_plate_quad_fit(PLATE_QUAD_COMP_FIT_LEFT_COEF, feature);
    const float fit_right_raw = dot_plate_quad_fit(PLATE_QUAD_COMP_FIT_RIGHT_COEF, feature);
    float left_floor_delta = est.roll_left_delta;
    float right_floor_delta = est.roll_right_delta;
    bool prefer_floor_on_conflict = false;
    bool mixed_floor_active = false;
    if (residual_view_floor) {
        const float view_delta_gain =
            PLATE_QUAD_COMP_RESIDUAL_ROLL_VIEW_DELTA_GAIN +
            PLATE_QUAD_COMP_BBOX_VIEW_EXTRA_GAIN * bbox_view_conf;
        left_floor_delta = view_sign * std::fabs(est.roll_left_delta) *
                           view_delta_gain;
        right_floor_delta = view_sign * std::fabs(est.roll_right_delta) *
                            view_delta_gain;
        est.delta_source = "fit_residual_bbox_view_floor";
        prefer_floor_on_conflict = true;
    } else if (mixed_view_conf > 0.25f) {
        left_floor_delta = (mixed_view_target - left_shear) * PLATE_QUAD_COMP_MIXED_VIEW_DELTA_GAIN;
        right_floor_delta = (mixed_view_target - right_shear) * PLATE_QUAD_COMP_MIXED_VIEW_DELTA_GAIN;
        est.delta_source = "fit_mixed_view_floor";
        prefer_floor_on_conflict = true;
        mixed_floor_active = true;
    } else if (mixed_roll_conf > 0.16f) {
        left_floor_delta = est.roll_left_delta * PLATE_QUAD_COMP_MIXED_ROLL_DELTA_GAIN;
        right_floor_delta = est.roll_right_delta * PLATE_QUAD_COMP_MIXED_ROLL_DELTA_GAIN;
        est.delta_source = "fit_mixed_roll_floor";
        prefer_floor_on_conflict = true;
        mixed_floor_active = true;
    }
    if (rotation_unlock_evidence > 0.08f) {
        est.delta_source += "_rot_unlock";
    }
    if (est.high_roll_unlock > 0.08f) {
        est.delta_source += "_high_roll_unlock";
    }
    if (est.affine_rotation_unlock > 0.08f) {
        est.delta_source += "_affine_unlock";
    }
    est.under_planned_dx = std::max(std::fabs(left_floor_delta), std::fabs(right_floor_delta));
    est.under_move_score = mixed_floor_active ? 1.0f : 0.0f;
    const SideDeltaV2 left_delta_est =
        estimate_side_delta_v2(fit_left_raw,
                               left_floor_delta,
                               est.projection_persp_lock,
                               est.rotation_confidence,
                               prefer_floor_on_conflict);
    const SideDeltaV2 right_delta_est =
        estimate_side_delta_v2(fit_right_raw,
                               right_floor_delta,
                               est.projection_persp_lock,
                               est.rotation_confidence,
                               prefer_floor_on_conflict);
    float left_delta = left_delta_est.final_delta;
    float right_delta = right_delta_est.final_delta;
    if (weak_view_overfill_guard > 0.02f) {
        left_delta *= weak_view_overfill_scale;
        right_delta *= weak_view_overfill_scale;
        est.delta_source += "_overfill_guard";
    }
    est.base_left_delta = left_delta_est.fit_delta;
    est.base_right_delta = right_delta_est.fit_delta;
    est.fit_left_delta = left_delta;
    est.fit_right_delta = right_delta;
    est.base_delta = (est.base_left_delta * std::fabs(left_h) + est.base_right_delta * std::fabs(right_h)) /
                     std::max(1e-6f, std::fabs(left_h) + std::fabs(right_h));
    est.raw_delta = est.base_delta;
    est.final_delta = (left_delta * std::fabs(left_h) + right_delta * std::fabs(right_h)) /
                      std::max(1e-6f, std::fabs(left_h) + std::fabs(right_h));
    est.delta_shear = est.final_delta;
    est.shear = est.final_delta;
    est.target_shear = est.current_shear + est.final_delta;
    est.target_shear = clamp_float(est.target_shear,
                                   -PLATE_QUAD_COMP_TARGET_SHEAR_LIMIT,
                                   PLATE_QUAD_COMP_TARGET_SHEAR_LIMIT);
    est.view_shear = est.target_shear;
    est.delta_direction = 0.0f;

    const float bbox_fit = bbox_fit_score(ordered, box);
    const float bbox_suppress = smoothstep01((bbox_fit - 0.55f) / 0.30f);
    est.left_gain = std::fabs(left_delta) / std::max(1e-6f, std::fabs(est.base_left_delta));
    est.right_gain = std::fabs(right_delta) / std::max(1e-6f, std::fabs(est.base_right_delta));
    est.gain = std::max(est.left_gain, est.right_gain);
    est.effective_gain = est.gain;
    est.gain_raw = std::max(std::fabs(est.base_left_delta), std::fabs(est.base_right_delta));
    est.need_score = std::max(smoothstep01((std::fabs(left_delta) - 0.045f) / 0.22f),
                              smoothstep01((std::fabs(right_delta) - 0.045f) / 0.22f));
    est.missing_shear = std::max(std::fabs(left_delta), std::fabs(right_delta));
    est.missing_px_norm = 0.5f * est.missing_shear * std::max(std::fabs(left_h), std::fabs(right_h)) / mean_tb_len;
    est.coverage = bbox_fit;
    est.coverage_scale = bbox_suppress;
    est.over_risk = std::max(est.projection_persp_lock, weak_view_overfill_guard);
    est.deficit_ratio = clamp_float(1.0f - bbox_fit, 0.0f, 1.0f);
    est.fit_quality = est.geometry_quality * (1.0f - 0.50f * est.projection_persp_lock);
    est.fit_gain = est.gain;
    est.under_weight = std::max(left_delta_est.under_weight, right_delta_est.under_weight);
    est.roll_floor_weight = std::max(left_delta_est.floor_weight, right_delta_est.floor_weight);
    est.affine_floor_weight = est.affine_rotation_unlock * est.under_weight;
    est.fit_model_version = "ccpd_50k_ridge_mixed_rotation_v2_9_1";

    const float mean_plate_width = mean_tb_len;
    const float rotation_limit_boost = 1.0f + PLATE_QUAD_COMP_ROTATION_DX_LIMIT_BOOST * score_rotation_confidence;
    float max_point_dx = std::min(PLATE_QUAD_COMP_MAX_POINT_DX * rotation_limit_boost,
                                  std::min((PLATE_QUAD_COMP_BBOX_DX_RATIO +
                                            0.040f * score_rotation_confidence) *
                                               box.width,
                                           (PLATE_QUAD_COMP_WIDTH_DX_RATIO +
                                            0.060f * score_rotation_confidence) *
                                               mean_plate_width));
    if (mixed_floor_active) {
        max_point_dx = std::min(max_point_dx, PLATE_QUAD_COMP_MIXED_FLOOR_MAX_POINT_DX);
    }
    max_point_dx = std::max(1.0f, max_point_dx);
    est.effective_max_point_dx = max_point_dx;

    const float left_full_plan = left_delta * left_h;
    const float right_full_plan = right_delta * right_h;
    SideCornerDx best_left_move;
    SideCornerDx best_right_move;
    std::vector<Point2f> best_quad = ordered;
    float best_scale = 0.0f;
    float best_max_point_move = 0.0f;
    const float safety_scales[] = {1.0f, 0.75f, 0.5f, 0.25f};
    std::string reliable_reason;
    std::string reject_reason = "none";
    for (float scale : safety_scales) {
        const SideCornerDx left_move = split_side_dx_by_bbox_slack(ordered[0],
                                                                   ordered[3],
                                                                   left_full_plan * scale,
                                                                   box,
                                                                   max_point_dx);
        const SideCornerDx right_move = split_side_dx_by_bbox_slack(ordered[1],
                                                                    ordered[2],
                                                                    right_full_plan * scale,
                                                                    box,
                                                                    max_point_dx);
        const float max_point_move = std::max(left_move.max_abs_dx, right_move.max_abs_dx);
        if (max_point_move < PLATE_QUAD_COMP_MIN_POINT_MOVE_PX) {
            reject_reason = "small_move_after_safety_scale";
            continue;
        }

        std::vector<Point2f> compensated_quad = ordered;
        compensated_quad[0].x += left_move.top_dx;
        compensated_quad[3].x += left_move.bottom_dx;
        compensated_quad[1].x += right_move.top_dx;
        compensated_quad[2].x += right_move.bottom_dx;
        bool finite = true;
        for (const Point2f& p : compensated_quad) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                finite = false;
                break;
            }
        }
        if (!finite) {
            reject_reason = "corner_not_finite";
            continue;
        }

        const float new_top_len = point_norm(compensated_quad[1] - compensated_quad[0]);
        const float new_bottom_len = point_norm(compensated_quad[2] - compensated_quad[3]);
        if (new_top_len < PLATE_QUAD_COMP_WIDTH_KEEP_RATIO * top_len ||
            new_bottom_len < PLATE_QUAD_COMP_WIDTH_KEEP_RATIO * bottom_len) {
            reject_reason = "width_shrink_guard";
            continue;
        }
        if (!quad_looks_reliable(compensated_quad, box, image_size, reliable_reason)) {
            reject_reason = "corner_unreliable:" + reliable_reason;
            continue;
        }

        best_left_move = left_move;
        best_right_move = right_move;
        best_quad = compensated_quad;
        best_scale = scale;
        best_max_point_move = max_point_move;
        break;
    }

    if (best_scale <= 0.0f) {
        reason = (reject_reason == "small_move_after_safety_scale") ? "corner_raw_small_move" : reject_reason;
        est.reason = reason;
        est.selected_level = "raw";
        est.confidence = est.geometry_quality * est.gain;
        return false;
    }
    est.move_norm = best_max_point_move / std::max(1.0f, mean_plate_width);
    est.selected_level = comp_level_from_move(best_max_point_move, mean_plate_width);

    final_quad = best_quad;
    est.safety_scale = best_scale;
    est.dx_tl = best_left_move.top_dx;
    est.dx_bl = best_left_move.bottom_dx;
    est.dx_tr = best_right_move.top_dx;
    est.dx_br = best_right_move.bottom_dx;
    est.left_dx = best_left_move.full_dx;
    est.right_dx = best_right_move.full_dx;
    est.delta_shear = (est.left_dx / std::max(1e-6f, left_h) * std::fabs(left_h) +
                       est.right_dx / std::max(1e-6f, right_h) * std::fabs(right_h)) /
                      std::max(1e-6f, std::fabs(left_h) + std::fabs(right_h));
    est.final_delta = est.delta_shear;
    est.shear = est.delta_shear;
    est.confidence = est.fit_quality * clamp_float(0.45f + est.move_norm / 0.04f, 0.0f, 1.0f);
    est.reason = "corner_dx_" + est.selected_level;
    reason = est.reason;
    return true;
}
static Mat plate_crop(const Mat& img,
                      const Rect2f& box,
                      const std::vector<Point2f>& pts,
                      PlateCropDebug* debug = nullptr,
                      Size shape = Size(94, 24)) {
    if (debug) {
        debug->raw_quad = pts;
        debug->pre_comp_warp_quad.clear();
        debug->warp_quad.clear();
        debug->crop_method = "none";
        debug->quad_reliable = false;
        debug->quad_error_reason.clear();
        debug->quad_comp_attempted = false;
        debug->quad_compensated = false;
        debug->quad_comp_formula_version = "none";
        debug->quad_comp_method = "none";
        debug->quad_comp_reason.clear();
        debug->quad_comp_shear = 0.0f;
        debug->quad_comp_confidence = 0.0f;
        debug->quad_comp_target_shear = 0.0f;
        debug->quad_comp_current_shear = 0.0f;
        debug->quad_comp_current_left_shear = 0.0f;
        debug->quad_comp_current_right_shear = 0.0f;
        debug->quad_comp_raw_delta = 0.0f;
        debug->quad_comp_roll_shear = 0.0f;
        debug->quad_comp_roll_left_delta = 0.0f;
        debug->quad_comp_roll_right_delta = 0.0f;
        debug->quad_comp_view_shear = 0.0f;
        debug->quad_comp_perspective_weight = 0.0f;
        debug->quad_comp_delta_shear = 0.0f;
        debug->quad_comp_delta_direction = 0.0f;
        debug->quad_comp_left_dx = 0.0f;
        debug->quad_comp_right_dx = 0.0f;
        debug->quad_comp_dx_tl = 0.0f;
        debug->quad_comp_dx_tr = 0.0f;
        debug->quad_comp_dx_br = 0.0f;
        debug->quad_comp_dx_bl = 0.0f;
        debug->quad_comp_gain = 0.0f;
        debug->quad_comp_roll_score = 0.0f;
        debug->quad_comp_persp_score = 0.0f;
        debug->quad_comp_persp_apply = 0.0f;
        debug->quad_comp_pure_persp_score = 0.0f;
        debug->quad_comp_left_gain = 0.0f;
        debug->quad_comp_right_gain = 0.0f;
        debug->quad_comp_missing_shear = 0.0f;
        debug->quad_comp_missing_px_norm = 0.0f;
        debug->quad_comp_need_score = 0.0f;
        debug->quad_comp_geometry_quality = 0.0f;
        debug->quad_comp_gain_raw = 0.0f;
        debug->quad_comp_coverage = 0.0f;
        debug->quad_comp_coverage_scale = 0.0f;
        debug->quad_comp_over_risk = 0.0f;
        debug->quad_comp_effective_gain = 0.0f;
        debug->quad_comp_effective_max_point_dx = 0.0f;
        debug->quad_comp_under_planned_dx = 0.0f;
        debug->quad_comp_under_move_score = 0.0f;
        debug->quad_comp_under_evidence_score = 0.0f;
        debug->quad_comp_under_deficit_score = 0.0f;
        debug->quad_comp_under_target_dx = 0.0f;
        debug->quad_comp_under_boost = 1.0f;
        debug->quad_comp_deficit_ratio = 0.0f;
        debug->quad_comp_horizontal_angle_deg = 0.0f;
        debug->quad_comp_diag1 = 0.0f;
        debug->quad_comp_diag2 = 0.0f;
        debug->quad_comp_diag_ratio = 0.0f;
        debug->quad_comp_diag_rel = 0.0f;
        debug->quad_comp_signed_diag_rel = 0.0f;
        debug->quad_comp_tb_rel = 0.0f;
        debug->quad_comp_signed_tb_rel = 0.0f;
        debug->quad_comp_signed_lr_rel = 0.0f;
        debug->quad_comp_short_rel = 0.0f;
        debug->quad_comp_perspective_strength = 0.0f;
        debug->quad_comp_diag_delta = 0.0f;
        debug->quad_comp_base_delta = 0.0f;
        debug->quad_comp_final_delta = 0.0f;
        debug->quad_comp_base_left_delta = 0.0f;
        debug->quad_comp_base_right_delta = 0.0f;
        debug->quad_comp_fit_left_delta = 0.0f;
        debug->quad_comp_fit_right_delta = 0.0f;
        debug->quad_comp_fit_gain = 1.0f;
        debug->quad_comp_under_weight = 0.0f;
        debug->quad_comp_roll_floor_weight = 0.0f;
        debug->quad_comp_fit_quality = 0.0f;
        debug->quad_comp_projection_persp_lock_raw = 0.0f;
        debug->quad_comp_projection_persp_lock = 0.0f;
        debug->quad_comp_rotation_confidence = 0.0f;
        debug->quad_comp_roll_residual_evidence = 0.0f;
        debug->quad_comp_high_roll_unlock = 0.0f;
        debug->quad_comp_horizontal_reliability = 0.0f;
        debug->quad_comp_rotation_unlock_evidence = 0.0f;
        debug->quad_comp_diag_mid_norm = 0.0f;
        debug->quad_comp_affine_shape_score = 0.0f;
        debug->quad_comp_trapezoid_score = 0.0f;
        debug->quad_comp_affine_rotation_unlock = 0.0f;
        debug->quad_comp_affine_floor_weight = 0.0f;
        debug->quad_comp_tilt_share = 0.0f;
        debug->quad_comp_safety_scale = 0.0f;
        debug->quad_comp_move_norm = 0.0f;
        debug->quad_comp_fit_model_version = "none";
        debug->quad_comp_delta_source = "none";
        debug->quad_comp_view_case = "none";
        debug->quad_comp_selected_level = "raw";
        debug->quad_comp_candidate_count = 0;
        debug->quad_comp_quality_raw = -1.0f;
        debug->quad_comp_quality_weak = -1.0f;
        debug->quad_comp_quality_medium = -1.0f;
        debug->quad_comp_quality_strong = -1.0f;
        debug->quad_comp_quality_margin = 0.0f;
        debug->quad_comp_geom_ms = 0.0;
        debug->quad_comp_select_ms = 0.0;
        debug->quad_comp_ms = 0.0;
        debug->quad_comp_candidate_levels.clear();
        debug->quad_comp_candidate_qualities.clear();
        debug->quad_comp_candidate_quads.clear();
    }
    if (img.empty()) return Mat();

    std::vector<Point2f> ordered;
    std::string reason;
    bool ordered_ok = order_plate_points_for_warp(pts, ordered);
    bool reliable = ordered_ok && quad_looks_reliable(ordered, box, img.size(), reason);
    if (reliable) {
        std::vector<Point2f> final_quad = ordered;
        if (debug) {
            debug->pre_comp_warp_quad = ordered;
        }
        if (plate_quad_compensation_enabled()) {
            auto comp_start = std::chrono::high_resolution_clock::now();
            std::vector<Point2f> selected_quad = ordered;
            QuadGeometryCompEstimate est;
            std::string comp_reason;
            auto geom_start = std::chrono::high_resolution_clock::now();
            bool applied = compensate_plate_quad_x_corner_geometry_bbox(ordered,
                                                                        box,
                                                                        img.size(),
                                                                        est,
                                                                        selected_quad,
                                                                        comp_reason);
            est.geom_ms = ms_since(geom_start);
            est.select_ms = 0.0;
            if (applied) {
                final_quad = selected_quad;
            }
            double comp_ms = ms_since(comp_start);
            if (debug) {
                debug->quad_comp_attempted = true;
                debug->quad_comp_formula_version = PLATE_QUAD_COMP_FORMULA_VERSION;
                debug->quad_comp_method = PLATE_QUAD_COMP_FORMULA_VERSION;
                debug->quad_comp_ms = comp_ms;
                debug->quad_comp_geom_ms = est.geom_ms;
                debug->quad_comp_select_ms = est.select_ms;
                debug->quad_comp_shear = est.shear;
                debug->quad_comp_confidence = est.confidence;
                debug->quad_comp_target_shear = est.target_shear;
                debug->quad_comp_current_shear = est.current_shear;
                debug->quad_comp_current_left_shear = est.current_left_shear;
                debug->quad_comp_current_right_shear = est.current_right_shear;
                debug->quad_comp_raw_delta = est.raw_delta;
                debug->quad_comp_roll_shear = est.roll_shear;
                debug->quad_comp_roll_left_delta = est.roll_left_delta;
                debug->quad_comp_roll_right_delta = est.roll_right_delta;
                debug->quad_comp_view_shear = est.view_shear;
                debug->quad_comp_perspective_weight = est.perspective_weight;
                debug->quad_comp_delta_shear = est.delta_shear;
                debug->quad_comp_delta_direction = est.delta_direction;
                debug->quad_comp_left_dx = est.left_dx;
                debug->quad_comp_right_dx = est.right_dx;
                debug->quad_comp_dx_tl = est.dx_tl;
                debug->quad_comp_dx_tr = est.dx_tr;
                debug->quad_comp_dx_br = est.dx_br;
                debug->quad_comp_dx_bl = est.dx_bl;
                debug->quad_comp_gain = est.gain;
                debug->quad_comp_roll_score = est.roll_score;
                debug->quad_comp_persp_score = est.persp_score;
                debug->quad_comp_persp_apply = est.persp_apply;
                debug->quad_comp_pure_persp_score = est.pure_persp_score;
                debug->quad_comp_left_gain = est.left_gain;
                debug->quad_comp_right_gain = est.right_gain;
                debug->quad_comp_missing_shear = est.missing_shear;
                debug->quad_comp_missing_px_norm = est.missing_px_norm;
                debug->quad_comp_need_score = est.need_score;
                debug->quad_comp_geometry_quality = est.geometry_quality;
                debug->quad_comp_gain_raw = est.gain_raw;
                debug->quad_comp_coverage = est.coverage;
                debug->quad_comp_coverage_scale = est.coverage_scale;
                debug->quad_comp_over_risk = est.over_risk;
                debug->quad_comp_effective_gain = est.effective_gain;
                debug->quad_comp_effective_max_point_dx = est.effective_max_point_dx;
                debug->quad_comp_under_planned_dx = est.under_planned_dx;
                debug->quad_comp_under_move_score = est.under_move_score;
                debug->quad_comp_under_evidence_score = est.under_evidence_score;
                debug->quad_comp_under_deficit_score = est.under_deficit_score;
                debug->quad_comp_under_target_dx = est.under_target_dx;
                debug->quad_comp_under_boost = est.under_boost;
                debug->quad_comp_deficit_ratio = est.deficit_ratio;
                debug->quad_comp_horizontal_angle_deg = est.horizontal_angle_deg;
                debug->quad_comp_diag1 = est.diag1;
                debug->quad_comp_diag2 = est.diag2;
                debug->quad_comp_diag_ratio = est.diag_ratio;
                debug->quad_comp_diag_rel = est.diag_rel;
                debug->quad_comp_signed_diag_rel = est.signed_diag_rel;
                debug->quad_comp_tb_rel = est.tb_rel;
                debug->quad_comp_signed_tb_rel = est.signed_tb_rel;
                debug->quad_comp_signed_lr_rel = est.signed_lr_rel;
                debug->quad_comp_short_rel = est.short_rel;
                debug->quad_comp_perspective_strength = est.perspective_strength;
                debug->quad_comp_diag_delta = est.diag_delta;
                debug->quad_comp_base_delta = est.base_delta;
                debug->quad_comp_final_delta = est.final_delta;
                debug->quad_comp_base_left_delta = est.base_left_delta;
                debug->quad_comp_base_right_delta = est.base_right_delta;
                debug->quad_comp_fit_left_delta = est.fit_left_delta;
                debug->quad_comp_fit_right_delta = est.fit_right_delta;
                debug->quad_comp_fit_gain = est.fit_gain;
                debug->quad_comp_under_weight = est.under_weight;
                debug->quad_comp_roll_floor_weight = est.roll_floor_weight;
                debug->quad_comp_fit_quality = est.fit_quality;
                debug->quad_comp_projection_persp_lock_raw = est.projection_persp_lock_raw;
                debug->quad_comp_projection_persp_lock = est.projection_persp_lock;
                debug->quad_comp_rotation_confidence = est.rotation_confidence;
                debug->quad_comp_roll_residual_evidence = est.roll_residual_evidence;
                debug->quad_comp_high_roll_unlock = est.high_roll_unlock;
                debug->quad_comp_horizontal_reliability = est.horizontal_reliability;
                debug->quad_comp_rotation_unlock_evidence = est.rotation_unlock_evidence;
                debug->quad_comp_diag_mid_norm = est.diag_mid_norm;
                debug->quad_comp_affine_shape_score = est.affine_shape_score;
                debug->quad_comp_trapezoid_score = est.trapezoid_score;
                debug->quad_comp_affine_rotation_unlock = est.affine_rotation_unlock;
                debug->quad_comp_affine_floor_weight = est.affine_floor_weight;
                debug->quad_comp_tilt_share = est.tilt_share;
                debug->quad_comp_safety_scale = est.safety_scale;
                debug->quad_comp_move_norm = est.move_norm;
                debug->quad_comp_fit_model_version = est.fit_model_version;
                debug->quad_comp_delta_source = est.delta_source;
                debug->quad_comp_view_case = est.view_case;
                debug->quad_comp_selected_level = est.selected_level;
                debug->quad_comp_candidate_count = est.candidate_count;
                debug->quad_comp_quality_raw = est.quality_raw;
                debug->quad_comp_quality_weak = est.quality_weak;
                debug->quad_comp_quality_medium = est.quality_medium;
                debug->quad_comp_quality_strong = est.quality_strong;
                debug->quad_comp_quality_margin = est.quality_margin;
                debug->quad_comp_candidate_levels = est.candidate_levels;
                debug->quad_comp_candidate_qualities = est.candidate_qualities;
                debug->quad_comp_candidate_quads = est.candidate_quads;
                debug->quad_comp_reason = comp_reason;
            }
            if (applied) {
                if (debug) {
                    debug->quad_compensated = true;
                }
            }
        }
        if (debug) {
            debug->warp_quad = final_quad;
            debug->crop_method = "quad";
            debug->quad_reliable = true;
            debug->quad_error_reason = "ok";
        }
        return crop_from_quad(img, final_quad, shape);
    }

    if (!ordered_ok) reason = "order_failed";
    std::vector<Point2f> fallback_quad = bbox_quad_for_warp(box, img.size());
    if (debug) {
        if (ordered_ok) debug->pre_comp_warp_quad = ordered;
        debug->warp_quad = fallback_quad;
        debug->crop_method = "bbox_fallback";
        debug->quad_reliable = false;
        debug->quad_error_reason = reason.empty() ? "unreliable_quad" : reason;
    }
    return crop_from_quad(img, fallback_quad, shape);
}

// 车牌检测后处理：
// 分数过滤 -> DFL 解码 -> 关键点解码 -> NMS -> 裁剪（最多 batch=4）。
bool detect_plate_candidates(const Mat& mid_img,
                             RknnModel& det_model,
                             float conf_thres,
                             float iou_thres,
                             std::vector<Mat>& crops,
                             std::vector<Rect2f>& boxes,
                             std::vector<int>& class_ids,
                             bool input_is_rgb,
                             double* npu_ms,
                             RknnStageTiming* timing,
                             std::vector<float>* scores,
                             std::vector<std::vector<Point2f>>* keypoints,
                             std::vector<PlateCropDebug>* crop_debugs,
                             std::vector<float>* raw_output) {
    if (npu_ms) *npu_ms = 0.0;
    if (timing) timing->reset();
    crops.clear();
    boxes.clear();
    class_ids.clear();
    if (scores) scores->clear();
    if (keypoints) keypoints->clear();
    if (crop_debugs) crop_debugs->clear();
    if (raw_output) raw_output->clear();
    // 线程局部缓存，避免在线程函数中反复分配。
    static thread_local Mat rgb;
    if (input_is_rgb) rgb = mid_img;
    else {
#ifdef USE_RGA
        rga_cvt_bgr_to_rgb(mid_img, rgb);
#else
        cv::cvtColor(mid_img, rgb, cv::COLOR_BGR2RGB);
#endif
    }

    rknn_input in[1];
    memset(in, 0, sizeof(in));
    in[0].index = 0;
    in[0].type = RKNN_TENSOR_UINT8;
    in[0].size = rgb.total() * rgb.elemSize();
    in[0].fmt = RKNN_TENSOR_NHWC;
    in[0].buf = rgb.data;
    auto input_start = std::chrono::high_resolution_clock::now();
    if (rknn_inputs_set(det_model.ctx, det_model.num_in, in) != RKNN_SUCC) return false;
    if (timing) timing->input_ms += ms_since(input_start);
    auto npu_start = std::chrono::high_resolution_clock::now();
    double run_start_ms = steady_ms_now();
    int run_ret = rknn_run(det_model.ctx, nullptr);
    double run_end_ms = steady_ms_now();
    if (npu_ms) {
        *npu_ms = ms_since(npu_start);
    }
    if (timing) timing->run_ms += npu_ms ? *npu_ms : ms_since(npu_start);
    if (run_ret >= 0) {
        record_rknn_run_interval(1, run_start_ms, run_end_ms);
        if (timing) timing->run_count += 1;
    }
    if (run_ret < 0) return false;

    rknn_output out[1];
    memset(out, 0, sizeof(out));
    out[0].want_float = 1;
    auto output_start = std::chrono::high_resolution_clock::now();
    if (rknn_outputs_get(det_model.ctx, 1, out, nullptr) != RKNN_SUCC) return false;
    if (timing) timing->output_ms += ms_since(output_start);

    bool ok = false;
    auto post_start = std::chrono::high_resolution_clock::now();
    do {
        float* p = static_cast<float*>(out[0].buf);
        if (raw_output && p && out[0].size > 0) {
            size_t count = static_cast<size_t>(out[0].size) / sizeof(float);
            raw_output->assign(p, p + count);
        }
        TensorCNView view;
        if (!infer_cn_view(det_model.out_attrs[0], p, view)) break;
        const size_t required_output_bytes =
                static_cast<size_t>(view.channels) *
                static_cast<size_t>(view.num_preds) * sizeof(float);
        if (!p || static_cast<size_t>(out[0].size) < required_output_bytes) {
            std::cerr << "[NPU] v291 detector output is smaller than tensor contract: actual="
                      << out[0].size << " required=" << required_output_bytes << std::endl;
            break;
        }
        int num_preds = view.num_preds;
        if (num_preds <= 0) break;

        const std::vector<Point2f>* grid = nullptr;
        const std::vector<int>* strides = nullptr;
        if (!get_grid_cached(num_preds, grid, strides)) break;

        // v2.1 车牌 detector 为单类输出：DFL 64 通道 + score 1 通道 + 4 点关键点 8 通道。
        bool dfl_mode = (view.channels >= 64 + PLATE_DET_CLASS_COUNT + 8);
        int cls_base = dfl_mode ? 64 : 4;
        int kpt_base = cls_base + PLATE_DET_CLASS_COUNT;
        int kpt_values = view.channels - kpt_base;
        int kpt_step = (kpt_values >= 12) ? 3 : 2;
        if (view.channels < kpt_base + 3 * kpt_step + 2) break;
        bool cls_is_logit = !dfl_mode && cls_looks_like_logit(view, cls_base, PLATE_DET_CLASS_COUNT);

        std::vector<ScoreCandidate> cls_pass;
        cls_pass.reserve(num_preds / 16 + 1);
        for (int i = 0; i < num_preds; ++i) {
            // 分类分支起点由输出模式决定：DFL 头为 64，noDFL/已解码头为 4。
            float max_score = -1.0f;
            for (int c = 0; c < PLATE_DET_CLASS_COUNT; ++c) {
                float s = view.at(cls_base + c, i);
                if (cls_is_logit) s = sigmoidf(s);
                if (s > max_score) max_score = s;
            }
            if (max_score < conf_thres) continue;
            cls_pass.push_back({i, max_score});
        }
        if (cls_pass.empty()) break;

        // 车牌分支也做 TopK 限流，避免复杂场景下 DFL decode 与 NMS 吃掉推理吞吐。
        keep_topk_candidates(cls_pass, PLATE_TOPK_DECODE);

        std::vector<Rect2f> fb;
        std::vector<std::vector<Point2f>> kp;
        std::vector<float> fs;
        std::vector<int> fc;
        fb.reserve(cls_pass.size());
        kp.reserve(cls_pass.size());
        fs.reserve(cls_pass.size());
        fc.reserve(cls_pass.size());
        for (const auto& cand : cls_pass) {
            int i = cand.idx;
            int cls = 0;
            float max_score = -1.0f;
            for (int c = 0; c < PLATE_DET_CLASS_COUNT; ++c) {
                float s = view.at(cls_base + c, i);
                if (cls_is_logit) s = sigmoidf(s);
                if (s > max_score) {
                    max_score = s;
                    cls = c;
                }
            }
            Rect2f b;
            if (dfl_mode) b = decode_dfl_box(view, i, (*grid)[i], (*strides)[i]);
            else b = decode_xywh_box(view, i);
            if (b.width <= 0 || b.height <= 0) continue;

            // 4 个关键点用于车牌透视矫正。
            std::vector<Point2f> pts = decode_plate_keypoints(view,
                                                              i,
                                                              kpt_base,
                                                              kpt_step,
                                                              (*grid)[i],
                                                              (*strides)[i],
                                                              dfl_mode);

            fb.push_back(b);
            kp.push_back(pts);
            fs.push_back(max_score);
            fc.push_back(cls);
        }

        if (fb.empty()) break;
        if (static_cast<int>(fb.size()) > PLATE_TOPK_NMS) {
            fb.resize(PLATE_TOPK_NMS);
            kp.resize(PLATE_TOPK_NMS);
            fs.resize(PLATE_TOPK_NMS);
            fc.resize(PLATE_TOPK_NMS);
        }
        std::vector<int> keep = nms_indices(fb, fs, iou_thres);
        if (keep.empty()) break;

        // 识别模型单帧最多处理 4 个车牌裁剪图。
        int batch = std::min(static_cast<int>(keep.size()), 4);
        for (int i = 0; i < batch; ++i) {
            int idx = keep[i];
            PlateCropDebug crop_debug;
            Mat crop = plate_crop(mid_img, fb[idx], kp[idx], &crop_debug);
            if (timing) {
                timing->quad_comp_ms += crop_debug.quad_comp_ms;
                timing->quad_comp_geom_ms += crop_debug.quad_comp_geom_ms;
                timing->quad_comp_select_ms += crop_debug.quad_comp_select_ms;
                if (crop_debug.quad_comp_attempted) timing->quad_comp_attempt_count += 1;
                if (crop_debug.quad_compensated) timing->quad_comp_apply_count += 1;
            }
            if (crop.empty()) continue;
            if (input_is_rgb) {
                // 检测模型输入为 RGB；LPRNet 识别模型沿用训练口径，需要 BGR 裁剪图。
                cv::cvtColor(crop, crop, cv::COLOR_RGB2BGR);
            }
            boxes.push_back(fb[idx]);
            class_ids.push_back(PLATE_TYPE_UNKNOWN);
            if (scores) scores->push_back(fs[idx]);
            if (keypoints) keypoints->push_back(kp[idx]);
            if (crop_debugs) crop_debugs->push_back(crop_debug);
            crops.push_back(crop);
        }
        ok = !boxes.empty();
    } while (false);
    if (timing) timing->post_ms += ms_since(post_start);

    rknn_outputs_release(det_model.ctx, 1, out);
    return ok;
}

constexpr int CHAR_PROVINCE_LAST = 30;
constexpr int CHAR_POLICE = 31;
constexpr int CHAR_CONSULATE = 32;
constexpr int CHAR_DIGIT_FIRST = 33;
constexpr int CHAR_DIGIT_LAST = 42;
constexpr int CHAR_LETTER_FIRST = 43;
constexpr int CHAR_LETTER_LAST = 68;
constexpr int CHAR_EMBASSY = 69;
constexpr int CHAR_HK = 70;
constexpr int CHAR_MACAU = 71;

static bool is_province_token(int idx) {
    return idx >= 0 && idx <= CHAR_PROVINCE_LAST;
}

static bool is_ascii_alnum_token(int idx) {
    return (idx >= CHAR_DIGIT_FIRST && idx <= CHAR_DIGIT_LAST) ||
           (idx >= CHAR_LETTER_FIRST && idx <= CHAR_LETTER_LAST);
}

static bool is_letter_i_or_o_token(int idx) {
    const auto& list = chars();
    return idx >= 0 &&
           idx < static_cast<int>(list.size()) &&
           (list[idx] == "I" || list[idx] == "O");
}

static bool is_chinese_token(int idx) {
    return is_province_token(idx) ||
           idx == CHAR_POLICE ||
           idx == CHAR_CONSULATE ||
           idx == CHAR_EMBASSY ||
           idx == CHAR_HK ||
           idx == CHAR_MACAU;
}

static bool body_is_domestic_alnum(const std::vector<int>& tokens, size_t begin, size_t end) {
    if (begin > end || end > tokens.size()) return false;
    for (size_t i = begin; i < end; ++i) {
        if (!is_ascii_alnum_token(tokens[i]) || is_letter_i_or_o_token(tokens[i])) return false;
    }
    return true;
}

static bool tokens_to_text(const std::vector<int>& tokens, std::string& out_text, std::string& out_pinyin) {
    const auto& char_list = chars();
    const auto& py_list = pinyin_chars();
    out_text.clear();
    out_pinyin.clear();
    for (size_t i = 0; i < tokens.size(); ++i) {
        int idx = tokens[i];
        if (idx < 0 ||
            idx >= static_cast<int>(char_list.size()) ||
            idx >= static_cast<int>(py_list.size())) {
            return false;
        }
        out_text += char_list[idx];
        out_pinyin += py_list[idx];
        if (i == 0 && is_province_token(idx)) out_pinyin += "-";
    }
    return true;
}

static int infer_plate_type_from_text_tokens(const std::vector<int>& tokens) {
    if (tokens.empty()) return -1;

    bool has_chinese = false;
    for (int idx : tokens) {
        if (is_chinese_token(idx)) {
            has_chinese = true;
            break;
        }
    }

    if (!has_chinese) {
        if (tokens.size() < 4 || tokens.size() > 10) return -1;
        for (int idx : tokens) {
            if (!is_ascii_alnum_token(idx)) return -1;
        }
        return PLATE_TYPE_FOREIGN;
    }

    if (tokens.size() < 7 || tokens.size() > 8) return -1;

    if (tokens.back() == CHAR_EMBASSY) {
        return body_is_domestic_alnum(tokens, 0, tokens.size() - 1) ? PLATE_TYPE_EMBASSY
                                                                   : -1;
    }
    if (is_province_token(tokens[0]) && tokens.back() == CHAR_CONSULATE) {
        return body_is_domestic_alnum(tokens, 1, tokens.size() - 1) ? PLATE_TYPE_CONSULATE
                                                                   : -1;
    }
    if (is_province_token(tokens[0]) &&
        chars()[tokens[0]] == "粤" &&
        tokens.size() == 7 &&
        tokens[1] >= 0 &&
        tokens[1] < static_cast<int>(chars().size()) &&
        chars()[tokens[1]] == "Z" &&
        (tokens.back() == CHAR_HK || tokens.back() == CHAR_MACAU)) {
        if (!body_is_domestic_alnum(tokens, 1, tokens.size() - 1)) return -1;
        return tokens.back() == CHAR_HK ? PLATE_TYPE_HK : PLATE_TYPE_MACAU;
    }

    bool has_police = false;
    for (int idx : tokens) {
        if (idx == CHAR_POLICE) {
            has_police = true;
            break;
        }
    }
    if (has_police) {
        if (!is_province_token(tokens[0])) return -1;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == CHAR_POLICE) continue;
            if (!is_ascii_alnum_token(tokens[i]) || is_letter_i_or_o_token(tokens[i])) return -1;
        }
        return PLATE_TYPE_POLICE;
    }

    if (!is_province_token(tokens[0])) return -1;
    if (tokens.size() != 7 && tokens.size() != 8) return -1;
    if (!body_is_domestic_alnum(tokens, 1, tokens.size())) return -1;

    return PLATE_TYPE_UNKNOWN;
}

static int estimate_plate_type_from_crop_color(const Mat& crop_bgr) {
    if (crop_bgr.empty() || crop_bgr.channels() != 3) return PLATE_TYPE_UNKNOWN;

    Rect roi(0,
             std::max(0, crop_bgr.rows / 6),
             crop_bgr.cols,
             std::max(1, crop_bgr.rows * 2 / 3));
    Mat sample = crop_bgr(roi);
    Mat hsv;
    cv::cvtColor(sample, hsv, cv::COLOR_BGR2HSV);

    int blue = 0;
    int green = 0;
    int yellow = 0;
    int white = 0;
    int black = 0;
    int total = hsv.rows * hsv.cols;
    for (int y = 0; y < hsv.rows; ++y) {
        const cv::Vec3b* row = hsv.ptr<cv::Vec3b>(y);
        for (int x = 0; x < hsv.cols; ++x) {
            int h = row[x][0];
            int s = row[x][1];
            int v = row[x][2];
            if (h >= 90 && h <= 130 && s >= 60 && v >= 45) blue++;
            if (h >= 35 && h <= 90 && s >= 45 && v >= 45) green++;
            if (h >= 15 && h <= 40 && s >= 55 && v >= 70) yellow++;
            if (s <= 55 && v >= 170) white++;
            if (v <= 70) black++;
        }
    }
    if (total <= 0) return PLATE_TYPE_UNKNOWN;

    struct ColorVote {
        int type;
        int count;
    };
    ColorVote votes[] = {
        {PLATE_TYPE_BLUE, blue},
        {PLATE_TYPE_GREEN, green},
        {PLATE_TYPE_YELLOW, yellow},
        {PLATE_TYPE_WHITE, white},
        {PLATE_TYPE_BLACK, black},
    };
    ColorVote best = votes[0];
    for (const auto& v : votes) {
        if (v.count > best.count) best = v;
    }
    double ratio = static_cast<double>(best.count) / static_cast<double>(total);
    return ratio >= 0.18 ? best.type : PLATE_TYPE_UNKNOWN;
}

static bool gen_text(float* prediction,
                     int num_chars,
                     int seq_len,
                     std::string& out_text,
                     std::string& out_pinyin,
                     int& out_type,
                     std::vector<int>* raw_tokens = nullptr,
                     std::vector<int>* ctc_tokens = nullptr,
                     std::string* token_text = nullptr) {
    const auto& char_list = chars();
    int effective_chars = std::min(num_chars, static_cast<int>(char_list.size()));
    if (!prediction || effective_chars <= 1 || seq_len <= 0) return false;

    std::vector<int> pre;
    pre.reserve(seq_len);
    for (int j = 0; j < seq_len; ++j) {
        int max_idx = 0;
        float max_val = prediction[j];
        for (int i = 1; i < effective_chars; ++i) {
            float v = prediction[i * seq_len + j];
            if (v > max_val) {
                max_val = v;
                max_idx = i;
            }
        }
        pre.push_back(max_idx);
    }
    if (raw_tokens) *raw_tokens = pre;

    int blank = effective_chars - 1;
    std::vector<int> dedup;
    int prev = pre[0];
    if (prev != blank) dedup.push_back(prev);
    for (size_t i = 1; i < pre.size(); ++i) {
        int c = pre[i];
        if (c == prev || c == blank) {
            if (c == blank) prev = c;
            continue;
        }
        dedup.push_back(c);
        prev = c;
    }
    if (ctc_tokens) *ctc_tokens = dedup;
    if (token_text) {
        std::string py_unused;
        if (!tokens_to_text(dedup, *token_text, py_unused)) token_text->clear();
    }

    int text_type = infer_plate_type_from_text_tokens(dedup);
    if (text_type < 0) return false;
    out_type = text_type;
    if (!tokens_to_text(dedup, out_text, out_pinyin)) return false;
    return true;
}

Mat pre_img(const Mat& img) {
    // 使用 letterbox 保持长宽比，保证检测几何一致性。
    int nw = 0, nh = 0, sx = 0, sy = 0;
    get_scaling_info(img.cols, img.rows, nw, nh, sx, sy);

    static thread_local Mat resized;
    if (resized.cols != nw || resized.rows != nh) resized.create(nh, nw, CV_8UC3);
#ifdef USE_RGA
    if (!rga_resize(img, resized)) {
        static std::atomic<bool> warned(false);
        if (!warned.exchange(true)) {
            std::cerr << "[RGA] legacy pre_img resize failed, fallback to cv::resize" << std::endl;
        }
        cv::resize(img, resized, Size(nw, nh));
    }
#else
    cv::resize(img, resized, Size(nw, nh));
#endif

    // 背景填充为训练常用的 padding 颜色。
    static thread_local Mat bg(DET_INPUT_H, DET_INPUT_W, CV_8UC3);
    bg.setTo(Scalar(DET_PAD_VALUE, DET_PAD_VALUE, DET_PAD_VALUE));
    resized.copyTo(bg(Rect(sx, sy, nw, nh)));
    // 返回 thread_local 背景图视图，避免额外 clone 拷贝。
    return bg;
}

Mat pre_img_rgb565_to_rgb(const uint8_t* rgb565,
                          int width,
                          int height,
                          bool* used_rga,
                          bool force_cpu) {
    if (used_rga) *used_rga = false;
    if (!rgb565 || width <= 0 || height <= 0) return Mat();

    // 与 pre_img() 保持完全相同的 letterbox 几何，避免后处理坐标映射漂移。
    int nw = 0, nh = 0, sx = 0, sy = 0;
    get_scaling_info(width, height, nw, nh, sx, sy);

    static thread_local Mat resized_rgb;
    if (resized_rgb.cols != nw || resized_rgb.rows != nh || resized_rgb.type() != CV_8UC3) {
        resized_rgb.create(nh, nw, CV_8UC3);
    }

    bool rga_ok = false;
#ifdef USE_RGA
    if (!force_cpu) {
        RgaRgb565ToRgbMode mode = select_rga_preproc_mode();
        if (mode == RgaRgb565ToRgbMode::Hybrid) {
            static thread_local Mat resized_565;
            rga_ok = rga_hybrid_preproc(rgb565, width, height, resized_rgb, resized_565);
        } else {
            rga_ok = rga_preproc_apply(mode, rgb565, width, height, resized_rgb);
        }
    }
    if (!force_cpu && !rga_ok) {
        static std::atomic<bool> warned(false);
        if (!warned.exchange(true)) {
            std::cerr << "[RGA] preproc failed at runtime, fallback to CPU RGB565->RGB resize" << std::endl;
        }
    }
#endif

    if (!rga_ok) {
        // CPU fallback 复刻旧主程序的手写 RGB565 解析；仅在强制 CPU 或 RGA 缩放异常时使用。
        cpu_resize_rgb565_to_rgb(rgb565, width, height, nw, nh, resized_rgb);
    }

    static thread_local Mat bg_rgb(DET_INPUT_H, DET_INPUT_W, CV_8UC3);
    make_letterbox_rgb(resized_rgb, sx, sy, nw, nh, bg_rgb);

    static std::atomic<bool> route_reported(false);
    if (!route_reported.exchange(true)) {
        const char* route = force_cpu ? "cpu-forced" : (rga_ok ? "rga" : "cpu-fallback");
        std::cerr << "[NPU] v291 preprocess route=" << route
                  << " source=" << width << "x" << height
                  << " resized=" << nw << "x" << nh
                  << " input=" << DET_INPUT_W << "x" << DET_INPUT_H
                  << std::endl;
    }

    if (used_rga) *used_rga = rga_ok;
    return bg_rgb;
}

std::string current_preproc_mode_name(bool force_cpu) {
    if (force_cpu) return "cpu_forced";
#ifdef USE_RGA
    return rga_preproc_mode_name(select_rga_preproc_mode());
#else
    return "cpu";
#endif
}

void convert_input_box_to_orig(const Rect2f& in_box, Rect2f& out_box, int ow, int oh) {
    // 反变换：将 letterbox 坐标映射回原图坐标。
    int nw = 0, nh = 0, sx = 0, sy = 0;
    get_scaling_info(ow, oh, nw, nh, sx, sy);
    float scale_x = static_cast<float>(nw) / ow;
    float scale_y = static_cast<float>(nh) / oh;
    float x = (in_box.x - sx) / scale_x;
    float y = (in_box.y - sy) / scale_y;
    float w = in_box.width / scale_x;
    float h = in_box.height / scale_y;
    x = std::max(0.0f, std::min(x, static_cast<float>(ow - 1)));
    y = std::max(0.0f, std::min(y, static_cast<float>(oh - 1)));
    w = std::max(0.0f, std::min(w, static_cast<float>(ow) - x));
    h = std::max(0.0f, std::min(h, static_cast<float>(oh) - y));
    out_box = Rect2f(x, y, w, h);
}

static Point2f convert_input_point_to_orig_impl(Point2f p, int ow, int oh) {
    int nw = 0, nh = 0, sx = 0, sy = 0;
    get_scaling_info(ow, oh, nw, nh, sx, sy);
    float scale_x = static_cast<float>(nw) / ow;
    float scale_y = static_cast<float>(nh) / oh;
    float x = (p.x - sx) / scale_x;
    float y = (p.y - sy) / scale_y;
    x = std::max(0.0f, std::min(x, static_cast<float>(ow - 1)));
    y = std::max(0.0f, std::min(y, static_cast<float>(oh - 1)));
    return Point2f(x, y);
}

void convert_input_points_to_orig(const std::vector<Point2f>& in_points,
                                  std::vector<Point2f>& out_points,
                                  int ow,
                                  int oh) {
    out_points.clear();
    out_points.reserve(in_points.size());
    if (ow <= 0 || oh <= 0) return;
    for (Point2f p : in_points) {
        out_points.push_back(convert_input_point_to_orig_impl(p, ow, oh));
    }
}

Mat crop_plate_from_original_bgr(const Mat& orig_bgr,
                                 const Rect2f& input_box,
                                 const std::vector<Point2f>& input_points,
                                 PlateCropDebug* debug,
                                 Size shape) {
    if (orig_bgr.empty()) return Mat();
    Rect2f orig_box;
    convert_input_box_to_orig(input_box, orig_box, orig_bgr.cols, orig_bgr.rows);

    std::vector<Point2f> orig_points;
    convert_input_points_to_orig(input_points, orig_points, orig_bgr.cols, orig_bgr.rows);
    return plate_crop(orig_bgr, orig_box, orig_points, debug, shape);
}

bool detect_yolov8_objects(const Mat& mid_img,
                           RknnModel& model,
                           int class_count,
                           float conf_thres,
                           float iou_thres,
                           std::vector<YoloObjectDetection>& detections,
                           bool input_is_rgb,
                           int topk_decode,
                           int topk_nms,
                           double* npu_ms,
                           RknnStageTiming* timing) {
    detections.clear();
    if (npu_ms) *npu_ms = 0.0;
    if (timing) timing->reset();
    if (mid_img.empty() || class_count <= 0) return false;

    static thread_local Mat rgb;
    if (input_is_rgb) rgb = mid_img;
    else {
#ifdef USE_RGA
        rga_cvt_bgr_to_rgb(mid_img, rgb);
#else
        cv::cvtColor(mid_img, rgb, cv::COLOR_BGR2RGB);
#endif
    }

    rknn_input in[1];
    memset(in, 0, sizeof(in));
    in[0].index = 0;
    in[0].type = RKNN_TENSOR_UINT8;
    in[0].size = rgb.total() * rgb.elemSize();
    in[0].fmt = RKNN_TENSOR_NHWC;
    in[0].buf = rgb.data;
    auto input_start = std::chrono::high_resolution_clock::now();
    if (rknn_inputs_set(model.ctx, model.num_in, in) != RKNN_SUCC) return false;
    if (timing) timing->input_ms += ms_since(input_start);

    auto npu_start = std::chrono::high_resolution_clock::now();
    double run_start_ms = steady_ms_now();
    int run_ret = rknn_run(model.ctx, nullptr);
    double run_end_ms = steady_ms_now();
    if (npu_ms) *npu_ms = ms_since(npu_start);
    if (timing) timing->run_ms += npu_ms ? *npu_ms : ms_since(npu_start);
    if (run_ret >= 0) {
        record_rknn_run_interval(4, run_start_ms, run_end_ms);
        if (timing) timing->run_count += 1;
    }
    if (run_ret < 0) return false;

    rknn_output out[1];
    memset(out, 0, sizeof(out));
    out[0].want_float = 1;
    auto output_start = std::chrono::high_resolution_clock::now();
    if (rknn_outputs_get(model.ctx, 1, out, nullptr) != RKNN_SUCC) return false;
    if (timing) timing->output_ms += ms_since(output_start);

    bool ok = false;
    auto post_start = std::chrono::high_resolution_clock::now();
    do {
        float* p = static_cast<float*>(out[0].buf);
        TensorCNView view;
        if (!infer_cn_view(model.out_attrs[0], p, view)) break;

        int num_preds = view.num_preds;
        if (num_preds <= 0) break;

        int cls_cnt = std::max(1, class_count);
        bool dfl_mode = (view.channels >= 64 + cls_cnt) && first64_looks_like_dfl(view);
        int cls_base = dfl_mode ? 64 : 4;
        if (cls_base >= view.channels) break;

        cls_cnt = std::min(cls_cnt, view.channels - cls_base);
        if (cls_cnt <= 0) break;

        bool cls_is_logit = cls_looks_like_logit(view, cls_base, cls_cnt);

        const std::vector<Point2f>* grid = nullptr;
        const std::vector<int>* strides = nullptr;
        if (dfl_mode && !get_grid_cached(num_preds, grid, strides)) break;

        std::vector<YoloScoreCandidate> cls_pass;
        cls_pass.reserve(num_preds / 8 + 1);
        for (int i = 0; i < num_preds; ++i) {
            float max_score = -1.0f;
            int best_class = -1;
            for (int c = 0; c < cls_cnt; ++c) {
                float s = view.at(cls_base + c, i);
                if (cls_is_logit) s = sigmoidf(s);
                if (s > max_score) {
                    max_score = s;
                    best_class = c;
                }
            }
            if (max_score < conf_thres || best_class < 0) continue;
            cls_pass.push_back({i, max_score, best_class});
        }
        if (cls_pass.empty()) break;

        keep_topk_yolo_candidates(cls_pass, topk_decode);

        std::vector<YoloObjectDetection> decoded;
        decoded.reserve(cls_pass.size());
        for (const auto& c : cls_pass) {
            Rect2f b;
            if (dfl_mode) b = decode_dfl_box(view, c.idx, (*grid)[c.idx], (*strides)[c.idx]);
            else b = decode_xywh_box(view, c.idx);
            if (b.width <= 0 || b.height <= 0) continue;

            YoloObjectDetection det;
            det.input_box = b;
            det.score = c.score;
            det.class_id = c.class_id;
            decoded.push_back(det);
        }
        if (decoded.empty()) break;

        std::sort(decoded.begin(), decoded.end(), [](const YoloObjectDetection& a, const YoloObjectDetection& b) {
            return a.score > b.score;
        });
        if (topk_nms > 0 && static_cast<int>(decoded.size()) > topk_nms) {
            decoded.resize(topk_nms);
        }

        for (int cls = 0; cls < cls_cnt; ++cls) {
            std::vector<Rect2f> boxes;
            std::vector<float> scores;
            std::vector<int> map_idx;
            for (size_t i = 0; i < decoded.size(); ++i) {
                if (decoded[i].class_id != cls) continue;
                boxes.push_back(decoded[i].input_box);
                scores.push_back(decoded[i].score);
                map_idx.push_back(static_cast<int>(i));
            }
            if (boxes.empty()) continue;
            std::vector<int> keep = nms_indices(boxes, scores, iou_thres);
            for (int idx : keep) {
                detections.push_back(decoded[map_idx[idx]]);
            }
        }

        std::sort(detections.begin(), detections.end(), [](const YoloObjectDetection& a, const YoloObjectDetection& b) {
            return a.score > b.score;
        });
        ok = !detections.empty();
    } while (false);
    if (timing) timing->post_ms += ms_since(post_start);

    rknn_outputs_release(model.ctx, 1, out);
    return ok;
}

bool recognize_plate_crops(const std::vector<Mat>& crops,
                           const std::vector<Rect2f>& in_boxes,
                           const std::vector<int>& in_color_ids,
                           RknnModel& reco_model,
                           std::vector<std::string>& plates,
                           std::vector<std::string>& pinyins,
                           std::vector<Rect2f>& out_boxes,
                           std::vector<int>& out_color_ids,
                           double* npu_ms,
                           RknnStageTiming* timing,
                           std::vector<PlateOcrDebug>* ocr_debugs,
                           std::vector<float>* raw_output) {
    if (npu_ms) *npu_ms = 0.0;
    if (timing) timing->reset();
    if (ocr_debugs) ocr_debugs->clear();
    if (raw_output) raw_output->clear();
    int batch = static_cast<int>(crops.size());
    if (batch <= 0 || batch > 4) return false;
    if (in_boxes.size() != crops.size() || in_color_ids.size() != crops.size()) return false;
    for (const Mat& crop : crops) {
        if (crop.empty() || crop.rows != 24 || crop.cols != 94 || crop.type() != CV_8UC3) {
            std::cerr << "[NPU] v291 OCR crop contract mismatch" << std::endl;
            return false;
        }
    }

    auto input_start = std::chrono::high_resolution_clock::now();
    // 按裁剪数量动态设置识别 batch。batch 不变时跳过 shape 更新，减少 RKNN 控制开销。
    static thread_local rknn_context cached_ctx = 0;
    static thread_local int cached_batch = -1;
    if (cached_ctx != reco_model.ctx || cached_batch != batch) {
        rknn_tensor_attr in_attr;
        memset(&in_attr, 0, sizeof(in_attr));
        in_attr.index = 0;
        if (rknn_query(reco_model.ctx,
                       RKNN_QUERY_INPUT_ATTR,
                       &in_attr,
                       sizeof(in_attr)) != RKNN_SUCC || in_attr.n_dims == 0) {
            return false;
        }
        in_attr.dims[0] = batch;
        if (rknn_set_input_shape(reco_model.ctx, &in_attr) != RKNN_SUCC) return false;
        cached_ctx = reco_model.ctx;
        cached_batch = batch;
    }

    // 打包识别模型所需 NHWC 输入张量。注意：LPRNet 训练口径为 BGR，调用方需保证 crops 为 BGR。
    static thread_local std::vector<uint8_t> input_data;
    input_data.resize(batch * 24 * 94 * 3);
    for (int i = 0; i < batch; ++i) {
        uint8_t* dst = input_data.data() + i * 24 * 94 * 3;
        for (int r = 0; r < 24; ++r) memcpy(dst + r * 94 * 3, crops[i].ptr<uint8_t>(r), 94 * 3);
    }

    rknn_input in[1];
    memset(in, 0, sizeof(in));
    in[0].index = 0;
    in[0].type = RKNN_TENSOR_UINT8;
    in[0].size = input_data.size();
    in[0].fmt = RKNN_TENSOR_NHWC;
    in[0].buf = input_data.data();
    if (rknn_inputs_set(reco_model.ctx, reco_model.num_in, in) != RKNN_SUCC) return false;
    if (timing) timing->input_ms += ms_since(input_start);
    auto npu_start = std::chrono::high_resolution_clock::now();
    double run_start_ms = steady_ms_now();
    int run_ret = rknn_run(reco_model.ctx, nullptr);
    double run_end_ms = steady_ms_now();
    if (npu_ms) {
        *npu_ms = ms_since(npu_start);
    }
    if (timing) timing->run_ms += npu_ms ? *npu_ms : ms_since(npu_start);
    if (run_ret >= 0) {
        record_rknn_run_interval(2, run_start_ms, run_end_ms);
        if (timing) timing->run_count += 1;
    }
    if (run_ret < 0) return false;

    rknn_output out[1];
    memset(out, 0, sizeof(out));
    out[0].want_float = 1;
    auto output_start = std::chrono::high_resolution_clock::now();
    if (rknn_outputs_get(reco_model.ctx, 1, out, nullptr) != RKNN_SUCC) return false;
    if (timing) timing->output_ms += ms_since(output_start);

    // 解码失败的 crop 显式回传 UNKNOWN，交给显示稳定器投票后再决定是否隐藏。
    bool ok = false;
    auto post_start = std::chrono::high_resolution_clock::now();
    do {
        float* p = static_cast<float*>(out[0].buf);
        if (raw_output && p && out[0].size > 0) {
            size_t count = static_cast<size_t>(out[0].size) / sizeof(float);
            raw_output->assign(p, p + count);
        }
        int num_chars = static_cast<int>(chars().size());
        int seq_len = 18;
        for (int d = 0; d < reco_model.out_attrs[0].n_dims; ++d) {
            int val = reco_model.out_attrs[0].dims[d];
            if (val >= 66 && val <= 90) num_chars = val;
            if (val >= 16 && val <= 24) seq_len = val;
        }

        int per = num_chars * seq_len;
        const size_t required_output_bytes = static_cast<size_t>(batch) *
                static_cast<size_t>(per) * sizeof(float);
        if (!p || static_cast<size_t>(out[0].size) < required_output_bytes) {
            std::cerr << "[NPU] v291 OCR output is smaller than dynamic batch contract: actual="
                      << out[0].size << " required=" << required_output_bytes
                      << " batch=" << batch << std::endl;
            break;
        }
        for (int i = 0; i < batch; ++i) {
            std::string text;
            std::string py;
            int plate_type = PLATE_TYPE_UNKNOWN;
            PlateOcrDebug dbg;
            dbg.num_chars = num_chars;
            dbg.seq_len = seq_len;
            bool text_ok = gen_text(p + i * per,
                                    num_chars,
                                    seq_len,
                                    text,
                                    py,
                                    plate_type,
                                    ocr_debugs ? &dbg.raw_tokens : nullptr,
                                    ocr_debugs ? &dbg.ctc_tokens : nullptr,
                                    ocr_debugs ? &dbg.token_text : nullptr);
            if (!text_ok) {
                text = "UNKNOWN";
                py.clear();
                plate_type = PLATE_TYPE_UNKNOWN;
            } else if (plate_type == PLATE_TYPE_UNKNOWN) {
                plate_type = estimate_plate_type_from_crop_color(crops[i]);
            }
            plates.push_back(text);
            pinyins.push_back(py);
            out_boxes.push_back(in_boxes[i]);
            out_color_ids.push_back(plate_type);
            if (ocr_debugs) ocr_debugs->push_back(std::move(dbg));
        }
        ok = !out_boxes.empty();
    } while (false);
    if (timing) timing->post_ms += ms_since(post_start);

    rknn_outputs_release(reco_model.ctx, 1, out);
    return ok;
}

bool detect_pedestrian(const Mat& mid_img,
                       RknnModel& ped_model,
                       int ped_classes,
                       float conf_thres,
                       float iou_thres,
                       std::vector<Rect2f>& boxes,
                       std::vector<float>& scores,
                       bool input_is_rgb,
                       int topk_decode,
                       int topk_nms,
                       double* npu_ms,
                       RknnStageTiming* timing) {
    if (npu_ms) *npu_ms = 0.0;
    if (timing) timing->reset();
    // 与车牌检测一致的预处理路径。
    static thread_local Mat rgb;
    if (input_is_rgb) rgb = mid_img;
    else {
#ifdef USE_RGA
        rga_cvt_bgr_to_rgb(mid_img, rgb);
#else
        cv::cvtColor(mid_img, rgb, cv::COLOR_BGR2RGB);
#endif
    }

    rknn_input in[1];
    memset(in, 0, sizeof(in));
    in[0].index = 0;
    in[0].type = RKNN_TENSOR_UINT8;
    in[0].size = rgb.total() * rgb.elemSize();
    in[0].fmt = RKNN_TENSOR_NHWC;
    in[0].buf = rgb.data;
    auto input_start = std::chrono::high_resolution_clock::now();
    if (rknn_inputs_set(ped_model.ctx, ped_model.num_in, in) != RKNN_SUCC) return false;
    if (timing) timing->input_ms += ms_since(input_start);
    auto npu_start = std::chrono::high_resolution_clock::now();
    double run_start_ms = steady_ms_now();
    int run_ret = rknn_run(ped_model.ctx, nullptr);
    double run_end_ms = steady_ms_now();
    if (npu_ms) {
        *npu_ms = ms_since(npu_start);
    }
    if (timing) timing->run_ms += npu_ms ? *npu_ms : ms_since(npu_start);
    if (run_ret >= 0) {
        record_rknn_run_interval(3, run_start_ms, run_end_ms);
        if (timing) timing->run_count += 1;
    }
    if (run_ret < 0) return false;

    rknn_output out[1];
    memset(out, 0, sizeof(out));
    out[0].want_float = 1;
    auto output_start = std::chrono::high_resolution_clock::now();
    if (rknn_outputs_get(ped_model.ctx, 1, out, nullptr) != RKNN_SUCC) return false;
    if (timing) timing->output_ms += ms_since(output_start);

    bool ok = false;
    auto post_start = std::chrono::high_resolution_clock::now();
    do {
        float* p = static_cast<float*>(out[0].buf);
        TensorCNView view;
        if (!infer_cn_view(ped_model.out_attrs[0], p, view)) break;

        int num_preds = view.num_preds;
        if (num_preds <= 0) break;

        // ped_classes 支持单类或多类行人头切换。
        int cls_cnt = std::max(1, ped_classes);

        // 模式 A：DFL 原始输出（前 64 通道是 DFL）。
        // 模式 B：Detect 默认已解码输出（前 4 通道是 xywh）。
        bool dfl_mode = (view.channels >= 64 + cls_cnt) && first64_looks_like_dfl(view);
        int cls_base = dfl_mode ? 64 : 4;
        if (cls_base >= view.channels) break;

        cls_cnt = std::min(cls_cnt, view.channels - cls_base);
        if (cls_cnt <= 0) break;

        bool cls_is_logit = cls_looks_like_logit(view, cls_base, cls_cnt);

        const std::vector<Point2f>* grid = nullptr;
        const std::vector<int>* strides = nullptr;
        if (dfl_mode && !get_grid_cached(num_preds, grid, strides)) break;

        std::vector<ScoreCandidate> cls_pass;
        cls_pass.reserve(num_preds / 8 + 1);
        for (int i = 0; i < num_preds; ++i) {
            float max_score = -1.0f;
            for (int c = 0; c < cls_cnt; ++c) {
                float s = view.at(cls_base + c, i);
                if (cls_is_logit) s = sigmoidf(s);
                if (s > max_score) max_score = s;
            }
            if (max_score < conf_thres) continue;
            ScoreCandidate c;
            c.idx = i;
            c.score = max_score;
            cls_pass.push_back(c);
        }
        if (cls_pass.empty()) break;

        // 第一层限流：只解码 TopK 高分候选，降低 DFL CPU 开销。
        keep_topk_candidates(cls_pass, topk_decode);

        std::vector<Rect2f> fb;
        std::vector<float> fs;
        fb.reserve(cls_pass.size());
        fs.reserve(cls_pass.size());
        for (const auto& c : cls_pass) {
            Rect2f b;
            if (dfl_mode) b = decode_dfl_box(view, c.idx, (*grid)[c.idx], (*strides)[c.idx]);
            else b = decode_xywh_box(view, c.idx);
            if (b.width <= 0 || b.height <= 0) continue;
            fb.push_back(b);
            fs.push_back(c.score);
        }

        if (fb.empty()) break;
        // 第二层限流：NMS 前截断候选，限制 O(n^2) 比较规模。
        if (topk_nms > 0 && static_cast<int>(fb.size()) > topk_nms) {
            fb.resize(topk_nms);
            fs.resize(topk_nms);
        }
        std::vector<int> keep = nms_indices(fb, fs, iou_thres);
        for (int idx : keep) {
            boxes.push_back(fb[idx]);
            scores.push_back(fs[idx]);
        }
        ok = !boxes.empty();
    } while (false);
    if (timing) timing->post_ms += ms_since(post_start);

    rknn_outputs_release(ped_model.ctx, 1, out);
    return ok;
}

TrafficLightState detect_light_state(const Mat& frame, const Rect& light_roi_raw) {
    // 在信号灯 ROI 内做简单 HSV 颜色投票。
    Rect roi = light_roi_raw & Rect(0, 0, frame.cols, frame.rows);
    if (roi.area() <= 0) return TrafficLightState::Unknown;
    Mat hsv;
    cv::cvtColor(frame(roi), hsv, cv::COLOR_BGR2HSV);

    Mat red1, red2, green;
    cv::inRange(hsv, Scalar(0, 100, 100), Scalar(10, 255, 255), red1);
    cv::inRange(hsv, Scalar(160, 100, 100), Scalar(180, 255, 255), red2);
    cv::inRange(hsv, Scalar(35, 80, 80), Scalar(90, 255, 255), green);

    int red = cv::countNonZero(red1) + cv::countNonZero(red2);
    int grn = cv::countNonZero(green);
    int min_cnt = std::max(10, roi.area() / 30);
    if (red >= min_cnt && red > static_cast<int>(grn * 1.2f)) return TrafficLightState::Red;
    if (grn >= min_cnt && grn > static_cast<int>(red * 1.2f)) return TrafficLightState::Green;
    return TrafficLightState::Unknown;
}

float quad_intersection_area(const Rect2f& box, const std::vector<Point>& quad) {
    // 计算行人框与用户输入斑马线四边形的重叠面积。
    if (quad.size() != 4) return 0.0f;
    std::vector<Point2f> box_poly = {
        Point2f(box.x, box.y),
        Point2f(box.x + box.width, box.y),
        Point2f(box.x + box.width, box.y + box.height),
        Point2f(box.x, box.y + box.height)
    };
    std::vector<Point2f> quad_f;
    quad_f.reserve(4);
    for (const auto& p : quad) quad_f.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
    std::vector<Point2f> inter;
    float area = cv::intersectConvexConvex(quad_f, box_poly, inter, true);
    return std::max(0.0f, area);
}

std::string light_text(TrafficLightState s) {
    if (s == TrafficLightState::Red) return "RED";
    if (s == TrafficLightState::Green) return "GREEN";
    return "UNKNOWN";
}

std::string ped_text(PedStatus s) {
    if (s == PedStatus::Normal) return "Normal";
    if (s == PedStatus::Outside) return "OutsideCrosswalk";
    if (s == PedStatus::RedAccum) return "RedAccumulating";
    if (s == PedStatus::RedViolation) return "RedViolation";
    return "Unknown";
}

Scalar ped_color(PedStatus s) {
    if (s == PedStatus::Normal) return Scalar(0, 255, 0);
    if (s == PedStatus::Outside) return Scalar(0, 165, 255);
    if (s == PedStatus::RedAccum) return Scalar(0, 255, 255);
    if (s == PedStatus::RedViolation) return Scalar(0, 0, 255);
    return Scalar(255, 255, 255);
}

}  // namespace plate_v291
}  // namespace demo
