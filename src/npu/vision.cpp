#include "vision.h"

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

#ifdef USE_RGA
#include "RgaUtils.h"
#include <RgaApi.h>
#include "im2d.h"
#include "rga.h"
#endif

namespace demo {
namespace vision {

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
    float scale = std::min(static_cast<float>(DET_INPUT_W) / ow, static_cast<float>(DET_INPUT_H) / oh);
    nw = std::max(4, std::min(DET_INPUT_W, (static_cast<int>(ow * scale) / 4) * 4));
    nh = std::max(4, std::min(DET_INPUT_H, (static_cast<int>(oh * scale) / 4) * 4));
    sx = (DET_INPUT_W - nw) / 2;
    sy = (DET_INPUT_H - nh) / 2;
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

// 根据 4 个关键点做透视矫正，得到固定识别输入（94x24）。
static Mat plate_crop(const Mat& img, std::vector<Point2f>& pts, Size shape = Size(94, 24)) {
    std::sort(pts.begin(), pts.end(), [](const Point2f& a, const Point2f& b){ return a.x < b.x; });
    std::vector<Point2f> left(pts.begin(), pts.begin() + 2);
    std::vector<Point2f> right(pts.begin() + 2, pts.end());
    std::sort(left.begin(), left.end(), [](const Point2f& a, const Point2f& b){ return a.y < b.y; });
    std::sort(right.begin(), right.end(), [](const Point2f& a, const Point2f& b){ return a.y < b.y; });
    std::vector<Point2f> src = {left[0], left[1], right[0], right[1]};
    std::vector<Point2f> dst = {
        Point2f(0, 0), Point2f(0, static_cast<float>(shape.height)),
        Point2f(static_cast<float>(shape.width), 0), Point2f(static_cast<float>(shape.width), static_cast<float>(shape.height))
    };
    Mat m = cv::getPerspectiveTransform(src, dst);
    Mat crop;
    cv::warpPerspective(img, crop, m, shape);
    return crop;
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
                             RknnStageTiming* timing) {
    if (npu_ms) *npu_ms = 0.0;
    if (timing) timing->reset();
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
        TensorCNView view;
        if (!infer_cn_view(det_model.out_attrs[0], p, view)) break;
        int num_preds = view.num_preds;
        if (num_preds <= 0) break;

        const std::vector<Point2f>* grid = nullptr;
        const std::vector<int>* strides = nullptr;
        if (!get_grid_cached(num_preds, grid, strides)) break;

        // 车牌模型可能是 DFL 原始头（64+5+关键点）或 noDFL/已解码头（4+5+关键点）。
        bool dfl_mode = (view.channels >= 64 + PLATE_CLASS_COUNT + 8);
        int cls_base = dfl_mode ? 64 : 4;
        int kpt_base = cls_base + PLATE_CLASS_COUNT;
        int kpt_values = view.channels - kpt_base;
        int kpt_step = (kpt_values >= 12) ? 3 : 2;
        if (view.channels < kpt_base + 3 * kpt_step + 2) break;
        bool cls_is_logit = cls_looks_like_logit(view, cls_base, PLATE_CLASS_COUNT);

        std::vector<ScoreCandidate> cls_pass;
        cls_pass.reserve(num_preds / 16 + 1);
        for (int i = 0; i < num_preds; ++i) {
            // 分类分支起点由输出模式决定：DFL 头为 64，noDFL/已解码头为 4。
            float max_score = -1.0f;
            for (int c = 0; c < PLATE_CLASS_COUNT; ++c) {
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
            for (int c = 0; c < PLATE_CLASS_COUNT; ++c) {
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
            Mat crop = plate_crop(mid_img, kp[idx]);
            if (input_is_rgb) {
                // 检测模型输入为 RGB；LPRNet 识别模型沿用训练口径，需要 BGR 裁剪图。
                cv::cvtColor(crop, crop, cv::COLOR_RGB2BGR);
            }
            boxes.push_back(fb[idx]);
            class_ids.push_back(fc[idx]);
            crops.push_back(crop);
        }
        ok = !boxes.empty();
    } while (false);
    if (timing) timing->post_ms += ms_since(post_start);

    rknn_outputs_release(det_model.ctx, 1, out);
    return ok;
}

// 将识别 logits 解码为文本，并做 CTC 去重与类别相关合法性校验。
static bool gen_text(float* prediction, int num_chars, int seq_len, int class_id, std::string& out_text, std::string& out_pinyin) {
    std::vector<int> pre;
    for (int j = 0; j < seq_len; ++j) {
        int max_idx = 0;
        float max_val = prediction[j];
        for (int i = 1; i < num_chars; ++i) {
            float v = prediction[i * seq_len + j];
            if (v > max_val) {
                max_val = v;
                max_idx = i;
            }
        }
        pre.push_back(max_idx);
    }

    const auto& char_list = chars();
    // 最后一个符号视为 blank。
    int blank = static_cast<int>(char_list.size()) - 1;
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

    // 绿牌通常 8 位，其它牌通常 7 位。
    int expected = (class_id == 1) ? 8 : 7;
    if (static_cast<int>(dedup.size()) != expected) return false;
    if (dedup[0] > 32) return false;

    int check_end = (class_id == 3 || class_id == 4) ? (expected - 1) : expected;
    for (int i = 1; i < check_end; ++i) if (dedup[i] <= 32) return false;
    // 警用/领馆类别强制使用约定后缀标记。
    if (class_id == 3) dedup.back() = 31;
    if (class_id == 4) dedup.back() = 32;

    const auto& py_list = pinyin_chars();
    out_text.clear();
    out_pinyin.clear();
    for (size_t i = 0; i < dedup.size(); ++i) {
        int idx = dedup[i];
        out_text += char_list[idx];
        out_pinyin += py_list[idx];
        if (i == 0) out_pinyin += "-";
    }
    return true;
}

Mat pre_img(const Mat& img) {
    // 使用 letterbox 保持长宽比，保证检测几何一致性。
    float scale = std::min(static_cast<float>(DET_INPUT_W) / img.cols, static_cast<float>(DET_INPUT_H) / img.rows);
    int nw = std::max(4, std::min(DET_INPUT_W, (static_cast<int>(img.cols * scale) / 4) * 4));
    int nh = std::max(4, std::min(DET_INPUT_H, (static_cast<int>(img.rows * scale) / 4) * 4));

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
    int sx = (DET_INPUT_W - nw) / 2;
    int sy = (DET_INPUT_H - nh) / 2;
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

bool crop_plate_box_from_original_rgb565(const uint8_t* source_rgb565,
                                         std::size_t source_bytes,
                                         int source_width,
                                         int source_height,
                                         int detector_frame_width,
                                         int detector_frame_height,
                                         const Rect2f& input_box,
                                         Mat& crop_bgr,
                                         std::string* reason,
                                         double* convert_ms,
                                         double* resize_ms,
                                         Size shape) {
    crop_bgr.release();
    if (reason) reason->clear();
    if (convert_ms) *convert_ms = 0.0;
    if (resize_ms) *resize_ms = 0.0;

    const std::size_t required = source_width > 0 && source_height > 0
            ? static_cast<std::size_t>(source_width) *
              static_cast<std::size_t>(source_height) * 2u
            : 0u;
    if (!source_rgb565 || required == 0u || source_bytes < required ||
        detector_frame_width <= 0 || detector_frame_height <= 0 ||
        shape.width <= 0 || shape.height <= 0) {
        if (reason) *reason = "legacy_original_source_invalid";
        return false;
    }

    Rect2f detector_box;
    convert_input_box_to_orig(input_box, detector_box,
                              detector_frame_width, detector_frame_height);
    if (!std::isfinite(detector_box.x) || !std::isfinite(detector_box.y) ||
        !std::isfinite(detector_box.width) || !std::isfinite(detector_box.height) ||
        detector_box.width < 2.0f || detector_box.height < 2.0f) {
        if (reason) *reason = "legacy_original_box_invalid";
        return false;
    }

    // 给检测框保留少量边缘，避免低分辨率量化后裁掉首尾字符。
    const float pad_x = detector_box.width * 0.04f;
    const float pad_y = detector_box.height * 0.08f;
    const float scale_x = static_cast<float>(source_width) /
            static_cast<float>(detector_frame_width);
    const float scale_y = static_cast<float>(source_height) /
            static_cast<float>(detector_frame_height);
    const int x0 = std::max(0, static_cast<int>(std::floor(
            (detector_box.x - pad_x) * scale_x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(
            (detector_box.y - pad_y) * scale_y)));
    const int x1 = std::min(source_width, static_cast<int>(std::ceil(
            (detector_box.x + detector_box.width + pad_x) * scale_x)));
    const int y1 = std::min(source_height, static_cast<int>(std::ceil(
            (detector_box.y + detector_box.height + pad_y) * scale_y)));
    const int crop_width = x1 - x0;
    const int crop_height = y1 - y0;
    if (crop_width < 2 || crop_height < 2) {
        if (reason) *reason = "legacy_original_roi_invalid";
        return false;
    }

    const auto convert_begin = std::chrono::high_resolution_clock::now();
    Mat local_bgr(crop_height, crop_width, CV_8UC3);
    for (int row = 0; row < crop_height; ++row) {
        const uint8_t* src = source_rgb565 +
                (static_cast<std::size_t>(y0 + row) *
                 static_cast<std::size_t>(source_width) +
                 static_cast<std::size_t>(x0)) * 2u;
        uint8_t* dst = local_bgr.ptr<uint8_t>(row);
        for (int col = 0; col < crop_width; ++col) {
            const uint16_t px = static_cast<uint16_t>(src[col * 2]) |
                    static_cast<uint16_t>(src[col * 2 + 1] << 8);
            dst[col * 3 + 0] = static_cast<uint8_t>((px & 0x1fu) << 3);
            dst[col * 3 + 1] = static_cast<uint8_t>(((px >> 5) & 0x3fu) << 2);
            dst[col * 3 + 2] = static_cast<uint8_t>(((px >> 11) & 0x1fu) << 3);
        }
    }
    if (convert_ms) {
        *convert_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() -
                    convert_begin).count();
    }

    const auto resize_begin = std::chrono::high_resolution_clock::now();
    cv::resize(local_bgr, crop_bgr, shape, 0.0, 0.0, cv::INTER_LINEAR);
    if (resize_ms) {
        *resize_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() -
                    resize_begin).count();
    }
    if (crop_bgr.empty() || crop_bgr.cols != shape.width ||
        crop_bgr.rows != shape.height || crop_bgr.type() != CV_8UC3) {
        crop_bgr.release();
        if (reason) *reason = "legacy_original_resize_failed";
        return false;
    }
    if (reason) *reason = "ok";
    return true;
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
                           RknnStageTiming* timing) {
    if (npu_ms) *npu_ms = 0.0;
    if (timing) timing->reset();
    int batch = static_cast<int>(crops.size());
    if (batch <= 0) return false;
    if (in_boxes.size() != crops.size() || in_color_ids.size() != crops.size()) return false;

    auto input_start = std::chrono::high_resolution_clock::now();
    // 按裁剪数量动态设置识别 batch。batch 不变时跳过 shape 更新，减少 RKNN 控制开销。
    static thread_local rknn_context cached_ctx = 0;
    static thread_local int cached_batch = -1;
    if (cached_ctx != reco_model.ctx || cached_batch != batch) {
        rknn_tensor_attr in_attr;
        memset(&in_attr, 0, sizeof(in_attr));
        in_attr.index = 0;
        rknn_query(reco_model.ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
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
        int num_chars = static_cast<int>(chars().size());
        int seq_len = 18;
        for (int d = 0; d < reco_model.out_attrs[0].n_dims; ++d) {
            int val = reco_model.out_attrs[0].dims[d];
            if (val >= 66 && val <= 90) num_chars = val;
            if (val >= 16 && val <= 24) seq_len = val;
        }

        int per = num_chars * seq_len;
        for (int i = 0; i < batch; ++i) {
            std::string text;
            std::string py;
            if (!gen_text(p + i * per, num_chars, seq_len, in_color_ids[i], text, py)) {
                text = "UNKNOWN";
                py.clear();
            }
            plates.push_back(text);
            pinyins.push_back(py);
            out_boxes.push_back(in_boxes[i]);
            out_color_ids.push_back(in_color_ids[i]);
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

}  // namespace vision
}  // namespace demo
