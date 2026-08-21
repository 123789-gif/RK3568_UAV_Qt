#include "ui/wrong_way_preview_widget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

QColor laneColor(int lane)
{
    static const QColor colors[] = {
        QColor(0, 220, 255), QColor(255, 70, 220),
        QColor(255, 190, 0), QColor(80, 220, 100)
    };
    return colors[std::max(0, lane) % 4];
}

QColor vehicleColor(NpuVehicleDirectionState state)
{
    switch (state) {
    case NpuVehicleDirectionState::WrongWay: return QColor(255, 55, 55);
    case NpuVehicleDirectionState::Normal: return QColor(45, 230, 115);
    case NpuVehicleDirectionState::Stopped: return QColor(255, 190, 35);
    case NpuVehicleDirectionState::Unknown:
    default: return QColor(70, 190, 255);
    }
}

QString vehicleStateText(NpuVehicleDirectionState state)
{
    switch (state) {
    case NpuVehicleDirectionState::WrongWay: return QStringLiteral("WRONG");
    case NpuVehicleDirectionState::Normal: return QStringLiteral("OK");
    case NpuVehicleDirectionState::Stopped: return QStringLiteral("STOP");
    case NpuVehicleDirectionState::Unknown:
    default: return QString();
    }
}

} // namespace

WrongWayPreviewWidget::WrongWayPreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("wrongWayPreview"));
    setMinimumSize(460, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void WrongWayPreviewWidget::setFrame(const QImage& frame)
{
    m_frame = frame;
    update();
}

void WrongWayPreviewWidget::setConfig(const VehicleDirectionConfig& config)
{
    m_config = sanitizeVehicleDirectionConfig(config);
    if (m_config.lanes.empty()) m_selectedLane = -1;
    else m_selectedLane = std::max(0, std::min(m_selectedLane,
                                               static_cast<int>(m_config.lanes.size()) - 1));
    emit selectedLaneChanged(m_selectedLane);
    update();
}

VehicleDirectionConfig WrongWayPreviewWidget::config() const { return m_config; }
void WrongWayPreviewWidget::setResult(const NpuFrameResult& result) { m_result = result; update(); }
int WrongWayPreviewWidget::selectedLane() const { return m_selectedLane; }

void WrongWayPreviewWidget::addLane()
{
    if (m_config.lanes.size() >= 18u) {
        emit statusMessage(QStringLiteral("最多支持18条车道"));
        return;
    }
    m_selectedLane = static_cast<int>(m_config.lanes.size());
    m_pendingQuad.clear();
    m_editMode = EditMode::Quad;
    emit statusMessage(QStringLiteral("新增车道%1：请依次点击4个凸四边形顶点")
                       .arg(m_selectedLane + 1));
    update();
}

void WrongWayPreviewWidget::deleteSelectedLane()
{
    if (m_selectedLane < 0 || m_selectedLane >= static_cast<int>(m_config.lanes.size())) return;
    m_config.lanes.erase(m_config.lanes.begin() + m_selectedLane);
    ++m_config.revision;
    m_selectedLane = m_config.lanes.empty()
            ? -1 : std::min(m_selectedLane, static_cast<int>(m_config.lanes.size()) - 1);
    m_pendingQuad.clear();
    m_editMode = EditMode::None;
    publishConfig(QStringLiteral("已删除车道，当前共%1条")
                  .arg(static_cast<int>(m_config.lanes.size())));
}

void WrongWayPreviewWidget::setSelectedLane(int laneIndex)
{
    if (laneIndex < 0 || laneIndex >= static_cast<int>(m_config.lanes.size())) {
        if (m_config.lanes.empty()) laneIndex = -1;
        else return;
    }
    if (m_selectedLane == laneIndex) return;
    m_selectedLane = laneIndex;
    m_editMode = EditMode::None;
    m_pendingQuad.clear();
    emit selectedLaneChanged(m_selectedLane);
    if (m_selectedLane >= 0) {
        emit statusMessage(QStringLiteral("已选择车道%1").arg(m_selectedLane + 1));
    }
    update();
}

void WrongWayPreviewWidget::beginLaneQuadEdit()
{
    if (m_selectedLane < 0) {
        addLane();
        return;
    }
    m_pendingQuad.clear();
    m_editMode = EditMode::Quad;
    emit statusMessage(QStringLiteral("重画车道%1：请依次点击4个顶点").arg(m_selectedLane + 1));
    update();
}

void WrongWayPreviewWidget::beginDirectionEdit()
{
    if (m_selectedLane < 0 || m_selectedLane >= static_cast<int>(m_config.lanes.size())) {
        emit statusMessage(QStringLiteral("请先绘制并选择一条车道"));
        return;
    }
    m_editMode = EditMode::Direction;
    m_draggingDirection = false;
    emit statusMessage(QStringLiteral("在车道内沿正常行驶方向拖动箭头"));
    update();
}

void WrongWayPreviewWidget::undoLastPoint()
{
    if (m_editMode == EditMode::Quad && !m_pendingQuad.empty()) {
        m_pendingQuad.pop_back();
        emit statusMessage(QStringLiteral("已撤销顶点，当前%1/4")
                           .arg(static_cast<int>(m_pendingQuad.size())));
        update();
    }
}

void WrongWayPreviewWidget::clearAllLanes()
{
    m_config.lanes.clear();
    ++m_config.revision;
    m_selectedLane = -1;
    m_pendingQuad.clear();
    m_editMode = EditMode::None;
    publishConfig(QStringLiteral("已清除全部车道"));
}

QRectF WrongWayPreviewWidget::imageTargetRect() const
{
    if (m_frame.isNull()) return QRectF(rect());
    QSizeF target = m_frame.size();
    target.scale(size(), Qt::KeepAspectRatio);
    return QRectF((width() - target.width()) * 0.5,
                  (height() - target.height()) * 0.5,
                  target.width(), target.height());
}

bool WrongWayPreviewWidget::widgetToNormalized(const QPointF& point, QPointF* normalized) const
{
    if (!normalized) return false;
    const QRectF target = imageTargetRect();
    if (!target.contains(point) || target.width() <= 0.0 || target.height() <= 0.0) return false;
    normalized->setX(std::max(0.0, std::min(1.0, (point.x() - target.left()) / target.width())));
    normalized->setY(std::max(0.0, std::min(1.0, (point.y() - target.top()) / target.height())));
    return true;
}

QPointF WrongWayPreviewWidget::normalizedToWidget(const VehicleDirectionPointF& point) const
{
    const QRectF target = imageTargetRect();
    return QPointF(target.left() + point.x * target.width(),
                   target.top() + point.y * target.height());
}

void WrongWayPreviewWidget::publishConfig(const QString& message)
{
    m_config = sanitizeVehicleDirectionConfig(m_config);
    emit configEdited(m_config);
    emit selectedLaneChanged(m_selectedLane);
    emit statusMessage(message);
    update();
}

void WrongWayPreviewWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Qt::black);
    const QRectF target = imageTargetRect();
    if (!m_frame.isNull()) painter.drawImage(target, m_frame);
    else {
        painter.setPen(QColor(150, 165, 180));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("请先启动PCIe视频并选择单路CH"));
    }
    painter.save();
    painter.setClipRect(target);
    for (std::size_t laneIndex = 0; laneIndex < m_config.lanes.size(); ++laneIndex) {
        const auto& lane = m_config.lanes[laneIndex];
        QPolygonF polygon;
        QPointF center;
        for (const auto& point : lane.quad) {
            const QPointF widgetPoint = normalizedToWidget(point);
            polygon << widgetPoint;
            center += widgetPoint;
        }
        center *= 0.25;
        const QColor color = laneColor(static_cast<int>(laneIndex));
        QPen pen(color, static_cast<int>(laneIndex) == m_selectedLane ? 4.0 : 2.5);
        painter.setPen(pen);
        painter.setBrush(QColor(color.red(), color.green(), color.blue(), 22));
        painter.drawPolygon(polygon);
        QPointF direction(lane.direction.x, lane.direction.y);
        const QPointF end = center + direction * std::max(45.0, std::min(target.width(), target.height()) * 0.15);
        painter.drawLine(center, end);
        const double angle = std::atan2(direction.y(), direction.x());
        QPolygonF head;
        head << end
             << end - QPointF(std::cos(angle - 0.55), std::sin(angle - 0.55)) * 14.0
             << end - QPointF(std::cos(angle + 0.55), std::sin(angle + 0.55)) * 14.0;
        painter.setBrush(color);
        painter.drawPolygon(head);
        painter.drawText(polygon.boundingRect().topLeft() + QPointF(4, 18),
                         QStringLiteral("车道%1").arg(laneIndex + 1));
    }

    if (m_result.task == NpuTaskType::VehicleWrongWay && m_result.ready) {
        for (const NpuBox& box : m_result.vehicles) {
            QRectF rect(target.left() + box.x / 640.0 * target.width(),
                        target.top() + box.y / 360.0 * target.height(),
                        box.w / 640.0 * target.width(),
                        box.h / 360.0 * target.height());
            painter.setPen(QPen(vehicleColor(box.vehicleDirectionState), 3.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect);
            const QString state = vehicleStateText(box.vehicleDirectionState);
            if (!state.isEmpty()) painter.drawText(rect.topLeft() + QPointF(2, 18), state);
        }
    }

    if (m_editMode == EditMode::Quad && !m_pendingQuad.empty()) {
        QPolygonF pending;
        for (const QPointF& point : m_pendingQuad) {
            VehicleDirectionPointF normalized;
            normalized.x = static_cast<float>(point.x());
            normalized.y = static_cast<float>(point.y());
            pending << normalizedToWidget(normalized);
        }
        painter.setPen(QPen(QColor(255, 255, 255), 2.0, Qt::DashLine));
        painter.drawPolyline(pending);
        for (const QPointF& point : pending) painter.drawEllipse(point, 5.0, 5.0);
    }
    if (m_editMode == EditMode::Direction && m_draggingDirection) {
        VehicleDirectionPointF a {static_cast<float>(m_directionStart.x()), static_cast<float>(m_directionStart.y())};
        VehicleDirectionPointF b {static_cast<float>(m_directionEnd.x()), static_cast<float>(m_directionEnd.y())};
        painter.setPen(QPen(QColor(255, 255, 255), 3.0, Qt::DashLine));
        painter.drawLine(normalizedToWidget(a), normalizedToWidget(b));
    }
    painter.restore();
}

void WrongWayPreviewWidget::mousePressEvent(QMouseEvent* event)
{
    QPointF normalized;
    if (!widgetToNormalized(event->localPos(), &normalized)) return;
    if (m_editMode == EditMode::None) {
        for (int laneIndex = static_cast<int>(m_config.lanes.size()) - 1;
             laneIndex >= 0; --laneIndex) {
            QPolygonF polygon;
            const auto& lane = m_config.lanes[static_cast<std::size_t>(laneIndex)];
            for (const auto& point : lane.quad) {
                polygon << QPointF(point.x, point.y);
            }
            if (polygon.containsPoint(normalized, Qt::OddEvenFill)) {
                setSelectedLane(laneIndex);
                return;
            }
        }
    } else if (m_editMode == EditMode::Quad) {
        m_pendingQuad.push_back(normalized);
        if (m_pendingQuad.size() == 4u) {
            VehicleLaneDirectionConfig lane;
            for (int i = 0; i < 4; ++i) {
                lane.quad[static_cast<std::size_t>(i)].x = static_cast<float>(m_pendingQuad[i].x());
                lane.quad[static_cast<std::size_t>(i)].y = static_cast<float>(m_pendingQuad[i].y());
            }
            if (m_selectedLane == static_cast<int>(m_config.lanes.size()))
                m_config.lanes.push_back(lane);
            else if (m_selectedLane >= 0)
                m_config.lanes[static_cast<std::size_t>(m_selectedLane)].quad = lane.quad;
            m_pendingQuad.clear();
            m_editMode = EditMode::None;
            ++m_config.revision;
            QString error;
            if (!validateVehicleDirectionConfig(m_config, &error)) {
                emit statusMessage(QStringLiteral("车道无效：%1，请重画").arg(error));
            } else {
                publishConfig(QStringLiteral("车道%1已完成，请设置正常方向").arg(m_selectedLane + 1));
            }
        }
        update();
    } else if (m_editMode == EditMode::Direction) {
        m_draggingDirection = true;
        m_directionStart = normalized;
        m_directionEnd = normalized;
    }
}

void WrongWayPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_draggingDirection) return;
    QPointF normalized;
    if (widgetToNormalized(event->localPos(), &normalized)) {
        m_directionEnd = normalized;
        update();
    }
}

void WrongWayPreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    if (!m_draggingDirection || m_selectedLane < 0 ||
        m_selectedLane >= static_cast<int>(m_config.lanes.size())) return;
    m_draggingDirection = false;
    const QPointF delta = m_directionEnd - m_directionStart;
    const double norm = std::hypot(delta.x(), delta.y());
    if (norm < 0.01) {
        emit statusMessage(QStringLiteral("方向箭头过短，请重新拖动"));
        return;
    }
    auto& direction = m_config.lanes[static_cast<std::size_t>(m_selectedLane)].direction;
    direction.x = static_cast<float>(delta.x() / norm);
    direction.y = static_cast<float>(delta.y() / norm);
    ++m_config.revision;
    m_editMode = EditMode::None;
    publishConfig(QStringLiteral("车道%1正常方向已更新").arg(m_selectedLane + 1));
}
