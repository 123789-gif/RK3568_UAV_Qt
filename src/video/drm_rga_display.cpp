#include "video/drm_rga_display.h"
#include "npu/npu_inference_worker.h"
#include "npu/plate_type_ids.h"
#include "pcie/pcie_frame_receiver.h"
#include "system/perf_counters.h"
#include "system/system_monitor.h"

#include <QDebug>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <cmath>
#include <vector>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/types.h>
#include <linux/fb.h>

#include <drm/drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <RgaApi.h>
#include <RockchipRga.h>

#ifndef RK_FORMAT_RGB_565
#define RK_FORMAT_RGB_565 0x4
#endif

static uint64_t now_ns()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
}

static constexpr int PCIE_FRAME_W = PcieFrameReceiver::kFrameWidth;
static constexpr int PCIE_FRAME_H = PcieFrameReceiver::kFrameHeight;
static constexpr int PCIE_BPP = PcieFrameReceiver::kBytesPerPixel;
static constexpr std::size_t PCIE_FRAME_SIZE = PcieFrameReceiver::kFrameBytes;

static QString fpga_mode_name(std::uint32_t mode)
{
    switch (mode) {
    case 0U: return QStringLiteral("原始画面");
    case 1U: return QStringLiteral("反色诊断");
    case 2U: return QStringLiteral("固定色诊断");
    case 3U: return QStringLiteral("Gamma暗光增强");
    case 4U: return QStringLiteral("Sobel边缘检测");
    case 5U: return QStringLiteral("图像去雾");
    default: return QStringLiteral("未知模式%1").arg(mode);
    }
}

static QString ethernet_output_name(bool enable)
{
    return enable ? QStringLiteral("开启网络视频输出")
                  : QStringLiteral("关闭网络视频输出");
}

static int rga_crop_scale_rgb565(const void *srcPtr,
                                  int srcW,
                                  int srcH,
                                  const VideoTileRoi &crop,
                                  void *dstPtr,
                                  int dstW,
                                  int dstH,
                                  int dstStrideW,
                                  int dstStrideH)
{
    rga_info_t src;
    rga_info_t dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    src.fd = -1;
    src.mmuFlag = 1;
    src.virAddr = const_cast<void *>(srcPtr);
    src.rotation = 0;
    rga_set_rect(&src.rect, crop.x, crop.y, crop.width, crop.height,
                 srcW, srcH, RK_FORMAT_RGB_565);

    dst.fd = -1;
    dst.mmuFlag = 1;
    dst.virAddr = dstPtr;
    dst.rotation = 0;
    rga_set_rect(&dst.rect, 0, 0, dstW, dstH,
                 dstStrideW, dstStrideH, RK_FORMAT_RGB_565);

    return c_RkRgaBlit(&src, &dst, nullptr);
}


static void prepare_and_submit_npu_frame(NpuInferenceWorker *worker,
                                         const uint8_t *frameData,
                                         uint64_t frameId,
                                         const VideoTileRoi &sourceRoi,
                                         int sourceChannel,
                                         uint64_t sourceRevision,
                                         std::vector<uint8_t> &npuTileBuffer,
                                         uint64_t &npuCropFailTotal)
{
    if (!worker || !frameData || !worker->wantsFrame()) {
        return;
    }

    const int cropRet = rga_crop_scale_rgb565(frameData,
                                              PCIE_FRAME_W,
                                              PCIE_FRAME_H,
                                              sourceRoi,
                                              npuTileBuffer.data(),
                                              640,
                                              360,
                                              640,
                                              360);
    if (cropRet != 0) {
        ++npuCropFailTotal;
        if (npuCropFailTotal <= 10) {
            qDebug() << "RGA display-priority NPU input failed ret=" << cropRet
                     << "channel=" << sourceChannel;
        }
        return;
    }

    NpuOriginalFrameView originalView;
    originalView.data = frameData;
    originalView.bytes = static_cast<std::size_t>(PCIE_FRAME_W) *
            static_cast<std::size_t>(PCIE_FRAME_H) * PCIE_BPP;
    originalView.width = PCIE_FRAME_W;
    originalView.height = PCIE_FRAME_H;
    originalView.stridePixels = PCIE_FRAME_W;
    originalView.roiX = sourceRoi.x;
    originalView.roiY = sourceRoi.y;
    originalView.roiWidth = sourceRoi.width;
    originalView.roiHeight = sourceRoi.height;

    worker->submitFrame(frameId,
                        npuTileBuffer.data(),
                        npuTileBuffer.size(),
                        640,
                        360,
                        sourceChannel,
                        sourceRevision,
                        &originalView);
}

static VideoRoute decodeVideoRoute(uint64_t state)
{
    return sanitizeVideoRoute(static_cast<int>(state & 0xffu));
}

static uint64_t decodeVideoRouteRevision(uint64_t state)
{
    return state >> 8;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void fill_rect_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                             int x, int y, int w, int h, uint16_t color)
{
    if (!fb || pitchBytes <= 0 || fbW <= 0 || fbH <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fbW) w = fbW - x;
    if (y + h > fbH) h = fbH - y;
    if (w <= 0 || h <= 0) return;

    uint8_t *base = static_cast<uint8_t *>(fb);
    for (int yy = 0; yy < h; ++yy) {
        uint16_t *row = reinterpret_cast<uint16_t *>(base + (y + yy) * pitchBytes) + x;
        for (int xx = 0; xx < w; ++xx) row[xx] = color;
    }
}

static void draw_rect_outline_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                     int x, int y, int w, int h, int thickness, uint16_t color)
{
    if (thickness <= 0) thickness = 1;
    fill_rect_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, thickness, color);
    fill_rect_rgb565(fb, pitchBytes, fbW, fbH, x, y + h - thickness, w, thickness, color);
    fill_rect_rgb565(fb, pitchBytes, fbW, fbH, x, y, thickness, h, color);
    fill_rect_rgb565(fb, pitchBytes, fbW, fbH, x + w - thickness, y, thickness, h, color);
}

static const char *glyph5x7(char c)
{
    switch (c) {
    case '0': return "11110" "10010" "10010" "10010" "10010" "10010" "11110";
    case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
    case '2': return "11110" "00010" "00010" "11110" "10000" "10000" "11110";
    case '3': return "11110" "00010" "00010" "01110" "00010" "00010" "11110";
    case '4': return "10010" "10010" "10010" "11110" "00010" "00010" "00010";
    case '5': return "11110" "10000" "10000" "11110" "00010" "00010" "11110";
    case '6': return "11110" "10000" "10000" "11110" "10010" "10010" "11110";
    case '7': return "11110" "00010" "00010" "00100" "00100" "01000" "01000";
    case '8': return "11110" "10010" "10010" "11110" "10010" "10010" "11110";
    case '9': return "11110" "10010" "10010" "11110" "00010" "00010" "11110";
    case 'A': return "01100" "10010" "10010" "11110" "10010" "10010" "10010";
    case 'B': return "11100" "10010" "10010" "11100" "10010" "10010" "11100";
    case 'C': return "01110" "10000" "10000" "10000" "10000" "10000" "01110";
    case 'D': return "11100" "10010" "10010" "10010" "10010" "10010" "11100";
    case 'E': return "11110" "10000" "10000" "11100" "10000" "10000" "11110";
    case 'F': return "11110" "10000" "10000" "11100" "10000" "10000" "10000";
    case 'G': return "01110" "10000" "10000" "10110" "10010" "10010" "01110";
    case 'H': return "10010" "10010" "10010" "11110" "10010" "10010" "10010";
    case 'I': return "11100" "01000" "01000" "01000" "01000" "01000" "11100";
    case 'J': return "00110" "00010" "00010" "00010" "00010" "10010" "01100";
    case 'K': return "10010" "10100" "11000" "11000" "10100" "10010" "10010";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11110";
    case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'N': return "10010" "11010" "10110" "10010" "10010" "10010" "10010";
    case 'O': return "01100" "10010" "10010" "10010" "10010" "10010" "01100";
    case 'P': return "11100" "10010" "10010" "11100" "10000" "10000" "10000";
    case 'Q': return "01100" "10010" "10010" "10010" "10110" "10010" "01110";
    case 'R': return "11100" "10010" "10010" "11100" "10100" "10010" "10010";
    case 'S': return "01110" "10000" "10000" "01100" "00010" "00010" "11100";
    case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'U': return "10010" "10010" "10010" "10010" "10010" "10010" "01100";
    case 'V': return "10001" "10001" "10001" "10001" "01010" "01010" "00100";
    case 'W': return "10001" "10001" "10001" "10101" "10101" "11011" "10001";
    case 'X': return "10001" "01010" "00100" "00100" "00100" "01010" "10001";
    case 'Y': return "10001" "01010" "00100" "00100" "00100" "00100" "00100";
    case 'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
    case ':': return "00000" "00100" "00100" "00000" "00100" "00100" "00000";
    case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
    case '%': return "11001" "11010" "00100" "01000" "10110" "00110" "00000";
    case '-': return "00000" "00000" "00000" "11110" "00000" "00000" "00000";
    case '/': return "00001" "00010" "00010" "00100" "01000" "01000" "10000";
    case '+': return "00000" "00100" "00100" "11111" "00100" "00100" "00000";
    case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    default:  return "11111" "10001" "00110" "00100" "00110" "10001" "11111";
    }
}

static void draw_char_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                             int x, int y, char c, int scale, uint16_t color)
{
    const char *g = glyph5x7(c);
    for (int gy = 0; gy < 7; ++gy) {
        for (int gx = 0; gx < 5; ++gx) {
            if (g[gy * 5 + gx] == '1') {
                fill_rect_rgb565(fb, pitchBytes, fbW, fbH,
                                  x + gx * scale, y + gy * scale,
                                  scale, scale, color);
            }
        }
    }
}

static void draw_text_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                             int x, int y, const char *text, int scale, uint16_t color)
{
    int cx = x;
    for (const char *p = text; *p; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        draw_char_rgb565(fb, pitchBytes, fbW, fbH, cx, y, c, scale, color);
        cx += 6 * scale;
    }
}

static void draw_text_shadow_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                    int x, int y, const char *text, int scale,
                                    uint16_t fg, uint16_t shadow)
{
    const int off = (scale >= 3) ? 2 : 1;
    draw_text_rgb565(fb, pitchBytes, fbW, fbH, x + off, y, text, scale, shadow);
    draw_text_rgb565(fb, pitchBytes, fbW, fbH, x - off, y, text, scale, shadow);
    draw_text_rgb565(fb, pitchBytes, fbW, fbH, x, y + off, text, scale, shadow);
    draw_text_rgb565(fb, pitchBytes, fbW, fbH, x, y - off, text, scale, shadow);
    draw_text_rgb565(fb, pitchBytes, fbW, fbH, x + off, y + off, text, scale, shadow);
    draw_text_rgb565(fb, pitchBytes, fbW, fbH, x, y, text, scale, fg);
}


static const uint8_t *hud_glyph8x12(char c)
{
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    static const uint8_t space[12] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    static const uint8_t unknown[12] = {0x7e,0x42,0x06,0x0c,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00};
    switch (c) {
    case '0': { static const uint8_t g[12] = {0x3c,0x66,0xc3,0xc3,0xcb,0xd3,0xe3,0xc3,0xc3,0x66,0x3c,0x00}; return g; }
    case '1': { static const uint8_t g[12] = {0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00}; return g; }
    case '2': { static const uint8_t g[12] = {0x3c,0x66,0xc3,0x03,0x06,0x0c,0x18,0x30,0x60,0xc0,0xff,0x00}; return g; }
    case '3': { static const uint8_t g[12] = {0x7e,0xc3,0x03,0x06,0x1c,0x06,0x03,0x03,0xc3,0x66,0x3c,0x00}; return g; }
    case '4': { static const uint8_t g[12] = {0x06,0x0e,0x1e,0x36,0x66,0xc6,0xff,0x06,0x06,0x06,0x1f,0x00}; return g; }
    case '5': { static const uint8_t g[12] = {0xff,0xc0,0xc0,0xc0,0xfc,0x06,0x03,0x03,0xc3,0x66,0x3c,0x00}; return g; }
    case '6': { static const uint8_t g[12] = {0x1e,0x30,0x60,0xc0,0xfc,0xc6,0xc3,0xc3,0xc3,0x66,0x3c,0x00}; return g; }
    case '7': { static const uint8_t g[12] = {0xff,0xc3,0x06,0x06,0x0c,0x0c,0x18,0x18,0x30,0x30,0x30,0x00}; return g; }
    case '8': { static const uint8_t g[12] = {0x3c,0x66,0xc3,0xc3,0x66,0x3c,0x66,0xc3,0xc3,0x66,0x3c,0x00}; return g; }
    case '9': { static const uint8_t g[12] = {0x3c,0x66,0xc3,0xc3,0xc3,0x67,0x3f,0x03,0x06,0x0c,0x78,0x00}; return g; }
    case 'A': { static const uint8_t g[12] = {0x18,0x3c,0x66,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0xc3,0xc3,0x00}; return g; }
    case 'B': { static const uint8_t g[12] = {0xfc,0x66,0x63,0x63,0x66,0x7c,0x66,0x63,0x63,0x66,0xfc,0x00}; return g; }
    case 'C': { static const uint8_t g[12] = {0x3e,0x63,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0x63,0x3e,0x00}; return g; }
    case 'D': { static const uint8_t g[12] = {0xfc,0x66,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x66,0xfc,0x00}; return g; }
    case 'E': { static const uint8_t g[12] = {0xff,0xc0,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0xc0,0xc0,0xff,0x00}; return g; }
    case 'F': { static const uint8_t g[12] = {0xff,0xc0,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0x00}; return g; }
    case 'G': { static const uint8_t g[12] = {0x3e,0x63,0xc0,0xc0,0xc0,0xcf,0xc3,0xc3,0xc3,0x63,0x3e,0x00}; return g; }
    case 'H': { static const uint8_t g[12] = {0xc3,0xc3,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x00}; return g; }
    case 'I': { static const uint8_t g[12] = {0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00}; return g; }
    case 'J': { static const uint8_t g[12] = {0x1f,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0xc6,0xc6,0x7c,0x00}; return g; }
    case 'K': { static const uint8_t g[12] = {0xc3,0xc6,0xcc,0xd8,0xf0,0xe0,0xf0,0xd8,0xcc,0xc6,0xc3,0x00}; return g; }
    case 'L': { static const uint8_t g[12] = {0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xff,0x00}; return g; }
    case 'M': { static const uint8_t g[12] = {0xc3,0xe7,0xff,0xdb,0xdb,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x00}; return g; }
    case 'N': { static const uint8_t g[12] = {0xc3,0xe3,0xf3,0xdb,0xcf,0xc7,0xc3,0xc3,0xc3,0xc3,0xc3,0x00}; return g; }
    case 'O': { static const uint8_t g[12] = {0x3c,0x66,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x66,0x3c,0x00}; return g; }
    case 'P': { static const uint8_t g[12] = {0xfc,0x66,0x63,0x63,0x66,0x7c,0x60,0x60,0x60,0x60,0x60,0x00}; return g; }
    case 'Q': { static const uint8_t g[12] = {0x3c,0x66,0xc3,0xc3,0xc3,0xc3,0xdb,0xcf,0xc6,0x67,0x3f,0x03}; return g; }
    case 'R': { static const uint8_t g[12] = {0xfc,0x66,0x63,0x63,0x66,0x7c,0x78,0x6c,0x66,0x63,0xc3,0x00}; return g; }
    case 'S': { static const uint8_t g[12] = {0x3e,0x63,0xc0,0xc0,0x70,0x1c,0x06,0x03,0x03,0xc6,0x7c,0x00}; return g; }
    case 'T': { static const uint8_t g[12] = {0xff,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}; return g; }
    case 'U': { static const uint8_t g[12] = {0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x66,0x3c,0x00}; return g; }
    case 'V': { static const uint8_t g[12] = {0xc3,0xc3,0xc3,0xc3,0x66,0x66,0x66,0x3c,0x3c,0x18,0x18,0x00}; return g; }
    case 'W': { static const uint8_t g[12] = {0xc3,0xc3,0xc3,0xc3,0xdb,0xdb,0xdb,0xff,0xe7,0xc3,0xc3,0x00}; return g; }
    case 'X': { static const uint8_t g[12] = {0xc3,0x66,0x66,0x3c,0x18,0x18,0x3c,0x66,0x66,0xc3,0xc3,0x00}; return g; }
    case 'Y': { static const uint8_t g[12] = {0xc3,0x66,0x66,0x3c,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}; return g; }
    case 'Z': { static const uint8_t g[12] = {0xff,0x03,0x06,0x0c,0x18,0x18,0x30,0x60,0xc0,0xc0,0xff,0x00}; return g; }
    case '.': { static const uint8_t g[12] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}; return g; }
    case '/': { static const uint8_t g[12] = {0x03,0x06,0x06,0x0c,0x0c,0x18,0x18,0x30,0x30,0x60,0xc0,0x00}; return g; }
    case ':': { static const uint8_t g[12] = {0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00}; return g; }
    case '-': { static const uint8_t g[12] = {0x00,0x00,0x00,0x00,0x00,0x7e,0x7e,0x00,0x00,0x00,0x00,0x00}; return g; }
    case '+': { static const uint8_t g[12] = {0x00,0x00,0x18,0x18,0x18,0x7e,0x7e,0x18,0x18,0x18,0x00,0x00}; return g; }
    case '%': { static const uint8_t g[12] = {0xc2,0xc6,0x0c,0x18,0x30,0x60,0xc6,0x86,0x00,0x00,0x00,0x00}; return g; }
    case '(': { static const uint8_t g[12] = {0x0c,0x18,0x30,0x60,0x60,0x60,0x60,0x60,0x30,0x18,0x0c,0x00}; return g; }
    case ')': { static const uint8_t g[12] = {0x30,0x18,0x0c,0x06,0x06,0x06,0x06,0x06,0x0c,0x18,0x30,0x00}; return g; }
    case ' ': return space;
    default: return unknown;
    }
}


static uint8_t rgb565_r8(uint16_t c)
{
    return static_cast<uint8_t>(((c >> 11) & 0x1f) * 255 / 31);
}

static uint8_t rgb565_g8(uint16_t c)
{
    return static_cast<uint8_t>(((c >> 5) & 0x3f) * 255 / 63);
}

static uint8_t rgb565_b8(uint16_t c)
{
    return static_cast<uint8_t>((c & 0x1f) * 255 / 31);
}

static uint16_t blend_rgb565_over(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    const int inv = 255 - alpha;
    const int r = (static_cast<int>(rgb565_r8(src)) * alpha + static_cast<int>(rgb565_r8(dst)) * inv + 127) / 255;
    const int g = (static_cast<int>(rgb565_g8(src)) * alpha + static_cast<int>(rgb565_g8(dst)) * inv + 127) / 255;
    const int b = (static_cast<int>(rgb565_b8(src)) * alpha + static_cast<int>(rgb565_b8(dst)) * inv + 127) / 255;
    return rgb565(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

static void blend_pixel_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                               int x, int y, uint16_t color, uint8_t alpha)
{
    if (!fb || x < 0 || y < 0 || x >= fbW || y >= fbH) return;
    uint8_t *base = static_cast<uint8_t *>(fb);
    uint16_t *dst = reinterpret_cast<uint16_t *>(base + y * pitchBytes) + x;
    *dst = blend_rgb565_over(*dst, color, alpha);
}

static bool hud_glyph_bit(const uint8_t *g, int x, int y)
{
    if (!g || x < 0 || x >= 8 || y < 0 || y >= 12) return false;
    return (g[y] & (0x80u >> x)) != 0;
}

static bool hud_glyph_bit_12x18(const uint8_t *g, int x, int y)
{
    if (!g || x < 0 || x >= 12 || y < 0 || y >= 18) return false;
    const int sx = (x * 8) / 12;
    const int sy = (y * 12) / 18;
    return hud_glyph_bit(g, sx, sy);
}

static void draw_hud_char_12x18_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                        int x, int y, char c, uint16_t color)
{
    const uint8_t *g = hud_glyph8x12(c);
    const uint8_t edgeAlpha = 48;

    for (int gy = 0; gy < 18; ++gy) {
        for (int gx = 0; gx < 12; ++gx) {
            if (!hud_glyph_bit_12x18(g, gx, gy)) continue;
            if (!hud_glyph_bit_12x18(g, gx - 1, gy)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx - 1, y + gy, color, edgeAlpha);
            if (!hud_glyph_bit_12x18(g, gx + 1, gy)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx + 1, y + gy, color, edgeAlpha);
            if (!hud_glyph_bit_12x18(g, gx, gy - 1)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx, y + gy - 1, color, edgeAlpha);
            if (!hud_glyph_bit_12x18(g, gx, gy + 1)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx, y + gy + 1, color, edgeAlpha);
        }
    }

    for (int gy = 0; gy < 18; ++gy) {
        for (int gx = 0; gx < 12; ++gx) {
            if (hud_glyph_bit_12x18(g, gx, gy)) {
                fill_rect_rgb565(fb, pitchBytes, fbW, fbH, x + gx, y + gy, 1, 1, color);
            }
        }
    }
}

static void draw_hud_text_12x18_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                        int x, int y, const char *text, uint16_t color)
{
    constexpr int kPitch = 13;
    int cx = x;
    for (const char *p = text; *p; ++p) {
        draw_hud_char_12x18_rgb565(fb, pitchBytes, fbW, fbH, cx, y, *p, color);
        cx += kPitch;
    }
}

static void draw_hud_char_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                  int x, int y, char c, int scale, uint16_t color)
{
    const uint8_t *g = hud_glyph8x12(c);
    if (scale <= 1) {
        // Native-size HUD text: add a very small soft edge around glyph pixels.
        // This keeps the no-Qt/no-FreeType lightweight path, but reduces the harsh 1-bit look.
        const uint8_t edgeAlpha = 58;
        for (int gy = 0; gy < 12; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                if (!hud_glyph_bit(g, gx, gy)) continue;
                if (!hud_glyph_bit(g, gx - 1, gy)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx - 1, y + gy, color, edgeAlpha);
                if (!hud_glyph_bit(g, gx + 1, gy)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx + 1, y + gy, color, edgeAlpha);
                if (!hud_glyph_bit(g, gx, gy - 1)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx, y + gy - 1, color, edgeAlpha);
                if (!hud_glyph_bit(g, gx, gy + 1)) blend_pixel_rgb565(fb, pitchBytes, fbW, fbH, x + gx, y + gy + 1, color, edgeAlpha);
            }
        }
        for (int gy = 0; gy < 12; ++gy) {
            const uint8_t row = g[gy];
            for (int gx = 0; gx < 8; ++gx) {
                if (row & (0x80u >> gx)) {
                    fill_rect_rgb565(fb, pitchBytes, fbW, fbH, x + gx, y + gy, 1, 1, color);
                }
            }
        }
        return;
    }

    for (int gy = 0; gy < 12; ++gy) {
        const uint8_t row = g[gy];
        for (int gx = 0; gx < 8; ++gx) {
            if (row & (0x80u >> gx)) {
                fill_rect_rgb565(fb, pitchBytes, fbW, fbH,
                                  x + gx * scale, y + gy * scale,
                                  scale, scale, color);
            }
        }
    }
}

static void draw_hud_text_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                  int x, int y, const char *text, int scale, uint16_t color)
{
    const int pitch = (scale <= 1) ? 8 : (9 * scale);
    int cx = x;
    for (const char *p = text; *p; ++p) {
        draw_hud_char_rgb565(fb, pitchBytes, fbW, fbH, cx, y, *p, scale, color);
        cx += pitch;
    }
}

static void blit_rgb565_block(void *fb, int pitchBytes, int fbW, int fbH,
                              int x, int y, int w, int h, const std::vector<uint16_t> &pixels)
{
    if (!fb || pixels.empty() || w <= 0 || h <= 0) return;
    if (x >= fbW || y >= fbH || x + w <= 0 || y + h <= 0) return;
    const int srcX0 = std::max(0, -x);
    const int srcY0 = std::max(0, -y);
    const int dstX0 = std::max(0, x);
    const int dstY0 = std::max(0, y);
    const int copyW = std::min(w - srcX0, fbW - dstX0);
    const int copyH = std::min(h - srcY0, fbH - dstY0);
    if (copyW <= 0 || copyH <= 0) return;
    uint8_t *base = static_cast<uint8_t *>(fb);
    for (int row = 0; row < copyH; ++row) {
        const uint16_t *src = pixels.data() + (srcY0 + row) * w + srcX0;
        uint16_t *dst = reinterpret_cast<uint16_t *>(base + (dstY0 + row) * pitchBytes) + dstX0;
        memcpy(dst, src, static_cast<size_t>(copyW) * sizeof(uint16_t));
    }
}

static uint16_t hud_load_color(int value)
{
    if (value >= 95) return rgb565(255, 72, 72);
    if (value >= 80) return rgb565(255, 220, 0);
    return rgb565(245, 248, 255);
}

static uint16_t hud_temp_color(double tempC)
{
    if (tempC >= 85.0) return rgb565(255, 72, 72);
    if (tempC >= 75.0) return rgb565(255, 220, 0);
    return rgb565(245, 248, 255);
}

static uint16_t hud_fps_color(double fps, double target)
{
    if (target <= 0.1) return rgb565(245, 248, 255);
    if (fps < target * 0.70) return rgb565(255, 72, 72);
    if (fps < target * 0.85) return rgb565(255, 220, 0);
    return rgb565(245, 248, 255);
}

static uint16_t hud_latency_color(double valueMs, double warnMs, double badMs)
{
    if (valueMs >= badMs) return rgb565(255, 72, 72);
    if (valueMs >= warnMs) return rgb565(255, 220, 0);
    return rgb565(245, 248, 255);
}

static void hud_format_percent(char *buf, size_t size, int value)
{
    if (value >= 0) snprintf(buf, size, "%3d%%", value);
    else snprintf(buf, size, " NA ");
}

static void hud_format_temp(char *buf, size_t size, double value)
{
    if (value >= 0.0) snprintf(buf, size, "%4.1f C", value);
    else snprintf(buf, size, " NA   ");
}

static void hud_format_fps(char *buf, size_t size, double value)
{
    snprintf(buf, size, "%4.1f", value);
}

static void hud_format_ai_fps(char *buf, size_t size, double value, int target)
{
    if (target > 0) snprintf(buf, size, "%4.1f/%02d", value, target);
    else snprintf(buf, size, "%4.1f/--", value);
}

static void hud_format_ms(char *buf, size_t size, double us)
{
    snprintf(buf, size, "%4.1f", us / 1000.0);
}


static int scale_capture_x(float x, int fbW, int sourceW);
static int scale_capture_y(float y, int fbH, int sourceH);

struct PlateOsdStyle {
    uint16_t frameColor = 0;
    QColor background;
    QColor text;
    uint16_t shadowColor = 0;
};

static PlateOsdStyle plate_osd_style(int classId)
{
    PlateOsdStyle st;
    st.shadowColor = rgb565(0, 0, 0);
    switch (classId) {
    case plate_types::Blue:
        st.frameColor = rgb565(0, 96, 255);
        st.background = QColor(0, 84, 210, 232);
        st.text = QColor(255, 255, 255);
        break;
    case plate_types::Green:
        st.frameColor = rgb565(0, 210, 80);
        st.background = QColor(0, 185, 80, 232);
        st.text = QColor(0, 0, 0);
        break;
    case plate_types::Yellow:
        st.frameColor = rgb565(255, 220, 0);
        st.background = QColor(245, 210, 0, 232);
        st.text = QColor(0, 0, 0);
        break;
    case plate_types::White:
    case plate_types::Police:
    case plate_types::Embassy:
        st.frameColor = rgb565(255, 255, 255);
        st.background = QColor(245, 245, 245, 235);
        st.text = QColor(0, 0, 0);
        break;
    case plate_types::Black:
    case plate_types::Consulate:
        st.frameColor = rgb565(20, 20, 20);
        st.background = QColor(10, 10, 10, 235);
        st.text = QColor(255, 255, 255);
        break;
    case plate_types::HongKong:
    case plate_types::Macau:
        st.frameColor = rgb565(0, 96, 255);
        st.background = QColor(235, 235, 235, 235);
        st.text = QColor(0, 0, 0);
        break;
    case plate_types::Foreign:
        st.frameColor = rgb565(180, 180, 180);
        st.background = QColor(40, 40, 40, 232);
        st.text = QColor(255, 255, 255);
        break;
    default:
        st.frameColor = rgb565(255, 220, 0);
        st.background = QColor(35, 35, 35, 220);
        st.text = QColor(255, 255, 255);
        break;
    }
    return st;
}

static QString strip_plate_separators(QString s)
{
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        if (c.isSpace() || c == '-' || c == '_' || c == ':' || c == '.') {
            continue;
        }
        out.append(c.toUpper());
    }
    return out;
}

static bool is_chinese_plate_province(QChar c)
{
    static const QString provinces = QStringLiteral("京沪津渝冀晋蒙辽吉黑苏浙皖闽赣鲁豫鄂湘粤桂琼川贵云藏陕甘青宁新警领");
    return provinces.contains(c);
}

static bool starts_with_ascii_token(const QString &s, const char *token)
{
    const QString t = QString::fromLatin1(token);
    return s.left(t.size()).compare(t, Qt::CaseInsensitive) == 0;
}

static bool match_pinyin_province(const QString &compact, QString &province, int &tokenLen)
{
    struct Entry { const char *token; const char *province; };
    // Keep ambiguous model tokens exactly aligned with demo::pinyin_chars().
    static const Entry entries[] = {
        {"LINGCON", "领"}, {"JINGPOL", "警"},
        {"QIONG", "琼"}, {"CHUAN", "川"}, {"XIANG", "湘"}, {"ZANG", "藏"},
        {"MENG", "蒙"}, {"LIAO", "辽"}, {"ZHE", "浙"}, {"WAN", "皖"},
        {"MIN", "闽"}, {"GAN2", "甘"}, {"GAN", "赣"}, {"YUE", "粤"},
        {"GUI2", "贵"}, {"GUI", "桂"}, {"YUN", "云"}, {"SHAN", "陕"},
        {"QING", "青"}, {"NING", "宁"}, {"XIN", "新"}, {"JING", "京"},
        {"HU", "沪"}, {"JIN2", "晋"}, {"JIN", "津"}, {"YU2", "豫"},
        {"YU", "渝"}, {"JI2", "吉"}, {"JI", "冀"}, {"HEI", "黑"},
        {"SU", "苏"}, {"LU", "鲁"}, {"E", "鄂"}
    };
    for (const Entry &e : entries) {
        if (starts_with_ascii_token(compact, e.token)) {
            province = QString::fromUtf8(e.province);
            tokenLen = static_cast<int>(strlen(e.token));
            return true;
        }
    }
    return false;
}

static QString format_plate_text_for_osd(const NpuBox &box)
{
    // Prefer real Chinese OCR output. Use pinyin label only as fallback.
    QString raw = QString::fromUtf8(box.text.c_str()).trimmed();
    if (raw.isEmpty()) {
        raw = QString::fromUtf8(box.label.c_str()).trimmed();
    }
    if (raw.isEmpty()) {
        return QString();
    }

    QString province;
    QString tail;
    const QString compact = strip_plate_separators(raw);
    if (compact.isEmpty()) {
        return QString();
    }

    const QChar first = compact.at(0);
    if (is_chinese_plate_province(first)) {
        province = QString(first);
        tail = compact.mid(1);
    } else {
        int tokenLen = 0;
        if (match_pinyin_province(compact, province, tokenLen)) {
            tail = compact.mid(tokenLen);
        } else {
            // Fall back to readable ASCII if the province token is unknown.
            QString fallback = compact;
            if (fallback.size() > 2) {
                fallback.insert(2, QLatin1Char(' '));
            }
            return fallback.left(12);
        }
    }

    tail = strip_plate_separators(tail);
    if (tail.isEmpty()) {
        return province;
    }

    const QString city = tail.left(1);
    const QString number = tail.mid(1);
    if (number.isEmpty()) {
        return province + city;
    }
    return province + city + QStringLiteral(" ") + number;
}

struct CachedRgb565Label {
    int width = 0;
    int height = 0;
    std::vector<uint16_t> pixels;

    bool empty() const {
        return width <= 0 || height <= 0 || pixels.empty();
    }

    void clear() {
        width = 0;
        height = 0;
        pixels.clear();
    }
};

static bool plate_label_needs_red_suffix(int classId, const QString &text)
{
    return (classId == plate_types::Police ||
            classId == plate_types::Consulate ||
            classId == plate_types::Embassy) && text.size() > 1;
}

static QColor plate_label_suffix_color(int classId)
{
    if (classId == plate_types::Police ||
        classId == plate_types::Consulate ||
        classId == plate_types::Embassy) {
        return QColor(220, 0, 0);
    }
    return QColor();
}

static CachedRgb565Label render_plate_label_rgb565(int fbW, const QString &text, int classId)
{
    CachedRgb565Label out;
    if (text.isEmpty()) {
        return out;
    }

    const int pixelSize = (fbW >= 1600) ? 36 : 26;
    QFont font(QStringLiteral("Sans Serif"));
    font.setPixelSize(pixelSize);
    font.setBold(true);
    QFontMetrics fm(font);

    const int padX = 10;
    const int padY = 5;
    const int textW = std::max(1, fm.horizontalAdvance(text));
    const int textH = std::max(1, fm.height());
    const int labelW = textW + padX * 2;
    const int labelH = textH + padY * 2;
    if (labelW <= 0 || labelH <= 0) {
        return out;
    }

    const PlateOsdStyle st = plate_osd_style(classId);
    QColor bg = st.background;
    bg.setAlpha(255);  // 预渲染为不透明 RGB565 块，避免每帧 alpha 混合。
    QColor textColor = st.text;
    textColor.setAlpha(255);

    QImage img(labelW, labelH, QImage::Format_RGB32);
    img.fill(bg.rgb());
    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(0, 0, 0));
    painter.drawRect(0, 0, labelW - 1, labelH - 1);
    painter.setFont(font);
    const int textY = padY + fm.ascent();
    if (plate_label_needs_red_suffix(classId, text)) {
        const QString prefix = text.left(text.size() - 1);
        const QString suffix = text.right(1);
        painter.setPen(textColor);
        painter.drawText(padX, textY, prefix);
        painter.setPen(plate_label_suffix_color(classId));
        painter.drawText(padX + fm.horizontalAdvance(prefix), textY, suffix);
    } else {
        painter.setPen(textColor);
        painter.drawText(padX, textY, text);
    }
    painter.end();

    out.width = labelW;
    out.height = labelH;
    out.pixels.resize(static_cast<std::size_t>(labelW) * static_cast<std::size_t>(labelH));
    for (int yy = 0; yy < labelH; ++yy) {
        const QRgb *srcRow = reinterpret_cast<const QRgb *>(img.constScanLine(yy));
        uint16_t *dstRow = out.pixels.data() + static_cast<std::size_t>(yy) * static_cast<std::size_t>(labelW);
        for (int xx = 0; xx < labelW; ++xx) {
            const QRgb px = srcRow[xx];
            dstRow[xx] = rgb565(static_cast<uint8_t>(qRed(px)),
                                static_cast<uint8_t>(qGreen(px)),
                                static_cast<uint8_t>(qBlue(px)));
        }
    }
    return out;
}

static void blit_plate_label_rgb565(void *fb, int pitchBytes, int fbW, int fbH,
                                    int anchorX, int anchorY,
                                    const CachedRgb565Label &label)
{
    if (!fb || label.empty()) {
        return;
    }

    int x = anchorX - label.width / 2;
    int y = anchorY - label.height - 8;
    if (x < 2) x = 2;
    if (x + label.width > fbW - 2) x = fbW - label.width - 2;
    if (y < 2) y = anchorY + 8;
    if (y + label.height > fbH - 2) y = fbH - label.height - 2;
    if (x < 0 || y < 0) {
        return;
    }

    const int startX = std::max(0, -x);
    const int startY = std::max(0, -y);
    const int endX = std::min(label.width, fbW - x);
    const int endY = std::min(label.height, fbH - y);
    if (startX >= endX || startY >= endY) {
        return;
    }

    uint8_t *base = static_cast<uint8_t *>(fb);
    const int copyPixels = endX - startX;
    const std::size_t copyBytes = static_cast<std::size_t>(copyPixels) * sizeof(uint16_t);
    for (int yy = startY; yy < endY; ++yy) {
        const uint16_t *srcRow = label.pixels.data() +
                static_cast<std::size_t>(yy) * static_cast<std::size_t>(label.width) + startX;
        uint16_t *dstRow = reinterpret_cast<uint16_t *>(base + (y + yy) * pitchBytes) + x + startX;
        std::memcpy(dstRow, srcRow, copyBytes);
    }
}

struct DirectPlateLabelCacheEntry {
    QString text;
    int classId = -1000;
    int fbW = 0;
    uint64_t useSerial = 0;
    CachedRgb565Label label;
};

static const CachedRgb565Label* cached_plate_label(int fbW,
                                                    const QString& text,
                                                    int classId)
{
    static std::vector<DirectPlateLabelCacheEntry> cache;
    static uint64_t serial = 0;
    ++serial;

    for (DirectPlateLabelCacheEntry& entry : cache) {
        if (entry.fbW == fbW && entry.classId == classId && entry.text == text) {
            entry.useSerial = serial;
            return &entry.label;
        }
    }

    DirectPlateLabelCacheEntry* target = nullptr;
    constexpr std::size_t kMaxCacheEntries = 16;
    if (cache.size() < kMaxCacheEntries) {
        cache.push_back(DirectPlateLabelCacheEntry());
        target = &cache.back();
    } else {
        target = &*std::min_element(
                    cache.begin(), cache.end(),
                    [](const DirectPlateLabelCacheEntry& a,
                       const DirectPlateLabelCacheEntry& b) {
            return a.useSerial < b.useSerial;
        });
    }

    target->text = text;
    target->classId = classId;
    target->fbW = fbW;
    target->useSerial = serial;
    target->label = render_plate_label_rgb565(fbW, text, classId);
    return &target->label;
}

static void draw_plate_labels_from_result(void *fb,
                                          int pitchBytes,
                                          int fbW,
                                          int fbH,
                                          int sourceW,
                                          int sourceH,
                                          const NpuFrameResult& result)
{
    // 位置、文字和颜色全部直接使用NPU线程输出的同一份稳定结果。DRM层只
    // 缓存文字位图，不再进行第二次轨迹关联、EMA或文字投票。
    for (const NpuBox& box : result.plates) {
        const QString text = format_plate_text_for_osd(box);
        if (text.isEmpty() || box.w <= 1.0f || box.h <= 1.0f) {
            continue;
        }

        const CachedRgb565Label* label =
                cached_plate_label(fbW, text, box.classId);
        if (!label || label->empty()) {
            continue;
        }

        const int x = scale_capture_x(box.x, fbW, sourceW);
        const int y = scale_capture_y(box.y, fbH, sourceH);
        const int w = std::max(2, scale_capture_x(box.w, fbW, sourceW));
        blit_plate_label_rgb565(fb, pitchBytes, fbW, fbH,
                                x + w / 2, y, *label);
    }
}

struct OsdStats {
    double rxFps = 0.0;
    double rgaFps = 0.0;
    double flipFps = 0.0;
    double aiFps = 0.0;
    int aiTargetFps = 0;
    double rgaAvgUs = 0.0;
    double osdAvgUs = 0.0;
    double plateOsdAvgUs = 0.0;
    double npuPrepareAvgUs = 0.0;
    uint64_t failTotal = 0;
    int cpuLoad = -1;
    int npuLoad = -1;
    double tempC = -1.0;
};

static const char* npu_status_text(const NpuFrameResult& result);
static std::string osd_ascii_label(const std::string& label, const char* fallback);


static int draw_perf_hud(void *fb, int pitchBytes, int fbW, int fbH, const OsdStats &st)
{
    const int cardX = (fbW >= 1600) ? 16 : 10;
    const int cardY = (fbW >= 1600) ? 14 : 8;
    const int cardW = std::min(fbW - cardX - 8, 486);
    if (!fb || cardW <= 0) return 0;

    const int stripeW = 2;
    const int padX = 8;
    const int padY = 6;
    const int rowH = 30;
    const int glyphH = 18;
    const int textYOffset = (rowH - glyphH) / 2;
    const int cardH = padY * 2 + rowH * 3;

    struct HudCache {
        int fbW = 0;
        int w = 0;
        int h = 0;
        uint64_t lastUpdateNs = 0;
        std::vector<uint16_t> pixels;
    };
    static HudCache cache;

    const uint64_t tNow = now_ns();
    if (cache.fbW != fbW || cache.w != cardW || cache.h != cardH ||
        cache.pixels.empty() || tNow - cache.lastUpdateNs >= 250000000ULL) {
        cache.fbW = fbW;
        cache.w = cardW;
        cache.h = cardH;
        cache.lastUpdateNs = tNow;
        cache.pixels.assign(static_cast<size_t>(cardW) * static_cast<size_t>(cardH), rgb565(12, 18, 25));

        const int pitch = cardW * static_cast<int>(sizeof(uint16_t));
        const uint16_t bg = rgb565(12, 18, 25);
        const uint16_t bg2 = rgb565(15, 23, 32);
        const uint16_t border = rgb565(48, 60, 72);
        const uint16_t divider = rgb565(32, 42, 54);
        const uint16_t accent = rgb565(0, 214, 214);
        const uint16_t label = rgb565(168, 182, 196);
        const uint16_t dim = rgb565(118, 130, 144);
        const uint16_t red = rgb565(255, 72, 72);

        fill_rect_rgb565(cache.pixels.data(), pitch, cardW, cardH, 0, 0, cardW, cardH, bg);
        fill_rect_rgb565(cache.pixels.data(), pitch, cardW, cardH, stripeW, 0, cardW - stripeW, cardH, bg2);
        fill_rect_rgb565(cache.pixels.data(), pitch, cardW, cardH, 0, 0, stripeW, cardH, accent);
        draw_rect_outline_rgb565(cache.pixels.data(), pitch, cardW, cardH, 0, 0, cardW, cardH, 1, border);
        fill_rect_rgb565(cache.pixels.data(), pitch, cardW, cardH, stripeW, padY + rowH, cardW - stripeW, 1, divider);
        fill_rect_rgb565(cache.pixels.data(), pitch, cardW, cardH, stripeW, padY + rowH * 2, cardW - stripeW, 1, divider);

        const int x0 = stripeW + padX;
        const int y0 = padY + textYOffset;
        auto drawTextPx = [&](int row, int x, const char *text, uint16_t color) {
            draw_hud_text_12x18_rgb565(cache.pixels.data(), pitch, cardW, cardH,
                                        x0 + x, y0 + row * rowH,
                                        text, color);
        };
        auto drawMetric = [&](int row,
                              int keyX,
                              int colonX,
                              int valueX,
                              const char *key,
                              const char *value,
                              uint16_t valueColor) {
            drawTextPx(row, keyX, key, label);
            drawTextPx(row, colonX, ":", dim);
            drawTextPx(row, valueX, value, valueColor);
        };

        char buf[32];

        constexpr int kRowLabelX = 0;
        constexpr int kMetric1KeyX = 78;
        constexpr int kMetric1ColonX = 117;
        constexpr int kMetric1ValueX = 133;
        constexpr int kMetric2KeyX = 196;
        constexpr int kMetric2ColonX = 235;
        constexpr int kMetric2ValueX = 251;
        constexpr int kMetric3KeyX = 330;
        constexpr int kMetric3ColonX = 369;
        constexpr int kMetric3ValueX = 385;

        // SYS row: system resources. TMP value keeps a small gap before C for readability.
        drawTextPx(0, kRowLabelX, "SYS", accent);
        hud_format_percent(buf, sizeof(buf), st.cpuLoad);
        drawMetric(0, kMetric1KeyX, kMetric1ColonX, kMetric1ValueX,
                   "CPU", buf, st.cpuLoad >= 0 ? hud_load_color(st.cpuLoad) : dim);
        hud_format_percent(buf, sizeof(buf), st.npuLoad);
        drawMetric(0, kMetric2KeyX, kMetric2ColonX, kMetric2ValueX,
                   "NPU", buf, st.npuLoad >= 0 ? hud_load_color(st.npuLoad) : dim);
        hud_format_temp(buf, sizeof(buf), st.tempC);
        drawMetric(0, kMetric3KeyX, kMetric3ColonX, kMetric3ValueX,
                   "TMP", buf, st.tempC >= 0.0 ? hud_temp_color(st.tempC) : dim);

        // FPS row: keep RX label for the PCIe receive path summary; no repeated row unit text.
        drawTextPx(1, kRowLabelX, "FPS", accent);
        hud_format_fps(buf, sizeof(buf), st.rxFps);
        drawMetric(1, kMetric1KeyX, kMetric1ColonX, kMetric1ValueX,
                   "RX", buf, hud_fps_color(st.rxFps, 55.0));
        // Show only real-time AI FPS in the compact HDMI HUD. The target FPS remains
        // available on the Qt AI page, where detailed inference diagnostics belong.
        hud_format_fps(buf, sizeof(buf), st.aiFps);
        drawMetric(1, kMetric2KeyX, kMetric2ColonX, kMetric2ValueX,
                   "AI", buf, hud_fps_color(st.aiFps, st.aiTargetFps > 0 ? static_cast<double>(st.aiTargetFps) : 0.0));
        hud_format_fps(buf, sizeof(buf), st.flipFps);
        drawMetric(1, kMetric3KeyX, kMetric3ColonX, kMetric3ValueX,
                   "DSP", buf, hud_fps_color(st.flipFps, 55.0));

        // LAT row: use ms as the implicit unit; avoid widening the row title.
        drawTextPx(2, kRowLabelX, "LAT", accent);
        hud_format_ms(buf, sizeof(buf), st.rgaAvgUs);
        drawMetric(2, kMetric1KeyX, kMetric1ColonX, kMetric1ValueX,
                   "RGA", buf, hud_latency_color(st.rgaAvgUs / 1000.0, 4.0, 8.0));
        hud_format_ms(buf, sizeof(buf), st.npuPrepareAvgUs);
        drawMetric(2, kMetric2KeyX, kMetric2ColonX, kMetric2ValueX,
                   "NP", buf, hud_latency_color(st.npuPrepareAvgUs / 1000.0, 8.0, 14.0));
        hud_format_ms(buf, sizeof(buf), st.osdAvgUs);
        drawMetric(2, kMetric3KeyX, kMetric3ColonX, kMetric3ValueX,
                   "OSD", buf, hud_latency_color(st.osdAvgUs / 1000.0, 4.0, 8.0));

        if (st.failTotal > 0) {
            fill_rect_rgb565(cache.pixels.data(), pitch, cardW, cardH, cardW - 54, 0,
                              54, 14, red);
            draw_hud_text_rgb565(cache.pixels.data(), pitch, cardW, cardH,
                                  cardW - 48, 2,
                                  "FAIL", 1, rgb565(255, 255, 255));
        }
    }

    blit_rgb565_block(fb, pitchBytes, fbW, fbH, cardX, cardY, cache.w, cache.h, cache.pixels);
    return cardY + cache.h + 6;
}

static void draw_perf_hud_with_ai_summary(void *fb, int pitchBytes, int fbW, int fbH,
                                          const OsdStats &stats,
                                          const NpuFrameResult *result)
{
    const int y0 = draw_perf_hud(fb, pitchBytes, fbW, fbH, stats);
    if (!result || result->error.empty()) {
        return;
    }

    const int scale = (fbW >= 1600) ? 2 : 1;
    const int x0 = (fbW >= 1600) ? 22 : 12;
    const uint16_t shadow = rgb565(0, 0, 0);
    const uint16_t red = rgb565(255, 72, 72);
    const std::string err = osd_ascii_label(result->error, "ERR");
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x0, y0 + 4,
                            err.c_str(), scale, red, shadow);
}

static int scale_capture_x(float x, int fbW, int sourceW)
{
    return static_cast<int>(x * static_cast<float>(fbW) / static_cast<float>(sourceW) + 0.5f);
}

static int scale_capture_y(float y, int fbH, int sourceH)
{
    return static_cast<int>(y * static_cast<float>(fbH) / static_cast<float>(sourceH) + 0.5f);
}

static void draw_line_rgb565(void *fb,
                             int pitchBytes,
                             int fbW,
                             int fbH,
                             int x0,
                             int y0,
                             int x1,
                             int y1,
                             int thickness,
                             uint16_t color)
{
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    const int radius = std::max(0, thickness / 2);

    for (;;) {
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                const int x = x0 + ox;
                const int y = y0 + oy;
                if (x >= 0 && x < fbW && y >= 0 && y < fbH) {
                    uint16_t *row = reinterpret_cast<uint16_t *>(
                                static_cast<uint8_t *>(fb) + y * pitchBytes);
                    row[x] = color;
                }
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twiceError = 2 * error;
        if (twiceError >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twiceError <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static uint16_t violation_light_color(NpuTrafficLightState state)
{
    switch (state) {
    case NpuTrafficLightState::Red:
        return rgb565(255, 60, 60);
    case NpuTrafficLightState::Green:
        return rgb565(40, 230, 100);
    case NpuTrafficLightState::Unknown:
    default:
        return rgb565(60, 180, 255);
    }
}

static void draw_violation_regions(void *fb,
                                   int pitchBytes,
                                   int fbW,
                                   int fbH,
                                   const NpuFrameResult& result)
{
    if (result.task != NpuTaskType::TrafficViolation) {
        return;
    }

    const ViolationRuleConfig& config = result.violationConfig;
    const uint16_t lightColor = violation_light_color(result.trafficLight);
    const uint16_t crosswalkColor = rgb565(255, 220, 30);

    if (config.lightRoiValid) {
        const int x = static_cast<int>(config.lightX * fbW + 0.5f);
        const int y = static_cast<int>(config.lightY * fbH + 0.5f);
        const int w = std::max(2, static_cast<int>(config.lightW * fbW + 0.5f));
        const int h = std::max(2, static_cast<int>(config.lightH * fbH + 0.5f));
        draw_rect_outline_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, h, 3, lightColor);
    }

    if (config.crosswalkValid) {
        int px[4] = {};
        int py[4] = {};
        for (int i = 0; i < 4; ++i) {
            px[i] = static_cast<int>(config.crosswalk[static_cast<std::size_t>(i)].x * fbW + 0.5f);
            py[i] = static_cast<int>(config.crosswalk[static_cast<std::size_t>(i)].y * fbH + 0.5f);
        }
        for (int i = 0; i < 4; ++i) {
            const int next = (i + 1) % 4;
            draw_line_rgb565(fb, pitchBytes, fbW, fbH,
                             px[i], py[i], px[next], py[next], 3, crosswalkColor);
        }
    }
}

static void draw_wrong_way_regions(void *fb,
                                   int pitchBytes,
                                   int fbW,
                                   int fbH,
                                   const NpuFrameResult& result)
{
    if (result.task != NpuTaskType::VehicleWrongWay) return;
    static const uint16_t colors[] = {
        rgb565(0, 220, 255), rgb565(255, 70, 220),
        rgb565(255, 190, 0), rgb565(80, 220, 100)
    };
    for (std::size_t laneIndex = 0;
         laneIndex < result.wrongWayConfig.lanes.size(); ++laneIndex) {
        const VehicleLaneDirectionConfig& lane = result.wrongWayConfig.lanes[laneIndex];
        const uint16_t color = colors[laneIndex % 4];
        int px[4] = {};
        int py[4] = {};
        float centerX = 0.0f;
        float centerY = 0.0f;
        for (int i = 0; i < 4; ++i) {
            px[i] = static_cast<int>(lane.quad[static_cast<std::size_t>(i)].x * fbW + 0.5f);
            py[i] = static_cast<int>(lane.quad[static_cast<std::size_t>(i)].y * fbH + 0.5f);
            centerX += px[i];
            centerY += py[i];
        }
        for (int i = 0; i < 4; ++i) {
            const int next = (i + 1) % 4;
            draw_line_rgb565(fb, pitchBytes, fbW, fbH,
                             px[i], py[i], px[next], py[next], 3, color);
        }
        centerX *= 0.25f;
        centerY *= 0.25f;
        const float arrowLength = std::max(35.0f, std::min(fbW, fbH) * 0.10f);
        const int endX = static_cast<int>(centerX + lane.direction.x * arrowLength);
        const int endY = static_cast<int>(centerY + lane.direction.y * arrowLength);
        draw_line_rgb565(fb, pitchBytes, fbW, fbH,
                         static_cast<int>(centerX), static_cast<int>(centerY),
                         endX, endY, 4, color);
        const float angle = std::atan2(lane.direction.y, lane.direction.x);
        const float head = 14.0f;
        draw_line_rgb565(fb, pitchBytes, fbW, fbH, endX, endY,
                         static_cast<int>(endX - head * std::cos(angle - 0.55f)),
                         static_cast<int>(endY - head * std::sin(angle - 0.55f)), 4, color);
        draw_line_rgb565(fb, pitchBytes, fbW, fbH, endX, endY,
                         static_cast<int>(endX - head * std::cos(angle + 0.55f)),
                         static_cast<int>(endY - head * std::sin(angle + 0.55f)), 4, color);
    }
}

static const char* npu_status_text(const NpuFrameResult& result)
{
    if (!result.enabled) return "AI OFF";
    if (!result.modelsReady && !result.error.empty()) return "AI MODEL ERR";
    if (!result.ready) return "AI LOADING";
    return "AI ON";
}

static std::string osd_ascii_label(const std::string& label, const char* fallback)
{
    std::string out;
    const std::string& src = label.empty() ? std::string(fallback) : label;
    for (char c : src) {
        if (c >= 32 && c <= 126) {
            out.push_back(c);
        }
        if (out.size() >= 18) {
            break;
        }
    }
    return out.empty() ? fallback : out;
}

static const char* vehicle_osd_label(int classId)
{
    switch (classId) {
    case 0: return "CAR";
    case 1: return "BUS";
    case 2: return "VAN";
    case 3: return "TRUCK";
    default: return "VEH";
    }
}

static uint16_t vehicle_osd_color(int classId)
{
    switch (classId) {
    case 0: return rgb565(0, 210, 255);
    case 1: return rgb565(255, 190, 0);
    case 2: return rgb565(130, 230, 80);
    case 3: return rgb565(255, 90, 0);
    default: return rgb565(220, 220, 220);
    }
}

static void draw_npu_boxes(void *fb, int pitchBytes, int fbW, int fbH,
                           int sourceW, int sourceH,
                           const NpuFrameResult &result,
                           uint64_t *plateLabelNs)
{
    if (!result.enabled || !result.ready) {
        return;
    }

    const int scale = (fbW >= 1600) ? 2 : 1;
    const int lineH = 9 * scale;
    const uint16_t shadow = rgb565(0, 0, 0);
    const uint16_t red = rgb565(255, 48, 48);
    const uint16_t white = rgb565(255, 255, 255);

    for (const NpuBox& box : result.plates) {
        const int x = scale_capture_x(box.x, fbW, sourceW);
        const int y = scale_capture_y(box.y, fbH, sourceH);
        const int w = std::max(2, scale_capture_x(box.w, fbW, sourceW));
        const int h = std::max(2, scale_capture_y(box.h, fbH, sourceH));
        const PlateOsdStyle st = plate_osd_style(box.classId);
        draw_rect_outline_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, h, 3, st.frameColor);
    }
    const uint64_t plateLabelBeginNs = now_ns();
    draw_plate_labels_from_result(fb, pitchBytes, fbW, fbH,
                                  sourceW, sourceH, result);
    if (plateLabelNs) {
        *plateLabelNs += now_ns() - plateLabelBeginNs;
    }

    draw_violation_regions(fb, pitchBytes, fbW, fbH, result);
    draw_wrong_way_regions(fb, pitchBytes, fbW, fbH, result);

    for (const NpuBox& box : result.pedestrians) {
        const int x = scale_capture_x(box.x, fbW, sourceW);
        const int y = scale_capture_y(box.y, fbH, sourceH);
        const int w = std::max(2, scale_capture_x(box.w, fbW, sourceW));
        const int h = std::max(2, scale_capture_y(box.h, fbH, sourceH));

        uint16_t pedestrianColor = rgb565(40, 230, 100);
        const char *stateLabel = "PED";
        if (box.pedestrianState == NpuPedestrianState::RedAccumulating) {
            pedestrianColor = rgb565(255, 215, 30);
            stateLabel = "WAIT";
        } else if (box.pedestrianState == NpuPedestrianState::RedViolation) {
            pedestrianColor = red;
            stateLabel = "VIOL";
        }
        draw_rect_outline_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, h, 3, pedestrianColor);

        char label[64];
        if (result.task == NpuTaskType::TrafficViolation && box.trackId >= 0) {
            snprintf(label, sizeof(label), "%s ID%d %.2F", stateLabel, box.trackId, box.score);
        } else {
            snprintf(label, sizeof(label), "PED %.2F", box.score);
        }
        const int ty = std::max(2, y - lineH - 2);
        draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, ty, label, scale, white, shadow);
    }


    for (const NpuBox& box : result.vehicles) {
        const int x = scale_capture_x(box.x, fbW, sourceW);
        const int y = scale_capture_y(box.y, fbH, sourceH);
        const int w = std::max(2, scale_capture_x(box.w, fbW, sourceW));
        const int h = std::max(2, scale_capture_y(box.h, fbH, sourceH));
        uint16_t color = vehicle_osd_color(box.classId);
        const char *stateLabel = vehicle_osd_label(box.classId);
        if (result.task == NpuTaskType::VehicleWrongWay) {
            switch (box.vehicleDirectionState) {
            case NpuVehicleDirectionState::WrongWay:
                color = red;
                stateLabel = "WRONG";
                break;
            case NpuVehicleDirectionState::Normal:
                color = rgb565(40, 230, 100);
                stateLabel = "OK";
                break;
            case NpuVehicleDirectionState::Stopped:
                color = rgb565(255, 190, 0);
                stateLabel = "STOP";
                break;
            case NpuVehicleDirectionState::Unknown:
            default:
                // 时序模式不显示UNKNOWN文字，只保留普通车辆框。
                stateLabel = "";
                break;
            }
        }
        draw_rect_outline_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, h, 3, color);

        char label[64];
        if (result.task == NpuTaskType::VehicleWrongWay) {
            snprintf(label, sizeof(label), "%s", stateLabel);
        } else {
            snprintf(label, sizeof(label), "%s %.2F", vehicle_osd_label(box.classId), box.score);
        }
        const int ty = std::min(fbH - lineH - 2, std::max(2, y + h + 2));
        if (label[0] != '\0') {
            draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, ty, label, scale, white, shadow);
        }
    }
}

struct DrmFb {
    uint32_t fbId = 0;
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    void *map = nullptr;
};

struct DrmState {
    int fd = -1;
    drmModeRes *res = nullptr;
    drmModeConnector *conn = nullptr;
    drmModeEncoder *enc = nullptr;
    drmModeCrtc *oldCrtc = nullptr;
    uint32_t connId = 0;
    uint32_t crtcId = 0;
    drmModeModeInfo mode {};
    int modeW = 0;
    int modeH = 0;
    DrmFb fb[2];
};

static void drm_destroy_fb(DrmState &d, DrmFb &fb)
{
    if (d.fd < 0) return;
    if (fb.map && fb.map != MAP_FAILED) { munmap(fb.map, fb.size); fb.map = nullptr; }
    if (fb.fbId) { drmModeRmFB(d.fd, fb.fbId); fb.fbId = 0; }
    if (fb.handle) {
        struct drm_mode_destroy_dumb destroy {};
        destroy.handle = fb.handle;
        ioctl(d.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        fb.handle = 0;
    }
}

static void drm_cleanup(DrmState &d)
{
    if (d.fd >= 0 && d.oldCrtc) {
        drmModeSetCrtc(d.fd, d.oldCrtc->crtc_id, d.oldCrtc->buffer_id,
                       d.oldCrtc->x, d.oldCrtc->y, &d.connId, 1, &d.oldCrtc->mode);
    }
    drm_destroy_fb(d, d.fb[0]);
    drm_destroy_fb(d, d.fb[1]);
    if (d.oldCrtc) { drmModeFreeCrtc(d.oldCrtc); d.oldCrtc = nullptr; }
    if (d.enc) { drmModeFreeEncoder(d.enc); d.enc = nullptr; }
    if (d.conn) { drmModeFreeConnector(d.conn); d.conn = nullptr; }
    if (d.res) { drmModeFreeResources(d.res); d.res = nullptr; }
    if (d.fd >= 0) { ::close(d.fd); d.fd = -1; }
}

static bool mode_matches(const drmModeModeInfo &m, int w, int h)
{
    return int(m.hdisplay) == w && int(m.vdisplay) == h;
}

static bool drm_select_connector_and_mode(DrmState &d, int reqW, int reqH)
{
    d.res = drmModeGetResources(d.fd);
    if (!d.res) { perror("drmModeGetResources"); return false; }

    for (int i = 0; i < d.res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(d.fd, d.res->connectors[i]);
        if (!c) continue;

        // 只允许PCIe主视频占用HDMI，避免同时存在DSI和HDMI时误选DSI，
        // 从而破坏Qt/PTZ所在的DSI界面。
        const bool isHdmi =
                c->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
                c->connector_type == DRM_MODE_CONNECTOR_HDMIB;

        if (isHdmi &&
            c->connection == DRM_MODE_CONNECTED &&
            c->count_modes > 0) {
            int modeIdx = -1;
            if (reqW > 0 && reqH > 0) {
                for (int m = 0; m < c->count_modes; ++m) {
                    if (mode_matches(c->modes[m], reqW, reqH)) { modeIdx = m; break; }
                }
            }
            if (modeIdx < 0) {
                for (int m = 0; m < c->count_modes; ++m) {
                    if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { modeIdx = m; break; }
                }
            }
            if (modeIdx < 0) modeIdx = 0;
            d.conn = c;
            d.connId = c->connector_id;
            d.mode = c->modes[modeIdx];
            d.modeW = d.mode.hdisplay;
            d.modeH = d.mode.vdisplay;
            return true;
        }
        drmModeFreeConnector(c);
    }
    return false;
}

static bool drm_select_crtc(DrmState &d)
{
    if (d.conn->encoder_id) {
        d.enc = drmModeGetEncoder(d.fd, d.conn->encoder_id);
        if (d.enc && d.enc->crtc_id) {
            d.crtcId = d.enc->crtc_id;
            d.oldCrtc = drmModeGetCrtc(d.fd, d.crtcId);
            return true;
        }
    }
    for (int e = 0; e < d.conn->count_encoders; ++e) {
        drmModeEncoder *enc = drmModeGetEncoder(d.fd, d.conn->encoders[e]);
        if (!enc) continue;
        for (int c = 0; c < d.res->count_crtcs; ++c) {
            if (enc->possible_crtcs & (1 << c)) {
                d.enc = enc;
                d.crtcId = d.res->crtcs[c];
                d.oldCrtc = drmModeGetCrtc(d.fd, d.crtcId);
                return true;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return false;
}

static bool drm_create_fb(DrmState &d, int width, int height, DrmFb &fb)
{
    struct drm_mode_create_dumb creq {};
    creq.width = width;
    creq.height = height;
    creq.bpp = 16;
    if (ioctl(d.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) { perror("DRM_IOCTL_MODE_CREATE_DUMB"); return false; }
    fb.handle = creq.handle;
    fb.pitch = creq.pitch;
    fb.size = creq.size;
    if (drmModeAddFB(d.fd, width, height, 16, 16, fb.pitch, fb.handle, &fb.fbId) != 0) {
        perror("drmModeAddFB");
        drm_destroy_fb(d, fb);
        return false;
    }
    struct drm_mode_map_dumb mreq {};
    mreq.handle = fb.handle;
    if (ioctl(d.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        drm_destroy_fb(d, fb);
        return false;
    }
    fb.map = mmap(nullptr, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, mreq.offset);
    if (fb.map == MAP_FAILED) {
        perror("mmap DRM dumb buffer");
        fb.map = nullptr;
        drm_destroy_fb(d, fb);
        return false;
    }
    memset(fb.map, 0, fb.size);
    return true;
}

static bool drm_init(DrmState &d, const char *cardPath, int reqW, int reqH)
{
    d.fd = open(cardPath, O_RDWR | O_CLOEXEC);
    if (d.fd < 0) { perror("open DRM card"); return false; }
    if (!drm_select_connector_and_mode(d, reqW, reqH)) { fprintf(stderr, "no connected HDMI connector with mode\n"); return false; }
    if (!drm_select_crtc(d)) { fprintf(stderr, "no usable DRM CRTC\n"); return false; }
    qDebug() << "DRM selected:" << cardPath << "connector=" << d.connId
             << "crtc=" << d.crtcId << "mode=" << d.modeW << "x" << d.modeH
             << "name=" << d.mode.name;
    if (!drm_create_fb(d, d.modeW, d.modeH, d.fb[0])) return false;
    if (!drm_create_fb(d, d.modeW, d.modeH, d.fb[1])) return false;
    return true;
}

struct FlipWaitState { volatile int pending = 0; };

static void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *data)
{
    FlipWaitState *st = static_cast<FlipWaitState *>(data);
    if (st) st->pending = 0;
}

static bool drm_present_fb_blocking(DrmState &d, uint32_t fbId, bool *usedSetCrtc, const std::atomic<bool> &stopReq)
{
    if (usedSetCrtc) *usedSetCrtc = false;
    FlipWaitState wait {};
    wait.pending = 1;
    int ret = drmModePageFlip(d.fd, d.crtcId, fbId, DRM_MODE_PAGE_FLIP_EVENT, &wait);
    if (ret != 0) {
        int saved = errno;
        if (saved == EBUSY) {
            drmEventContext ev {};
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = page_flip_handler;
            struct pollfd pfd {};
            pfd.fd = d.fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 100);
            if (pr > 0 && (pfd.revents & POLLIN)) (void)drmHandleEvent(d.fd, &ev);
            wait.pending = 1;
            ret = drmModePageFlip(d.fd, d.crtcId, fbId, DRM_MODE_PAGE_FLIP_EVENT, &wait);
            saved = errno;
        }
        if (ret != 0) {
            if (saved == EINVAL || saved == EBUSY) {
                if (drmModeSetCrtc(d.fd, d.crtcId, fbId, 0, 0, &d.connId, 1, &d.mode) == 0) {
                    if (usedSetCrtc) *usedSetCrtc = true;
                    return true;
                }
            }
            fprintf(stderr, "drm present failed: errno=%d(%s), fb_id=%u crtc=%u conn=%u mode=%dx%d\n",
                    saved, strerror(saved), fbId, d.crtcId, d.connId, d.modeW, d.modeH);
            return false;
        }
    }

    drmEventContext ev {};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = page_flip_handler;
    while (wait.pending && !stopReq.load()) {
        struct pollfd pfd {};
        pfd.fd = d.fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll DRM event");
            return false;
        }
        if (pr == 0) {
            fprintf(stderr, "drm pageflip event timeout, fb_id=%u\n", fbId);
            return false;
        }
        if (pfd.revents & POLLIN) {
            int hr = drmHandleEvent(d.fd, &ev);
            if (hr != 0) {
                if (errno == EINTR) continue;
                perror("drmHandleEvent");
                return false;
            }
        } else {
            fprintf(stderr, "unexpected DRM poll revents=0x%x\n", pfd.revents);
            return false;
        }
    }
    return !stopReq.load();
}

DrmRgaDisplay::DrmRgaDisplay(QObject *parent)
    : QObject(parent)
    , m_npuWorker(new NpuInferenceWorker)
{
    m_npuWorker->setInputSource(0, 1);
}

DrmRgaDisplay::~DrmRgaDisplay()
{
    stopDisplay();
}

bool DrmRgaDisplay::startDisplay()
{
    std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMutex);
    if (m_running.load()) {
        lifecycleLock.unlock();
        emit stateChanged("RGA/DRM already running");
        return true;
    }

    // worker 可能因 DRM/PCIe 错误自主退出并将 m_running 清零；
    // std::thread 仍保持 joinable，必须先回收后才能重新赋值。
    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_stopReq.store(false);
    {
        std::lock_guard<std::mutex> lock(m_fpgaControlMutex);
        m_fpgaRequestPending = false;
    }
    m_running.store(true);
    if (m_npuWorker) {
        m_npuWorker->start();
    }
    try {
        m_thread = std::thread(&DrmRgaDisplay::workerMain, this);
    } catch (const std::system_error &error) {
        m_running.store(false);
        if (m_npuWorker) {
            m_npuWorker->stop();
        }
        lifecycleLock.unlock();
        setLastError(QStringLiteral("Failed to start display thread: %1")
                     .arg(QString::fromLocal8Bit(error.what())));
        return false;
    }
    lifecycleLock.unlock();
    emit stateChanged("RGA/DRM starting");
    return true;
}

void DrmRgaDisplay::stopDisplay()
{
    std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMutex);
    const bool hadWorker = m_thread.joinable();
    const bool wasRunning = m_running.load();

    m_stopReq.store(true);
    m_fpgaControlCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_npuWorker) {
        m_npuWorker->stop();
    }
    m_running.store(false);
    lifecycleLock.unlock();

    if (hadWorker || wasRunning) {
        emit stateChanged("RGA/DRM stopped");
    }
}

bool DrmRgaDisplay::isRunning() const { return m_running.load(); }
void DrmRgaDisplay::setOsdEnabled(bool enable) { m_osdEnabled.store(enable); }
bool DrmRgaDisplay::osdEnabled() const { return m_osdEnabled.load(); }
void DrmRgaDisplay::setPerfHudEnabled(bool enable) { m_perfHudEnabled.store(enable); }
bool DrmRgaDisplay::perfHudEnabled() const { return m_perfHudEnabled.load(); }
void DrmRgaDisplay::setDisplayMode(int mode) { m_displayMode.store(mode); }
int DrmRgaDisplay::displayMode() const { return m_displayMode.load(); }

void DrmRgaDisplay::setVideoRoute(VideoRoute route)
{
    route = sanitizeVideoRoute(static_cast<int>(route));
    const uint64_t oldState = m_videoRouteState.load();
    if (decodeVideoRoute(oldState) == route) {
        return;
    }

    const uint64_t revision = decodeVideoRouteRevision(oldState) + 1;
    const uint64_t newState = (revision << 8) |
            static_cast<uint64_t>(static_cast<std::uint8_t>(route));
    m_videoRouteState.store(newState);

    if (m_npuWorker) {
        m_npuWorker->setInputSource(videoRouteChannel(route), revision);
    }
    emit stateChanged(QString("Video route: %1").arg(videoRouteKey(route)));
}

VideoRoute DrmRgaDisplay::videoRoute() const
{
    return decodeVideoRoute(m_videoRouteState.load());
}

void DrmRgaDisplay::requestFpgaPreprocess(int mode, std::uint32_t param0)
{
    if (mode < 0 || mode >= 32) {
        emit fpgaApplyFinished(false,
                               static_cast<quint32>(mode < 0 ? 0 : mode),
                               0, 0, 0, 0, 0, 0, 0,
                               QStringLiteral("非法FPGA预处理模式"));
        return;
    }

    if (!m_running.load()) {
        emit fpgaApplyFinished(false,
                               static_cast<quint32>(mode),
                               0, 0, 0, 0, 0, 0, 0,
                               QStringLiteral("请先启动HDMI/PCIe显示，再应用FPGA预处理"));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_fpgaControlMutex);
        m_fpgaRequestedMode = static_cast<std::uint32_t>(mode);
        m_fpgaRequestedParam0 = param0;
        m_fpgaRequestPending = true;
    }
    m_fpgaControlCv.notify_one();
}

void DrmRgaDisplay::requestEthernetOutput(bool enable)
{
    if (!m_running.load()) {
        emit ethernetApplyFinished(false,
                                   enable,
                                   false,
                                   0, 0, 0, 0, 0, 0,
                                   QStringLiteral("请先启动HDMI/PCIe显示，再切换网络视频输出"));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_fpgaControlMutex);
        m_ethRequestedEnable = enable;
        m_ethRequestPending = true;
    }
    m_fpgaControlCv.notify_one();
}

bool DrmRgaDisplay::setOutputResolution(int width, int height)
{
    if (m_running.load()) {
        setLastError("运行中不能修改 HDMI 分辨率，请先停止显示");
        return false;
    }
    if (width <= 0 || height <= 0) {
        setLastError("非法输出分辨率");
        return false;
    }
    m_reqWidth.store(width);
    m_reqHeight.store(height);
    return true;
}

int DrmRgaDisplay::outputWidth() const { return m_reqWidth.load(); }
int DrmRgaDisplay::outputHeight() const { return m_reqHeight.load(); }

void DrmRgaDisplay::setNpuConfig(const NpuTaskConfig &config)
{
    if (m_npuWorker) {
        m_npuWorker->setConfig(config);
    }
}

NpuTaskConfig DrmRgaDisplay::npuConfig() const
{
    return m_npuWorker ? m_npuWorker->config() : NpuTaskConfig();
}

void DrmRgaDisplay::setNpuEnabled(bool enable)
{
    if (m_npuWorker) {
        m_npuWorker->setEnabled(enable);
    }
}

void DrmRgaDisplay::setNpuTargetFps(int fps)
{
    if (m_npuWorker) {
        m_npuWorker->setTargetFps(fps);
    }
}

void DrmRgaDisplay::setNpuConfidence(float confidence)
{
    if (m_npuWorker) {
        m_npuWorker->setConfidence(confidence);
    }
}

void DrmRgaDisplay::setNpuNmsThreshold(float threshold)
{
    if (m_npuWorker) {
        m_npuWorker->setNmsThreshold(threshold);
    }
}

void DrmRgaDisplay::setNpuTask(NpuTaskType task)
{
    if (m_npuWorker) {
        m_npuWorker->setTask(task);
    }
}

void DrmRgaDisplay::setViolationConfig(const ViolationRuleConfig &config)
{
    if (m_npuWorker) {
        m_npuWorker->setViolationConfig(config);
    }
}

ViolationRuleConfig DrmRgaDisplay::violationConfig() const
{
    return m_npuWorker ? m_npuWorker->violationConfig() : ViolationRuleConfig();
}

void DrmRgaDisplay::setWrongWayConfig(const VehicleDirectionConfig &config)
{
    if (m_npuWorker) {
        m_npuWorker->setWrongWayConfig(config);
    }
}

VehicleDirectionConfig DrmRgaDisplay::wrongWayConfig() const
{
    return m_npuWorker ? m_npuWorker->wrongWayConfig() : VehicleDirectionConfig();
}

NpuFrameResult DrmRgaDisplay::latestNpuResult() const
{
    return m_npuWorker ? m_npuWorker->latestResult() : NpuFrameResult();
}

std::vector<NpuFrameResult> DrmRgaDisplay::npuResultsAfter(uint64_t frameId,
                                                          std::size_t maxResults) const
{
    return m_npuWorker
            ? m_npuWorker->resultsAfter(frameId, maxResults)
            : std::vector<NpuFrameResult>();
}

QString DrmRgaDisplay::lastErrorString() const { return m_lastError; }

void DrmRgaDisplay::setLastError(const QString &msg)
{
    m_lastError = msg;
    qDebug() << "[DrmRgaDisplay]" << msg;
    emit errorOccurred(msg);
}

void DrmRgaDisplay::workerMain()
{
    const char *devPath = "/dev/pango_pci_driver";
    const char *drmPath = "/dev/dri/card0";
    const int reqW = m_reqWidth.load();
    const int reqH = m_reqHeight.load();

    PcieFrameReceiver pcie;
    DrmState drm;
    std::atomic<bool> fpgaControlStop {false};
    std::thread fpgaControlThread;

    emit stateChanged("Opening PCIe driver");
    if (!pcie.open(devPath)) {
        setLastError(QString::fromStdString(pcie.lastError()));
        goto cleanup;
    }

    {
        const PcieFrameReceiver::LinkInfo &info = pcie.linkInfo();
        if (info.valid) {
            qDebug() << "PCI"
                     << "vendor=0x" + QString::number(info.vendorId, 16)
                     << "device=0x" + QString::number(info.deviceId, 16)
                     << "link=gen" << info.linkSpeed
                     << "x" << info.linkWidth
                     << "mps=" << info.mps
                     << "mrrs=" << info.mrrs;
        } else if (!pcie.lastWarning().empty()) {
            emit stateChanged(QString("Warning: %1")
                              .arg(QString::fromStdString(pcie.lastWarning())));
        }
    }

    if (!pcie.startStream()) {
        setLastError(QString::fromStdString(pcie.lastError()));
        goto cleanup;
    }

    if (pcie.pioControlActive()) {
        emit stateChanged("PCIe PIO start written");
    } else {
        const QString warning = pcie.lastWarning().empty()
                ? QStringLiteral("BAR0 PIO map failed, continue")
                : QString::fromStdString(pcie.lastWarning());
        emit stateChanged(QString("Warning: %1").arg(warning));
    }

    // FPGA预处理与Ethernet输出控制共用一个控制线程和同一个设备fd，
    // 避免第二次open/close触发驱动释放DMA缓冲。
    // 视频read()与控制ioctl可并行；驱动内部对控制ioctl进行串行化。
    fpgaControlThread = std::thread([this, &pcie, &fpgaControlStop]() {
        auto publishFpgaStatus = [this, &pcie](const QString &prefix) -> bool {
            PcieFrameReceiver::FpgaCaps caps;
            PcieFrameReceiver::FpgaStatus status;
            std::string capsError;
            std::string statusError;

            const bool capsOk = pcie.getFpgaCaps(caps, &capsError);
            const bool statusOk = pcie.getFpgaStatus(status, &statusError);
            if (!capsOk || !statusOk) {
                QString message = prefix;
                if (!capsOk) {
                    message += QStringLiteral(" GET_CAPS失败：%1")
                            .arg(QString::fromStdString(capsError));
                }
                if (!statusOk) {
                    message += QStringLiteral(" GET_STATUS失败：%1")
                            .arg(QString::fromStdString(statusError));
                }
                emit fpgaControlInfoUpdated(false,
                                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            message.trimmed());
                return false;
            }

            emit fpgaControlInfoUpdated(true,
                                        caps.ipVersion,
                                        caps.capability,
                                        caps.algorithmBitmap,
                                        caps.streamCount,
                                        status.activeMode,
                                        status.ackSequence,
                                        status.status,
                                        status.errorStatus,
                                        status.frameCount,
                                        status.lastApplyFrame,
                                        prefix);
            return true;
        };

        auto publishEthernetStatus = [this, &pcie](const QString &prefix) -> bool {
            PcieFrameReceiver::EthernetCaps caps;
            PcieFrameReceiver::EthernetStatus status;
            std::string capsError;
            std::string statusError;

            const bool capsOk = pcie.getEthernetCaps(caps, &capsError);
            const bool statusOk = pcie.getEthernetStatus(status, &statusError);
            if (!capsOk || !statusOk) {
                QString message = prefix;
                if (!capsOk) {
                    message += QStringLiteral(" GET_ETH_CAPS失败：%1")
                            .arg(QString::fromStdString(capsError));
                }
                if (!statusOk) {
                    message += QStringLiteral(" GET_ETH_STATUS失败：%1")
                            .arg(QString::fromStdString(statusError));
                }
                emit ethernetControlInfoUpdated(false,
                                                0, 0, 0, 0, 0, 0, 0,
                                                false, false,
                                                0, 0, 0, 0, 0,
                                                message.trimmed());
                return false;
            }

            emit ethernetControlInfoUpdated(true,
                                            caps.ipVersion,
                                            caps.capability,
                                            caps.outputWidth,
                                            caps.outputHeight,
                                            caps.outputFps,
                                            caps.destinationIpv4,
                                            caps.destinationPort,
                                            status.requestedEnable,
                                            status.activeEnable,
                                            status.ackSequence,
                                            status.status,
                                            status.errorStatus,
                                            status.frameCount,
                                            status.lastApplyFrame,
                                            prefix);
            return true;
        };

        bool fpgaControlReady = publishFpgaStatus(
                    QStringLiteral("FPGA预处理控制已就绪"));
        bool ethernetControlReady = publishEthernetStatus(
                    QStringLiteral("网络视频输出控制已就绪"));

        while (!fpgaControlStop.load()) {
            bool processFpgaRequest = false;
            bool processEthernetRequest = false;
            std::uint32_t requestedMode = 0;
            std::uint32_t requestedParam0 = 0;
            bool requestedEthernetEnable = false;

            {
                std::unique_lock<std::mutex> lock(m_fpgaControlMutex);
                const bool allReady = fpgaControlReady && ethernetControlReady;
                auto wakePredicate = [this, &fpgaControlStop]() {
                    return fpgaControlStop.load()
                            || m_fpgaRequestPending
                            || m_ethRequestPending;
                };

                if (allReady) {
                    m_fpgaControlCv.wait(lock, wakePredicate);
                } else {
                    // 初次读取失败时每秒重试，避免驱动/FPGA刚就绪的短暂竞态
                    // 让某一组按钮永久不可用。
                    (void)m_fpgaControlCv.wait_for(
                                lock,
                                std::chrono::seconds(1),
                                wakePredicate);
                }

                if (fpgaControlStop.load()) {
                    break;
                }

                processFpgaRequest = m_fpgaRequestPending;
                if (processFpgaRequest) {
                    requestedMode = m_fpgaRequestedMode;
                    requestedParam0 = m_fpgaRequestedParam0;
                    m_fpgaRequestPending = false;
                }

                processEthernetRequest = m_ethRequestPending;
                if (processEthernetRequest) {
                    requestedEthernetEnable = m_ethRequestedEnable;
                    m_ethRequestPending = false;
                }
            }

            if (!processFpgaRequest && !processEthernetRequest) {
                if (!fpgaControlReady) {
                    fpgaControlReady = publishFpgaStatus(
                                QStringLiteral("正在重试FPGA预处理控制状态"));
                }
                if (!ethernetControlReady) {
                    ethernetControlReady = publishEthernetStatus(
                                QStringLiteral("正在重试网络视频输出控制状态"));
                }
                continue;
            }

            if (processFpgaRequest) {
                PcieFrameReceiver::FpgaApplyResult result;
                std::string applyError;
                // 正常情况下在一个帧周期内完成；250ms兼容偶发坏帧重同步。
                const bool success = pcie.applyFpgaPreprocess(requestedMode,
                                                              requestedParam0,
                                                              250U,
                                                              result,
                                                              &applyError);

                PcieFrameReceiver::FpgaStatus afterStatus;
                std::string statusError;
                const bool statusOk = pcie.getFpgaStatus(afterStatus, &statusError);
                const std::uint32_t activeMode = statusOk
                        ? afterStatus.activeMode : result.activeMode;
                const std::uint32_t ackSequence = statusOk
                        ? afterStatus.ackSequence : result.ackSequence;
                const std::uint32_t statusValue = statusOk
                        ? afterStatus.status : result.status;
                const std::uint32_t errorStatus = statusOk
                        ? afterStatus.errorStatus : result.errorStatus;
                const std::uint32_t applyFrame = statusOk
                        ? afterStatus.lastApplyFrame : 0U;

                QString message;
                if (success) {
                    message = QStringLiteral("已应用%1：seq=%2 ack=%3，耗时%4 us，生效帧%5")
                            .arg(fpga_mode_name(requestedMode))
                            .arg(result.sequence)
                            .arg(ackSequence)
                            .arg(result.latencyUs)
                            .arg(applyFrame);
                } else {
                    message = QStringLiteral("应用%1失败：%2；active=%3 seq=%4 ack=%5 status=0x%6 error=0x%7")
                            .arg(fpga_mode_name(requestedMode),
                                 QString::fromStdString(applyError),
                                 fpga_mode_name(activeMode))
                            .arg(result.sequence)
                            .arg(ackSequence)
                            .arg(statusValue, 8, 16, QLatin1Char('0'))
                            .arg(errorStatus, 8, 16, QLatin1Char('0'));
                    if (!statusOk && !statusError.empty()) {
                        message += QStringLiteral("；状态读取失败：%1")
                                .arg(QString::fromStdString(statusError));
                    }
                }

                emit fpgaApplyFinished(success,
                                       requestedMode,
                                       activeMode,
                                       result.sequence,
                                       ackSequence,
                                       statusValue,
                                       errorStatus,
                                       result.latencyUs,
                                       applyFrame,
                                       message);

                fpgaControlReady = publishFpgaStatus(success
                        ? QStringLiteral("FPGA预处理状态已刷新")
                        : QStringLiteral("FPGA预处理应用失败后状态已刷新"));
            }

            if (processEthernetRequest) {
                PcieFrameReceiver::EthernetApplyResult result;
                std::string applyError;
                const bool success = pcie.setEthernetOutput(requestedEthernetEnable,
                                                            250U,
                                                            result,
                                                            &applyError);

                PcieFrameReceiver::EthernetStatus afterStatus;
                std::string statusError;
                const bool statusOk = pcie.getEthernetStatus(afterStatus, &statusError);
                const bool activeEnable = statusOk
                        ? afterStatus.activeEnable : result.activeEnable;
                const std::uint32_t ackSequence = statusOk
                        ? afterStatus.ackSequence : result.ackSequence;
                const std::uint32_t statusValue = statusOk
                        ? afterStatus.status : result.status;
                const std::uint32_t errorStatus = statusOk
                        ? afterStatus.errorStatus : result.errorStatus;
                const std::uint32_t applyFrame = statusOk
                        ? afterStatus.lastApplyFrame : result.lastApplyFrame;

                QString message;
                if (success) {
                    message = QStringLiteral("%1成功：seq=%2 ack=%3，耗时%4 us，生效帧%5")
                            .arg(ethernet_output_name(requestedEthernetEnable))
                            .arg(result.sequence)
                            .arg(ackSequence)
                            .arg(result.latencyUs)
                            .arg(applyFrame);
                } else {
                    message = QStringLiteral("%1失败：%2；active=%3 seq=%4 ack=%5 status=0x%6 error=0x%7")
                            .arg(ethernet_output_name(requestedEthernetEnable),
                                 QString::fromStdString(applyError),
                                 activeEnable ? QStringLiteral("开启") : QStringLiteral("关闭"))
                            .arg(result.sequence)
                            .arg(ackSequence)
                            .arg(statusValue, 8, 16, QLatin1Char('0'))
                            .arg(errorStatus, 8, 16, QLatin1Char('0'));
                    if (!statusOk && !statusError.empty()) {
                        message += QStringLiteral("；状态读取失败：%1")
                                .arg(QString::fromStdString(statusError));
                    }
                }

                emit ethernetApplyFinished(success,
                                           requestedEthernetEnable,
                                           activeEnable,
                                           result.sequence,
                                           ackSequence,
                                           statusValue,
                                           errorStatus,
                                           result.latencyUs,
                                           applyFrame,
                                           message);

                ethernetControlReady = publishEthernetStatus(success
                        ? QStringLiteral("网络视频输出状态已刷新")
                        : QStringLiteral("网络视频输出切换失败后状态已刷新"));
            }
        }
    });

    emit stateChanged("Initializing DRM");
    if (!drm_init(drm, drmPath, reqW, reqH)) {
        setLastError("DRM init failed");
        goto cleanup;
    }
    if (c_RkRgaInit() != 0) emit stateChanged("Warning: c_RkRgaInit returned non-zero");
    emit stateChanged("RGA/DRM display running");

    {
        int front = 0;
        if (drmModeSetCrtc(drm.fd, drm.crtcId, drm.fb[front].fbId, 0, 0, &drm.connId, 1, &drm.mode) != 0) {
            setLastError(QString("drmModeSetCrtc failed: %1").arg(strerror(errno)));
            goto cleanup;
        }

        uint64_t lastNs = now_ns();
        uint64_t lastSensorNs = 0;
        uint64_t readCnt = 0, validCnt = 0, rgaCnt = 0, flipCnt = 0, failCnt = 0, rgaNsSum = 0;
        uint64_t failTotal = 0;
        rkqt::perf::AverageCounter osdCounter;
        rkqt::perf::AverageCounter plateOsdCounter;
        rkqt::perf::AverageCounter npuPrepareCounter;
        uint64_t npuCropFailTotal = 0;
        OsdStats osdStats {};
        std::vector<uint8_t> npuTileBuffer(640u * 360u * PCIE_BPP);
        std::vector<uint8_t> previewBuffer(640u * 360u * PCIE_BPP);
        uint64_t lastPreviewNs = 0;

        while (!m_stopReq.load()) {
            PcieFrameReceiver::FrameView frame;
            const PcieFrameReceiver::WaitResult waitResult = pcie.waitNextFrame(frame);
            readCnt++;
            if (waitResult == PcieFrameReceiver::WaitResult::Error) {
                setLastError(QString::fromStdString(pcie.lastError()));
                break;
            }
            if (waitResult != PcieFrameReceiver::WaitResult::FrameReady) {
                continue;
            }

            const int index = frame.bufferIndex;
            validCnt++;

            const uint64_t routeState = m_videoRouteState.load();
            const VideoRoute route = decodeVideoRoute(routeState);
            const uint64_t routeRevision = decodeVideoRouteRevision(routeState);
            const VideoTileRoi sourceRoi = videoRouteRoi(route);
            const int sourceChannel = videoRouteChannel(route);

            int back = front ^ 1;
            uint64_t r0 = now_ns();
            int r = rga_crop_scale_rgb565(frame.data,
                                          PCIE_FRAME_W,
                                          PCIE_FRAME_H,
                                          sourceRoi,
                                          drm.fb[back].map,
                                          drm.modeW,
                                          drm.modeH,
                                          drm.fb[back].pitch / PCIE_BPP,
                                          drm.modeH);
            uint64_t r1 = now_ns();
            if (r) {
                failCnt++;
                failTotal++;
                if (failCnt <= 10) qDebug() << "RGA to DRM fb failed ret=" << r << "index=" << index;
                continue;
            }
            rgaCnt++;
            rgaNsSum += (r1 - r0);

            uint64_t sensorNow = now_ns();
            if (sensorNow - lastSensorNs >= 1000000000ULL || lastSensorNs == 0) {
                const rkqt::system::SystemSnapshot snapshot = rkqt::system::SystemMonitor::readSnapshot();
                osdStats.cpuLoad = snapshot.cpuLoadPercent;
                osdStats.npuLoad = snapshot.npuLoadPercent;
                osdStats.tempC = snapshot.socTempC;
                lastSensorNs = sensorNow;
            }

            NpuFrameResult ai = latestNpuResult();
            const bool hasMatchingAi = ai.sourceChannel == sourceChannel &&
                    ai.sourceRevision == routeRevision;
            if (ai.enabled) {
                osdStats.aiFps = ai.fps;
                osdStats.aiTargetFps = ai.targetFps;
            } else {
                osdStats.aiFps = 0.0;
                osdStats.aiTargetFps = 0;
            }

            const bool perfHudEnabled = m_perfHudEnabled.load();
            const bool aiOverlayEnabled = m_osdEnabled.load();
            if (perfHudEnabled || aiOverlayEnabled || m_displayMode.load() == 1) {
                const uint64_t osdBeginNs = now_ns();
                uint64_t plateLabelNsThisFrame = 0;

                if (perfHudEnabled) {
                    draw_perf_hud_with_ai_summary(drm.fb[back].map, drm.fb[back].pitch,
                                                  drm.modeW, drm.modeH, osdStats,
                                                  &ai);

                    char routeLabel[16];
                    if (sourceChannel > 0) {
                        snprintf(routeLabel, sizeof(routeLabel), "CAM %d", sourceChannel);
                    } else {
                        snprintf(routeLabel, sizeof(routeLabel), "ALL");
                    }
                    const int routeScale = drm.modeW >= 1600 ? 2 : 1;
                    draw_text_shadow_rgb565(drm.fb[back].map, drm.fb[back].pitch,
                                            drm.modeW, drm.modeH,
                                            drm.modeW - 110 * routeScale, 24,
                                            routeLabel, routeScale,
                                            rgb565(0, 255, 0), rgb565(0, 0, 0));
                }

                if (aiOverlayEnabled && hasMatchingAi) {
                    // 总览模式提交给 NPU 的输入已经缩小到 640x360；
                    // 绘制时将该坐标系直接拉伸映射回 1920x1080 HDMI。
                    const int aiSourceW = 640;
                    const int aiSourceH = 360;
                    draw_npu_boxes(drm.fb[back].map, drm.fb[back].pitch,
                                   drm.modeW, drm.modeH, aiSourceW, aiSourceH, ai,
                                   &plateLabelNsThisFrame);
                }

                if (m_displayMode.load() == 1) {
                    const uint16_t green = rgb565(0, 255, 0);
                    const uint16_t yellow = rgb565(255, 220, 0);
                    draw_rect_outline_rgb565(drm.fb[back].map, drm.fb[back].pitch, drm.modeW, drm.modeH,
                                              60, 60, 420, 220, 4, green);
                    draw_rect_outline_rgb565(drm.fb[back].map, drm.fb[back].pitch, drm.modeW, drm.modeH,
                                              1100, 300, 520, 300, 4, yellow);
                }

                osdCounter.addNs(now_ns() - osdBeginNs);
                if (plateLabelNsThisFrame > 0) {
                    plateOsdCounter.addNs(plateLabelNsThisFrame);
                }
            }

            bool usedSetCrtc = false;
            if (drm_present_fb_blocking(drm, drm.fb[back].fbId, &usedSetCrtc, m_stopReq)) {
                Q_UNUSED(usedSetCrtc);
                front = back;
                flipCnt++;

                // 显示链路优先：当前帧完成 RGA 和 DRM flip 之后，才准备 NPU 输入。
                // 这样九宫格总览/单路高清输入的 RGA 缩小不会阻塞当前 HDMI 帧提交。
                // NPU worker 仍使用 latest-only 单槽机制，推理慢时覆盖旧待处理帧，
                // 只影响后续 AI 结果刷新，不应直接反压当前显示输出。
                if (m_npuWorker && m_npuWorker->wantsFrame()) {
                    const uint64_t npuPrepareBeginNs = now_ns();
                    prepare_and_submit_npu_frame(m_npuWorker.get(),
                                                 frame.data,
                                                 validCnt,
                                                 sourceRoi,
                                                 sourceChannel,
                                                 routeRevision,
                                                 npuTileBuffer,
                                                 npuCropFailTotal);
                    npuPrepareCounter.addNs(now_ns() - npuPrepareBeginNs);
                }

                // DSI配置预览只以5 Hz生成，并且放在当前HDMI帧完成page flip之后，
                // 保持主显示链路优先。预览使用当前视频选路的同一640x360坐标系。
                const uint64_t previewNowNs = now_ns();
                if (lastPreviewNs == 0 || previewNowNs - lastPreviewNs >= 200000000ULL) {
                    const int previewRet = rga_crop_scale_rgb565(frame.data,
                                                                 PCIE_FRAME_W,
                                                                 PCIE_FRAME_H,
                                                                 sourceRoi,
                                                                 previewBuffer.data(),
                                                                 640,
                                                                 360,
                                                                 640,
                                                                 360);
                    if (previewRet == 0) {
                        const QImage view(previewBuffer.data(),
                                          640,
                                          360,
                                          640 * PCIE_BPP,
                                          QImage::Format_RGB16);
                        emit previewFrameReady(view.copy(),
                                               static_cast<quint64>(validCnt),
                                               sourceChannel,
                                               static_cast<quint64>(routeRevision));
                        lastPreviewNs = previewNowNs;
                    }
                }
            } else {
                failCnt++;
                failTotal++;
                usleep(1000);
            }

            uint64_t now = now_ns();
            if (now - lastNs >= 1000000000ULL) {
                double sec = double(now - lastNs) / 1e9;
                double readFps = readCnt / sec;
                double validFps = validCnt / sec;
                double rgaFps = rgaCnt / sec;
                double flipFps = flipCnt / sec;
                double rgaAvgUs = rgaCnt ? double(rgaNsSum) / double(rgaCnt) / 1000.0 : 0.0;

                osdStats.rxFps = validFps;
                osdStats.rgaFps = rgaFps;
                osdStats.flipFps = flipFps;
                osdStats.rgaAvgUs = rgaAvgUs;
                osdStats.osdAvgUs = osdCounter.averageUs();
                osdStats.plateOsdAvgUs = plateOsdCounter.averageUs();
                osdStats.npuPrepareAvgUs = npuPrepareCounter.averageUs();
                osdStats.failTotal = failTotal;

                emit statsUpdated(readFps, validFps, rgaFps, flipFps, rgaAvgUs,
                                  int(failCnt),
                                  osdStats.cpuLoad,
                                  osdStats.npuLoad,
                                  osdStats.tempC,
                                  osdStats.osdAvgUs,
                                  osdStats.plateOsdAvgUs,
                                  osdStats.npuPrepareAvgUs);
                {
                    const NpuFrameResult ai = latestNpuResult();
                    QString aiStatus;
                    bool aiReady = ai.ready;
                    double aiFps = ai.fps;
                    int plateCount = static_cast<int>(ai.plates.size());
                    int pedestrianCount = static_cast<int>(ai.pedestrians.size());
                    int vehicleCount = static_cast<int>(ai.vehicles.size());
                    if (!ai.enabled) {
                        aiStatus = "off";
                    } else if (!ai.error.empty()) {
                        aiStatus = QString::fromStdString(ai.error);
                    } else if (ai.ready) {
                        aiStatus = QString("%1 %2 ok %3ms")
                                .arg(sourceChannel > 0
                                     ? QString("CH%1").arg(sourceChannel)
                                     : QStringLiteral("ALL"))
                                .arg(QString::fromLatin1(npuTaskKey(ai.task)))
                                .arg(ai.lastMs, 0, 'f', 1);
                    } else {
                        aiStatus = "loading";
                    }
                    emit aiStatsUpdated(ai.enabled,
                                        aiReady,
                                        aiFps,
                                        plateCount,
                                        pedestrianCount,
                                        vehicleCount,
                                        aiStatus);
                }

                readCnt = validCnt = rgaCnt = flipCnt = failCnt = rgaNsSum = 0;
                osdCounter.reset();
                plateOsdCounter.reset();
                npuPrepareCounter.reset();
                lastNs = now;
            }
        }
    }

cleanup:
    emit stateChanged("RGA/DRM cleanup");
    fpgaControlStop.store(true);
    m_fpgaControlCv.notify_all();
    if (fpgaControlThread.joinable()) {
        fpgaControlThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_fpgaControlMutex);
        m_fpgaRequestPending = false;
        m_ethRequestPending = false;
    }
    emit fpgaControlInfoUpdated(false,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                QStringLiteral("PCIe显示已停止，FPGA控制不可用"));
    emit ethernetControlInfoUpdated(false,
                                    0, 0, 0, 0, 0, 0, 0,
                                    false, false,
                                    0, 0, 0, 0, 0,
                                    QStringLiteral("PCIe显示已停止，网络视频输出控制不可用"));
    pcie.close();
    // drm_init 失败时也可能已经打开 fd、获取 connector/CRTC 或创建首个 FB。
    // drm_cleanup 对空成员安全，始终调用以释放部分初始化资源。
    drm_cleanup(drm);
    if (m_npuWorker) m_npuWorker->stop();

    m_running.store(false);
    emit stateChanged("RGA/DRM exited");
}
