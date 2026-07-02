#ifndef PTZ_CAPTURE_WORKER_H
#define PTZ_CAPTURE_WORKER_H

#include "ptz/v4l2_device_detector.h"

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

    // 仅在采集停止时应用。UI先完成设备解析，再同时配置视频与VISCA线程。
    bool setCaptureConfig(const PtzResolvedDevice &config, QString *errorMessage = nullptr);
    PtzResolvedDevice captureConfig() const;

    // Qt主线程定时调用，只取最新一帧，避免事件队列累积造成延迟。
    bool takeLatestFrame(QImage *frame);
    void clearLatestFrame();

public slots:
    void startCapture();
    void stopCapture();

signals:
    void stateChanged(bool running, QString message);
    void statsUpdated(double captureFps,
                      double decodeFps,
                      double decodeAvgUs,
                      int failCount);
    // 设备在运行期间被拔出或底层V4L2链路失效时通知UI触发重新枚举。
    void deviceLost(QString videoDevice, QString reason);

private:
    void workerMain();

private:
    std::thread m_thread;
    std::atomic<bool> m_running {false};
    std::atomic<bool> m_stopReq {false};

    mutable std::mutex m_configMutex;
    PtzResolvedDevice m_config;

    std::mutex m_frameMutex;
    QImage m_latestFrame;
    bool m_framePending = false;
};

#endif // PTZ_CAPTURE_WORKER_H
