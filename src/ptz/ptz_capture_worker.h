#ifndef PTZ_CAPTURE_WORKER_H
#define PTZ_CAPTURE_WORKER_H

#include <QObject>
#include <QImage>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

class PtzCaptureWorker : public QObject
{
    Q_OBJECT

public:
    explicit PtzCaptureWorker(QObject *parent = nullptr);
    ~PtzCaptureWorker();

    bool isRunning() const;

    // Qt主线程定时调用，只取最新一帧，避免事件队列累积造成延迟。
    bool takeLatestFrame(QImage *frame);

public slots:
    void startCapture();
    void stopCapture();

signals:
    void stateChanged(bool running, QString message);
    void statsUpdated(double captureFps,
                      double decodeFps,
                      double decodeAvgUs,
                      int failCount);

private:
    void workerMain();

private:
    std::thread m_thread;
    std::atomic<bool> m_running {false};
    std::atomic<bool> m_stopReq {false};

    std::mutex m_frameMutex;
    QImage m_latestFrame;
    bool m_framePending = false;
};

#endif // PTZ_CAPTURE_WORKER_H

