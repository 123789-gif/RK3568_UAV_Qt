#include "system/fan_controller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>

FanController::FanController(QObject *parent)
    : QObject(parent)
{
}

bool FanController::initialize()
{
    m_lastError.clear();

    if (!ensureExported()) {
        m_available = false;
        return false;
    }

    if (!refresh()) {
        m_available = false;
        return false;
    }

    // 新导出的PWM通道可能尚未设置周期。只初始化为关闭状态，避免程序启动时意外启停风扇。
    if (m_periodNs <= 0) {
        if (!apply(false, 30)) {
            m_available = false;
            return false;
        }
    }

    m_available = true;
    return true;
}

bool FanController::refresh()
{
    if (!ensureExported()) {
        m_available = false;
        return false;
    }

    int enable = 0;
    int period = 0;
    int duty = 0;

    if (!readIntFile(pwmFile(QStringLiteral("enable")), &enable)
            || !readIntFile(pwmFile(QStringLiteral("period")), &period)
            || !readIntFile(pwmFile(QStringLiteral("duty_cycle")), &duty)) {
        m_available = false;
        return false;
    }

    m_enabled = (enable != 0);
    m_periodNs = period;
    m_dutyNs = duty;

    if (period > 0) {
        m_dutyPercent = std::clamp(
                    static_cast<int>((static_cast<long long>(duty) * 100LL + period / 2) / period),
                    0,
                    100);
    }

    m_available = true;
    m_lastError.clear();
    return true;
}

bool FanController::apply(bool enabled, int dutyPercent)
{
    m_lastError.clear();

    if (!ensureExported()) {
        m_available = false;
        return false;
    }

    dutyPercent = std::clamp(dutyPercent, 0, 100);

    int currentEnable = 0;
    int currentPeriod = 0;
    int currentDuty = 0;

    if (!readIntFile(pwmFile(QStringLiteral("enable")), &currentEnable)
            || !readIntFile(pwmFile(QStringLiteral("period")), &currentPeriod)
            || !readIntFile(pwmFile(QStringLiteral("duty_cycle")), &currentDuty)) {
        m_available = false;
        return false;
    }

    // 调整PWM参数前先关闭通道。该驱动在已经关闭时重复写enable=0可能返回EINVAL，
    // 因此只在当前确实处于使能状态时执行关闭。
    if (currentEnable != 0) {
        if (!writeTextFile(pwmFile(QStringLiteral("enable")), QByteArrayLiteral("0"))) {
            m_available = false;
            return false;
        }
    }

    QString polarity;
    if (!readTextFile(pwmFile(QStringLiteral("polarity")), &polarity)) {
        m_available = false;
        return false;
    }
    const bool polarityNeedsChange =
            polarity.trimmed() != QStringLiteral("normal");
    const bool periodNeedsChange =
            currentPeriod != m_targetPeriodNs;

    /*
     * RK3568当前PWM驱动在通道刚导出时为：
     *   period=0, duty_cycle=0, polarity=inversed, enable=0
     * 此时先写duty_cycle或polarity会返回EINVAL，必须先建立有效period。
     */
    if (currentPeriod <= 0) {
        if (!writeTextFile(pwmFile(QStringLiteral("period")),
                           QByteArray::number(m_targetPeriodNs))) {
            m_available = false;
            return false;
        }
        currentPeriod = m_targetPeriodNs;
        currentDuty = 0;
    } else {
        // 已存在有效周期时，只有在要改周期或极性且当前占空比非零时才先清零，
        // 避免旧duty_cycle超过新period，也避免带负载切换极性。
        if ((periodNeedsChange || polarityNeedsChange) && currentDuty != 0) {
            if (!writeTextFile(pwmFile(QStringLiteral("duty_cycle")),
                               QByteArrayLiteral("0"))) {
                m_available = false;
                return false;
            }
            currentDuty = 0;
        }

        if (periodNeedsChange) {
            if (!writeTextFile(pwmFile(QStringLiteral("period")),
                               QByteArray::number(m_targetPeriodNs))) {
                m_available = false;
                return false;
            }
            currentPeriod = m_targetPeriodNs;
        }
    }

    // polarity必须在period已经有效之后设置。
    if (polarityNeedsChange) {
        if (!writeTextFile(pwmFile(QStringLiteral("polarity")),
                           QByteArrayLiteral("normal"))) {
            m_available = false;
            return false;
        }
    }

    const int dutyNs = static_cast<int>(
                (static_cast<long long>(m_targetPeriodNs) * dutyPercent) / 100LL);

    if (!writeTextFile(pwmFile(QStringLiteral("duty_cycle")),
                       QByteArray::number(dutyNs))) {
        m_available = false;
        return false;
    }

    if (enabled) {
        if (!writeTextFile(pwmFile(QStringLiteral("enable")), QByteArrayLiteral("1"))) {
            m_available = false;
            return false;
        }
    }

    if (!refresh()) {
        return false;
    }

    // PWM为开启且占空比为0时，硬件不转，但保留用户的“开启”状态。
    m_available = true;
    return true;
}

bool FanController::isAvailable() const
{
    return m_available;
}

bool FanController::isEnabled() const
{
    return m_enabled;
}

int FanController::dutyPercent() const
{
    return m_dutyPercent;
}

int FanController::periodNs() const
{
    return m_periodNs;
}

QString FanController::lastError() const
{
    return m_lastError;
}

QString FanController::statusText() const
{
    if (!m_available) {
        return m_lastError.isEmpty()
                ? QStringLiteral("PWM风扇不可用")
                : QStringLiteral("PWM风扇不可用：%1").arg(m_lastError);
    }

    return QStringLiteral("PWM0：%1，占空比%2%，周期%3 ns")
            .arg(m_enabled ? QStringLiteral("开启") : QStringLiteral("关闭"))
            .arg(m_dutyPercent)
            .arg(m_periodNs);
}

bool FanController::ensureExported()
{
    if (!QFileInfo::exists(m_chipPath)) {
        setError(QStringLiteral("PWM控制器不存在：%1").arg(m_chipPath));
        return false;
    }

    if (QFileInfo::exists(m_pwmPath)) {
        return true;
    }

    QFile exportFile(m_chipPath + QStringLiteral("/export"));
    if (!exportFile.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("无法打开PWM export：%1").arg(exportFile.errorString()));
        return false;
    }

    const qint64 written = exportFile.write("0", 1);
    exportFile.close();

    // 通道可能已由其他进程导出，写export会失败，但目录随后仍可能存在。
    for (int i = 0; i < 20; ++i) {
        if (QFileInfo::exists(m_pwmPath)) {
            return true;
        }
        QThread::msleep(25);
    }

    if (written != 1) {
        setError(QStringLiteral("导出PWM0失败：%1").arg(exportFile.errorString()));
    } else {
        setError(QStringLiteral("导出PWM0后节点未出现：%1").arg(m_pwmPath));
    }
    return false;
}

bool FanController::readIntFile(const QString &path, int *value)
{
    if (!value) {
        setError(QStringLiteral("读取PWM参数时输出指针为空"));
        return false;
    }

    QString text;
    if (!readTextFile(path, &text)) {
        return false;
    }

    bool ok = false;
    const int parsed = text.trimmed().toInt(&ok);
    if (!ok) {
        setError(QStringLiteral("PWM参数不是有效整数：%1 = %2").arg(path, text.trimmed()));
        return false;
    }

    *value = parsed;
    return true;
}

bool FanController::readTextFile(const QString &path, QString *value)
{
    if (!value) {
        setError(QStringLiteral("读取PWM参数时输出指针为空"));
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("无法读取%1：%2").arg(path, file.errorString()));
        return false;
    }

    *value = QString::fromLocal8Bit(file.readAll());
    return true;
}

bool FanController::writeTextFile(const QString &path, const QByteArray &value)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("无法写入%1：%2；请确认程序具有root权限")
                 .arg(path, file.errorString()));
        return false;
    }

    const qint64 written = file.write(value);
    if (written != value.size()) {
        setError(QStringLiteral("写入%1失败：%2")
                 .arg(path, file.errorString()));
        file.close();
        return false;
    }

    if (!file.flush()) {
        setError(QStringLiteral("刷新%1失败：%2")
                 .arg(path, file.errorString()));
        file.close();
        return false;
    }

    file.close();
    return true;
}

QString FanController::pwmFile(const QString &name) const
{
    return m_pwmPath + QLatin1Char('/') + name;
}

void FanController::setError(const QString &message)
{
    m_lastError = message;
}
