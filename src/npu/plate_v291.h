#pragma once

#include "demo_types.h"
#include "plate_type_ids.h"

#include <cstddef>

namespace demo {
namespace plate_v291 {

// v2.9.1 候选模型的固定接口。与旧 768x448/五类车牌检测链路隔离，
// 避免为了测试新车牌模型而改变现有行人模型的输入契约。
constexpr int DET_INPUT_W = 640;
constexpr int DET_INPUT_H = 320;
constexpr int DET_PAD_VALUE = 114;
constexpr int PLATE_DET_CLASS_COUNT = 1;
constexpr bool DEFAULT_PLATE_QUAD_COMPENSATE = true;

constexpr int PLATE_TYPE_UNKNOWN = plate_types::Unknown;
constexpr int PLATE_TYPE_BLUE = plate_types::Blue;
constexpr int PLATE_TYPE_GREEN = plate_types::Green;
constexpr int PLATE_TYPE_YELLOW = plate_types::Yellow;
constexpr int PLATE_TYPE_WHITE = plate_types::White;
constexpr int PLATE_TYPE_BLACK = plate_types::Black;
constexpr int PLATE_TYPE_POLICE = plate_types::Police;
constexpr int PLATE_TYPE_CONSULATE = plate_types::Consulate;
constexpr int PLATE_TYPE_EMBASSY = plate_types::Embassy;
constexpr int PLATE_TYPE_HK = plate_types::HongKong;
constexpr int PLATE_TYPE_MACAU = plate_types::Macau;
constexpr int PLATE_TYPE_FOREIGN = plate_types::Foreign;

inline const std::vector<std::string>& chars() {
    static const std::vector<std::string> v = {
        "京","沪","津","渝","冀","晋","蒙","辽","吉","黑",
        "苏","浙","皖","闽","赣","鲁","豫","鄂","湘","粤",
        "桂","琼","川","贵","云","藏","陕","甘","青","宁",
        "新","警","领",
        "0","1","2","3","4","5","6","7","8","9",
        "A","B","C","D","E","F","G","H","I","J",
        "K","L","M","N","O","P","Q","R","S","T",
        "U","V","W","X","Y","Z","使","港","澳","-"
    };
    return v;
}

inline const std::vector<std::string>& pinyin_chars() {
    static const std::vector<std::string> p = {
        "JING","HU","JIN","YU","JI","JIN2","MENG","LIAO","JI2","HEI",
        "SU","ZHE","WAN","MIN","GAN","LU","YU2","E","XIANG","YUE",
        "GUI","QIONG","CHUAN","GUI2","YUN","ZANG","SHAN","GAN2","QING","NING",
        "XIN","JINGPOL","LINGCON",
        "0","1","2","3","4","5","6","7","8","9",
        "A","B","C","D","E","F","G","H","I","J",
        "K","L","M","N","O","P","Q","R","S","T",
        "U","V","W","X","Y","Z","SHI","GANG","AO","-"
    };
    return p;
}

// RKNN stage timing. input_ms is rknn_inputs_set/control overhead, run_ms is
// pure rknn_run time, output_ms is rknn_outputs_get, post_ms is decode/NMS/OCR.
struct RknnStageTiming {
    double input_ms = 0.0;
    double run_ms = 0.0;
    double output_ms = 0.0;
    double post_ms = 0.0;
    double quad_comp_ms = 0.0;
    double quad_comp_geom_ms = 0.0;
    double quad_comp_select_ms = 0.0;
    uint64_t quad_comp_attempt_count = 0;
    uint64_t quad_comp_apply_count = 0;
    uint64_t run_count = 0;

    void reset() {
        input_ms = 0.0;
        run_ms = 0.0;
        output_ms = 0.0;
        post_ms = 0.0;
        quad_comp_ms = 0.0;
        quad_comp_geom_ms = 0.0;
        quad_comp_select_ms = 0.0;
        quad_comp_attempt_count = 0;
        quad_comp_apply_count = 0;
        run_count = 0;
    }
};

struct RknnRunInterval {
    uint64_t seq = 0;
    int branch = 0;  // 1: plate detect, 2: plate OCR, 3: pedestrian detect, 4: generic object detect.
    double start_ms = 0.0;
    double end_ms = 0.0;
};

struct YoloObjectDetection {
    Rect2f input_box;
    float score = 0.0f;
    int class_id = -1;
};

struct PlateCropDebug {
    // Shared crop diagnostics. Real-time only aggregates the timing/counter
    // fields; one-shot image tests serialize the full structure to JSON.
    std::vector<Point2f> raw_quad;
    std::vector<Point2f> pre_comp_warp_quad;
    std::vector<Point2f> warp_quad;
    std::string crop_method = "none";
    bool quad_reliable = false;
    std::string quad_error_reason;
    bool quad_comp_attempted = false;
    bool quad_compensated = false;
    std::string quad_comp_formula_version = "none";
    std::string quad_comp_method = "none";
    std::string quad_comp_reason;
    float quad_comp_shear = 0.0f;
    float quad_comp_confidence = 0.0f;
    float quad_comp_target_shear = 0.0f;
    float quad_comp_current_shear = 0.0f;
    float quad_comp_raw_delta = 0.0f;
    float quad_comp_roll_shear = 0.0f;
    float quad_comp_roll_left_delta = 0.0f;
    float quad_comp_roll_right_delta = 0.0f;
    float quad_comp_view_shear = 0.0f;
    float quad_comp_perspective_weight = 0.0f;
    float quad_comp_delta_shear = 0.0f;
    float quad_comp_delta_direction = 0.0f;
    float quad_comp_left_dx = 0.0f;
    float quad_comp_right_dx = 0.0f;
    float quad_comp_dx_tl = 0.0f;
    float quad_comp_dx_tr = 0.0f;
    float quad_comp_dx_br = 0.0f;
    float quad_comp_dx_bl = 0.0f;
    float quad_comp_gain = 0.0f;
    float quad_comp_roll_score = 0.0f;
    float quad_comp_persp_score = 0.0f;
    float quad_comp_persp_apply = 0.0f;
    float quad_comp_pure_persp_score = 0.0f;
    float quad_comp_left_gain = 0.0f;
    float quad_comp_right_gain = 0.0f;
    float quad_comp_missing_shear = 0.0f;
    float quad_comp_missing_px_norm = 0.0f;
    float quad_comp_need_score = 0.0f;
    float quad_comp_geometry_quality = 0.0f;
    float quad_comp_gain_raw = 0.0f;
    float quad_comp_coverage = 0.0f;
    float quad_comp_coverage_scale = 0.0f;
    float quad_comp_over_risk = 0.0f;
    float quad_comp_effective_gain = 0.0f;
    float quad_comp_effective_max_point_dx = 0.0f;
    float quad_comp_under_planned_dx = 0.0f;
    float quad_comp_under_move_score = 0.0f;
    float quad_comp_under_evidence_score = 0.0f;
    float quad_comp_under_deficit_score = 0.0f;
    float quad_comp_under_target_dx = 0.0f;
    float quad_comp_under_boost = 1.0f;
    float quad_comp_deficit_ratio = 0.0f;
    float quad_comp_horizontal_angle_deg = 0.0f;
    float quad_comp_current_left_shear = 0.0f;
    float quad_comp_current_right_shear = 0.0f;
    float quad_comp_diag1 = 0.0f;
    float quad_comp_diag2 = 0.0f;
    float quad_comp_diag_ratio = 0.0f;
    float quad_comp_diag_rel = 0.0f;
    float quad_comp_signed_diag_rel = 0.0f;
    float quad_comp_tb_rel = 0.0f;
    float quad_comp_signed_tb_rel = 0.0f;
    float quad_comp_signed_lr_rel = 0.0f;
    float quad_comp_short_rel = 0.0f;
    float quad_comp_perspective_strength = 0.0f;
    float quad_comp_diag_delta = 0.0f;
    float quad_comp_base_delta = 0.0f;
    float quad_comp_final_delta = 0.0f;
    float quad_comp_base_left_delta = 0.0f;
    float quad_comp_base_right_delta = 0.0f;
    float quad_comp_fit_left_delta = 0.0f;
    float quad_comp_fit_right_delta = 0.0f;
    float quad_comp_fit_gain = 1.0f;
    float quad_comp_under_weight = 0.0f;
    float quad_comp_roll_floor_weight = 0.0f;
    float quad_comp_fit_quality = 0.0f;
    float quad_comp_projection_persp_lock_raw = 0.0f;
    float quad_comp_projection_persp_lock = 0.0f;
    float quad_comp_rotation_confidence = 0.0f;
    float quad_comp_roll_residual_evidence = 0.0f;
    float quad_comp_high_roll_unlock = 0.0f;
    float quad_comp_horizontal_reliability = 0.0f;
    float quad_comp_rotation_unlock_evidence = 0.0f;
    float quad_comp_diag_mid_norm = 0.0f;
    float quad_comp_affine_shape_score = 0.0f;
    float quad_comp_trapezoid_score = 0.0f;
    float quad_comp_affine_rotation_unlock = 0.0f;
    float quad_comp_affine_floor_weight = 0.0f;
    float quad_comp_tilt_share = 0.0f;
    float quad_comp_safety_scale = 0.0f;
    float quad_comp_move_norm = 0.0f;
    std::string quad_comp_fit_model_version = "none";
    std::string quad_comp_delta_source = "none";
    std::string quad_comp_view_case = "none";
    std::string quad_comp_selected_level = "raw";
    // Legacy compatibility fields. geometry_corner_dx_v2_9_1 no longer runs
    // crop-candidate image scoring, so quality values stay at -1 and
    // quad_comp_select_ms stays at 0.
    int quad_comp_candidate_count = 0;
    float quad_comp_quality_raw = -1.0f;
    float quad_comp_quality_weak = -1.0f;
    float quad_comp_quality_medium = -1.0f;
    float quad_comp_quality_strong = -1.0f;
    float quad_comp_quality_margin = 0.0f;
    double quad_comp_geom_ms = 0.0;
    double quad_comp_select_ms = 0.0;
    double quad_comp_ms = 0.0;
    std::vector<std::string> quad_comp_candidate_levels;
    std::vector<float> quad_comp_candidate_qualities;
    std::vector<std::vector<Point2f>> quad_comp_candidate_quads;
};

struct PlateOcrDebug {
    std::vector<int> raw_tokens;
    std::vector<int> ctc_tokens;
    std::string token_text;
    int num_chars = 0;
    int seq_len = 0;
};

// Collect completed rknn_run intervals with sequence id greater than after_seq.
void collect_rknn_run_intervals(uint64_t after_seq,
                                uint64_t* latest_seq,
                                std::vector<RknnRunInterval>* out);

void set_plate_quad_compensation_enabled(bool enabled);
bool plate_quad_compensation_enabled();

// Letterbox resize to detector input size.
Mat pre_img(const Mat& img);

// Build RGB detector input directly from the 1080p RGB565 capture buffer.
Mat pre_img_rgb565_to_rgb(const uint8_t* rgb565,
                          int width,
                          int height,
                          bool* used_rga = nullptr,
                          bool force_cpu = false);

// Current preprocessing route name for console/OSD diagnostics.
std::string current_preproc_mode_name(bool force_cpu = false);

// Map a detector-input box back to original image coordinates.
void convert_input_box_to_orig(const Rect2f& in_box, Rect2f& out_box, int ow, int oh);

// Map detector-input keypoints back to original image coordinates.
void convert_input_points_to_orig(const std::vector<Point2f>& in_points,
                                  std::vector<Point2f>& out_points,
                                  int ow,
                                  int oh);

// Crop an OCR plate image from the original BGR image using detector-input geometry.
Mat crop_plate_from_original_bgr(const Mat& orig_bgr,
                                 const Rect2f& input_box,
                                 const std::vector<Point2f>& input_points,
                                 PlateCropDebug* debug = nullptr,
                                 Size shape = Size(94, 24));

// 将已经完成排序/可靠性检查/x补偿的检测输入四点映射到原始RGB565选路图，
// 只转换局部包围区域并直接透视生成OCR所需BGR图。此接口不会再次补偿角点。
bool crop_plate_from_original_rgb565(const uint8_t* source_rgb565,
                                     std::size_t source_bytes,
                                     int source_width,
                                     int source_height,
                                     int detector_frame_width,
                                     int detector_frame_height,
                                     const std::vector<Point2f>& final_input_quad,
                                     Mat& crop_bgr,
                                     std::string* reason = nullptr,
                                     double* convert_ms = nullptr,
                                     double* warp_ms = nullptr,
                                     Size shape = Size(94, 24));

// Generic YOLOv8 object detector for DFL raw heads and noDFL decoded heads.
bool detect_yolov8_objects(const Mat& mid_img,
                           RknnModel& model,
                           int class_count,
                           float conf_thres,
                           float iou_thres,
                           std::vector<YoloObjectDetection>& detections,
                           bool input_is_rgb = false,
                           int topk_decode = DEFAULT_PED_TOPK_DECODE,
                           int topk_nms = DEFAULT_PED_TOPK_NMS,
                           double* npu_ms = nullptr,
                           RknnStageTiming* timing = nullptr);

// Stage 1: plate detection, geometry decode, NMS and perspective crops.
// color_ids keeps the legacy parameter name; v2.1 fills business plate type ids
// and detector candidates start as PLATE_TYPE_UNKNOWN before OCR postprocess.
bool detect_plate_candidates(const Mat& mid_img,
                             RknnModel& det_model,
                             float conf_thres,
                             float iou_thres,
                             std::vector<Mat>& crops,
                             std::vector<Rect2f>& boxes,
                             std::vector<int>& color_ids,
                             bool input_is_rgb = false,
                             double* npu_ms = nullptr,
                             RknnStageTiming* timing = nullptr,
                             std::vector<float>* scores = nullptr,
                             std::vector<std::vector<Point2f>>* keypoints = nullptr,
                             std::vector<PlateCropDebug>* crop_debugs = nullptr,
                             std::vector<float>* raw_output = nullptr);

// Stage 2: LPRNet OCR for plate crops. out_color_ids are business plate type ids.
bool recognize_plate_crops(const std::vector<Mat>& crops,
                           const std::vector<Rect2f>& in_boxes,
                           const std::vector<int>& in_color_ids,
                           RknnModel& reco_model,
                           std::vector<std::string>& plates,
                           std::vector<std::string>& pinyins,
                           std::vector<Rect2f>& out_boxes,
                           std::vector<int>& out_color_ids,
                           double* npu_ms = nullptr,
                           RknnStageTiming* timing = nullptr,
                           std::vector<PlateOcrDebug>* ocr_debugs = nullptr,
                           std::vector<float>* raw_output = nullptr);

// Run pedestrian detection on one preprocessed detector input frame.
bool detect_pedestrian(const Mat& mid_img,
                       RknnModel& ped_model,
                       int ped_classes,
                       float conf_thres,
                       float iou_thres,
                       std::vector<Rect2f>& boxes,
                       std::vector<float>& scores,
                       bool input_is_rgb = false,
                       int topk_decode = DEFAULT_PED_TOPK_DECODE,
                       int topk_nms = DEFAULT_PED_TOPK_NMS,
                       double* npu_ms = nullptr,
                       RknnStageTiming* timing = nullptr);

// Estimate traffic-light state in the configured ROI.
TrafficLightState detect_light_state(const Mat& frame, const Rect& light_roi_raw);

// Geometric overlap area between a pedestrian box and the crosswalk polygon.
float quad_intersection_area(const Rect2f& box, const std::vector<Point>& quad);

std::string light_text(TrafficLightState s);
std::string ped_text(PedStatus s);
Scalar ped_color(PedStatus s);

}  // namespace plate_v291
}  // namespace demo
