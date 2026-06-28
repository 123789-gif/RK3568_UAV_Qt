#ifndef FAN_CONTROLLER_H
#define FAN_CONTROLLER_H

#include <QObject>
#include <QString>

class FanController : public QObject
{
public:
    explicit FanController(QObject *parent = nullptr);

    bool initialize();
    bool refresh();
    bool apply(bool enabled, int dutyPercent);

    bool isAvailable() const;
    bool isEnabled() const;
    int dutyPercent() const;
    int periodNs() const;
    QString lastError() const;
    QString statusText() const;

private:
    bool ensureExported();
    bool readIntFile(const QString &path, int *value);
    bool readTextFile(const QString &path, QString *value);
    bool writeTextFile(const QString &path, const QByteArray &value);
    QString pwmFile(const QString &name) const;
    void setError(const QString &message);

private:
    const QString m_chipPath = QStringLiteral("/sys/class/pwm/pwmchip0");
    const QString m_pwmPath = QStringLiteral("/sys/class/pwm/pwmchip0/pwm0");
    const int m_targetPeriodNs = 200000;

    bool m_available = false;
    bool m_enabled = false;
    int m_periodNs = 0;
    int m_dutyNs = 0;
    int m_dutyPercent = 30;
    QString m_lastError;
};

#endif // FAN_CONTROLLER_H
