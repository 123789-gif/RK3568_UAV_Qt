#include "ui/violation_preview_widget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

QColor lightColor(NpuTrafficLightState state)
{
    switch (state) {
    case NpuTrafficLightState::Red:
        return QColor(255, 70, 70);
    case NpuTrafficLightState::Green:
        return QColor(50, 230, 110);
    case NpuTrafficLightState::Unknown:
    default:
        return QColor(80, 190, 255);
    }
}

QColor pedestrianColor(NpuPedestrianState state)
{
    switch (state) {
    case NpuPedestrianState::RedAccumulating:
        return QColor(255, 210, 40);
    case NpuPedestrianState::RedViolation:
        return QColor(255, 60, 60);
    case NpuPedestrianState::Normal:
    default:
        return QColor(45, 230, 115);
    }
}

} // namespace

ViolationPreviewWidget::ViolationPreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("violationPreview"));
    setMinimumSize(460, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void ViolationPreviewWidget::setFrame(const QImage& frame)
{
    m_frame = frame;
    update();
}

void ViolationPreviewWidget::setConfig(const ViolationRuleConfig& config)
{
    m_config = sanitizeViolationRuleConfig(config);
    update();
}

ViolationRuleConfig ViolationPreviewWidget::config() const
{
    return m_config;
}

void ViolationPreviewWidget::setResult(const NpuFrameResult& result)
{
    m_result = result;
    update();
}

void ViolationPreviewWidget::beginTrafficLightEdit()
{
    m_editMode = EditMode::TrafficLight;
    m_dragging = false;
    m_pendingCrosswalk.clear();
    emit statusMessage(QStringLiteral("请在预览画面中拖动矩形，框选交通信号灯区域"));
    update();
}

void ViolationPreviewWidget::beginCrosswalkEdit()
{
    m_editMode = EditMode::Crosswalk;
    m_dragging = false;
    m_pendingCrosswalk.clear();
    emit statusMessage(QStringLiteral("请依次点击斑马线区域的4个顶点"));
    update();
}

void ViolationPreviewWidget::cancelEdit()
{
    m_editMode = EditMode::None;
    m_dragging = false;
    m_pendingCrosswalk.clear();
    emit statusMessage(QStringLiteral("已退出区域编辑"));
    update();
}

void ViolationPreviewWidget::undoLastPoint()
{
    if (m_editMode == EditMode::Crosswalk && !m_pendingCrosswalk.empty()) {
        m_pendingCrosswalk.pop_back();
        emit statusMessage(QStringLiteral("已撤销上一个斑马线顶点，当前%1/4")
                           .arg(static_cast<int>(m_pendingCrosswalk.size())));
        update();
        return;
    }

    if (m_editMode == EditMode::TrafficLight && m_config.lightRoiValid) {
        m_config.lightRoiValid = false;
        ++m_config.revision;
        publishEditedConfig(QStringLiteral("已清除交通信号灯区域"));
        return;
    }

    if (m_config.crosswalkValid) {
        m_config.crosswalkValid = false;
        ++m_config.revision;
        publishEditedConfig(QStringLiteral("已清除斑马线区域"));
    }
}

void ViolationPreviewWidget::clearRegions()
{
    m_config.lightRoiValid = false;
    m_config.crosswalkValid = false;
    ++m_config.revision;
    m_pendingCrosswalk.clear();
    m_dragging = false;
    publishEditedConfig(QStringLiteral("已清除全部违法检测区域"));
}

QRectF ViolationPreviewWidget::imageTargetRect() const
{
    if (m_frame.isNull() || width() <= 0 || height() <= 0) {
        return QRectF(rect());
    }
    const QSizeF sourceSize(m_frame.size());
    QSizeF targetSize = sourceSize;
    targetSize.scale(size(), Qt::KeepAspectRatio);
    const QPointF topLeft((width() - targetSize.width()) * 0.5,
                          (height() - targetSize.height()) * 0.5);
    return QRectF(topLeft, targetSize);
}

bool ViolationPreviewWidget::widgetToNormalized(const QPointF& widgetPoint,
                                                 QPointF* normalized) const
{
    if (!normalized) {
        return false;
    }
    const QRectF target = imageTargetRect();
    if (!target.contains(widgetPoint) || target.width() <= 0.0 || target.height() <= 0.0) {
        return false;
    }
    normalized->setX(std::max(0.0, std::min(1.0,
                         (widgetPoint.x() - target.left()) / target.width())));
    normalized->setY(std::max(0.0, std::min(1.0,
                         (widgetPoint.y() - target.top()) / target.height())));
    return true;
}

QPointF ViolationPreviewWidget::normalizedToWidget(const ViolationPointF& point) const
{
    const QRectF target = imageTargetRect();
    return QPointF(target.left() + point.x * target.width(),
                   target.top() + point.y * target.height());
}

QRectF ViolationPreviewWidget::normalizedRectToWidget(float x,
                                                       float y,
                                                       float w,
                                                       float h) const
{
    const QRectF target = imageTargetRect();
    return QRectF(target.left() + x * target.width(),
                  target.top() + y * target.height(),
                  w * target.width(),
                  h * target.height());
}

void ViolationPreviewWidget::publishEditedConfig(const QString& message)
{
    m_config = sanitizeViolationRuleConfig(m_config);
    emit configEdited(m_config);
    emit statusMessage(message);
    update();
}

void ViolationPreviewWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(0, 0, 0));

    const QRectF target = imageTargetRect();
    if (!m_frame.isNull()) {
        painter.drawImage(target, m_frame);
    } else {
        painter.setPen(QColor(150, 165, 180));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("请先启动PCIe视频，等待预览帧"));
    }

    painter.save();
    painter.setClipRect(target);

    if (m_config.lightRoiValid) {
        QPen lightPen(lightColor(m_result.trafficLight), 3.0);
        painter.setPen(lightPen);
        painter.setBrush(Qt::NoBrush);
        const QRectF roi = normalizedRectToWidget(m_config.lightX,
                                                   m_config.lightY,
                                                   m_config.lightW,
                                                   m_config.lightH);
        painter.drawRect(roi);
        painter.drawText(roi.topLeft() + QPointF(4.0, 18.0), QStringLiteral("信号灯"));
    }

    if (m_config.crosswalkValid) {
        QPolygonF polygon;
        for (const ViolationPointF& point : m_config.crosswalk) {
            polygon << normalizedToWidget(point);
        }
        painter.setPen(QPen(QColor(255, 215, 45), 3.0));
        painter.setBrush(QColor(255, 215, 45, 35));
        painter.drawPolygon(polygon);
    }

    if ((m_result.task == NpuTaskType::Pedestrian ||
         m_result.task == NpuTaskType::TrafficViolation) && m_result.ready) {
        for (const NpuBox& box : m_result.pedestrians) {
            const QRectF pedestrian(target.left() + box.x / 640.0 * target.width(),
                                    target.top() + box.y / 360.0 * target.height(),
                                    box.w / 640.0 * target.width(),
                                    box.h / 360.0 * target.height());
            const bool rawModel = m_result.task == NpuTaskType::Pedestrian;
            painter.setPen(QPen(rawModel ? QColor(45, 230, 115)
                                        : pedestrianColor(box.pedestrianState), 3.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(pedestrian);
            painter.drawText(pedestrian.topLeft() + QPointF(2.0, 18.0),
                             rawModel
                             ? QStringLiteral("PED %1").arg(box.score, 0, 'f', 2)
                             : QStringLiteral("ID %1").arg(box.trackId));
        }
    }

    if (m_editMode == EditMode::TrafficLight && m_dragging) {
        const float x = static_cast<float>(std::min(m_dragStartNorm.x(), m_dragCurrentNorm.x()));
        const float y = static_cast<float>(std::min(m_dragStartNorm.y(), m_dragCurrentNorm.y()));
        const float w = static_cast<float>(std::abs(m_dragCurrentNorm.x() - m_dragStartNorm.x()));
        const float h = static_cast<float>(std::abs(m_dragCurrentNorm.y() - m_dragStartNorm.y()));
        painter.setPen(QPen(QColor(80, 190, 255), 2.0, Qt::DashLine));
        painter.setBrush(QColor(80, 190, 255, 25));
        painter.drawRect(normalizedRectToWidget(x, y, w, h));
    }

    if (m_editMode == EditMode::Crosswalk && !m_pendingCrosswalk.empty()) {
        QPolygonF pending;
        for (const QPointF& normalized : m_pendingCrosswalk) {
            ViolationPointF point;
            point.x = static_cast<float>(normalized.x());
            point.y = static_cast<float>(normalized.y());
            pending << normalizedToWidget(point);
        }
        painter.setPen(QPen(QColor(255, 215, 45), 2.0, Qt::DashLine));
        if (pending.size() > 1) {
            painter.drawPolyline(pending);
        }
        painter.setBrush(QColor(255, 215, 45));
        for (const QPointF& point : pending) {
            painter.drawEllipse(point, 5.0, 5.0);
        }
    }

    painter.restore();

    painter.setPen(QColor(225, 235, 245));
    QString modeText;
    if (m_editMode == EditMode::TrafficLight) {
        modeText = QStringLiteral("编辑模式：拖动交通信号灯矩形");
    } else if (m_editMode == EditMode::Crosswalk) {
        modeText = QStringLiteral("编辑模式：点击斑马线顶点 %1/4")
                .arg(static_cast<int>(m_pendingCrosswalk.size()));
    }
    if (!modeText.isEmpty()) {
        painter.fillRect(QRectF(8, 8, 330, 30), QColor(0, 0, 0, 170));
        painter.drawText(QRectF(14, 8, 320, 30), Qt::AlignVCenter | Qt::AlignLeft, modeText);
    }
}

void ViolationPreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    QPointF normalized;
    if (!widgetToNormalized(event->localPos(), &normalized)) {
        return;
    }

    if (m_editMode == EditMode::TrafficLight) {
        m_dragging = true;
        m_dragStartNorm = normalized;
        m_dragCurrentNorm = normalized;
        update();
        return;
    }

    if (m_editMode == EditMode::Crosswalk) {
        m_pendingCrosswalk.push_back(normalized);
        if (m_pendingCrosswalk.size() == 4) {
            for (int i = 0; i < 4; ++i) {
                m_config.crosswalk[static_cast<std::size_t>(i)].x =
                        static_cast<float>(m_pendingCrosswalk[static_cast<std::size_t>(i)].x());
                m_config.crosswalk[static_cast<std::size_t>(i)].y =
                        static_cast<float>(m_pendingCrosswalk[static_cast<std::size_t>(i)].y());
            }
            m_config.crosswalkValid = true;
            ++m_config.revision;
            m_pendingCrosswalk.clear();
            m_editMode = EditMode::None;
            publishEditedConfig(QStringLiteral("斑马线区域已更新，点击“保存配置”写入文件"));
        } else {
            emit statusMessage(QStringLiteral("斑马线顶点：%1/4")
                               .arg(static_cast<int>(m_pendingCrosswalk.size())));
            update();
        }
        return;
    }

    QWidget::mousePressEvent(event);
}

void ViolationPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_editMode == EditMode::TrafficLight && m_dragging) {
        QPointF normalized;
        if (widgetToNormalized(event->localPos(), &normalized)) {
            m_dragCurrentNorm = normalized;
            update();
        }
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ViolationPreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton &&
        m_editMode == EditMode::TrafficLight && m_dragging) {
        QPointF normalized;
        if (widgetToNormalized(event->localPos(), &normalized)) {
            m_dragCurrentNorm = normalized;
        }
        m_dragging = false;

        const float x = static_cast<float>(std::min(m_dragStartNorm.x(), m_dragCurrentNorm.x()));
        const float y = static_cast<float>(std::min(m_dragStartNorm.y(), m_dragCurrentNorm.y()));
        const float w = static_cast<float>(std::abs(m_dragCurrentNorm.x() - m_dragStartNorm.x()));
        const float h = static_cast<float>(std::abs(m_dragCurrentNorm.y() - m_dragStartNorm.y()));
        if (w >= 0.01f && h >= 0.01f) {
            m_config.lightX = x;
            m_config.lightY = y;
            m_config.lightW = w;
            m_config.lightH = h;
            m_config.lightRoiValid = true;
            ++m_config.revision;
            m_editMode = EditMode::None;
            publishEditedConfig(QStringLiteral("交通信号灯区域已更新，点击“保存配置”写入文件"));
        } else {
            emit statusMessage(QStringLiteral("交通信号灯区域过小，请重新拖动"));
            update();
        }
        return;
    }
    QWidget::mouseReleaseEvent(event);
}
