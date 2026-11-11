#include "npu/npu_inference_worker.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <rknn_api.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
std::string modelPath()
{
    if (const char *d = std::getenv("UAV_NPU_MODEL_DIR"))
        return std::string(d) + "/drone.rknn";
    char path[4096];
    const auto n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0)
        return "models/drone.rknn";
    path[n] = 0;
    std::string p(path);
    return p.substr(0, p.rfind('/')) + "/../models/drone.rknn";
}
float iou(const NpuBox &a, const NpuBox &b)
{
    float w = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    float h = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    float inter = w * h;
    return inter / std::max(1e-8f, a.w * a.h + b.w * b.h - inter);
}
} // namespace
struct NpuInferenceWorker::Impl
{
    struct Frame
    {
        std::vector<std::uint8_t> bytes;
        int width = 0, height = 0, channel = 0;
        std::uint64_t id = 0, sourceRevision = 0, configRevision = 0;
        double copyMs = 0;
    };
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable cv;
    std::thread thread;
    bool running = false, quitting = false, pendingValid = false;
    NpuTaskConfig cfg;
    int channel = 0, processed = 0, dropped = 0;
    std::uint64_t sourceRevision = 1;
    Clock::time_point nextSubmit{}, nextModelAttempt{};
    Frame pending;
    NpuFrameResult latest;
    std::deque<NpuFrameResult> history;
    rknn_context ctx = 0;
    std::string modelError;
    Clock::time_point lastPublish{};

    ~Impl()
    {
        stop();
    }
    void clearState()
    {
        latest = NpuFrameResult();
        latest.enabled = cfg.enabled;
        latest.configRevision = cfg.revision;
        latest.sourceRevision = sourceRevision;
        latest.sourceChannel = channel;
        latest.targetFps = cfg.targetFps;
        latest.confidence = cfg.confidence;
        latest.nmsThreshold = cfg.nmsThreshold;
        latest.status = cfg.enabled ? "waiting for video" : "off";
        pendingValid = false;
        history.clear();
        nextSubmit = {};
        lastPublish = {};
    }
    void configure(NpuTaskConfig c)
    {
        std::lock_guard<std::mutex> lock(mutex);
        c = sanitizeNpuTaskConfig(c);
        c.revision = cfg.revision + 1;
        cfg = c;
        clearState();
        cv.notify_all();
    }
    bool wantsLocked() const
    {
        return running && !quitting && cfg.enabled && Clock::now() >= nextSubmit;
    }
    void start()
    {
        std::lock_guard<std::mutex> life(lifecycle);
        std::lock_guard<std::mutex> lock(mutex);
        if (running)
            return;
        quitting = false;
        running = true;
        clearState();
        try
        {
            thread = std::thread([this] { loop(); });
        }
        catch (...)
        {
            running = false;
            throw;
        }
    }
    void stop()
    {
        std::lock_guard<std::mutex> life(lifecycle);
        {
            std::lock_guard<std::mutex> lock(mutex);
            quitting = true;
            cv.notify_all();
        }
        if (thread.joinable())
            thread.join();
        {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
            pendingValid = false;
        }
    }
    bool ensureModel()
    {
        if (ctx)
            return true;
        if (Clock::now() < nextModelAttempt)
            return false;
        nextModelAttempt = Clock::now() + std::chrono::seconds(3);
        try
        {
            const std::string path = modelPath();
            std::ifstream f(path, std::ios::binary);
            if (!f)
                throw std::runtime_error("missing drone.rknn: " + path);
            std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
            if (bytes.empty() || bytes.size() > 128u * 1024u * 1024u)
                throw std::runtime_error("invalid drone model size");
            if (rknn_init(&ctx, bytes.data(), static_cast<std::uint32_t>(bytes.size()), 0,
                          nullptr) != RKNN_SUCC)
                throw std::runtime_error(
                    "rknn_init failed: check model/runtime/driver compatibility");
            rknn_sdk_version ver{};
            if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC)
                std::cerr << "[DRONE] runtime=" << ver.api_version << " driver=" << ver.drv_version
                          << std::endl;
            rknn_input_output_num num{};
            if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) != RKNN_SUCC ||
                num.n_input != 1 || num.n_output != 1)
                throw std::runtime_error("model contract: require 1 input and 1 output");
            rknn_tensor_attr in{}, out{};
            in.index = 0;
            out.index = 0;
            if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in)) != RKNN_SUCC ||
                rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out, sizeof(out)) != RKNN_SUCC)
                throw std::runtime_error("tensor query failed");
            const bool inputOk = in.n_dims == 4 && in.dims[0] == 1 &&
                                 ((in.fmt == RKNN_TENSOR_NHWC && in.dims[1] == 640 &&
                                   in.dims[2] == 640 && in.dims[3] == 3) ||
                                  (in.fmt == RKNN_TENSOR_NCHW && in.dims[1] == 3 &&
                                   in.dims[2] == 640 && in.dims[3] == 640));
            if (!inputOk || out.n_dims != 3 || out.dims[0] != 1 || out.dims[1] != 5 ||
                out.dims[2] != 8400)
                throw std::runtime_error(
                    "model contract: require RGB 640x640, decoded [1,5,8400] single-class YOLOv8");
            nextModelAttempt = {};
            modelError.clear();
            std::cerr << "[DRONE] loaded " << path << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            modelError = e.what();
            std::cerr << "[DRONE] " << modelError << std::endl;
            if (ctx)
            {
                rknn_destroy(ctx);
                ctx = 0;
            }
            return false;
        }
    }
    NpuFrameResult infer(const Frame &f, const NpuTaskConfig &c)
    {
        NpuFrameResult r;
        r.frameId = f.id;
        r.configRevision = c.revision;
        r.sourceRevision = f.sourceRevision;
        r.sourceChannel = f.channel;
        r.enabled = true;
        r.targetFps = c.targetFps;
        r.confidence = c.confidence;
        r.nmsThreshold = c.nmsThreshold;
        r.copyMs = f.copyMs;
        const auto begin = Clock::now();
        if (!ensureModel())
        {
            r.error = modelError;
            r.status = "model error";
            return r;
        }
        r.modelsReady = true;
        try
        {
            const auto pre = Clock::now();
            // Explicit RGB565 decoding matches the capture contract; avoid BSP-dependent channel
            // conversion.
            cv::Mat rgb(f.height, f.width, CV_8UC3);
            for (int y = 0; y < f.height; ++y)
            {
                auto *d = rgb.ptr<std::uint8_t>(y);
                const auto *s = f.bytes.data() + y * f.width * 2;
                for (int x = 0; x < f.width; ++x)
                {
                    const unsigned v = s[x * 2] | (unsigned(s[x * 2 + 1]) << 8);
                    d[x * 3] = ((v >> 11) & 31) * 255 / 31;
                    d[x * 3 + 1] = ((v >> 5) & 63) * 255 / 63;
                    d[x * 3 + 2] = (v & 31) * 255 / 31;
                }
            }
            const double scale = std::min(640.0 / f.width, 640.0 / f.height);
            const int rw = std::max(1, int(std::round(f.width * scale))),
                      rh = std::max(1, int(std::round(f.height * scale)));
            const int px = (640 - rw) / 2, py = (640 - rh) / 2;
            cv::Mat input(640, 640, CV_8UC3, cv::Scalar(114, 114, 114)), resized;
            cv::resize(rgb, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
            resized.copyTo(input(cv::Rect(px, py, rw, rh)));
            r.preMs = elapsed(pre);
            rknn_input ri{};
            ri.index = 0;
            ri.buf = input.data;
            ri.size = 640 * 640 * 3;
            ri.type = RKNN_TENSOR_UINT8;
            ri.fmt = RKNN_TENSOR_NHWC;
            ri.pass_through = 0;
            if (rknn_inputs_set(ctx, 1, &ri) != RKNN_SUCC)
                throw std::runtime_error("rknn_inputs_set failed");
            const auto run = Clock::now();
            if (rknn_run(ctx, nullptr) != RKNN_SUCC)
                throw std::runtime_error("rknn_run failed");
            r.rknnRunMs = elapsed(run);
            rknn_output output{};
            output.index = 0;
            output.want_float = 1;
            output.is_prealloc = 0;
            if (rknn_outputs_get(ctx, 1, &output, nullptr) != RKNN_SUCC)
                throw std::runtime_error("rknn_outputs_get failed");
            struct Release
            {
                rknn_context ctx;
                rknn_output *o;
                ~Release()
                {
                    rknn_outputs_release(ctx, 1, o);
                }
            } release{ctx, &output};
            if (!output.buf || output.size < 5u * 8400u * sizeof(float))
                throw std::runtime_error("short model output");
            const auto post = Clock::now();
            const auto *o = static_cast<const float *>(output.buf);
            std::vector<NpuBox> candidates;
            for (int i = 0; i < 8400; ++i)
            {
                const float score = o[4 * 8400 + i];
                if (!std::isfinite(score) || score < c.confidence || score > 1.001f)
                    continue;
                float cx = o[i], cy = o[8400 + i], w = o[2 * 8400 + i], h = o[3 * 8400 + i];
                if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) ||
                    !std::isfinite(h) || w <= 0 || h <= 0)
                    continue;
                const float x1 =
                    std::clamp(float((cx - w / 2 - px) * f.width / rw), 0.f, float(f.width));
                const float y1 =
                    std::clamp(float((cy - h / 2 - py) * f.height / rh), 0.f, float(f.height));
                const float x2 =
                    std::clamp(float((cx + w / 2 - px) * f.width / rw), 0.f, float(f.width));
                const float y2 =
                    std::clamp(float((cy + h / 2 - py) * f.height / rh), 0.f, float(f.height));
                if (x2 <= x1 || y2 <= y1)
                    continue;
                NpuBox b;
                b.x = x1 * 640 / f.width;
                b.y = y1 * 360 / f.height;
                b.w = (x2 - x1) * 640 / f.width;
                b.h = (y2 - y1) * 360 / f.height;
                b.score = score;
                candidates.push_back(b);
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const NpuBox &a, const NpuBox &b) { return a.score > b.score; });
            if (candidates.size() > 1000)
                candidates.resize(1000);
            for (const auto &b : candidates)
            {
                bool keep = true;
                for (const auto &k : r.drones)
                    if (iou(b, k) > c.nmsThreshold)
                    {
                        keep = false;
                        break;
                    }
                if (keep)
                    r.drones.push_back(b);
                if (r.drones.size() >= 100)
                    break;
            }
            r.postMs = elapsed(post);
            r.ready = true;
            r.status = "ok";
        }
        catch (const std::exception &e)
        {
            r.error = e.what();
            r.ready = false;
            r.drones.clear();
        }
        r.lastMs = elapsed(begin);
        return r;
    }
    void loop()
    {
        for (;;)
        {
            Frame f;
            NpuTaskConfig c;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return quitting || pendingValid; });
                if (quitting)
                    break;
                f = std::move(pending);
                pendingValid = false;
                c = cfg;
            }
            if (!c.enabled || c.revision != f.configRevision)
                continue;
            NpuFrameResult r;
            try
            {
                r = infer(f, c);
            }
            catch (const std::exception &e)
            {
                r.frameId = f.id;
                r.enabled = true;
                r.configRevision = c.revision;
                r.sourceRevision = f.sourceRevision;
                r.sourceChannel = f.channel;
                r.error = e.what();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (quitting || cfg.revision != c.revision || channel != f.channel ||
                    sourceRevision != f.sourceRevision)
                    continue;
                const auto now = Clock::now();
                if (r.ready)
                {
                    ++processed;
                    if (lastPublish.time_since_epoch().count())
                        r.fps = 1.0 / std::chrono::duration<double>(now - lastPublish).count();
                    lastPublish = now;
                }
                r.processed = processed;
                r.dropped = dropped;
                latest = r;
                history.push_back(std::move(r));
                if (history.size() > 64)
                    history.pop_front();
            }
        }
        if (ctx)
        {
            rknn_destroy(ctx);
            ctx = 0;
        }
    }
};
NpuInferenceWorker::NpuInferenceWorker() : m_impl(new Impl) {}
NpuInferenceWorker::~NpuInferenceWorker() = default;
void NpuInferenceWorker::start()
{
    m_impl->start();
}
void NpuInferenceWorker::stop()
{
    m_impl->stop();
}
void NpuInferenceWorker::setConfig(const NpuTaskConfig &c)
{
    m_impl->configure(c);
}
NpuTaskConfig NpuInferenceWorker::config() const
{
    std::lock_guard<std::mutex> l(m_impl->mutex);
    return m_impl->cfg;
}
void NpuInferenceWorker::setEnabled(bool b)
{
    auto c = config();
    c.enabled = b;
    setConfig(c);
}
bool NpuInferenceWorker::enabled() const
{
    return config().enabled;
}
void NpuInferenceWorker::setTask(NpuTaskType t)
{
    auto c = config();
    c.task = t;
    setConfig(c);
}
NpuTaskType NpuInferenceWorker::task() const
{
    return config().task;
}
void NpuInferenceWorker::setTargetFps(int v)
{
    auto c = config();
    c.targetFps = v;
    setConfig(c);
}
int NpuInferenceWorker::targetFps() const
{
    return config().targetFps;
}
void NpuInferenceWorker::setConfidence(float v)
{
    auto c = config();
    c.confidence = v;
    setConfig(c);
}
float NpuInferenceWorker::confidence() const
{
    return config().confidence;
}
void NpuInferenceWorker::setNmsThreshold(float v)
{
    auto c = config();
    c.nmsThreshold = v;
    setConfig(c);
}
float NpuInferenceWorker::nmsThreshold() const
{
    return config().nmsThreshold;
}
void NpuInferenceWorker::setInputSource(int c, std::uint64_t rev)
{
    std::lock_guard<std::mutex> l(m_impl->mutex);
    if (c != m_impl->channel || rev != m_impl->sourceRevision)
    {
        m_impl->channel = c;
        m_impl->sourceRevision = rev;
        m_impl->clearState();
    }
}
bool NpuInferenceWorker::wantsFrame() const
{
    std::lock_guard<std::mutex> l(m_impl->mutex);
    return m_impl->wantsLocked();
}
void NpuInferenceWorker::submitFrame(std::uint64_t id, const std::uint8_t *data, std::size_t bytes,
                                     int w, int h, int c, std::uint64_t rev,
                                     const NpuOriginalFrameView *v)
{
    const auto begin = Clock::now();
    std::lock_guard<std::mutex> l(m_impl->mutex);
    if (!m_impl->wantsLocked() || c != m_impl->channel || rev != m_impl->sourceRevision)
        return;
    int x = 0, y = 0, rw = w, rh = h, stride = w;
    if (v)
    {
        data = v->data;
        bytes = v->bytes;
        w = v->width;
        h = v->height;
        stride = v->stridePixels;
        x = v->roiX;
        y = v->roiY;
        rw = v->roiWidth;
        rh = v->roiHeight;
    }
    if (!data || w <= 0 || h <= 0 || w > 4096 || h > 4096 || stride < w || stride > 8192 || x < 0 ||
        y < 0 || rw <= 0 || rh <= 0 || x + rw > w || y + rh > h ||
        bytes < std::size_t(stride) * h * 2)
        return;
    Impl::Frame f;
    f.width = rw;
    f.height = rh;
    f.id = id;
    f.channel = c;
    f.sourceRevision = rev;
    f.configRevision = m_impl->cfg.revision;
    f.bytes.resize(std::size_t(rw) * rh * 2);
    for (int row = 0; row < rh; ++row)
        std::memcpy(f.bytes.data() + std::size_t(row) * rw * 2,
                    data + (std::size_t(y + row) * stride + x) * 2, std::size_t(rw) * 2);
    f.copyMs = elapsed(begin);
    if (m_impl->pendingValid)
        ++m_impl->dropped;
    m_impl->pending = std::move(f);
    m_impl->pendingValid = true;
    m_impl->nextSubmit = Clock::now() + std::chrono::microseconds(1000000 / m_impl->cfg.targetFps);
    m_impl->cv.notify_one();
}
NpuFrameResult NpuInferenceWorker::latestResult() const
{
    std::lock_guard<std::mutex> l(m_impl->mutex);
    return m_impl->latest;
}
std::vector<NpuFrameResult> NpuInferenceWorker::resultsAfter(std::uint64_t id, std::size_t n) const
{
    std::lock_guard<std::mutex> l(m_impl->mutex);
    std::vector<NpuFrameResult> v;
    for (const auto &r : m_impl->history)
        if (r.frameId > id && v.size() < n)
            v.push_back(r);
    return v;
}
