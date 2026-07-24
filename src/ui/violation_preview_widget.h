#pragma once

#include "npu/npu_result_types.h"
#include "npu/violation_rule_config.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <vector>

class ViolationPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    enum class EditMode
    {
        None,
        TrafficLight,
        Crosswalk
    };

    explicit ViolationPreviewWidget(QWidget* parent = nullptr);

    void setFrame(const QImage& frame);
    void setConfig(const ViolationRuleConfig& config);
    ViolationRuleConfig config() const;
    void setResult(const NpuFrameResult& result);

    void beginTrafficLightEdit();
    void beginCrosswalkEdit();
    void cancelEdit();
    void undoLastPoint();
    void clearRegions();

signals:
    void configEdited(ViolationRuleConfig config);
    void statusMessage(QString message);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF imageTargetRect() const;
    bool widgetToNormalized(const QPointF& widgetPoint, QPointF* normalized) const;
    QPointF normalizedToWidget(const ViolationPointF& point) const;
    QRectF normalizedRectToWidget(float x, float y, float w, float h) const;
    void publishEditedConfig(const QString& message);

private:
    QImage m_frame;
    ViolationRuleConfig m_config;
    NpuFrameResult m_result;
    EditMode m_editMode = EditMode::None;
    bool m_dragging = false;
    QPointF m_dragStartNorm;
    QPointF m_dragCurrentNorm;
    std::vector<QPointF> m_pendingCrosswalk;
};
