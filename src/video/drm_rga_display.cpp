#include "video/drm_rga_display.h"
#include "npu/npu_inference_worker.h"
#include "pcie/pcie_frame_receiver.h"

#include <QDebug>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <string>

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

static int rga_scale_rgb565(const void *srcPtr, void *dstPtr, int dstW, int dstH, int dstStrideW, int dstStrideH)
{
    rga_info_t src;
    rga_info_t dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    src.fd = -1;
    src.mmuFlag = 1;
    src.virAddr = const_cast<void *>(srcPtr);
    src.rotation = 0;
    rga_set_rect(&src.rect, 0, 0, PCIE_FRAME_W, PCIE_FRAME_H, PCIE_FRAME_W, PCIE_FRAME_H, RK_FORMAT_RGB_565);

    dst.fd = -1;
    dst.mmuFlag = 1;
    dst.virAddr = dstPtr;
    dst.rotation = 0;
    rga_set_rect(&dst.rect, 0, 0, dstW, dstH, dstStrideW, dstStrideH, RK_FORMAT_RGB_565);

    return c_RkRgaBlit(&src, &dst, nullptr);
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
    case 'C': return "01110" "10000" "10000" "10000" "10000" "10000" "01110";
    case 'D': return "11100" "10010" "10010" "10010" "10010" "10010" "11100";
    case 'E': return "11110" "10000" "10000" "11100" "10000" "10000" "11110";
    case 'F': return "11110" "10000" "10000" "11100" "10000" "10000" "10000";
    case 'G': return "01110" "10000" "10000" "10110" "10010" "10010" "01110";
    case 'I': return "11100" "01000" "01000" "01000" "01000" "01000" "11100";
    case 'K': return "10010" "10100" "11000" "11000" "10100" "10010" "10010";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11110";
    case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'N': return "10010" "11010" "10110" "10010" "10010" "10010" "10010";
    case 'P': return "11100" "10010" "10010" "11100" "10000" "10000" "10000";
    case 'R': return "11100" "10010" "10010" "11100" "10100" "10010" "10010";
    case 'S': return "01110" "10000" "10000" "01100" "00010" "00010" "11100";
    case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'U': return "10010" "10010" "10010" "10010" "10010" "10010" "01100";
    case 'X': return "10001" "01010" "00100" "00100" "00100" "01010" "10001";
    case ':': return "00000" "00100" "00100" "00000" "00100" "00100" "00000";
    case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
    case '%': return "11001" "11010" "00100" "01000" "10110" "00110" "00000";
    case '-': return "00000" "00000" "00000" "11110" "00000" "00000" "00000";
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

static std::string read_file_string(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return std::string();
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = 0;
    return std::string(buf);
}

static int read_npu_load_percent()
{
    std::string s = read_file_string("/sys/kernel/debug/rknpu/load");
    int v = -1;
    if (sscanf(s.c_str(), "NPU load: %d%%", &v) == 1) return v;
    const char *p = s.c_str();
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            if (sscanf(p, "%d", &v) == 1) return v;
        }
        ++p;
    }
    s = read_file_string("/sys/class/devfreq/fde40000.npu/load");
    if (sscanf(s.c_str(), "%d", &v) == 1) return v;
    return -1;
}

static double read_soc_temp_c()
{
    const char *paths[] = {
        "/sys/devices/virtual/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone0/temp"
    };
    for (const char *path : paths) {
        std::string s = read_file_string(path);
        if (!s.empty()) {
            long v = 0;
            if (sscanf(s.c_str(), "%ld", &v) == 1) return double(v) / 1000.0;
        }
    }
    return -1.0;
}

struct OsdStats {
    double rxFps = 0.0;
    double rgaFps = 0.0;
    double flipFps = 0.0;
    double rgaAvgUs = 0.0;
    uint64_t failTotal = 0;
    int npuLoad = -1;
    double tempC = -1.0;
};

static void draw_osd(void *fb, int pitchBytes, int fbW, int fbH, const OsdStats &st)
{
    const int scale = (fbW >= 1600) ? 3 : 2;
    const int x = 24;
    const int y = 24;
    const int lineH = 9 * scale;

    const uint16_t shadow = rgb565(0, 0, 0);
    const uint16_t green = rgb565(0, 255, 0);
    const uint16_t white = rgb565(255, 255, 255);
    const uint16_t yellow = rgb565(255, 220, 0);
    const uint16_t red = rgb565(255, 40, 40);

    char line[96];
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y,
                            "RK3568 PCIE RGA DRM", scale, yellow, shadow);

    snprintf(line, sizeof(line), "RX   %.1F", st.rxFps);
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y + lineH * 1, line, scale, green, shadow);

    snprintf(line, sizeof(line), "RGA  %.1F", st.rgaFps);
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y + lineH * 2, line, scale, green, shadow);

    snprintf(line, sizeof(line), "FLIP %.1F", st.flipFps);
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y + lineH * 3, line, scale, green, shadow);

    if (st.npuLoad >= 0) snprintf(line, sizeof(line), "NPU  %d%%", st.npuLoad);
    else snprintf(line, sizeof(line), "NPU  NA");
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y + lineH * 4, line, scale, white, shadow);

    if (st.tempC >= 0.0) snprintf(line, sizeof(line), "TEMP %.1FC", st.tempC);
    else snprintf(line, sizeof(line), "TEMP NA");
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y + lineH * 5, line, scale, white, shadow);

    snprintf(line, sizeof(line), "FAIL %" PRIu64, st.failTotal);
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, y + lineH * 6, line, scale,
                            st.failTotal ? red : white, shadow);
}

static int scale_capture_x(float x, int fbW)
{
    return static_cast<int>(x * static_cast<float>(fbW) / static_cast<float>(PCIE_FRAME_W) + 0.5f);
}

static int scale_capture_y(float y, int fbH)
{
    return static_cast<int>(y * static_cast<float>(fbH) / static_cast<float>(PCIE_FRAME_H) + 0.5f);
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

static void draw_npu_overlay(void *fb, int pitchBytes, int fbW, int fbH, const NpuFrameResult &result)
{
    const int scale = (fbW >= 1600) ? 2 : 1;
    const int lineH = 9 * scale;
    const int x0 = 24;
    const int y0 = 24 + lineH * 8;
    const uint16_t shadow = rgb565(0, 0, 0);
    const uint16_t green = rgb565(0, 255, 0);
    const uint16_t yellow = rgb565(255, 220, 0);
    const uint16_t red = rgb565(255, 48, 48);
    const uint16_t white = rgb565(255, 255, 255);

    char line[96];
    snprintf(line, sizeof(line), "%s %s FPS %.1F P %d D %d",
             npu_status_text(result),
             npuTaskShortName(result.task),
             result.fps,
             static_cast<int>(result.plates.size()),
             static_cast<int>(result.pedestrians.size()));
    draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x0, y0, line, scale,
                            result.ready ? green : yellow, shadow);

    if (!result.error.empty()) {
        std::string err = osd_ascii_label(result.error, "ERR");
        draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x0, y0 + lineH, err.c_str(),
                                scale, red, shadow);
    }

    if (!result.enabled || !result.ready) {
        return;
    }

    for (const NpuBox& box : result.plates) {
        const int x = scale_capture_x(box.x, fbW);
        const int y = scale_capture_y(box.y, fbH);
        const int w = std::max(2, scale_capture_x(box.w, fbW));
        const int h = std::max(2, scale_capture_y(box.h, fbH));
        draw_rect_outline_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, h, 3, yellow);

        std::string label = "PLATE " + osd_ascii_label(box.label.empty() ? box.text : box.label, "PLATE");
        const int ty = std::max(2, y - lineH - 2);
        draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, ty, label.c_str(), scale, yellow, shadow);
    }

    for (const NpuBox& box : result.pedestrians) {
        const int x = scale_capture_x(box.x, fbW);
        const int y = scale_capture_y(box.y, fbH);
        const int w = std::max(2, scale_capture_x(box.w, fbW));
        const int h = std::max(2, scale_capture_y(box.h, fbH));
        draw_rect_outline_rgb565(fb, pitchBytes, fbW, fbH, x, y, w, h, 3, red);

        char label[48];
        snprintf(label, sizeof(label), "PED %.2F", box.score);
        const int ty = std::max(2, y - lineH - 2);
        draw_text_shadow_rgb565(fb, pitchBytes, fbW, fbH, x, ty, label, scale, white, shadow);
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
}

DrmRgaDisplay::~DrmRgaDisplay()
{
    stopDisplay();
}

bool DrmRgaDisplay::startDisplay()
{
    if (m_running.load()) {
        emit stateChanged("RGA/DRM already running");
        return true;
    }
    m_stopReq.store(false);
    m_running.store(true);
    if (m_npuWorker) {
        m_npuWorker->start();
    }
    m_thread = std::thread(&DrmRgaDisplay::workerMain, this);
    emit stateChanged("RGA/DRM starting");
    return true;
}

void DrmRgaDisplay::stopDisplay()
{
    if (!m_running.load()) {
        if (m_npuWorker) {
            m_npuWorker->stop();
        }
        return;
    }
    m_stopReq.store(true);
    if (m_thread.joinable()) m_thread.join();
    if (m_npuWorker) {
        m_npuWorker->stop();
    }
    m_running.store(false);
    emit stateChanged("RGA/DRM stopped");
}

bool DrmRgaDisplay::isRunning() const { return m_running.load(); }
void DrmRgaDisplay::setOsdEnabled(bool enable) { m_osdEnabled.store(enable); }
bool DrmRgaDisplay::osdEnabled() const { return m_osdEnabled.load(); }
void DrmRgaDisplay::setDisplayMode(int mode) { m_displayMode.store(mode); }
int DrmRgaDisplay::displayMode() const { return m_displayMode.load(); }

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
    bool drmInited = false;

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

    emit stateChanged("Initializing DRM");
    if (!drm_init(drm, drmPath, reqW, reqH)) {
        setLastError("DRM init failed");
        goto cleanup;
    }
    drmInited = true;

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
        OsdStats osdStats {};

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
            if (m_npuWorker) {
                m_npuWorker->submitFrame(validCnt,
                                         frame.data,
                                         frame.bytes,
                                         PCIE_FRAME_W,
                                         PCIE_FRAME_H);
            }

            int back = front ^ 1;
            uint64_t r0 = now_ns();
            int r = rga_scale_rgb565(frame.data, drm.fb[back].map,
                                     drm.modeW, drm.modeH,
                                     drm.fb[back].pitch / PCIE_BPP, drm.modeH);
            uint64_t r1 = now_ns();
            if (r) {
                failCnt++;
                failTotal++;
                if (failCnt <= 10) qDebug() << "RGA to DRM fb failed ret=" << r << "index=" << index;
                continue;
            }
            rgaCnt++;
            rgaNsSum += (r1 - r0);

            if (m_osdEnabled.load()) {
                uint64_t sensorNow = now_ns();
                if (sensorNow - lastSensorNs >= 1000000000ULL || lastSensorNs == 0) {
                    osdStats.npuLoad = read_npu_load_percent();
                    osdStats.tempC = read_soc_temp_c();
                    lastSensorNs = sensorNow;
                }
                draw_osd(drm.fb[back].map, drm.fb[back].pitch, drm.modeW, drm.modeH, osdStats);
                draw_npu_overlay(drm.fb[back].map, drm.fb[back].pitch, drm.modeW, drm.modeH,
                                 latestNpuResult());

                if (m_displayMode.load() == 1) {
                    const uint16_t green = rgb565(0, 255, 0);
                    const uint16_t yellow = rgb565(255, 220, 0);
                    draw_rect_outline_rgb565(drm.fb[back].map, drm.fb[back].pitch, drm.modeW, drm.modeH,
                                              60, 60, 420, 220, 4, green);
                    draw_rect_outline_rgb565(drm.fb[back].map, drm.fb[back].pitch, drm.modeW, drm.modeH,
                                              1100, 300, 520, 300, 4, yellow);
                }
            }

            bool usedSetCrtc = false;
            if (drm_present_fb_blocking(drm, drm.fb[back].fbId, &usedSetCrtc, m_stopReq)) {
                Q_UNUSED(usedSetCrtc);
                front = back;
                flipCnt++;
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
                osdStats.failTotal = failTotal;

                emit statsUpdated(readFps, validFps, rgaFps, flipFps, rgaAvgUs,
                                  int(failCnt), osdStats.npuLoad, osdStats.tempC);
                {
                    const NpuFrameResult ai = latestNpuResult();
                    QString aiStatus;
                    if (!ai.enabled) {
                        aiStatus = "off";
                    } else if (!ai.error.empty()) {
                        aiStatus = QString::fromStdString(ai.error);
                    } else if (ai.ready) {
                        aiStatus = QString("%1 ok %2ms")
                                .arg(QString::fromLatin1(npuTaskKey(ai.task)))
                                .arg(ai.lastMs, 0, 'f', 1);
                    } else {
                        aiStatus = "loading";
                    }
                    emit aiStatsUpdated(ai.enabled,
                                        ai.ready,
                                        ai.fps,
                                        static_cast<int>(ai.plates.size()),
                                        static_cast<int>(ai.pedestrians.size()),
                                        aiStatus);
                }

                readCnt = validCnt = rgaCnt = flipCnt = failCnt = rgaNsSum = 0;
                lastNs = now;
            }
        }
    }

cleanup:
    emit stateChanged("RGA/DRM cleanup");
    pcie.close();
    if (drmInited) drm_cleanup(drm);
    if (m_npuWorker) m_npuWorker->stop();

    m_running.store(false);
    emit stateChanged("RGA/DRM exited");
}
