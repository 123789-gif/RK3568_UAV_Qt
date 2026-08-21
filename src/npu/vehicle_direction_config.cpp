#include "npu/vehicle_direction_config.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxVehicleLanes = 18;

float clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

float cross(const VehicleDirectionPointF& a,
            const VehicleDirectionPointF& b,
            const VehicleDirectionPointF& c)
{
    return (b.x - a.x) * (c.y - a.y) -
            (b.y - a.y) * (c.x - a.x);
}

bool convexQuad(const std::array<VehicleDirectionPointF, 4>& quad)
{
    float sign = 0.0f;
    float area2 = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const VehicleDirectionPointF& a = quad[static_cast<std::size_t>(i)];
        const VehicleDirectionPointF& b = quad[static_cast<std::size_t>((i + 1) % 4)];
        const VehicleDirectionPointF& c = quad[static_cast<std::size_t>((i + 2) % 4)];
        const float value = cross(a, b, c);
        if (std::fabs(value) < 1e-6f) return false;
        if (sign == 0.0f) sign = value;
        else if (sign * value < 0.0f) return false;
        area2 += a.x * b.y - b.x * a.y;
    }
    return std::fabs(area2) >= 0.0002f;
}

QString pointText(const VehicleDirectionPointF& point)
{
    return QStringLiteral("%1,%2")
            .arg(point.x, 0, 'f', 6)
            .arg(point.y, 0, 'f', 6);
}

bool parsePoint(const QString& text, VehicleDirectionPointF* point)
{
    if (!point) return false;
    const QStringList fields = text.split(QLatin1Char(','), QString::SkipEmptyParts);
    if (fields.size() != 2) return false;
    bool okX = false;
    bool okY = false;
    const float x = fields.at(0).trimmed().toFloat(&okX);
    const float y = fields.at(1).trimmed().toFloat(&okY);
    if (!okX || !okY || !std::isfinite(x) || !std::isfinite(y)) return false;
    point->x = x;
    point->y = y;
    return true;
}

} // namespace

VehicleDirectionConfig sanitizeVehicleDirectionConfig(VehicleDirectionConfig config)
{
    if (config.lanes.size() > static_cast<std::size_t>(kMaxVehicleLanes)) {
        config.lanes.resize(kMaxVehicleLanes);
    }
    for (VehicleLaneDirectionConfig& lane : config.lanes) {
        for (VehicleDirectionPointF& point : lane.quad) {
            point.x = clamp01(point.x);
            point.y = clamp01(point.y);
        }
        const float norm = std::sqrt(lane.direction.x * lane.direction.x +
                                     lane.direction.y * lane.direction.y);
        if (std::isfinite(norm) && norm > 1e-6f) {
            lane.direction.x /= norm;
            lane.direction.y /= norm;
        }
    }
    config.directionWindowMs = std::max(200, std::min(2000, config.directionWindowMs));
    config.wrongWayHoldMs = std::max(100, std::min(3000, config.wrongWayHoldMs));
    config.wrongWayClearMs = std::max(200, std::min(5000, config.wrongWayClearMs));
    config.trackTtlMs = std::max(200, std::min(5000, config.trackTtlMs));
    config.stationaryHoldMs = std::max(200, std::min(3000, config.stationaryHoldMs));
    config.motionThreshold = std::max(0.005f, std::min(0.20f, config.motionThreshold));
    config.directionCosine = std::max(0.10f, std::min(0.95f, config.directionCosine));
    config.boxEma = std::max(0.05f, std::min(1.0f, config.boxEma));
    if (config.revision == 0) config.revision = 1;
    return config;
}

bool validateVehicleDirectionConfig(const VehicleDirectionConfig& requested,
                                    QString* errorMessage)
{
    const VehicleDirectionConfig config = sanitizeVehicleDirectionConfig(requested);
    if (config.lanes.empty() || config.lanes.size() > static_cast<std::size_t>(kMaxVehicleLanes)) {
        if (errorMessage) *errorMessage = QStringLiteral("必须配置1到18条车道");
        return false;
    }
    for (std::size_t i = 0; i < config.lanes.size(); ++i) {
        const VehicleLaneDirectionConfig& lane = config.lanes[i];
        for (const VehicleDirectionPointF& point : lane.quad) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                point.x < 0.0f || point.x > 1.0f ||
                point.y < 0.0f || point.y > 1.0f) {
                if (errorMessage) *errorMessage = QStringLiteral("车道%1存在越界坐标").arg(i + 1);
                return false;
            }
        }
        if (!convexQuad(lane.quad)) {
            if (errorMessage) *errorMessage = QStringLiteral("车道%1必须是非退化凸四边形").arg(i + 1);
            return false;
        }
        const float norm = std::sqrt(lane.direction.x * lane.direction.x +
                                     lane.direction.y * lane.direction.y);
        if (!std::isfinite(norm) || norm < 1e-6f) {
            if (errorMessage) *errorMessage = QStringLiteral("车道%1方向向量无效").arg(i + 1);
            return false;
        }
    }
    if (errorMessage) errorMessage->clear();
    return true;
}

bool loadVehicleDirectionConfig(const QString& path,
                                VehicleDirectionConfig* config,
                                QString* errorMessage)
{
    if (!config) {
        if (errorMessage) *errorMessage = QStringLiteral("车辆逆行配置输出指针为空");
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists()) {
        *config = VehicleDirectionConfig();
        if (errorMessage) *errorMessage = QStringLiteral("配置不存在，请先标注车道");
        return true;
    }

    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    VehicleDirectionConfig loaded;
    const int version = settings.value(QStringLiteral("meta/version"), 2).toInt();
    const int laneCount = settings.value(QStringLiteral("meta/lane_count"), 0).toInt();
    if (version != 2 || laneCount < 1 || laneCount > kMaxVehicleLanes) {
        if (errorMessage) *errorMessage = QStringLiteral("车辆逆行配置版本或车道数量无效");
        return false;
    }
    for (int laneIndex = 0; laneIndex < laneCount; ++laneIndex) {
        VehicleLaneDirectionConfig lane;
        const QString prefix = QStringLiteral("lane%1/").arg(laneIndex);
        bool valid = true;
        for (int p = 0; p < 4; ++p) {
            valid = parsePoint(settings.value(prefix + QStringLiteral("p%1").arg(p)).toString(),
                               &lane.quad[static_cast<std::size_t>(p)]) && valid;
        }
        valid = parsePoint(settings.value(prefix + QStringLiteral("direction")).toString(),
                           &lane.direction) && valid;
        if (!valid) {
            if (errorMessage) *errorMessage = QStringLiteral("车道%1字段缺失或格式错误").arg(laneIndex + 1);
            return false;
        }
        loaded.lanes.push_back(lane);
    }
    loaded.directionWindowMs = settings.value(QStringLiteral("rules/direction_window_ms"), 600).toInt();
    loaded.wrongWayHoldMs = settings.value(QStringLiteral("rules/wrong_way_hold_ms"), 350).toInt();
    loaded.wrongWayClearMs = settings.value(QStringLiteral("rules/wrong_way_clear_ms"), 800).toInt();
    loaded.trackTtlMs = settings.value(QStringLiteral("rules/track_ttl_ms"), 800).toInt();
    loaded.stationaryHoldMs = settings.value(QStringLiteral("rules/stationary_hold_ms"), 450).toInt();
    loaded.motionThreshold = settings.value(QStringLiteral("rules/motion_threshold"), 0.02).toFloat();
    loaded.directionCosine = settings.value(QStringLiteral("rules/direction_cosine"), 0.50).toFloat();
    loaded.boxEma = settings.value(QStringLiteral("rules/box_ema"), 0.30).toFloat();
    loaded.revision = settings.value(QStringLiteral("meta/revision"), 1).toULongLong();
    if (settings.status() != QSettings::NoError ||
        !validateVehicleDirectionConfig(loaded, errorMessage)) return false;
    *config = sanitizeVehicleDirectionConfig(loaded);
    if (errorMessage) errorMessage->clear();
    return true;
}

bool saveVehicleDirectionConfig(const QString& path,
                                const VehicleDirectionConfig& requested,
                                QString* errorMessage)
{
    if (!validateVehicleDirectionConfig(requested, errorMessage)) return false;
    const VehicleDirectionConfig config = sanitizeVehicleDirectionConfig(requested);
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建配置目录：%1").arg(info.absolutePath());
        return false;
    }
    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.clear();
    settings.setValue(QStringLiteral("meta/version"), 2);
    settings.setValue(QStringLiteral("meta/lane_count"), static_cast<int>(config.lanes.size()));
    settings.setValue(QStringLiteral("meta/revision"), QVariant::fromValue<qulonglong>(config.revision));
    for (std::size_t laneIndex = 0; laneIndex < config.lanes.size(); ++laneIndex) {
        const VehicleLaneDirectionConfig& lane = config.lanes[laneIndex];
        const QString prefix = QStringLiteral("lane%1/").arg(laneIndex);
        for (int p = 0; p < 4; ++p) {
            settings.setValue(prefix + QStringLiteral("p%1").arg(p),
                              pointText(lane.quad[static_cast<std::size_t>(p)]));
        }
        settings.setValue(prefix + QStringLiteral("direction"), pointText(lane.direction));
    }
    settings.setValue(QStringLiteral("rules/direction_window_ms"), config.directionWindowMs);
    settings.setValue(QStringLiteral("rules/wrong_way_hold_ms"), config.wrongWayHoldMs);
    settings.setValue(QStringLiteral("rules/wrong_way_clear_ms"), config.wrongWayClearMs);
    settings.setValue(QStringLiteral("rules/track_ttl_ms"), config.trackTtlMs);
    settings.setValue(QStringLiteral("rules/stationary_hold_ms"), config.stationaryHoldMs);
    settings.setValue(QStringLiteral("rules/motion_threshold"), config.motionThreshold);
    settings.setValue(QStringLiteral("rules/direction_cosine"), config.directionCosine);
    settings.setValue(QStringLiteral("rules/box_ema"), config.boxEma);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (errorMessage) *errorMessage = QStringLiteral("写入车辆逆行配置失败：%1").arg(path);
        return false;
    }
    if (errorMessage) errorMessage->clear();
    return true;
}
