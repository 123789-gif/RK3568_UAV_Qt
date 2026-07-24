#include "npu/violation_rule_config.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <algorithm>

namespace {

float clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

QString pointText(const ViolationPointF& point)
{
    return QStringLiteral("%1,%2")
            .arg(point.x, 0, 'f', 6)
            .arg(point.y, 0, 'f', 6);
}

bool parsePoint(const QString& text, ViolationPointF* point)
{
    if (!point) {
        return false;
    }
    const QStringList fields = text.split(QLatin1Char(','), QString::SkipEmptyParts);
    if (fields.size() != 2) {
        return false;
    }
    bool okX = false;
    bool okY = false;
    const float x = fields.at(0).trimmed().toFloat(&okX);
    const float y = fields.at(1).trimmed().toFloat(&okY);
    if (!okX || !okY) {
        return false;
    }
    point->x = clamp01(x);
    point->y = clamp01(y);
    return true;
}

} // namespace

ViolationRuleConfig sanitizeViolationRuleConfig(ViolationRuleConfig config)
{
    config.lightX = clamp01(config.lightX);
    config.lightY = clamp01(config.lightY);
    config.lightW = clamp01(config.lightW);
    config.lightH = clamp01(config.lightH);
    if (config.lightX + config.lightW > 1.0f) {
        config.lightW = 1.0f - config.lightX;
    }
    if (config.lightY + config.lightH > 1.0f) {
        config.lightH = 1.0f - config.lightY;
    }
    if (config.lightW < 0.005f || config.lightH < 0.005f) {
        config.lightRoiValid = false;
    }

    for (ViolationPointF& point : config.crosswalk) {
        point.x = clamp01(point.x);
        point.y = clamp01(point.y);
    }

    config.lightVoteWindow = std::max(1, std::min(15, config.lightVoteWindow));
    config.lightVoteMin = std::max(1, std::min(config.lightVoteWindow, config.lightVoteMin));
    config.unknownHoldMs = std::max(0, std::min(3000, config.unknownHoldMs));
    config.violationHoldMs = std::max(100, std::min(5000, config.violationHoldMs));
    config.trackTtlMs = std::max(200, std::min(5000, config.trackTtlMs));
    config.trackIouThreshold = std::max(0.05f, std::min(0.80f, config.trackIouThreshold));
    config.boxEma = std::max(0.0f, std::min(0.95f, config.boxEma));
    if (config.revision == 0) {
        config.revision = 1;
    }
    return config;
}

QString violationLightModeKey(ViolationLightMode mode)
{
    switch (mode) {
    case ViolationLightMode::ForceRed:
        return QStringLiteral("force_red");
    case ViolationLightMode::ForceGreen:
        return QStringLiteral("force_green");
    case ViolationLightMode::Auto:
    default:
        return QStringLiteral("auto");
    }
}

ViolationLightMode violationLightModeFromKey(const QString& key)
{
    const QString normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("force_red") || normalized == QStringLiteral("red")) {
        return ViolationLightMode::ForceRed;
    }
    if (normalized == QStringLiteral("force_green") || normalized == QStringLiteral("green")) {
        return ViolationLightMode::ForceGreen;
    }
    return ViolationLightMode::Auto;
}

bool loadViolationRuleConfig(const QString& path,
                             ViolationRuleConfig* config,
                             QString* errorMessage)
{
    if (!config) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("违法检测配置输出指针为空");
        }
        return false;
    }

    ViolationRuleConfig loaded;
    const QFileInfo info(path);
    if (!info.exists()) {
        *config = loaded;
        if (errorMessage) {
            *errorMessage = QStringLiteral("配置文件不存在，使用未划区默认值");
        }
        return true;
    }

    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");

    loaded.lightRoiValid = settings.value(QStringLiteral("traffic_light/valid"), false).toBool();
    loaded.lightX = settings.value(QStringLiteral("traffic_light/x"), 0.0).toFloat();
    loaded.lightY = settings.value(QStringLiteral("traffic_light/y"), 0.0).toFloat();
    loaded.lightW = settings.value(QStringLiteral("traffic_light/w"), 0.0).toFloat();
    loaded.lightH = settings.value(QStringLiteral("traffic_light/h"), 0.0).toFloat();

    loaded.crosswalkValid = settings.value(QStringLiteral("crosswalk/valid"), false).toBool();
    for (int i = 0; i < 4; ++i) {
        const QString key = QStringLiteral("crosswalk/p%1").arg(i);
        if (!parsePoint(settings.value(key, QStringLiteral("0,0")).toString(),
                        &loaded.crosswalk[static_cast<std::size_t>(i)])) {
            loaded.crosswalkValid = false;
        }
    }

    loaded.lightMode = violationLightModeFromKey(
                settings.value(QStringLiteral("rules/light_mode"), QStringLiteral("auto")).toString());
    loaded.lightVoteWindow = settings.value(QStringLiteral("rules/light_vote_window"), 5).toInt();
    loaded.lightVoteMin = settings.value(QStringLiteral("rules/light_vote_min"), 3).toInt();
    loaded.unknownHoldMs = settings.value(QStringLiteral("rules/unknown_hold_ms"), 300).toInt();
    loaded.violationHoldMs = settings.value(QStringLiteral("rules/violation_hold_ms"), 600).toInt();
    loaded.requireRedEntry = settings.value(QStringLiteral("rules/require_red_entry"), true).toBool();
    loaded.trackTtlMs = settings.value(QStringLiteral("rules/track_ttl_ms"), 1000).toInt();
    loaded.trackIouThreshold = settings.value(QStringLiteral("rules/track_iou_threshold"), 0.20).toFloat();
    loaded.boxEma = settings.value(QStringLiteral("rules/box_ema"), 0.50).toFloat();
    loaded.revision = settings.value(QStringLiteral("meta/revision"), 1).toULongLong();

    if (settings.status() != QSettings::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("读取违法检测配置失败：%1").arg(path);
        }
        return false;
    }

    *config = sanitizeViolationRuleConfig(loaded);
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool saveViolationRuleConfig(const QString& path,
                             const ViolationRuleConfig& config,
                             QString* errorMessage)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建配置目录：%1").arg(info.absolutePath());
        }
        return false;
    }

    const ViolationRuleConfig clean = sanitizeViolationRuleConfig(config);
    QSettings settings(path, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");

    settings.setValue(QStringLiteral("traffic_light/valid"), clean.lightRoiValid);
    settings.setValue(QStringLiteral("traffic_light/x"), clean.lightX);
    settings.setValue(QStringLiteral("traffic_light/y"), clean.lightY);
    settings.setValue(QStringLiteral("traffic_light/w"), clean.lightW);
    settings.setValue(QStringLiteral("traffic_light/h"), clean.lightH);

    settings.setValue(QStringLiteral("crosswalk/valid"), clean.crosswalkValid);
    for (int i = 0; i < 4; ++i) {
        settings.setValue(QStringLiteral("crosswalk/p%1").arg(i),
                          pointText(clean.crosswalk[static_cast<std::size_t>(i)]));
    }

    settings.setValue(QStringLiteral("rules/light_mode"), violationLightModeKey(clean.lightMode));
    settings.setValue(QStringLiteral("rules/light_vote_window"), clean.lightVoteWindow);
    settings.setValue(QStringLiteral("rules/light_vote_min"), clean.lightVoteMin);
    settings.setValue(QStringLiteral("rules/unknown_hold_ms"), clean.unknownHoldMs);
    settings.setValue(QStringLiteral("rules/violation_hold_ms"), clean.violationHoldMs);
    settings.setValue(QStringLiteral("rules/require_red_entry"), clean.requireRedEntry);
    settings.setValue(QStringLiteral("rules/track_ttl_ms"), clean.trackTtlMs);
    settings.setValue(QStringLiteral("rules/track_iou_threshold"), clean.trackIouThreshold);
    settings.setValue(QStringLiteral("rules/box_ema"), clean.boxEma);
    settings.setValue(QStringLiteral("meta/revision"), QVariant::fromValue<qulonglong>(clean.revision));
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("写入违法检测配置失败：%1").arg(path);
        }
        return false;
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}
