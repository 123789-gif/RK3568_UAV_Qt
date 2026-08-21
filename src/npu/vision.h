#pragma once

#include "demo_types.h"

namespace demo {
namespace vision {

// RKNN stage timing. input_ms is rknn_inputs_set/control overhead, run_ms is
// pure rknn_run time, output_ms is rknn_outputs_get, post_ms is decode/NMS/OCR.
struct RknnStageTiming {
    double input_ms = 0.0;
    double run_ms = 0.0;
    double output_ms = 0.0;
    double post_ms = 0.0;
    uint64_t run_count = 0;

    void reset() {
        input_ms = 0.0;
        run_ms = 0.0;
        output_ms = 0.0;
        post_ms = 0.0;
        run_count = 0;
    }
};

struct RknnRunInterval {
    uint64_t seq = 0;
    int branch = 0;  // 1: plate detect, 2: plate OCR, 3: pedestrian detect, 4: vehicle/generic detect.
    double start_ms = 0.0;
    double end_ms = 0.0;
};

struct YoloObjectDetection {
    Rect2f input_box;
    float score = 0.0f;
    int class_id = -1;
};

// Collect completed rknn_run intervals with sequence id greater than after_seq.
void collect_rknn_run_intervals(uint64_t after_seq,
                                uint64_t* latest_seq,
                                std::vector<RknnRunInterval>* out);

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

// Legacy检测器不输出稳定四点。将其检测输入矩形严格回映射到原始选路图，
// 从RGB565局部转换并缩放为94x24 OCR输入；不会伪造透视角点。
bool crop_plate_box_from_original_rgb565(const uint8_t* source_rgb565,
                                         std::size_t source_bytes,
                                         int source_width,
                                         int source_height,
                                         int detector_frame_width,
                                         int detector_frame_height,
                                         const Rect2f& input_box,
                                         Mat& crop_bgr,
                                         std::string* reason = nullptr,
                                         double* convert_ms = nullptr,
                                         double* resize_ms = nullptr,
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
bool detect_plate_candidates(const Mat& mid_img,
                             RknnModel& det_model,
                             float conf_thres,
                             float iou_thres,
                             std::vector<Mat>& crops,
                             std::vector<Rect2f>& boxes,
                             std::vector<int>& color_ids,
                             bool input_is_rgb = false,
                             double* npu_ms = nullptr,
                             RknnStageTiming* timing = nullptr);

// Stage 2: LPRNet OCR for plate crops.
bool recognize_plate_crops(const std::vector<Mat>& crops,
                           const std::vector<Rect2f>& in_boxes,
                           const std::vector<int>& in_color_ids,
                           RknnModel& reco_model,
                           std::vector<std::string>& plates,
                           std::vector<std::string>& pinyins,
                           std::vector<Rect2f>& out_boxes,
                           std::vector<int>& out_color_ids,
                           double* npu_ms = nullptr,
                           RknnStageTiming* timing = nullptr);

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

}  // namespace vision
}  // namespace demo
