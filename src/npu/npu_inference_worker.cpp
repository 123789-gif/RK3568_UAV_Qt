#include "npu/npu_inference_worker.h"

#include "npu/vision.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char* kPlateDetModel = "plate_det.rknn";
constexpr const char* kPlateRecoModel = "plate_reco.rknn";
constexpr const char* kPedModel = "ped_det.rknn";

struct FrameCopy
{
    uint64_t id = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
};

static int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static double elapsedMs(std::chrono::steady_clock::time_point begin)
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now() - begin).count();
}

static bool fileExists(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }
    std::fclose(fp);
    return true;
}

static std::string joinPath(const std::string& dir, const char* name)
{
    if (dir.empty()) {
        return name;
    }
    if (dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

static std::string executableDir()
{
    char path[PATH_MAX] = {};
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) {
        return {};
    }
    path[length] = '\0';
    std::string fullPath(path);
    const std::string::size_type slash = fullPath.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return fullPath.substr(0, slash);
}

static std::string findModelPath(const char* name)
{
    std::vector<std::string> dirs;
    if (const char* envDir = std::getenv("PTZ_NPU_MODEL_DIR")) {
        dirs.emplace_back(envDir);
    }

    const std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        // 正式部署结构：<app>/bin/rk_dual_screen_video + <app>/models/*.rknn
        dirs.emplace_back(exeDir + "/../models");
        // Qt Creator/手工构建结构：<build>/rk_dual_screen_video + <build>/models/*.rknn
        dirs.emplace_back(exeDir + "/models");
    }

    // 源码目录内直接运行时的兼容路径。
    dirs.emplace_back("./assets/models");
    dirs.emplace_back("./models");
    dirs.emplace_back("./model");
    dirs.emplace_back("../model");

    // 保留旧工程路径作为最后回退，确保本次目录重构不破坏现有板端行为。
    dirs.emplace_back("/userdata/Projects/icore_npu_rk/model");
    dirs.emplace_back("/userdata/Projects/icore_npu_src/model");

    for (const std::string& dir : dirs) {
        std::string path = joinPath(dir, name);
        if (fileExists(path)) {
            return path;
        }
    }
    return joinPath("./model", name);
}

static bool readFile(const std::string& path, std::vector<unsigned char>* out, std::string* error)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        if (error) {
            *error = "open model failed: " + path;
        }
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        if (error) {
            *error = "empty model: " + path;
        }
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    const std::size_t n = std::fread(out->data(), 1, out->size(), fp);
    std::fclose(fp);
    if (n != out->size()) {
        if (error) {
            *error = "read model failed: " + path;
        }
        return false;
    }
    return true;
}

static void releaseModel(demo::RknnModel& model)
{
    if (model.ctx) {
        rknn_destroy(model.ctx);
        model.ctx = 0;
    }
    delete[] model.in_attrs;
    delete[] model.out_attrs;
    model.in_attrs = nullptr;
    model.out_attrs = nullptr;
    model.num_in = 0;
    model.num_out = 0;
}

static bool loadModel(const std::string& path, demo::RknnModel* model, std::string* error)
{
    std::vector<unsigned char> buffer;
    if (!readFile(path, &buffer, error)) {
        return false;
    }

    releaseModel(*model);

    int ret = rknn_init(&model->ctx, buffer.data(), static_cast<uint32_t>(buffer.size()), 0, nullptr);
    if (ret != RKNN_SUCC) {
        if (error) {
            *error = "rknn_init failed: " + path + " ret=" + std::to_string(ret);
        }
        releaseModel(*model);
        return false;
    }

    rknn_input_output_num ioNum {};
    ret = rknn_query(model->ctx, RKNN_QUERY_IN_OUT_NUM, &ioNum, sizeof(ioNum));
    if (ret != RKNN_SUCC) {
        if (error) {
            *error = "query io num failed: " + path + " ret=" + std::to_string(ret);
        }
        releaseModel(*model);
        return false;
    }

    model->num_in = static_cast<int>(ioNum.n_input);
    model->num_out = static_cast<int>(ioNum.n_output);
    model->in_attrs = new rknn_tensor_attr[model->num_in]();
    model->out_attrs = new rknn_tensor_attr[model->num_out]();

    for (int i = 0; i < model->num_in; ++i) {
        model->in_attrs[i].index = static_cast<uint32_t>(i);
        ret = rknn_query(model->ctx, RKNN_QUERY_INPUT_ATTR, &model->in_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            if (error) {
                *error = "query input attr failed: " + path;
            }
            releaseModel(*model);
            return false;
        }
    }
    for (int i = 0; i < model->num_out; ++i) {
        model->out_attrs[i].index = static_cast<uint32_t>(i);
        ret = rknn_query(model->ctx, RKNN_QUERY_OUTPUT_ATTR, &model->out_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            if (error) {
                *error = "query output attr failed: " + path;
            }
            releaseModel(*model);
            return false;
        }
    }

    return true;
}

static NpuBox makeBox(const demo::Rect2f& r, float score, int classId,
                      const std::string& text = std::string(),
                      const std::string& label = std::string())
{
    NpuBox box;
    box.x = r.x;
    box.y = r.y;
    box.w = r.width;
    box.h = r.height;
    box.score = score;
    box.classId = classId;
    box.text = text;
    box.label = label;
    return box;
}

}  // namespace

struct NpuInferenceWorker::Impl
{
    std::atomic<bool> running {false};
    std::atomic<bool> stopReq {false};
    std::atomic<int64_t> lastSubmitMs {0};
    std::atomic<int> droppedFrames {0};
    std::atomic<int> processed {0};
    std::atomic<bool> modelsLoaded {false};

    std::thread thread;

    mutable std::mutex configMutex;
    NpuTaskConfig requestedConfig;

    mutable std::mutex pendingMutex;
    std::condition_variable pendingCv;
    bool pendingValid = false;
    FrameCopy pending;

    mutable std::mutex resultMutex;
    NpuFrameResult latest;
    std::deque<NpuFrameResult> resultHistory;

    demo::RknnModel plateDet;
    demo::RknnModel plateReco;
    demo::RknnModel ped;
    std::atomic<int> loadedTaskValue {-1};
    std::string modelError;

    int64_t fpsWindowMs = 0;
    int fpsWindowCount = 0;
    double currentFps = 0.0;

    Impl()
    {
        requestedConfig = sanitizeNpuTaskConfig(requestedConfig);
    }

    ~Impl()
    {
        stop();
        releaseAllModels();
    }

    void releaseAllModels()
    {
        releaseModel(plateDet);
        releaseModel(plateReco);
        releaseModel(ped);
        loadedTaskValue.store(-1);
        modelsLoaded.store(false);
    }

    NpuTaskConfig configSnapshot() const
    {
        std::lock_guard<std::mutex> lk(configMutex);
        return requestedConfig;
    }

    void updateConfig(const NpuTaskConfig& requested)
    {
        NpuTaskConfig next = sanitizeNpuTaskConfig(requested);
        {
            std::lock_guard<std::mutex> lk(configMutex);
            next.revision = requestedConfig.revision + 1;
            requestedConfig = next;
        }

        lastSubmitMs.store(0);

        if (!next.enabled) {
            std::lock_guard<std::mutex> lk(pendingMutex);
            pending = FrameCopy();
            pendingValid = false;
        }

        NpuFrameResult state;
        state.enabled = next.enabled;
        state.task = next.task;
        state.targetFps = next.targetFps;
        state.confidence = next.confidence;
        state.nmsThreshold = next.nmsThreshold;
        state.configRevision = next.revision;
        state.modelsReady = modelsLoaded.load() &&
                loadedTaskValue.load() == static_cast<int>(next.task);
        state.status = next.enabled ? "pending" : "off";
        state.processed = processed.load();
        state.dropped = droppedFrames.load();
        {
            std::lock_guard<std::mutex> lk(resultMutex);
            latest = std::move(state);
            resultHistory.clear();
        }
    }

    void start()
    {
        if (running.exchange(true)) {
            return;
        }
        stopReq.store(false);
        thread = std::thread(&Impl::loop, this);
    }

    void stop()
    {
        if (!running.load()) {
            return;
        }
        stopReq.store(true);
        pendingCv.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false);
    }

    bool ensureModels(NpuTaskType task)
    {
        if (modelsLoaded.load() &&
            loadedTaskValue.load() == static_cast<int>(task)) {
            return true;
        }

        // 模型只由推理线程装载和释放。切换任务时释放旧任务模型，
        // 避免行人和车牌三级模型长期同时占用NPU运行时与内存。
        releaseAllModels();

        std::string error;
        bool ok = false;
        if (task == NpuTaskType::PlateCascade) {
            const std::string plateDetPath = findModelPath(kPlateDetModel);
            const std::string plateRecoPath = findModelPath(kPlateRecoModel);
            ok = loadModel(plateDetPath, &plateDet, &error) &&
                 loadModel(plateRecoPath, &plateReco, &error);
        } else {
            const std::string pedPath = findModelPath(kPedModel);
            ok = loadModel(pedPath, &ped, &error);
        }

        if (!ok) {
            modelError = error;
            releaseAllModels();
            return false;
        }

        loadedTaskValue.store(static_cast<int>(task));
        modelsLoaded.store(true);
        fpsWindowMs = 0;
        fpsWindowCount = 0;
        currentFps = 0.0;
        modelError.clear();
        return true;
    }

    void submit(uint64_t frameId, const uint8_t* rgb565, std::size_t bytes, int width, int height)
    {
        const NpuTaskConfig cfg = configSnapshot();
        if (!cfg.enabled || !running.load() || !rgb565 || width <= 0 || height <= 0) {
            return;
        }

        const int64_t now = steadyMs();
        const int64_t last = lastSubmitMs.load();
        const int64_t minInterval = std::max<int64_t>(1, 1000 / cfg.targetFps);
        if (now - last < minInterval) {
            return;
        }
        lastSubmitMs.store(now);

        const std::size_t need = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 2u;
        if (bytes < need) {
            return;
        }

        FrameCopy copy;
        copy.id = frameId;
        copy.width = width;
        copy.height = height;
        copy.data.assign(rgb565, rgb565 + need);

        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            if (pendingValid) {
                droppedFrames.fetch_add(1);
            }
            pending = std::move(copy);
            pendingValid = true;
        }
        pendingCv.notify_one();
    }

    void loop()
    {
        while (!stopReq.load()) {
            FrameCopy frame;
            {
                std::unique_lock<std::mutex> lk(pendingMutex);
                pendingCv.wait(lk, [&] { return stopReq.load() || pendingValid; });
                if (stopReq.load()) {
                    break;
                }
                frame = std::move(pending);
                pending = FrameCopy();
                pendingValid = false;
            }
            process(frame);
        }
    }

    void publish(NpuFrameResult result, const NpuTaskConfig& cfg)
    {
        const NpuTaskConfig current = configSnapshot();
        if (current.revision != cfg.revision) {
            return;
        }
        result.enabled = current.enabled;
        result.task = cfg.task;
        result.targetFps = cfg.targetFps;
        result.confidence = cfg.confidence;
        result.nmsThreshold = cfg.nmsThreshold;
        result.configRevision = cfg.revision;
        result.modelsReady = modelsLoaded.load() &&
                loadedTaskValue.load() == static_cast<int>(cfg.task);
        result.processed = processed.load();
        result.dropped = droppedFrames.load();

        if (!current.enabled) {
            result.ready = false;
            result.fps = 0.0;
            result.status = "off";
            result.error.clear();
            result.plates.clear();
            result.pedestrians.clear();
        }

        std::lock_guard<std::mutex> lk(resultMutex);
        latest = result;
        if (result.frameId != 0) {
            resultHistory.push_back(std::move(result));
            while (resultHistory.size() > 64) {
                resultHistory.pop_front();
            }
        }
    }

    void process(const FrameCopy& frame)
    {
        const NpuTaskConfig cfg = configSnapshot();
        if (!cfg.enabled) {
            return;
        }

        NpuFrameResult result;
        result.frameId = frame.id;
        result.task = cfg.task;
        result.targetFps = cfg.targetFps;
        result.confidence = cfg.confidence;
        result.nmsThreshold = cfg.nmsThreshold;
        result.configRevision = cfg.revision;

        if (!ensureModels(cfg.task)) {
            result.ready = false;
            result.error = modelError;
            result.status = "model error";
            publish(std::move(result), cfg);
            return;
        }

        const auto totalBegin = std::chrono::steady_clock::now();
        const auto preBegin = std::chrono::steady_clock::now();
        bool usedRga = false;
        demo::Mat input = demo::vision::pre_img_rgb565_to_rgb(frame.data.data(),
                                                              frame.width,
                                                              frame.height,
                                                              &usedRga,
                                                              false);
        result.preMs = elapsedMs(preBegin);
        result.usedRga = usedRga;
        if (input.empty()) {
            result.ready = false;
            result.error = "preprocess failed";
            result.status = "preprocess failed";
            publish(std::move(result), cfg);
            return;
        }

        if (cfg.task == NpuTaskType::PlateCascade) {
            std::vector<demo::Mat> crops;
            std::vector<demo::Rect2f> plateBoxesIn;
            std::vector<int> plateColorIds;
            demo::vision::RknnStageTiming plateTiming;
            demo::vision::detect_plate_candidates(input,
                                                  plateDet,
                                                  cfg.confidence,
                                                  cfg.nmsThreshold,
                                                  crops,
                                                  plateBoxesIn,
                                                  plateColorIds,
                                                  true,
                                                  &result.plateMs,
                                                  &plateTiming);

            std::vector<std::string> plateTexts;
            std::vector<std::string> platePinyins;
            std::vector<demo::Rect2f> plateBoxesOut;
            std::vector<int> plateColorsOut;
            if (!crops.empty()) {
                double ocrMs = 0.0;
                demo::vision::RknnStageTiming ocrTiming;
                demo::vision::recognize_plate_crops(crops,
                                                    plateBoxesIn,
                                                    plateColorIds,
                                                    plateReco,
                                                    plateTexts,
                                                    platePinyins,
                                                    plateBoxesOut,
                                                    plateColorsOut,
                                                    &ocrMs,
                                                    &ocrTiming);
                result.plateMs += ocrMs;
            }

            const std::size_t plateCount = std::min(plateBoxesOut.size(), plateTexts.size());
            for (std::size_t i = 0; i < plateCount; ++i) {
                demo::Rect2f orig;
                demo::vision::convert_input_box_to_orig(plateBoxesOut[i], orig,
                                                        frame.width, frame.height);
                const std::string label = (i < platePinyins.size())
                        ? platePinyins[i] : plateTexts[i];
                const int colorId = (i < plateColorsOut.size()) ? plateColorsOut[i] : 0;
                // 当前plate接口未返回检测分数，使用负值明确表示“不可用”，
                // UI显示为“--”，避免伪造1.0置信度。
                result.plates.push_back(makeBox(orig, -1.0f, colorId, plateTexts[i], label));
            }
        } else {
            std::vector<demo::Rect2f> pedBoxesIn;
            std::vector<float> pedScores;
            demo::vision::RknnStageTiming pedTiming;
            demo::vision::detect_pedestrian(input,
                                            ped,
                                            1,
                                            cfg.confidence,
                                            cfg.nmsThreshold,
                                            pedBoxesIn,
                                            pedScores,
                                            true,
                                            demo::DEFAULT_PED_TOPK_DECODE,
                                            demo::DEFAULT_PED_TOPK_NMS,
                                            &result.pedMs,
                                            &pedTiming);
            for (std::size_t i = 0; i < pedBoxesIn.size(); ++i) {
                demo::Rect2f orig;
                demo::vision::convert_input_box_to_orig(pedBoxesIn[i], orig,
                                                        frame.width, frame.height);
                const float score = (i < pedScores.size()) ? pedScores[i] : 0.0f;
                result.pedestrians.push_back(
                            makeBox(orig, score, 0, std::string(), "person"));
            }
        }

        processed.fetch_add(1);
        ++fpsWindowCount;
        const int64_t now = steadyMs();
        if (fpsWindowMs == 0) {
            fpsWindowMs = now;
        }
        const int64_t dt = now - fpsWindowMs;
        if (dt >= 1000) {
            currentFps = fpsWindowCount * 1000.0 / static_cast<double>(dt);
            fpsWindowMs = now;
            fpsWindowCount = 0;
        }

        result.ready = true;
        result.fps = currentFps;
        result.lastMs = elapsedMs(totalBegin);
        result.status = npuTaskKey(cfg.task);
        publish(std::move(result), cfg);
    }
};

NpuInferenceWorker::NpuInferenceWorker()
    : m_impl(new Impl)
{
}

NpuInferenceWorker::~NpuInferenceWorker() = default;

void NpuInferenceWorker::start()
{
    m_impl->start();
}

void NpuInferenceWorker::stop()
{
    m_impl->stop();
}

void NpuInferenceWorker::setConfig(const NpuTaskConfig& config)
{
    m_impl->updateConfig(config);
}

NpuTaskConfig NpuInferenceWorker::config() const
{
    return m_impl->configSnapshot();
}

void NpuInferenceWorker::setEnabled(bool enabled)
{
    NpuTaskConfig cfg = config();
    cfg.enabled = enabled;
    setConfig(cfg);
}

bool NpuInferenceWorker::enabled() const
{
    return config().enabled;
}

void NpuInferenceWorker::setTask(NpuTaskType task)
{
    NpuTaskConfig cfg = config();
    cfg.task = task;
    setConfig(cfg);
}

NpuTaskType NpuInferenceWorker::task() const
{
    return config().task;
}

void NpuInferenceWorker::setTargetFps(int fps)
{
    NpuTaskConfig cfg = config();
    cfg.targetFps = fps;
    setConfig(cfg);
}

int NpuInferenceWorker::targetFps() const
{
    return config().targetFps;
}

void NpuInferenceWorker::setConfidence(float confidence)
{
    NpuTaskConfig cfg = config();
    cfg.confidence = confidence;
    setConfig(cfg);
}

float NpuInferenceWorker::confidence() const
{
    return config().confidence;
}

void NpuInferenceWorker::setNmsThreshold(float threshold)
{
    NpuTaskConfig cfg = config();
    cfg.nmsThreshold = threshold;
    setConfig(cfg);
}

float NpuInferenceWorker::nmsThreshold() const
{
    return config().nmsThreshold;
}

void NpuInferenceWorker::submitFrame(uint64_t frameId,
                                     const uint8_t* rgb565,
                                     std::size_t bytes,
                                     int width,
                                     int height)
{
    m_impl->submit(frameId, rgb565, bytes, width, height);
}

NpuFrameResult NpuInferenceWorker::latestResult() const
{
    const NpuTaskConfig cfg = m_impl->configSnapshot();
    std::lock_guard<std::mutex> lk(m_impl->resultMutex);
    NpuFrameResult result = m_impl->latest;
    result.enabled = cfg.enabled;
    result.task = cfg.task;
    result.targetFps = cfg.targetFps;
    result.confidence = cfg.confidence;
    result.nmsThreshold = cfg.nmsThreshold;
    result.configRevision = cfg.revision;
    result.modelsReady = m_impl->modelsLoaded.load() &&
                         m_impl->loadedTaskValue.load() == static_cast<int>(cfg.task);
    result.processed = m_impl->processed.load();
    result.dropped = m_impl->droppedFrames.load();
    if (!result.enabled) {
        result.ready = false;
        result.fps = 0.0;
        result.status = "off";
        result.error.clear();
        result.plates.clear();
        result.pedestrians.clear();
    }
    return result;
}


std::vector<NpuFrameResult> NpuInferenceWorker::resultsAfter(uint64_t frameId,
                                                              std::size_t maxResults) const
{
    std::vector<NpuFrameResult> results;
    if (maxResults == 0) {
        return results;
    }

    std::lock_guard<std::mutex> lk(m_impl->resultMutex);
    for (const NpuFrameResult& item : m_impl->resultHistory) {
        if (item.frameId > frameId) {
            results.push_back(item);
            if (results.size() >= maxResults) {
                break;
            }
        }
    }
    return results;
}
