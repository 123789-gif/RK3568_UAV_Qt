#include "npu/npu_inference_worker.h"

#include "npu/plate_type_ids.h"
#include "npu/plate_v291.h"
#include "npu/vision.h"
#include "npu/pedestrian_violation_engine.h"
#include "npu/vehicle_wrong_way_engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <initializer_list>
#include <limits.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char* kPlateDetModel = "plate_det.rknn";
constexpr const char* kPlateRecoModel = "plate_reco.rknn";
constexpr const char* kPedModel = "ped_det.rknn";
constexpr const char* kVehicleModel = "yolov8_car_640x320_mmse.rknn";

enum class PlateBackend
{
    Unknown,
    Legacy,
    V291
};

struct FrameCopy
{
    uint64_t id = 0;
    uint64_t sourceRevision = 0;
    int sourceChannel = 0;
    int width = 0;
    int height = 0;
    double copyMs = 0.0;
    std::vector<uint8_t> data;
    int originalWidth = 0;
    int originalHeight = 0;
    double originalCopyMs = 0.0;
    std::vector<uint8_t> originalData;
    std::string originalCopyError;
};

constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
constexpr int64_t kSubmitToleranceNs = 1000000LL;

static int64_t steadyNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static int64_t systemMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static int64_t targetPeriodNs(int targetFps)
{
    targetFps = std::max(1, targetFps);
    return (kNanosecondsPerSecond + targetFps / 2) / targetFps;
}

static double elapsedMs(std::chrono::steady_clock::time_point begin)
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now() - begin).count();
}


static void accumulateStageTiming(NpuFrameResult& result, const demo::vision::RknnStageTiming& timing)
{
    result.rknnInputMs += timing.input_ms;
    result.rknnRunMs += timing.run_ms;
    result.rknnOutputMs += timing.output_ms;
    result.rknnPostMs += timing.post_ms;
    result.rknnRunCount += timing.run_count;
}

static void accumulateStageTiming(NpuFrameResult& result,
                                  const demo::plate_v291::RknnStageTiming& timing)
{
    result.rknnInputMs += timing.input_ms;
    result.rknnRunMs += timing.run_ms;
    result.rknnOutputMs += timing.output_ms;
    result.rknnPostMs += timing.post_ms;
    result.rknnRunCount += timing.run_count;
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

static std::string findPlateModelPath(NpuPlateModelProfile profile,
                                      const char* name)
{
    std::vector<std::string> roots;
    if (const char* envRoot = std::getenv("PTZ_NPU_MODEL_ROOT")) {
        roots.emplace_back(envRoot);
    }

    const std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        roots.emplace_back(exeDir + "/../models");
        roots.emplace_back(exeDir + "/models");
    }
    roots.emplace_back("./assets/models");
    roots.emplace_back("./models");

    const char* profileDir = profile == NpuPlateModelProfile::V291Candidate
            ? "v291_candidate" : "";
    for (const std::string& root : roots) {
        const std::string dir = *profileDir
                ? joinPath(root, profileDir) : root;
        const std::string path = joinPath(dir, name);
        if (fileExists(path)) {
            return path;
        }
    }

    const std::string fallbackRoot = roots.empty() ? "./models" : roots.front();
    return *profileDir
            ? joinPath(joinPath(fallbackRoot, profileDir), name)
            : joinPath(fallbackRoot, name);
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

static bool tensorHasDim(const rknn_tensor_attr& attr, int expected)
{
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        if (static_cast<int>(attr.dims[i]) == expected) {
            return true;
        }
    }
    return false;
}

static std::string tensorShape(const rknn_tensor_attr& attr)
{
    std::string result = "[";
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        if (i != 0) {
            result += ",";
        }
        result += std::to_string(attr.dims[i]);
    }
    result += "]";
    return result;
}

static bool tensorMatches(const rknn_tensor_attr& attr,
                          std::initializer_list<int> requiredDims,
                          uint32_t expectedElements)
{
    if (expectedElements != 0 && attr.n_elems != expectedElements) {
        return false;
    }
    for (int dim : requiredDims) {
        if (!tensorHasDim(attr, dim)) {
            return false;
        }
    }
    return true;
}

static bool validateVehicleContract(const demo::RknnModel& model,
                                    std::string* error)
{
    if (model.num_in != 1 || model.num_out != 1 ||
        !model.in_attrs || !model.out_attrs) {
        if (error) *error = "vehicle model requires exactly one input and one output";
        return false;
    }
    const rknn_tensor_attr& input = model.in_attrs[0];
    const rknn_tensor_attr& output = model.out_attrs[0];
    const bool valid = tensorMatches(input, {3, 320, 640}, 3u * 320u * 640u) &&
            tensorMatches(output, {68, 4200}, 68u * 4200u);
    if (!valid && error) {
        *error = "unsupported vehicle model contract: input=" + tensorShape(input) +
                " output=" + tensorShape(output) +
                " expected input=[1,3,320,640] output=[1,68,4200]";
    }
    return valid;
}

static bool validatePedestrianContract(const demo::RknnModel& model,
                                       std::string* error)
{
    if (model.num_in != 1 || model.num_out != 1 ||
        !model.in_attrs || !model.out_attrs) {
        if (error) *error = "pedestrian model requires exactly one input and one output";
        return false;
    }
    const rknn_tensor_attr& input = model.in_attrs[0];
    const rknn_tensor_attr& output = model.out_attrs[0];
    const bool valid = tensorMatches(input, {3, 448, 768}, 3u * 448u * 768u) &&
            tensorMatches(output, {65, 7056}, 65u * 7056u);
    if (!valid && error) {
        *error = "unsupported pedestrian model contract: input=" + tensorShape(input) +
                " output=" + tensorShape(output) +
                " expected input=[1,3,448,768] output=[1,65,7056]";
    }
    return valid;
}

static PlateBackend validatePlateContract(const demo::RknnModel& det,
                                          const demo::RknnModel& reco,
                                          std::string* error)
{
    if (det.num_in != 1 || det.num_out != 1 ||
        reco.num_in != 1 || reco.num_out != 1 ||
        !det.in_attrs || !det.out_attrs || !reco.in_attrs || !reco.out_attrs) {
        if (error) {
            *error = "plate model contract requires exactly one input and one output";
        }
        return PlateBackend::Unknown;
    }

    const rknn_tensor_attr& detIn = det.in_attrs[0];
    const rknn_tensor_attr& detOut = det.out_attrs[0];
    const rknn_tensor_attr& recoIn = reco.in_attrs[0];
    const rknn_tensor_attr& recoOut = reco.out_attrs[0];

    const bool commonRecoInput = tensorHasDim(recoIn, 24) && tensorHasDim(recoIn, 94);
    const bool legacy =
            tensorMatches(detIn, {3, 448, 768}, 3u * 448u * 768u) &&
            tensorMatches(detOut, {77, 6720}, 77u * 6720u) &&
            commonRecoInput && tensorHasDim(recoOut, 68) && tensorHasDim(recoOut, 18);
    if (legacy) {
        return PlateBackend::Legacy;
    }

    const bool v291 =
            tensorMatches(detIn, {3, 320, 640}, 3u * 320u * 640u) &&
            tensorMatches(detOut, {73, 4200}, 73u * 4200u) &&
            commonRecoInput && tensorHasDim(recoOut, 73) && tensorHasDim(recoOut, 18);
    if (v291) {
        return PlateBackend::V291;
    }

    if (error) {
        *error = "unsupported plate model contract: det input=" + tensorShape(detIn) +
                " output=" + tensorShape(detOut) +
                " reco input=" + tensorShape(recoIn) +
                " output=" + tensorShape(recoOut);
    }
    return PlateBackend::Unknown;
}

static NpuPlatePostprocess platePostprocessModeFromEnvironment()
{
    const char* value = std::getenv("PTZ_PLATE_POSTPROCESS");
    if (!value) {
        return NpuPlatePostprocess::Stable;
    }
    std::string normalized(value);
    for (char& ch : normalized) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return normalized == "RAW" ? NpuPlatePostprocess::Raw
                                : NpuPlatePostprocess::Stable;
}

static NpuPlateCropSource plateCropSourceFromEnvironment()
{
    const char* value = std::getenv("PTZ_PLATE_CROP_SOURCE");
    if (!value) {
        return NpuPlateCropSource::DetectorInput;
    }
    std::string normalized(value);
    for (char& ch : normalized) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return normalized == "ORIGINAL" || normalized == "ORIGINAL_SOURCE"
            ? NpuPlateCropSource::OriginalSource
            : NpuPlateCropSource::DetectorInput;
}

static NpuPlateModelProfile plateModelProfileFromEnvironment()
{
    const char* value = std::getenv("PTZ_NPU_MODEL_PROFILE");
    if (!value) {
        return NpuPlateModelProfile::Legacy;
    }
    std::string normalized(value);
    for (char& ch : normalized) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return normalized == "LEGACY" ? NpuPlateModelProfile::Legacy
                                  : NpuPlateModelProfile::V291Candidate;
}

static bool v291ForceCpuFromEnvironment()
{
    const char* value = std::getenv("PTZ_V291_PREPROCESS");
    if (!value) {
        return false;
    }
    std::string normalized(value);
    for (char& ch : normalized) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return normalized == "CPU";
}

static int normalizeLegacyPlateType(int legacyClassId)
{
    switch (legacyClassId) {
    case 0: return plate_types::Blue;
    case 1: return plate_types::Green;
    case 2: return plate_types::Yellow;
    case 3: return plate_types::Police;
    case 4: return plate_types::Consulate;
    default: return plate_types::Unknown;
    }
}

static void normalizeLegacyPlateTypes(std::vector<int>* typeIds)
{
    if (!typeIds) {
        return;
    }
    for (int& typeId : *typeIds) {
        typeId = normalizeLegacyPlateType(typeId);
    }
}

static int denormalizeLegacyPlateType(int businessTypeId)
{
    switch (businessTypeId) {
    case plate_types::Blue: return 0;
    case plate_types::Green: return 1;
    case plate_types::Yellow: return 2;
    case plate_types::Police: return 3;
    case plate_types::Consulate: return 4;
    default: return 0;
    }
}

static void denormalizeLegacyPlateTypes(std::vector<int>* typeIds)
{
    if (!typeIds) {
        return;
    }
    for (int& typeId : *typeIds) {
        typeId = denormalizeLegacyPlateType(typeId);
    }
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

// 车牌检测模型逐帧输出的位置和OCR文本会有小幅抖动。这里复用旧版
// app_pcie 中已经验证过的“OCR缓存 + 轨迹关联 + 框EMA + 文本投票”思路，
// 但保留当前Qt工程的latest-only单工作线程、动态OCR batch和完整阶段计时。
constexpr float kPlateOcrReuseIou = 0.55f;
constexpr double kPlateOcrReuseSignatureDiff = 20.0;
constexpr int64_t kPlateOcrReuseTtlMs = 900;
constexpr int64_t kPlateOcrCacheKeepMs = 2500;
constexpr std::size_t kPlateMaxTracks = 4;
constexpr int kPlateMinConfirmedHits = 3;
constexpr int kPlateTrackTtlMs = 600;
constexpr float kPlateTrackIouMin = 0.25f;
constexpr float kPlateTrackCenterDistanceMax = 0.50f;
constexpr float kPlateStaticCenterDeadZonePx = 3.0f;
constexpr float kPlateStaticSizeDeadZoneRatio = 0.03f;
constexpr float kPlateSlowBoxAlpha = 0.25f;
constexpr float kPlateFastBoxAlpha = 0.55f;
constexpr float kPlateFastMotionDistance = 0.20f;
constexpr int kPlateTextSwitchVoteMin = 3;

// 车牌颜色使用轨迹级滑动窗口投票。领馆黑牌被偶发误判成蓝牌时，
// 需要更强的反向证据才允许从Consulate切换到Blue。
constexpr int kPlateColorVoteWindow = 7;
constexpr int kPlateColorVoteMin = 4;
constexpr int kPlateConsulateVoteMin = 3;
constexpr int kPlateConsulateToBlueVoteMin = 5;
constexpr int kPlateConsulateToBlueConsecutive = 3;

// 当前应用场景中的行人均为站立姿态。使用原图框高宽比过滤明显扁平的
// 非行人误检；保持上限较宽，避免远处细长目标或局部遮挡被过度过滤。
constexpr float kStandingPedestrianMinHeightPx = 24.0f;
constexpr float kStandingPedestrianMinHeightWidthRatio = 2.50f;
constexpr float kStandingPedestrianMaxHeightWidthRatio = 6.00f;

static bool isUnknownPlateText(const std::string& text)
{
    static const std::string kUnknown = "UNKNOWN";
    if (text.size() != kUnknown.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (static_cast<char>(std::toupper(ch)) != kUnknown[i]) {
            return false;
        }
    }
    return true;
}

static bool isDisplayablePlateText(const std::string& text)
{
    return !text.empty() && !isUnknownPlateText(text);
}

static std::string upperAscii(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

static bool plateTextImpliesConsulate(const std::string& plate,
                                      const std::string& pinyin)
{
    // OCR对领馆牌会强制使用“领/LINGCON”后缀。该语义证据比单帧颜色
    // 分类更可靠，可用于纠正黑色领馆牌偶发被分类成Blue的情况。
    return plate.find(u8"领") != std::string::npos ||
            upperAscii(pinyin).find("LINGCON") != std::string::npos;
}

static bool plateOcrColorsCompatible(int cachedColorId, int candidateColorId)
{
    // v2.9.1 检测阶段只产生 Unknown，实际类型在 OCR/颜色后处理后得到。
    // Unknown 不作为身份判据，仍由 IoU、裁剪签名和 TTL 共同约束复用。
    if (cachedColorId == plate_types::Unknown ||
        candidateColorId == plate_types::Unknown) {
        return true;
    }
    if (cachedColorId == candidateColorId) {
        return true;
    }
    // 仅允许“已确认领馆牌缓存 → 当前单帧误报Blue”的单向复用。
    // 反向不复用，避免普通蓝牌错误继承领馆后缀。
    return cachedColorId == plate_types::Consulate &&
            candidateColorId == plate_types::Blue;
}

static void convertPlateInputBoxToOrig(PlateBackend backend,
                                       const demo::Rect2f& inputBox,
                                       demo::Rect2f* origBox,
                                       int frameWidth,
                                       int frameHeight)
{
    if (!origBox) {
        return;
    }
    if (backend == PlateBackend::V291) {
        demo::plate_v291::convert_input_box_to_orig(inputBox,
                                                    *origBox,
                                                    frameWidth,
                                                    frameHeight);
    } else {
        demo::vision::convert_input_box_to_orig(inputBox,
                                                *origBox,
                                                frameWidth,
                                                frameHeight);
    }
}

static bool isStandingPedestrianBox(const demo::Rect2f& box)
{
    if (!std::isfinite(box.width) || !std::isfinite(box.height) ||
        box.width <= 1.0f || box.height <= 1.0f) {
        return false;
    }
    if (box.height < kStandingPedestrianMinHeightPx) {
        return false;
    }
    const float ratio = box.height / box.width;
    return ratio >= kStandingPedestrianMinHeightWidthRatio &&
            ratio <= kStandingPedestrianMaxHeightWidthRatio;
}

static float plateRectIou(const demo::Rect2f& a, const demo::Rect2f& b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float iw = std::max(0.0f, x2 - x1);
    const float ih = std::max(0.0f, y2 - y1);
    const float inter = iw * ih;
    const float uni = a.width * a.height + b.width * b.height - inter;
    return uni > 1.0e-6f ? inter / uni : 0.0f;
}

static float plateCenterDistanceNormalized(const demo::Rect2f& a,
                                           const demo::Rect2f& b)
{
    const float ax = a.x + a.width * 0.5f;
    const float ay = a.y + a.height * 0.5f;
    const float bx = b.x + b.width * 0.5f;
    const float by = b.y + b.height * 0.5f;
    const float dx = ax - bx;
    const float dy = ay - by;
    const float diag = std::sqrt(std::max(1.0f,
                                          a.width * a.width + a.height * a.height));
    return std::sqrt(dx * dx + dy * dy) / diag;
}

static float clampPlateAlpha(float value)
{
    if (!std::isfinite(value)) {
        return demo::DEFAULT_PLATE_BOX_EMA;
    }
    return std::max(0.01f, std::min(1.0f, value));
}

static demo::Rect2f plateEmaRect(const demo::Rect2f& oldBox,
                                 const demo::Rect2f& newBox,
                                 float alpha)
{
    const float a = clampPlateAlpha(alpha);
    const float b = 1.0f - a;
    return demo::Rect2f(oldBox.x * b + newBox.x * a,
                        oldBox.y * b + newBox.y * a,
                        oldBox.width * b + newBox.width * a,
                        oldBox.height * b + newBox.height * a);
}

static float plateAdaptiveBoxAlpha(const demo::Rect2f& oldBox,
                                   const demo::Rect2f& newBox)
{
    const float oldCx = oldBox.x + oldBox.width * 0.5f;
    const float oldCy = oldBox.y + oldBox.height * 0.5f;
    const float newCx = newBox.x + newBox.width * 0.5f;
    const float newCy = newBox.y + newBox.height * 0.5f;
    const float centerShift = std::sqrt((oldCx - newCx) * (oldCx - newCx) +
                                        (oldCy - newCy) * (oldCy - newCy));
    const float widthChange = std::fabs(newBox.width - oldBox.width) /
            std::max(1.0f, oldBox.width);
    const float heightChange = std::fabs(newBox.height - oldBox.height) /
            std::max(1.0f, oldBox.height);

    // 静态画面中检测框常有1～3像素波动。落在死区内时完全保持上一帧，
    // 不再让每帧微小检测噪声传到HDMI OSD。
    if (centerShift <= kPlateStaticCenterDeadZonePx &&
        widthChange <= kPlateStaticSizeDeadZoneRatio &&
        heightChange <= kPlateStaticSizeDeadZoneRatio) {
        return 0.0f;
    }

    const float distance = plateCenterDistanceNormalized(oldBox, newBox);
    return distance <= kPlateFastMotionDistance
            ? kPlateSlowBoxAlpha : kPlateFastBoxAlpha;
}

// 对94x24 OCR裁剪图生成8x4灰度签名。相同位置且内容近似时复用最近OCR，
// 既减少识别文本抖动，也减少不必要的第二级RKNN运行。
static std::vector<uint8_t> plateCropSignature(const demo::Mat& crop)
{
    constexpr int kSignatureWidth = 8;
    constexpr int kSignatureHeight = 4;
    std::vector<uint8_t> signature(kSignatureWidth * kSignatureHeight, 0);
    if (crop.empty() || crop.type() != CV_8UC3) {
        return signature;
    }

    for (int gy = 0; gy < kSignatureHeight; ++gy) {
        const int y0 = gy * crop.rows / kSignatureHeight;
        const int y1 = std::max(y0 + 1, (gy + 1) * crop.rows / kSignatureHeight);
        for (int gx = 0; gx < kSignatureWidth; ++gx) {
            const int x0 = gx * crop.cols / kSignatureWidth;
            const int x1 = std::max(x0 + 1, (gx + 1) * crop.cols / kSignatureWidth);
            uint64_t sum = 0;
            uint64_t count = 0;
            for (int y = y0; y < y1; ++y) {
                const cv::Vec3b* row = crop.ptr<cv::Vec3b>(y);
                for (int x = x0; x < x1; ++x) {
                    const cv::Vec3b& pixel = row[x];
                    sum += (static_cast<uint64_t>(pixel[0]) +
                            static_cast<uint64_t>(pixel[1]) * 2ULL +
                            static_cast<uint64_t>(pixel[2])) / 4ULL;
                    ++count;
                }
            }
            signature[gy * kSignatureWidth + gx] =
                    count ? static_cast<uint8_t>(sum / count) : 0;
        }
    }
    return signature;
}

static double plateSignatureDifference(const std::vector<uint8_t>& a,
                                       const std::vector<uint8_t>& b)
{
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return 999.0;
    }
    uint64_t sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += static_cast<uint64_t>(std::abs(static_cast<int>(a[i]) -
                                             static_cast<int>(b[i])));
    }
    return static_cast<double>(sum) / static_cast<double>(a.size());
}

struct PlateTemporalCandidate
{
    demo::Rect2f inputBox;
    demo::Rect2f origBox;
    int colorId = -1;
    std::string plate;
    std::string pinyin;
    std::vector<uint8_t> signature;
    int cacheIndex = -1;
    bool reusedOcr = false;
};

struct PlateOcrCacheEntry
{
    demo::Rect2f inputBox;
    int colorId = -1;
    std::string plate;
    std::string pinyin;
    std::vector<uint8_t> signature;
    int64_t lastOcrMs = 0;
    int64_t lastSeenMs = 0;
};

struct PlateTextVote
{
    std::string plate;
    std::string pinyin;
};

struct PlateTemporalTrack
{
    bool active = false;
    int slotId = 0;
    demo::Rect2f inputBoxEma;
    demo::Rect2f origBoxEma;
    std::deque<PlateTextVote> textVotes;
    std::string stablePlate;
    std::string stablePinyin;
    int stableColorId = -1;
    std::deque<int> colorVotes;
    int consecutiveBlueCount = 0;
    int hitCount = 0;
    int64_t lastSeenMs = 0;
};

class PlateTemporalPostprocessor
{
public:
    void reset()
    {
        contextValid_ = false;
        contextConfigRevision_ = 0;
        contextSourceRevision_ = 0;
        contextSourceChannel_ = -1;
        ocrCache_.clear();
        tracks_.clear();
    }

    void resetForContext(uint64_t configRevision,
                         int sourceChannel,
                         uint64_t sourceRevision)
    {
        if (!contextValid_ ||
            contextConfigRevision_ != configRevision ||
            contextSourceChannel_ != sourceChannel ||
            contextSourceRevision_ != sourceRevision) {
            ocrCache_.clear();
            tracks_.clear();
            contextValid_ = true;
            contextConfigRevision_ = configRevision;
            contextSourceChannel_ = sourceChannel;
            contextSourceRevision_ = sourceRevision;
        }
    }

    void prepareOcr(const std::vector<demo::Mat>& crops,
                    const std::vector<demo::Rect2f>& boxes,
                    const std::vector<int>& colorIds,
                    int64_t nowMs,
                    std::vector<PlateTemporalCandidate>* candidates,
                    std::vector<demo::Mat>* ocrCrops,
                    std::vector<demo::Rect2f>* ocrBoxes,
                    std::vector<int>* ocrColorIds,
                    std::vector<int>* ocrToCandidate)
    {
        candidates->clear();
        ocrCrops->clear();
        ocrBoxes->clear();
        ocrColorIds->clear();
        ocrToCandidate->clear();
        pruneOcrCache(nowMs);

        const std::size_t count = std::min(
                    kPlateMaxTracks,
                    std::min(crops.size(),
                             std::min(boxes.size(), colorIds.size())));
        candidates->reserve(count);
        std::vector<bool> cacheUsed(ocrCache_.size(), false);

        for (std::size_t i = 0; i < count; ++i) {
            PlateTemporalCandidate candidate;
            candidate.inputBox = boxes[i];
            candidate.colorId = colorIds[i];
            candidate.signature = plateCropSignature(crops[i]);

            const int cacheIndex = findReusableOcr(candidate,
                                                   cacheUsed,
                                                   nowMs);
            if (cacheIndex >= 0) {
                const PlateOcrCacheEntry& cached =
                        ocrCache_[static_cast<std::size_t>(cacheIndex)];
                candidate.plate = cached.plate;
                candidate.pinyin = cached.pinyin;
                candidate.colorId = cached.colorId;
                candidate.cacheIndex = cacheIndex;
                candidate.reusedOcr = true;
                cacheUsed[static_cast<std::size_t>(cacheIndex)] = true;
            } else {
                ocrToCandidate->push_back(static_cast<int>(candidates->size()));
                ocrCrops->push_back(crops[i]);
                ocrBoxes->push_back(boxes[i]);
                ocrColorIds->push_back(colorIds[i]);
            }
            candidates->push_back(std::move(candidate));
        }
    }

    void mergeOcrResults(const std::vector<std::string>& plates,
                         const std::vector<std::string>& pinyins,
                         const std::vector<int>& colorIds,
                         const std::vector<int>& ocrToCandidate,
                         std::vector<PlateTemporalCandidate>* candidates)
    {
        const std::size_t count = std::min(plates.size(), ocrToCandidate.size());
        for (std::size_t i = 0; i < count; ++i) {
            const int candidateIndex = ocrToCandidate[i];
            if (candidateIndex < 0 ||
                static_cast<std::size_t>(candidateIndex) >= candidates->size()) {
                continue;
            }
            PlateTemporalCandidate& candidate =
                    (*candidates)[static_cast<std::size_t>(candidateIndex)];
            candidate.plate = plates[i];
            candidate.pinyin = i < pinyins.size() ? pinyins[i] : std::string();
            if (i < colorIds.size()) {
                candidate.colorId = colorIds[i];
            }
        }
    }

    void stabilize(std::vector<PlateTemporalCandidate>* candidates,
                   PlateBackend backend,
                   int frameWidth,
                   int frameHeight,
                   int64_t nowMs,
                   std::vector<PlateTemporalCandidate>* stableOutput)
    {
        for (PlateTemporalCandidate& candidate : *candidates) {
            convertPlateInputBoxToOrig(backend,
                                       candidate.inputBox,
                                       &candidate.origBox,
                                       frameWidth,
                                       frameHeight);
            if (plateTextImpliesConsulate(candidate.plate, candidate.pinyin)) {
                candidate.colorId = plate_types::Consulate;
            }
        }
        updateOcrCache(*candidates, nowMs);

        std::stable_sort(candidates->begin(), candidates->end(),
                         [](const PlateTemporalCandidate& a,
                            const PlateTemporalCandidate& b) {
            const float ay = a.origBox.y + a.origBox.height * 0.5f;
            const float by = b.origBox.y + b.origBox.height * 0.5f;
            if (std::fabs(ay - by) > 80.0f) {
                return ay < by;
            }
            const float ax = a.origBox.x + a.origBox.width * 0.5f;
            const float bx = b.origBox.x + b.origBox.width * 0.5f;
            return ax < bx;
        });

        std::vector<bool> used(candidates->size(), false);
        for (PlateTemporalTrack& track : tracks_) {
            if (!track.active) {
                continue;
            }
            int bestIndex = -1;
            float bestScore = -1.0f;
            for (std::size_t i = 0; i < candidates->size(); ++i) {
                if (used[i]) {
                    continue;
                }
                const float iou = plateRectIou(track.origBoxEma,
                                               (*candidates)[i].origBox);
                const float distance = plateCenterDistanceNormalized(
                            track.origBoxEma, (*candidates)[i].origBox);
                if (iou < kPlateTrackIouMin &&
                    distance > kPlateTrackCenterDistanceMax) {
                    continue;
                }
                const float score = iou * 1.20f - distance * 0.35f;
                if (score > bestScore) {
                    bestScore = score;
                    bestIndex = static_cast<int>(i);
                }
            }
            if (bestIndex >= 0) {
                used[static_cast<std::size_t>(bestIndex)] = true;
                updateTrack(track,
                            (*candidates)[static_cast<std::size_t>(bestIndex)],
                            nowMs,
                            demo::DEFAULT_PLATE_BOX_EMA,
                            demo::DEFAULT_PLATE_TEXT_VOTE_N,
                            demo::DEFAULT_PLATE_COLOR_HOLD);
            }
        }

        for (std::size_t i = 0; i < candidates->size(); ++i) {
            if (used[i]) {
                continue;
            }
            const int slot = firstFreeSlot();
            if (slot < 0) {
                continue;
            }
            PlateTemporalTrack track;
            track.active = true;
            track.slotId = slot;
            track.inputBoxEma = (*candidates)[i].inputBox;
            track.origBoxEma = (*candidates)[i].origBox;
            updateTrack(track,
                        (*candidates)[i],
                        nowMs,
                        1.0f,
                        demo::DEFAULT_PLATE_TEXT_VOTE_N,
                        demo::DEFAULT_PLATE_COLOR_HOLD);
            tracks_.push_back(std::move(track));
        }

        const int ttlMs = kPlateTrackTtlMs;
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                     [nowMs, ttlMs](const PlateTemporalTrack& track) {
            return !track.active || nowMs - track.lastSeenMs > ttlMs;
        }), tracks_.end());

        std::sort(tracks_.begin(), tracks_.end(),
                  [](const PlateTemporalTrack& a, const PlateTemporalTrack& b) {
            return a.slotId < b.slotId;
        });

        stableOutput->clear();
        stableOutput->reserve(tracks_.size());
        for (const PlateTemporalTrack& track : tracks_) {
            if (!track.active ||
                track.hitCount < kPlateMinConfirmedHits ||
                nowMs - track.lastSeenMs > ttlMs ||
                !isDisplayablePlateText(track.stablePlate)) {
                continue;
            }
            PlateTemporalCandidate item;
            item.plate = track.stablePlate;
            item.pinyin = track.stablePinyin;
            item.inputBox = track.inputBoxEma;
            item.origBox = track.origBoxEma;
            item.colorId = track.stableColorId;
            stableOutput->push_back(std::move(item));
        }
    }

private:
    int findReusableOcr(const PlateTemporalCandidate& candidate,
                        const std::vector<bool>& used,
                        int64_t nowMs) const
    {
        int bestIndex = -1;
        float bestIou = 0.0f;
        for (std::size_t i = 0; i < ocrCache_.size(); ++i) {
            if (i < used.size() && used[i]) {
                continue;
            }
            const PlateOcrCacheEntry& entry = ocrCache_[i];
            if (!isDisplayablePlateText(entry.plate) ||
                !plateOcrColorsCompatible(entry.colorId, candidate.colorId) ||
                nowMs - entry.lastOcrMs > kPlateOcrReuseTtlMs ||
                plateSignatureDifference(candidate.signature, entry.signature) >
                    kPlateOcrReuseSignatureDiff) {
                continue;
            }
            const float iou = plateRectIou(candidate.inputBox, entry.inputBox);
            if (iou > bestIou) {
                bestIou = iou;
                bestIndex = static_cast<int>(i);
            }
        }
        return bestIou >= kPlateOcrReuseIou ? bestIndex : -1;
    }

    void pruneOcrCache(int64_t nowMs)
    {
        ocrCache_.erase(std::remove_if(ocrCache_.begin(), ocrCache_.end(),
                                       [nowMs](const PlateOcrCacheEntry& entry) {
            return nowMs - entry.lastSeenMs > kPlateOcrCacheKeepMs;
        }), ocrCache_.end());
        if (ocrCache_.size() > kPlateMaxTracks) {
            ocrCache_.resize(kPlateMaxTracks);
        }
    }

    void updateOcrCache(const std::vector<PlateTemporalCandidate>& candidates,
                        int64_t nowMs)
    {
        std::vector<PlateOcrCacheEntry> next;
        next.reserve(kPlateMaxTracks);
        std::vector<bool> oldUsed(ocrCache_.size(), false);

        for (const PlateTemporalCandidate& candidate : candidates) {
            if (!isDisplayablePlateText(candidate.plate) ||
                next.size() >= kPlateMaxTracks) {
                continue;
            }
            PlateOcrCacheEntry entry;
            entry.inputBox = candidate.inputBox;
            entry.colorId = candidate.colorId;
            entry.plate = candidate.plate;
            entry.pinyin = candidate.pinyin;
            entry.signature = candidate.signature;
            entry.lastSeenMs = nowMs;
            entry.lastOcrMs = nowMs;
            if (candidate.reusedOcr && candidate.cacheIndex >= 0 &&
                static_cast<std::size_t>(candidate.cacheIndex) < ocrCache_.size()) {
                entry.lastOcrMs = ocrCache_[static_cast<std::size_t>(candidate.cacheIndex)].lastOcrMs;
                oldUsed[static_cast<std::size_t>(candidate.cacheIndex)] = true;
            }
            next.push_back(std::move(entry));
        }

        for (std::size_t i = 0; i < ocrCache_.size() && next.size() < kPlateMaxTracks; ++i) {
            if (oldUsed[i] || nowMs - ocrCache_[i].lastSeenMs > kPlateOcrCacheKeepMs) {
                continue;
            }
            bool duplicate = false;
            for (const PlateOcrCacheEntry& entry : next) {
                if ((plateOcrColorsCompatible(entry.colorId, ocrCache_[i].colorId) ||
                     plateOcrColorsCompatible(ocrCache_[i].colorId, entry.colorId)) &&
                    plateRectIou(entry.inputBox, ocrCache_[i].inputBox) >= 0.25f) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                next.push_back(ocrCache_[i]);
            }
        }
        ocrCache_.swap(next);
    }

    static void pushTextVote(std::deque<PlateTextVote>* votes,
                             const PlateTemporalCandidate& candidate,
                             int voteCount)
    {
        if (!isDisplayablePlateText(candidate.plate)) {
            return;
        }
        votes->push_back({candidate.plate, candidate.pinyin});
        while (static_cast<int>(votes->size()) > voteCount) {
            votes->pop_front();
        }
    }

    static std::pair<std::string, int> majorityPlate(
            const std::deque<PlateTextVote>& votes)
    {
        std::map<std::string, int> counts;
        std::string best;
        int bestCount = 0;
        for (const PlateTextVote& vote : votes) {
            const int count = ++counts[vote.plate];
            if (count > bestCount) {
                best = vote.plate;
                bestCount = count;
            }
        }
        return {best, bestCount};
    }

    static std::string latestPinyinForPlate(
            const std::deque<PlateTextVote>& votes,
            const std::string& plate)
    {
        for (auto it = votes.rbegin(); it != votes.rend(); ++it) {
            if (it->plate == plate && !it->pinyin.empty()) {
                return it->pinyin;
            }
        }
        return {};
    }

    static std::pair<int, int> majorityColor(
            const std::deque<int>& votes)
    {
        std::map<int, int> counts;
        int bestColor = -1;
        int bestCount = 0;
        for (int colorId : votes) {
            if (colorId < 0 || colorId >= plate_types::Count) {
                continue;
            }
            const int count = ++counts[colorId];
            if (count > bestCount) {
                bestColor = colorId;
                bestCount = count;
            }
        }
        return {bestColor, bestCount};
    }

    static int colorVoteCount(const std::deque<int>& votes, int colorId)
    {
        return static_cast<int>(std::count(votes.begin(), votes.end(), colorId));
    }

    static void updateTrack(PlateTemporalTrack& track,
                            const PlateTemporalCandidate& candidate,
                            int64_t nowMs,
                            float alpha,
                            int voteCount,
                            int colorHold)
    {
        voteCount = std::max(1, std::min(12, voteCount));
        if (track.hitCount > 0) {
            const float adaptiveAlpha = plateAdaptiveBoxAlpha(
                        track.origBoxEma, candidate.origBox);
            if (adaptiveAlpha > 0.0f) {
                track.inputBoxEma = plateEmaRect(track.inputBoxEma,
                                                 candidate.inputBox,
                                                 adaptiveAlpha);
                track.origBoxEma = plateEmaRect(track.origBoxEma,
                                                candidate.origBox,
                                                adaptiveAlpha);
            }
        } else {
            track.inputBoxEma = candidate.inputBox;
            track.origBoxEma = candidate.origBox;
        }
        (void)alpha;

        pushTextVote(&track.textVotes, candidate, voteCount);
        const std::pair<std::string, int> voted = majorityPlate(track.textVotes);
        if (track.stablePlate.empty() && !voted.first.empty()) {
            track.stablePlate = voted.first;
            track.stablePinyin = latestPinyinForPlate(track.textVotes,
                                                      track.stablePlate);
        } else if (!voted.first.empty() && voted.first != track.stablePlate &&
                   voted.second >= kPlateTextSwitchVoteMin) {
            track.stablePlate = voted.first;
            track.stablePinyin = latestPinyinForPlate(track.textVotes,
                                                      track.stablePlate);
        } else if (candidate.plate == track.stablePlate &&
                   !candidate.pinyin.empty()) {
            track.stablePinyin = candidate.pinyin;
        }

        (void)colorHold;

        int observedColor = candidate.colorId;
        const bool semanticConsulate =
                plateTextImpliesConsulate(candidate.plate, candidate.pinyin);
        if (semanticConsulate) {
            observedColor = plate_types::Consulate;
        }
        if (observedColor >= 0 && observedColor < plate_types::Count) {
            track.colorVotes.push_back(observedColor);
            while (static_cast<int>(track.colorVotes.size()) >
                   kPlateColorVoteWindow) {
                track.colorVotes.pop_front();
            }
        }

        const std::pair<int, int> votedColor = majorityColor(track.colorVotes);
        const int consulateVotes = colorVoteCount(
                    track.colorVotes, plate_types::Consulate);
        const int blueVotes = colorVoteCount(
                    track.colorVotes, plate_types::Blue);

        if (track.stableColorId < 0) {
            track.stableColorId = observedColor;
        }

        if (semanticConsulate) {
            track.stableColorId = plate_types::Consulate;
            track.consecutiveBlueCount = 0;
        } else if (track.stableColorId == plate_types::Consulate) {
            if (observedColor == plate_types::Blue) {
                ++track.consecutiveBlueCount;
            } else {
                track.consecutiveBlueCount = 0;
            }

            // 已稳定的黑色领馆牌只有在窗口内Blue占绝对多数、且连续多帧
            // 均为Blue时才允许回切，避免单帧颜色分类错误影响OSD。
            if (blueVotes >= kPlateConsulateToBlueVoteMin &&
                blueVotes > consulateVotes &&
                track.consecutiveBlueCount >=
                    kPlateConsulateToBlueConsecutive) {
                track.stableColorId = plate_types::Blue;
                track.consecutiveBlueCount = 0;
            } else if (votedColor.first >= 0 &&
                       votedColor.first != plate_types::Blue &&
                       votedColor.first != plate_types::Consulate &&
                       votedColor.second >= kPlateColorVoteMin) {
                track.stableColorId = votedColor.first;
                track.consecutiveBlueCount = 0;
            }
        } else {
            track.consecutiveBlueCount = 0;
            if (consulateVotes >= kPlateConsulateVoteMin) {
                track.stableColorId = plate_types::Consulate;
            } else if (votedColor.first >= 0 &&
                       votedColor.first != track.stableColorId &&
                       votedColor.second >= kPlateColorVoteMin) {
                track.stableColorId = votedColor.first;
            }
        }

        ++track.hitCount;
        track.lastSeenMs = nowMs;
    }

    int firstFreeSlot() const
    {
        for (int slot = 0; slot < static_cast<int>(kPlateMaxTracks); ++slot) {
            bool used = false;
            for (const PlateTemporalTrack& track : tracks_) {
                if (track.active && track.slotId == slot) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                return slot;
            }
        }
        return -1;
    }

    bool contextValid_ = false;
    uint64_t contextConfigRevision_ = 0;
    uint64_t contextSourceRevision_ = 0;
    int contextSourceChannel_ = -1;
    std::vector<PlateOcrCacheEntry> ocrCache_;
    std::vector<PlateTemporalTrack> tracks_;
};

}  // namespace

struct NpuInferenceWorker::Impl
{
    std::atomic<bool> running {false};
    std::atomic<bool> stopReq {false};
    std::atomic<int64_t> nextSubmitNs {0};
    std::atomic<int> droppedFrames {0};
    std::atomic<int> processed {0};
    std::atomic<bool> modelsLoaded {false};
    std::atomic<int> inputSourceChannel {0};
    std::atomic<uint64_t> inputSourceRevision {1};

    std::thread thread;

    mutable std::mutex configMutex;
    NpuTaskConfig requestedConfig;

    mutable std::mutex pendingMutex;
    std::condition_variable pendingCv;
    bool pendingValid = false;
    FrameCopy pending;
    // 两个 latest-only 槽之间复用已分配容量，避免总览原图模式每帧反复
    // malloc/free 约 4.15 MB 缓冲造成堆抖动和异常退出风险。
    FrameCopy spare;

    mutable std::mutex resultMutex;
    NpuFrameResult latest;
    std::deque<NpuFrameResult> resultHistory;

    demo::RknnModel plateDet;
    demo::RknnModel plateReco;
    demo::RknnModel ped;
    demo::RknnModel vehicle;
    PlateBackend plateBackend = PlateBackend::Unknown;
    std::atomic<int> loadedPlateProfileValue {-1};
    std::atomic<bool> modelSwitching {false};
    bool v291ForceCpu = false;
    mutable std::mutex violationConfigMutex;
    ViolationRuleConfig violationConfig;
    PedestrianViolationEngine violationEngine;
    mutable std::mutex wrongWayConfigMutex;
    VehicleDirectionConfig wrongWayConfig;
    VehicleWrongWayEngine wrongWayEngine;
    PlateTemporalPostprocessor platePostprocessor;
    std::atomic<int> loadedTaskValue {-1};
    std::string modelError;
    uint64_t activePlateConfigRevision = 0;
    uint64_t activeWrongWayRuleRevision = 0;

    int64_t fpsWindowMs = 0;
    int fpsWindowCount = 0;
    double currentFps = 0.0;

    Impl()
    {
        requestedConfig = sanitizeNpuTaskConfig(requestedConfig);
        requestedConfig.plateModelProfile = plateModelProfileFromEnvironment();
        requestedConfig.platePostprocess = platePostprocessModeFromEnvironment();
        requestedConfig.plateCropSource = plateCropSourceFromEnvironment();
        requestedConfig = sanitizeNpuTaskConfig(requestedConfig);
        violationConfig = sanitizeViolationRuleConfig(violationConfig);
        wrongWayConfig = sanitizeVehicleDirectionConfig(wrongWayConfig);
        wrongWayEngine.setConfig(wrongWayConfig);
        v291ForceCpu = v291ForceCpuFromEnvironment();
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
        releaseModel(vehicle);
        plateBackend = PlateBackend::Unknown;
        loadedPlateProfileValue.store(-1);
        platePostprocessor.reset();
        wrongWayEngine.reset();
        loadedTaskValue.store(-1);
        modelsLoaded.store(false);
        activePlateConfigRevision = 0;
        activeWrongWayRuleRevision = 0;
    }

    NpuTaskConfig configSnapshot() const
    {
        std::lock_guard<std::mutex> lk(configMutex);
        return requestedConfig;
    }

    static std::size_t frameCapacity(const FrameCopy& frame)
    {
        return frame.data.capacity() + frame.originalData.capacity();
    }

    void keepLargerSpareLocked(FrameCopy&& frame)
    {
        frame.data.clear();
        frame.originalData.clear();
        frame.originalCopyError.clear();
        if (frameCapacity(frame) >= frameCapacity(spare)) {
            spare = std::move(frame);
        }
    }

    bool modelsMatchConfig(const NpuTaskConfig& cfg) const
    {
        if (!modelsLoaded.load() ||
            loadedTaskValue.load() != static_cast<int>(cfg.task)) {
            return false;
        }
        return cfg.task != NpuTaskType::PlateCascade ||
                loadedPlateProfileValue.load() ==
                static_cast<int>(cfg.plateModelProfile);
    }

    void fillModelState(NpuFrameResult* result,
                        const NpuTaskConfig& cfg) const
    {
        if (!result) {
            return;
        }
        result->requestedPlateModelProfile = cfg.plateModelProfile;
        result->platePostprocess = cfg.platePostprocess;
        result->requestedPlateCropSource = cfg.plateCropSource;
        result->modelSwitching = modelSwitching.load();
        const int active = loadedPlateProfileValue.load();
        result->activePlateModelProfileValid = active >= 0;
        if (active >= 0) {
            result->activePlateModelProfile =
                    static_cast<NpuPlateModelProfile>(active);
        }
        result->modelsReady = modelsMatchConfig(cfg);
    }

    void updateViolationConfig(const ViolationRuleConfig& requested)
    {
        ViolationRuleConfig next = sanitizeViolationRuleConfig(requested);
        std::lock_guard<std::mutex> lk(violationConfigMutex);
        if (next.revision <= violationConfig.revision) {
            next.revision = violationConfig.revision + 1;
        }
        violationConfig = next;
    }

    ViolationRuleConfig violationConfigSnapshot() const
    {
        std::lock_guard<std::mutex> lk(violationConfigMutex);
        return violationConfig;
    }

    void updateWrongWayConfig(const VehicleDirectionConfig& requested)
    {
        VehicleDirectionConfig next = sanitizeVehicleDirectionConfig(requested);
        std::lock_guard<std::mutex> lk(wrongWayConfigMutex);
        if (next.revision <= wrongWayConfig.revision) {
            next.revision = wrongWayConfig.revision + 1;
        }
        wrongWayConfig = next;
    }

    VehicleDirectionConfig wrongWayConfigSnapshot() const
    {
        std::lock_guard<std::mutex> lk(wrongWayConfigMutex);
        return wrongWayConfig;
    }

    void updateConfig(const NpuTaskConfig& requested)
    {
        NpuTaskConfig next = sanitizeNpuTaskConfig(requested);
        bool executionChanged = false;
        {
            std::lock_guard<std::mutex> lk(configMutex);
            executionChanged = next.task != requestedConfig.task ||
                    next.plateModelProfile != requestedConfig.plateModelProfile ||
                    next.platePostprocess != requestedConfig.platePostprocess ||
                    next.plateCropSource != requestedConfig.plateCropSource;
            next.revision = requestedConfig.revision + 1;
            requestedConfig = next;
        }

        nextSubmitNs.store(0);

        if (!next.enabled || executionChanged) {
            std::lock_guard<std::mutex> lk(pendingMutex);
            keepLargerSpareLocked(std::move(pending));
            pending = FrameCopy();
            pendingValid = false;
        }

        NpuFrameResult state;
        state.enabled = next.enabled;
        state.task = next.task;
        fillModelState(&state, next);
        state.targetFps = next.targetFps;
        state.confidence = next.confidence;
        state.nmsThreshold = next.nmsThreshold;
        state.configRevision = next.revision;
        if (next.task == NpuTaskType::TrafficViolation) {
            state.violationConfig = violationConfigSnapshot();
            state.violationRegionsConfigured = state.violationConfig.regionsValid();
        } else if (next.task == NpuTaskType::VehicleWrongWay) {
            state.wrongWayConfig = wrongWayConfigSnapshot();
            state.wrongWayRegionsConfigured = state.wrongWayConfig.regionsValid();
            state.wrongWayRuleEnabled = next.wrongWayRuleEnabled;
        }
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

    void updateInputSource(int sourceChannel, uint64_t sourceRevision)
    {
        sourceChannel = std::max(0, std::min(9, sourceChannel));
        inputSourceChannel.store(sourceChannel);
        inputSourceRevision.store(sourceRevision);
        nextSubmitNs.store(0);

        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            keepLargerSpareLocked(std::move(pending));
            pending = FrameCopy();
            pendingValid = false;
        }

        const NpuTaskConfig cfg = configSnapshot();
        NpuFrameResult state;
        state.enabled = cfg.enabled;
        state.task = cfg.task;
        fillModelState(&state, cfg);
        state.targetFps = cfg.targetFps;
        state.confidence = cfg.confidence;
        state.nmsThreshold = cfg.nmsThreshold;
        state.configRevision = cfg.revision;
        state.sourceChannel = sourceChannel;
        state.sourceRevision = sourceRevision;
        if (cfg.task == NpuTaskType::TrafficViolation) {
            state.violationConfig = violationConfigSnapshot();
            state.violationRegionsConfigured = state.violationConfig.regionsValid();
        } else if (cfg.task == NpuTaskType::VehicleWrongWay) {
            state.wrongWayConfig = wrongWayConfigSnapshot();
            state.wrongWayRegionsConfigured = state.wrongWayConfig.regionsValid();
            state.wrongWayRuleEnabled = cfg.wrongWayRuleEnabled;
        }
        state.status = cfg.enabled ? "pending" : "off";
        state.processed = processed.load();
        state.dropped = droppedFrames.load();

        std::lock_guard<std::mutex> lk(resultMutex);
        latest = std::move(state);
        resultHistory.clear();
    }

    bool wantsFrame() const
    {
        const NpuTaskConfig cfg = configSnapshot();
        if (!cfg.enabled || !running.load()) {
            return false;
        }

        const int64_t nowNs = steadyNs();
        const int64_t deadlineNs = nextSubmitNs.load(std::memory_order_acquire);
        return deadlineNs == 0 || nowNs + kSubmitToleranceNs >= deadlineNs;
    }

    bool claimFrameSlot(int targetFps, int64_t nowNs)
    {
        const int64_t periodNs = targetPeriodNs(targetFps);
        int64_t deadlineNs = nextSubmitNs.load(std::memory_order_acquire);

        for (;;) {
            int64_t nextDeadlineNs = 0;
            if (deadlineNs == 0) {
                // 首帧立即接收，并从实际首帧时刻建立理想时间轴。
                nextDeadlineNs = nowNs + periodNs;
            } else {
                if (nowNs + kSubmitToleranceNs < deadlineNs) {
                    return false;
                }

                // 沿原理想时间轴推进，而不是从本次实际提交时刻重新计时。
                // 当源帧率与目标帧率不可整除时，提交间隔可在相邻源帧之间
                // 交替，从而保持长期平均帧率；若生产者长时间停顿，则跳过
                // 已错过的截止点，避免恢复后突发补交旧任务。
                nextDeadlineNs = deadlineNs;
                do {
                    nextDeadlineNs += periodNs;
                } while (nextDeadlineNs <= nowNs);
            }

            if (nextSubmitNs.compare_exchange_weak(deadlineNs,
                                                   nextDeadlineNs,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool loadPlateProfile(NpuPlateModelProfile profile,
                          NpuPlatePostprocess postprocess,
                          std::string* error)
    {
        // 只接受profile对应目录中的显式模型对；禁止依赖旧的“按张量形状
        // 自动猜后端”行为，避免界面显示legacy但实际运行candidate（或相反）。
        const std::string plateDetPath = findPlateModelPath(profile,
                                                            kPlateDetModel);
        const std::string plateRecoPath = findPlateModelPath(profile,
                                                              kPlateRecoModel);
        bool ok = loadModel(plateDetPath, &plateDet, error) &&
                  loadModel(plateRecoPath, &plateReco, error);
        if (ok) {
            plateBackend = validatePlateContract(plateDet, plateReco, error);
            const PlateBackend expected =
                    profile == NpuPlateModelProfile::Legacy
                    ? PlateBackend::Legacy : PlateBackend::V291;
            if (plateBackend != expected) {
                if (error) {
                    *error = std::string("plate profile contract mismatch: requested=") +
                            npuPlateModelProfileKey(profile);
                }
                ok = false;
            }
        }
        if (!ok) {
            return false;
        }

        std::cerr << "[NPU] plate backend="
                  << npuPlateModelProfileKey(profile)
                  << " postprocess=" << npuPlatePostprocessKey(postprocess)
                  << " preprocess=" << (v291ForceCpu ? "cpu" : "auto")
                  << " det=" << plateDetPath
                  << " reco=" << plateRecoPath << std::endl;
        return true;
    }

    bool ensureModels(const NpuTaskConfig& cfg)
    {
        if (modelsMatchConfig(cfg)) {
            return true;
        }

        const int previousTask = loadedTaskValue.load();
        const int previousProfile = loadedPlateProfileValue.load();
        modelSwitching.store(true);
        if (cfg.task == NpuTaskType::PlateCascade) {
            std::cerr << "[NPU] profile switch begin: active="
                      << (previousProfile >= 0
                          ? npuPlateModelProfileKey(
                                static_cast<NpuPlateModelProfile>(previousProfile))
                          : "none")
                      << " requested="
                      << npuPlateModelProfileKey(cfg.plateModelProfile)
                      << std::endl;
        }

        // 模型只由推理线程装载和释放。切换任务时释放旧任务模型，
        // 切换车牌profile时也不让两套模型同时常驻。
        releaseAllModels();

        std::string error;
        bool ok = false;
        if (cfg.task == NpuTaskType::PlateCascade) {
            ok = loadPlateProfile(cfg.plateModelProfile,
                                  cfg.platePostprocess,
                                  &error);
        } else if (cfg.task == NpuTaskType::VehicleWrongWay) {
            const std::string vehiclePath = findModelPath(kVehicleModel);
            ok = loadModel(vehiclePath, &vehicle, &error) &&
                    validateVehicleContract(vehicle, &error);
        } else if (cfg.task == NpuTaskType::TrafficViolation) {
            // 违法检测第一轮只复用行人模型。交通灯状态由人工ROI内HSV判断，
            // 不再额外运行车辆模型，降低NPU占用并提高规则刷新率。
            const std::string pedPath = findModelPath(kPedModel);
            ok = loadModel(pedPath, &ped, &error) &&
                    validatePedestrianContract(ped, &error);
        } else {
            const std::string pedPath = findModelPath(kPedModel);
            ok = loadModel(pedPath, &ped, &error) &&
                    validatePedestrianContract(ped, &error);
        }

        if (!ok) {
            const std::string switchError = error;
            releaseAllModels();

            // 车牌profile切换失败时，自动重新装载切换前已验证的profile。
            if (cfg.task == NpuTaskType::PlateCascade &&
                previousTask == static_cast<int>(NpuTaskType::PlateCascade) &&
                previousProfile >= 0) {
                const NpuPlateModelProfile rollbackProfile =
                        static_cast<NpuPlateModelProfile>(previousProfile);
                std::string rollbackError;
                if (loadPlateProfile(rollbackProfile,
                                     cfg.platePostprocess,
                                     &rollbackError)) {
                    loadedTaskValue.store(
                                static_cast<int>(NpuTaskType::PlateCascade));
                    loadedPlateProfileValue.store(previousProfile);
                    modelsLoaded.store(true);

                    NpuTaskConfig reverted;
                    {
                        std::lock_guard<std::mutex> lk(configMutex);
                        if (requestedConfig.revision == cfg.revision) {
                            requestedConfig.plateModelProfile = rollbackProfile;
                            ++requestedConfig.revision;
                        }
                        reverted = requestedConfig;
                    }
                    modelError = "profile switch failed; rolled back: " +
                            switchError;
                    modelSwitching.store(false);
                    NpuFrameResult state;
                    state.enabled = reverted.enabled;
                    state.task = reverted.task;
                    state.configRevision = reverted.revision;
                    state.targetFps = reverted.targetFps;
                    state.confidence = reverted.confidence;
                    state.nmsThreshold = reverted.nmsThreshold;
                    state.status = "profile rollback";
                    state.error = modelError;
                    fillModelState(&state, reverted);
                    {
                        std::lock_guard<std::mutex> lk(resultMutex);
                        latest = std::move(state);
                        resultHistory.clear();
                    }
                    std::cerr << "[NPU] profile switch failed: requested="
                              << npuPlateModelProfileKey(cfg.plateModelProfile)
                              << " reason=" << switchError << std::endl;
                    std::cerr << "[NPU] profile rollback ready: active="
                              << npuPlateModelProfileKey(rollbackProfile)
                              << std::endl;
                    return false;
                }
                error = switchError + "; rollback failed: " + rollbackError;
            } else {
                error = switchError;
            }

            modelError = error;
            modelSwitching.store(false);
            return false;
        }

        loadedTaskValue.store(static_cast<int>(cfg.task));
        if (cfg.task == NpuTaskType::PlateCascade) {
            loadedPlateProfileValue.store(
                        static_cast<int>(cfg.plateModelProfile));
            std::cerr << "[NPU] profile switch ready: active="
                      << npuPlateModelProfileKey(cfg.plateModelProfile)
                      << " postprocess="
                      << npuPlatePostprocessKey(cfg.platePostprocess)
                      << std::endl;
        }
        modelsLoaded.store(true);
        modelSwitching.store(false);
        fpsWindowMs = 0;
        fpsWindowCount = 0;
        currentFps = 0.0;
        modelError.clear();
        return true;
    }

    void submit(uint64_t frameId,
                const uint8_t* rgb565,
                std::size_t bytes,
                int width,
                int height,
                int sourceChannel,
                uint64_t sourceRevision,
                const NpuOriginalFrameView* originalView)
    {
        const NpuTaskConfig cfg = configSnapshot();
        if (!cfg.enabled || !running.load() || !rgb565 || width <= 0 || height <= 0) {
            return;
        }
        if (sourceChannel < 0 || sourceChannel > 9 ||
            sourceChannel != inputSourceChannel.load() ||
            sourceRevision != inputSourceRevision.load()) {
            return;
        }

        const std::size_t need = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 2u;
        if (bytes < need) {
            return;
        }

        // wantsFrame() 仅用于在RGA裁剪前做低成本预检查；真正的时间槽
        // 在数据与来源均验证通过后由这里原子占用，避免双重判断使用不同
        // 时间基准，也避免无效帧推进调度时间轴。
        if (!claimFrameSlot(cfg.targetFps, steadyNs())) {
            return;
        }

        FrameCopy copy;
        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            copy = std::move(spare);
            spare = FrameCopy();
        }
        copy.id = frameId;
        copy.sourceChannel = sourceChannel;
        copy.sourceRevision = sourceRevision;
        copy.width = width;
        copy.height = height;
        copy.originalWidth = 0;
        copy.originalHeight = 0;
        copy.originalCopyMs = 0.0;
        copy.originalData.clear();
        copy.originalCopyError.clear();
        const auto copyBegin = std::chrono::steady_clock::now();
        copy.data.resize(need);
        std::memcpy(copy.data.data(), rgb565, need);
        copy.copyMs = elapsedMs(copyBegin);

        const bool wantsOriginal = cfg.task == NpuTaskType::PlateCascade &&
                cfg.plateCropSource == NpuPlateCropSource::OriginalSource;
        if (wantsOriginal) {
            const auto originalCopyBegin = std::chrono::steady_clock::now();
            if (!originalView || !originalView->data) {
                copy.originalCopyError = "original_frame_missing";
            } else if (originalView->width <= 0 || originalView->height <= 0 ||
                       originalView->stridePixels < originalView->width ||
                       originalView->roiX < 0 || originalView->roiY < 0 ||
                       originalView->roiWidth <= 0 || originalView->roiHeight <= 0 ||
                       originalView->roiX + originalView->roiWidth > originalView->width ||
                       originalView->roiY + originalView->roiHeight > originalView->height) {
                copy.originalCopyError = "original_view_invalid";
            } else {
                const std::size_t requiredBytes =
                        static_cast<std::size_t>(originalView->stridePixels) *
                        static_cast<std::size_t>(originalView->height) * 2u;
                if (originalView->bytes < requiredBytes) {
                    copy.originalCopyError = "original_view_truncated";
                } else {
                    copy.originalWidth = originalView->roiWidth;
                    copy.originalHeight = originalView->roiHeight;
                    const std::size_t rowBytes =
                            static_cast<std::size_t>(copy.originalWidth) * 2u;
                    copy.originalData.resize(rowBytes *
                                             static_cast<std::size_t>(copy.originalHeight));
                    for (int row = 0; row < copy.originalHeight; ++row) {
                        const std::size_t srcOffset =
                                (static_cast<std::size_t>(originalView->roiY + row) *
                                 static_cast<std::size_t>(originalView->stridePixels) +
                                 static_cast<std::size_t>(originalView->roiX)) * 2u;
                        std::memcpy(copy.originalData.data() +
                                    static_cast<std::size_t>(row) * rowBytes,
                                    originalView->data + srcOffset,
                                    rowBytes);
                    }
                }
            }
            copy.originalCopyMs = elapsedMs(originalCopyBegin);
        }

        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            if (pendingValid) {
                droppedFrames.fetch_add(1);
                keepLargerSpareLocked(std::move(pending));
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
            try {
                process(frame);
            } catch (const std::exception& error) {
                const NpuTaskConfig cfg = configSnapshot();
                NpuFrameResult result;
                result.frameId = frame.id;
                result.sourceChannel = frame.sourceChannel;
                result.sourceRevision = frame.sourceRevision;
                result.ready = false;
                result.error = std::string("inference exception: ") + error.what();
                result.status = "inference exception";
                publish(std::move(result), cfg);
            } catch (...) {
                const NpuTaskConfig cfg = configSnapshot();
                NpuFrameResult result;
                result.frameId = frame.id;
                result.sourceChannel = frame.sourceChannel;
                result.sourceRevision = frame.sourceRevision;
                result.ready = false;
                result.error = "unknown inference exception";
                result.status = "inference exception";
                publish(std::move(result), cfg);
            }
            {
                std::lock_guard<std::mutex> lk(pendingMutex);
                keepLargerSpareLocked(std::move(frame));
            }
        }
    }

    void publish(NpuFrameResult result, const NpuTaskConfig& cfg)
    {
        const NpuTaskConfig current = configSnapshot();
        if (current.revision != cfg.revision ||
            result.sourceRevision != inputSourceRevision.load() ||
            result.sourceChannel != inputSourceChannel.load()) {
            return;
        }
        result.enabled = current.enabled;
        result.task = cfg.task;
        result.targetFps = cfg.targetFps;
        result.confidence = cfg.confidence;
        result.nmsThreshold = cfg.nmsThreshold;
        result.configRevision = cfg.revision;
        result.wrongWayRuleEnabled = cfg.wrongWayRuleEnabled;
        fillModelState(&result, current);
        result.processed = processed.load();
        result.dropped = droppedFrames.load();

        if (!current.enabled) {
            result.ready = false;
            result.fps = 0.0;
            result.status = "off";
            result.error.clear();
            result.plates.clear();
            result.pedestrians.clear();
            result.vehicles.clear();
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
        if (!cfg.enabled || frame.sourceChannel < 0 || frame.sourceChannel > 9 ||
            frame.sourceChannel != inputSourceChannel.load() ||
            frame.sourceRevision != inputSourceRevision.load()) {
            return;
        }

        NpuFrameResult result;
        result.frameId = frame.id;
        result.copyMs = frame.copyMs;
        result.originalFrameCopyMs = frame.originalCopyMs;
        result.sourceChannel = frame.sourceChannel;
        result.sourceRevision = frame.sourceRevision;
        result.task = cfg.task;
        result.targetFps = cfg.targetFps;
        result.confidence = cfg.confidence;
        result.nmsThreshold = cfg.nmsThreshold;
        result.configRevision = cfg.revision;
        result.actualPlateCropSource = NpuPlateCropSource::DetectorInput;
        result.originalSourceWidth = frame.originalWidth;
        result.originalSourceHeight = frame.originalHeight;

        if (!ensureModels(cfg)) {
            result.ready = false;
            result.error = modelError;
            result.status = "model error";
            publish(std::move(result), cfg);
            return;
        }

        if (cfg.task == NpuTaskType::PlateCascade &&
            activePlateConfigRevision != cfg.revision) {
            activePlateConfigRevision = cfg.revision;
            std::cerr << "[NPU] plate config active: profile="
                      << npuPlateModelProfileKey(cfg.plateModelProfile)
                      << " postprocess="
                      << npuPlatePostprocessKey(cfg.platePostprocess)
                      << " preprocess=" << (v291ForceCpu ? "cpu" : "auto")
                      << " crop_source="
                      << npuPlateCropSourceKey(cfg.plateCropSource)
                      << " revision=" << cfg.revision << std::endl;
        }

        const auto totalBegin = std::chrono::steady_clock::now();
        const auto preBegin = std::chrono::steady_clock::now();
        bool usedRga = false;
        demo::Mat input;
        if (cfg.task == NpuTaskType::VehicleWrongWay ||
            (cfg.task == NpuTaskType::PlateCascade &&
             plateBackend == PlateBackend::V291)) {
            input = demo::plate_v291::pre_img_rgb565_to_rgb(frame.data.data(),
                                                            frame.width,
                                                            frame.height,
                                                            &usedRga,
                                                            v291ForceCpu);
        } else {
            input = demo::vision::pre_img_rgb565_to_rgb(frame.data.data(),
                                                        frame.width,
                                                        frame.height,
                                                        &usedRga,
                                                        false);
        }
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
            platePostprocessor.resetForContext(cfg.revision,
                                               frame.sourceChannel,
                                               frame.sourceRevision);

            std::vector<demo::Mat> crops;
            std::vector<demo::Rect2f> plateBoxesIn;
            std::vector<int> plateColorIds;
            const bool wantsOriginalRoi =
                    cfg.plateCropSource == NpuPlateCropSource::OriginalSource;
            std::vector<demo::plate_v291::PlateCropDebug> cropDebugs;
            if (plateBackend == PlateBackend::V291) {
                demo::plate_v291::RknnStageTiming plateTiming;
                demo::plate_v291::detect_plate_candidates(input,
                                                           plateDet,
                                                           cfg.confidence,
                                                           cfg.nmsThreshold,
                                                           crops,
                                                           plateBoxesIn,
                                                           plateColorIds,
                                                           true,
                                                           &result.plateMs,
                                                           &plateTiming,
                                                           nullptr,
                                                           nullptr,
                                                           wantsOriginalRoi
                                                           ? &cropDebugs : nullptr);
                accumulateStageTiming(result, plateTiming);
            } else {
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
                accumulateStageTiming(result, plateTiming);
                normalizeLegacyPlateTypes(&plateColorIds);
            }

            if (wantsOriginalRoi) {
                if (frame.originalData.empty() || frame.originalWidth <= 0 ||
                    frame.originalHeight <= 0) {
                    result.originalRoiFallbackCount =
                            std::max(1, static_cast<int>(crops.size()));
                    result.originalRoiFallbackReason = frame.originalCopyError.empty()
                            ? "original_frame_missing" : frame.originalCopyError;
                } else {
                    result.actualPlateCropSource = NpuPlateCropSource::OriginalSource;
                    for (std::size_t i = 0; i < crops.size(); ++i) {
                        demo::Mat originalCrop;
                        std::string cropReason;
                        double convertMs = 0.0;
                        double warpMs = 0.0;
                        bool cropped = false;
                        if (plateBackend == PlateBackend::V291) {
                            if (i >= cropDebugs.size() ||
                                cropDebugs[i].warp_quad.size() != 4u) {
                                cropReason = "final_warp_quad_missing";
                            } else {
                                cropped = demo::plate_v291::crop_plate_from_original_rgb565(
                                    frame.originalData.data(),
                                    frame.originalData.size(),
                                    frame.originalWidth,
                                    frame.originalHeight,
                                    frame.width,
                                    frame.height,
                                    cropDebugs[i].warp_quad,
                                    originalCrop,
                                    &cropReason,
                                    &convertMs,
                                    &warpMs);
                            }
                        } else if (i < plateBoxesIn.size()) {
                            cropped = demo::vision::crop_plate_box_from_original_rgb565(
                                        frame.originalData.data(),
                                        frame.originalData.size(),
                                        frame.originalWidth,
                                        frame.originalHeight,
                                        frame.width,
                                        frame.height,
                                        plateBoxesIn[i],
                                        originalCrop,
                                        &cropReason,
                                        &convertMs,
                                        &warpMs);
                        } else {
                            cropReason = "legacy_original_box_missing";
                        }
                        result.originalRoiConvertMs += convertMs;
                        result.originalRoiWarpMs += warpMs;
                        if (cropped) {
                            crops[i] = std::move(originalCrop);
                            ++result.originalRoiCropCount;
                        } else {
                            ++result.originalRoiFallbackCount;
                            if (result.originalRoiFallbackReason.empty()) {
                                result.originalRoiFallbackReason = cropReason.empty()
                                        ? "original_crop_failed" : cropReason;
                            }
                        }
                    }
                    // 一张车牌都没能使用原图时，实际数据源仍是低分辨率回退链路。
                    if (!crops.empty() && result.originalRoiCropCount == 0) {
                        result.actualPlateCropSource =
                                NpuPlateCropSource::DetectorInput;
                    }
                }
            }

            const int64_t plateNowMs = steadyMs();

            // raw 模式完全旁路 OCR 缓存、轨迹 TTL、框 EMA 和文字投票，
            // 用于隔离评估模型及逐帧前后处理本身。
            if (cfg.platePostprocess == NpuPlatePostprocess::Raw) {
                const std::size_t rawCount = std::min(
                            kPlateMaxTracks,
                            std::min(crops.size(),
                                     std::min(plateBoxesIn.size(),
                                              plateColorIds.size())));
                std::vector<demo::Mat> rawCrops(crops.begin(),
                                                crops.begin() + rawCount);
                std::vector<demo::Rect2f> rawBoxes(plateBoxesIn.begin(),
                                                   plateBoxesIn.begin() + rawCount);
                std::vector<int> rawTypes(plateColorIds.begin(),
                                          plateColorIds.begin() + rawCount);
                std::vector<std::string> recognizedPlates;
                std::vector<std::string> recognizedPinyins;
                std::vector<demo::Rect2f> recognizedBoxes;
                std::vector<int> recognizedTypes;
                double ocrMs = 0.0;
                if (plateBackend == PlateBackend::V291) {
                    demo::plate_v291::RknnStageTiming ocrTiming;
                    demo::plate_v291::recognize_plate_crops(rawCrops,
                                                            rawBoxes,
                                                            rawTypes,
                                                            plateReco,
                                                            recognizedPlates,
                                                            recognizedPinyins,
                                                            recognizedBoxes,
                                                            recognizedTypes,
                                                            &ocrMs,
                                                            &ocrTiming);
                    accumulateStageTiming(result, ocrTiming);
                } else {
                    demo::vision::RknnStageTiming ocrTiming;
                    std::vector<int> legacyTypes = rawTypes;
                    denormalizeLegacyPlateTypes(&legacyTypes);
                    demo::vision::recognize_plate_crops(rawCrops,
                                                        rawBoxes,
                                                        legacyTypes,
                                                        plateReco,
                                                        recognizedPlates,
                                                        recognizedPinyins,
                                                        recognizedBoxes,
                                                        recognizedTypes,
                                                        &ocrMs,
                                                        &ocrTiming);
                    accumulateStageTiming(result, ocrTiming);
                    normalizeLegacyPlateTypes(&recognizedTypes);
                }
                result.plateMs += ocrMs;

                const std::size_t count = std::min(
                            recognizedPlates.size(), recognizedBoxes.size());
                for (std::size_t i = 0; i < count; ++i) {
                    if (!isDisplayablePlateText(recognizedPlates[i])) {
                        continue;
                    }
                    demo::Rect2f origBox;
                    convertPlateInputBoxToOrig(plateBackend,
                                               recognizedBoxes[i],
                                               &origBox,
                                               frame.width,
                                               frame.height);
                    const int typeId = i < recognizedTypes.size()
                            ? recognizedTypes[i] : plate_types::Unknown;
                    const std::string pinyin = i < recognizedPinyins.size()
                            ? recognizedPinyins[i] : std::string();
                    result.plates.push_back(makeBox(origBox,
                                                    -1.0f,
                                                    typeId,
                                                    recognizedPlates[i],
                                                    !pinyin.empty()
                                                    ? pinyin
                                                    : recognizedPlates[i]));
                }
            } else {
            std::vector<PlateTemporalCandidate> candidates;
            std::vector<demo::Mat> ocrCrops;
            std::vector<demo::Rect2f> ocrBoxes;
            std::vector<int> ocrColorIds;
            std::vector<int> ocrToCandidate;
            platePostprocessor.prepareOcr(crops,
                                          plateBoxesIn,
                                          plateColorIds,
                                          plateNowMs,
                                          &candidates,
                                          &ocrCrops,
                                          &ocrBoxes,
                                          &ocrColorIds,
                                          &ocrToCandidate);

            if (!ocrCrops.empty()) {
                std::vector<std::string> recognizedPlates;
                std::vector<std::string> recognizedPinyins;
                std::vector<demo::Rect2f> recognizedBoxes;
                std::vector<int> recognizedColors;
                double ocrMs = 0.0;
                if (plateBackend == PlateBackend::V291) {
                    demo::plate_v291::RknnStageTiming ocrTiming;
                    demo::plate_v291::recognize_plate_crops(ocrCrops,
                                                            ocrBoxes,
                                                            ocrColorIds,
                                                            plateReco,
                                                            recognizedPlates,
                                                            recognizedPinyins,
                                                            recognizedBoxes,
                                                            recognizedColors,
                                                            &ocrMs,
                                                            &ocrTiming);
                    accumulateStageTiming(result, ocrTiming);
                } else {
                    demo::vision::RknnStageTiming ocrTiming;
                    std::vector<int> legacyTypes = ocrColorIds;
                    denormalizeLegacyPlateTypes(&legacyTypes);
                    demo::vision::recognize_plate_crops(ocrCrops,
                                                        ocrBoxes,
                                                        legacyTypes,
                                                        plateReco,
                                                        recognizedPlates,
                                                        recognizedPinyins,
                                                        recognizedBoxes,
                                                        recognizedColors,
                                                        &ocrMs,
                                                        &ocrTiming);
                    accumulateStageTiming(result, ocrTiming);
                    normalizeLegacyPlateTypes(&recognizedColors);
                }
                result.plateMs += ocrMs;
                // recognize_plate_crops保持输入batch顺序，按计划索引回填候选。
                platePostprocessor.mergeOcrResults(recognizedPlates,
                                                   recognizedPinyins,
                                                   recognizedColors,
                                                   ocrToCandidate,
                                                   &candidates);
            }

            std::vector<PlateTemporalCandidate> stablePlates;
            platePostprocessor.stabilize(&candidates,
                                         plateBackend,
                                         frame.width,
                                         frame.height,
                                         plateNowMs,
                                         &stablePlates);
            for (const PlateTemporalCandidate& plate : stablePlates) {
                const std::string label = !plate.pinyin.empty()
                        ? plate.pinyin : plate.plate;
                // 车牌检测接口未返回最终检测分数，继续用负值表示不可用，
                // 避免UI伪造1.0；位置、文字和颜色均为时序稳定结果。
                result.plates.push_back(makeBox(plate.origBox,
                                                -1.0f,
                                                plate.colorId,
                                                plate.plate,
                                                label));
            }
            }
        } else if (cfg.task == NpuTaskType::VehicleWrongWay) {
            std::vector<demo::plate_v291::YoloObjectDetection> detections;
            demo::plate_v291::RknnStageTiming vehicleTiming;
            demo::plate_v291::detect_yolov8_objects(input,
                                                     vehicle,
                                                     4,
                                                     cfg.confidence,
                                                     cfg.nmsThreshold,
                                                     detections,
                                                     true,
                                                     300,
                                                     128,
                                                     &result.vehicleMs,
                                                     &vehicleTiming);
            accumulateStageTiming(result, vehicleTiming);
            for (const auto& detection : detections) {
                demo::Rect2f orig;
                demo::plate_v291::convert_input_box_to_orig(detection.input_box,
                                                             orig,
                                                             frame.width,
                                                             frame.height);
                static const char* labels[] = {"car", "bus", "van", "truck"};
                const char* label = detection.class_id >= 0 && detection.class_id < 4
                        ? labels[detection.class_id] : "vehicle";
                result.vehicles.push_back(makeBox(orig,
                                                  detection.score,
                                                  detection.class_id,
                                                  std::string(),
                                                  label));
            }

            if (cfg.wrongWayRuleEnabled) {
                VehicleDirectionConfig directionSnapshot;
                {
                    std::lock_guard<std::mutex> lk(wrongWayConfigMutex);
                    directionSnapshot = wrongWayConfig;
                }
                if (activeWrongWayRuleRevision != cfg.revision) {
                    wrongWayEngine.reset();
                    activeWrongWayRuleRevision = cfg.revision;
                }
                wrongWayEngine.setConfig(directionSnapshot);
                wrongWayEngine.process(frame.id,
                                       static_cast<std::uint64_t>(steadyMs()),
                                       static_cast<std::uint64_t>(systemMs()),
                                       frame.width,
                                       frame.height,
                                       frame.sourceChannel,
                                       frame.sourceRevision,
                                       &result.vehicles,
                                       &result);
            } else if (activeWrongWayRuleRevision != 0) {
                wrongWayEngine.reset();
                activeWrongWayRuleRevision = 0;
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
            accumulateStageTiming(result, pedTiming);
            for (std::size_t i = 0; i < pedBoxesIn.size(); ++i) {
                demo::Rect2f orig;
                demo::vision::convert_input_box_to_orig(pedBoxesIn[i], orig,
                                                        frame.width, frame.height);
                const float score = (i < pedScores.size()) ? pedScores[i] : 0.0f;
                // 原始行人模型页用于验证模型本身，只保留置信度和NMS结果。
                // 站立姿态几何过滤仅属于旧违法规则任务，不能污染模型评估。
                if (cfg.task == NpuTaskType::TrafficViolation &&
                    !isStandingPedestrianBox(orig)) {
                    continue;
                }
                result.pedestrians.push_back(
                            makeBox(orig, score, 0, std::string(), "person"));
            }

            if (cfg.task == NpuTaskType::TrafficViolation) {
                ViolationRuleConfig violationSnapshot;
                {
                    std::lock_guard<std::mutex> lk(violationConfigMutex);
                    violationSnapshot = violationConfig;
                }
                violationEngine.setConfig(violationSnapshot);
                violationEngine.process(frame.data.data(),
                                        frame.data.size(),
                                        frame.width,
                                        frame.height,
                                        frame.id,
                                        static_cast<std::uint64_t>(steadyMs()),
                                        static_cast<std::uint64_t>(systemMs()),
                                        &result.pedestrians,
                                        &result);
            }
        }

        const int processedBefore = processed.load();
        const int originalLogInterval = std::max(1, cfg.targetFps * 5);
        if (cfg.task == NpuTaskType::PlateCascade &&
            cfg.plateCropSource == NpuPlateCropSource::OriginalSource &&
            processedBefore % originalLogInterval == 0) {
            std::cerr << "[NPU] original ROI source="
                      << result.originalSourceWidth << "x"
                      << result.originalSourceHeight
                      << " actual="
                      << npuPlateCropSourceKey(result.actualPlateCropSource)
                      << " copy_ms=" << result.originalFrameCopyMs
                      << " convert_ms=" << result.originalRoiConvertMs
                      << " warp_ms=" << result.originalRoiWarpMs
                      << " crops=" << result.originalRoiCropCount
                      << " fallback=" << result.originalRoiFallbackCount;
            if (!result.originalRoiFallbackReason.empty()) {
                std::cerr << " reason=" << result.originalRoiFallbackReason;
            }
            std::cerr << std::endl;
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


void NpuInferenceWorker::setViolationConfig(const ViolationRuleConfig& config)
{
    m_impl->updateViolationConfig(config);
}

ViolationRuleConfig NpuInferenceWorker::violationConfig() const
{
    return m_impl->violationConfigSnapshot();
}

void NpuInferenceWorker::setWrongWayConfig(const VehicleDirectionConfig& config)
{
    m_impl->updateWrongWayConfig(config);
}

VehicleDirectionConfig NpuInferenceWorker::wrongWayConfig() const
{
    return m_impl->wrongWayConfigSnapshot();
}

void NpuInferenceWorker::setInputSource(int sourceChannel, uint64_t sourceRevision)
{
    m_impl->updateInputSource(sourceChannel, sourceRevision);
}

bool NpuInferenceWorker::wantsFrame() const
{
    return m_impl->wantsFrame();
}

void NpuInferenceWorker::submitFrame(uint64_t frameId,
                                     const uint8_t* rgb565,
                                     std::size_t bytes,
                                     int width,
                                     int height,
                                     int sourceChannel,
                                     uint64_t sourceRevision,
                                     const NpuOriginalFrameView *originalView)
{
    try {
        m_impl->submit(frameId, rgb565, bytes, width, height,
                       sourceChannel, sourceRevision, originalView);
    } catch (const std::exception& error) {
        m_impl->droppedFrames.fetch_add(1);
        const NpuTaskConfig cfg = m_impl->configSnapshot();
        NpuFrameResult result;
        result.frameId = frameId;
        result.sourceChannel = sourceChannel;
        result.sourceRevision = sourceRevision;
        result.ready = false;
        result.error = std::string("frame submit exception: ") + error.what();
        result.status = "frame submit exception";
        m_impl->publish(std::move(result), cfg);
    } catch (...) {
        m_impl->droppedFrames.fetch_add(1);
        const NpuTaskConfig cfg = m_impl->configSnapshot();
        NpuFrameResult result;
        result.frameId = frameId;
        result.sourceChannel = sourceChannel;
        result.sourceRevision = sourceRevision;
        result.ready = false;
        result.error = "unknown frame submit exception";
        result.status = "frame submit exception";
        m_impl->publish(std::move(result), cfg);
    }
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
    result.sourceChannel = m_impl->inputSourceChannel.load();
    result.sourceRevision = m_impl->inputSourceRevision.load();
    m_impl->fillModelState(&result, cfg);
    result.processed = m_impl->processed.load();
    result.dropped = m_impl->droppedFrames.load();
    if (!result.enabled) {
        result.ready = false;
        result.fps = 0.0;
        result.status = "off";
        result.error.clear();
        result.plates.clear();
        result.pedestrians.clear();
        result.vehicles.clear();
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
