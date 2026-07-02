#ifndef PTZ_CONTROL_WORKER_H
#define PTZ_CONTROL_WORKER_H

#include "ptz/ptz_camera_profile.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

class PtzControlWorker : public QObject
{
    Q_OBJECT

public:
    explicit PtzControlWorker(QObject *parent = nullptr);
    ~PtzControlWorker();

    bool isRunning() const;
    bool isReady() const;

    // 仅在控制线程停止时应用。
    bool setCameraProfile(const PtzCameraProfile &profile,
                          QString *errorMessage = nullptr);
    PtzCameraProfile cameraProfile() const;

public slots:
    void startControl();
    void stopControl();

    void moveUp();
    void moveDown();
    void moveLeft();
    void moveRight();
    void moveUpLeft();
    void moveUpRight();
    void moveDownLeft();
    void moveDownRight();
    void stopPanTilt();
    void home();

    // VISCA速度等级：水平1～24，俯仰1～20。
    // 修改后从下一次方向按键开始生效。
    void setPanSpeed(int speed);
    void setTiltSpeed(int speed);

    void zoomTele();
    void zoomWide();
    void stopZoom();

signals:
    void stateChanged(bool ready, QString message);
    void commandError(QString message);

private:
    enum class Command {
        MoveUp,
        MoveDown,
        MoveLeft,
        MoveRight,
        MoveUpLeft,
        MoveUpRight,
        MoveDownLeft,
        MoveDownRight,
        StopPanTilt,
        Home,
        ZoomTele,
        ZoomWide,
        StopZoom
    };

    void enqueue(Command command);
    void workerMain();

private:
    std::thread m_thread;
    std::atomic<bool> m_running {false};
    std::atomic<bool> m_ready {false};
    std::atomic<bool> m_stopReq {false};

    mutable std::mutex m_configMutex;
    PtzCameraProfile m_profile;

    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<Command> m_queue;

    std::atomic<uint8_t> m_panSpeed {5};
    std::atomic<uint8_t> m_tiltSpeed {5};
};

#endif // PTZ_CONTROL_WORKER_H
