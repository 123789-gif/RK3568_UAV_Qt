#pragma once

#include "npu/npu_result_types.h"
#include "npu/vehicle_direction_config.h"

#include <QImage>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <vector>

class WrongWayPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WrongWayPreviewWidget(QWidget* parent = nullptr);

    void setFrame(const QImage& frame);
    void setConfig(const VehicleDirectionConfig& config);
    VehicleDirectionConfig config() const;
    void setResult(const NpuFrameResult& result);
    int selectedLane() const;

public slots:
    void addLane();
    void deleteSelectedLane();
    void beginLaneQuadEdit();
    void beginDirectionEdit();
    void undoLastPoint();
    void clearAllLanes();
    void setSelectedLane(int laneIndex);

signals:
    void configEdited(VehicleDirectionConfig config);
    void statusMessage(QString message);
    void selectedLaneChanged(int laneIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    enum class EditMode { None, Quad, Direction };

    QRectF imageTargetRect() const;
    bool widgetToNormalized(const QPointF& widgetPoint, QPointF* normalized) const;
    QPointF normalizedToWidget(const VehicleDirectionPointF& point) const;
    void publishConfig(const QString& message);

    QImage m_frame;
    VehicleDirectionConfig m_config;
    NpuFrameResult m_result;
    int m_selectedLane = -1;
    EditMode m_editMode = EditMode::None;
    std::vector<QPointF> m_pendingQuad;
    bool m_draggingDirection = false;
    QPointF m_directionStart;
    QPointF m_directionEnd;
};
