#ifndef PROCESS_SIGNAL_BRIDGE_H
#define PROCESS_SIGNAL_BRIDGE_H

#include <QObject>
#include <QString>

#include <csignal>

class QSocketNotifier;

// 将 POSIX 终止信号安全地转发到 Qt 事件循环。
// 信号处理函数只执行 async-signal-safe 的 write()，资源释放仍在主线程完成。
class ProcessSignalBridge : public QObject
{
    Q_OBJECT

public:
    explicit ProcessSignalBridge(QObject *parent = nullptr);
    ~ProcessSignalBridge();

    bool isInstalled() const;
    QString errorString() const;

signals:
    void terminationRequested(int signalNumber);

private:
    static void signalHandler(int signalNumber);
    void drainSignals();

private:
    int m_readFd = -1;
    int m_writeFd = -1;
    QSocketNotifier *m_notifier = nullptr;
    bool m_installed = false;
    struct sigaction m_oldSigterm {};
    struct sigaction m_oldSigint {};
    QString m_error;

    static volatile sig_atomic_t s_writeFd;
};

#endif // PROCESS_SIGNAL_BRIDGE_H
