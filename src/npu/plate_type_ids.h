#pragma once

// Qt 展示层和时序稳定层统一使用的业务车牌类型编号。
// 旧五类检测模型的类别在 npu_inference_worker.cpp 中映射到这里，
// v2.9.1 单类检测 + OCR/颜色后处理则直接产生这些编号。
namespace plate_types {

constexpr int Unknown = 0;
constexpr int Blue = 1;
constexpr int Green = 2;
constexpr int Yellow = 3;
constexpr int White = 4;
constexpr int Black = 5;
constexpr int Police = 6;
constexpr int Consulate = 7;
constexpr int Embassy = 8;
constexpr int HongKong = 9;
constexpr int Macau = 10;
constexpr int Foreign = 11;
constexpr int Count = 12;

}  // namespace plate_types
