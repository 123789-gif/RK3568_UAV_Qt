#include "ptz/ptz_control_worker.h"
#include "ptz/visca_serial.hpp"

#include <QDebug>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <unistd.h>

PtzControlWorker::PtzControlWorker(QObject *parent)
    : QObject(parent)
{
}

PtzControlWorker::~PtzControlWorker()
{
    stopControl();
}

bool PtzControlWorker::isRunning() const
{
    return m_running.load();
}

bool PtzControlWorker::isReady() const
{
    return m_ready.load();
}

bool PtzControlWorker::setCameraProfile(const PtzCameraProfile &profile,
                                        QString *errorMessage)
{
    if (m_running.load()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("VISCA控制运行中，必须先停止再切换配置");
        }
        return false;
    }
    if (profile.id.isEmpty() || profile.serialDevice.isEmpty()
            || !profile.viscaEnabled) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无效的VISCA相机配置");
        }
        return false;
    }
    if (profile.serialBaudRate != 9600) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("当前VISCA后端仅支持9600 8N1");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(m_configMutex);
    m_profile = profile;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

PtzCameraProfile PtzControlWorker::cameraProfile() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_profile;
}

void PtzControlWorker::startControl()
{
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        if (m_profile.id.isEmpty()) {
            emit stateChanged(false, QStringLiteral("PTZ控制未配置，请先应用相机配置"));
            return;
        }
    }

    if (m_running.load()) {
        emit stateChanged(m_ready.load(),
                          m_ready.load() ? "RS232 VISCA已连接" : "RS232 VISCA正在连接");
        return;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }

    m_stopReq.store(false);
    m_ready.store(false);
    m_running.store(true);
    m_thread = std::thread(&PtzControlWorker::workerMain, this);
}

void PtzControlWorker::stopControl()
{
    m_stopReq.store(true);
    m_queueCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_ready.store(false);
    m_running.store(false);
}

void PtzControlWorker::moveUp()        { enqueue(Command::MoveUp); }
void PtzControlWorker::moveDown()      { enqueue(Command::MoveDown); }
void PtzControlWorker::moveLeft()      { enqueue(Command::MoveLeft); }
void PtzControlWorker::moveRight()     { enqueue(Command::MoveRight); }
void PtzControlWorker::moveUpLeft()    { enqueue(Command::MoveUpLeft); }
void PtzControlWorker::moveUpRight()   { enqueue(Command::MoveUpRight); }
void PtzControlWorker::moveDownLeft()  { enqueue(Command::MoveDownLeft); }
void PtzControlWorker::moveDownRight() { enqueue(Command::MoveDownRight); }
void PtzControlWorker::stopPanTilt()   { enqueue(Command::StopPanTilt); }
void PtzControlWorker::home()          { enqueue(Command::Home); }

void PtzControlWorker::setPanSpeed(int speed)
{
    const int bounded = std::max(1, std::min(speed, 24));
    m_panSpeed.store(static_cast<uint8_t>(bounded));
}

void PtzControlWorker::setTiltSpeed(int speed)
{
    const int bounded = std::max(1, std::min(speed, 20));
    m_tiltSpeed.store(static_cast<uint8_t>(bounded));
}
void PtzControlWorker::zoomTele()     { enqueue(Command::ZoomTele); }
void PtzControlWorker::zoomWide()     { enqueue(Command::ZoomWide); }
void PtzControlWorker::stopZoom()     { enqueue(Command::StopZoom); }

void PtzControlWorker::enqueue(Command command)
{
    if (!m_running.load()) {
        emit commandError("RS232控制未启动");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        // 停止命令优先，清除尚未执行的同类运动命令，避免触摸抖动造成滞后。
        if (command == Command::StopPanTilt || command == Command::StopZoom) {
            m_queue.clear();
        }
        m_queue.push_back(command);
    }
    m_queueCv.notify_one();
}

void PtzControlWorker::workerMain()
{
    PtzCameraProfile profile;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        profile = m_profile;
    }

    ViscaSerial visca;
    const QByteArray encodedDevice = QFile::encodeName(profile.serialDevice);
    const char *device = encodedDevice.constData();

    if (!visca.openPort(device)) {
        m_running.store(false);
        emit stateChanged(false,
                          QString("打开%1失败：%2")
                          .arg(QString::fromLocal8Bit(device),
                               QString::fromStdString(visca.lastError())));
        return;
    }

    bool linkOk = false;
    for (int retry = 1; retry <= 3 && !m_stopReq.load(); ++retry) {
        emit stateChanged(false,
                          QString("VISCA初始化，第%1次尝试").arg(retry));
        const bool addressOk = visca.addressSet(800);
        const bool clearOk = addressOk && visca.interfaceClear(800);
        linkOk = addressOk && clearOk;
        if (!linkOk) {
            usleep(300000);
        }
    }

    if (!linkOk || m_stopReq.load()) {
        visca.closePort();
        m_running.store(false);
        m_ready.store(false);
        emit stateChanged(false,
                          linkOk ? "RS232控制已停止" : "VISCA初始化失败，请检查RS232连接");
        return;
    }

    m_ready.store(true);
    emit stateChanged(true,
                      QStringLiteral("%1：RS232 VISCA %2，9600 8N1")
                      .arg(profile.displayName, profile.serialDevice));

    while (!m_stopReq.load()) {
        Command command;
        bool haveCommand = false;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait_for(lock,
                               std::chrono::milliseconds(100),
                               [this] {
                                   return m_stopReq.load() || !m_queue.empty();
                               });

            if (m_stopReq.load()) {
                break;
            }

            if (!m_queue.empty()) {
                command = m_queue.front();
                m_queue.pop_front();
                haveCommand = true;
            }
        }

        if (!haveCommand) {
            (void)visca.drainPackets();
            continue;
        }

        const uint8_t panSpeed = m_panSpeed.load();
        const uint8_t tiltSpeed = m_tiltSpeed.load();

        bool ok = true;
        switch (command) {
        case Command::MoveUp:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x03, 0x01);
            break;
        case Command::MoveDown:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x03, 0x02);
            break;
        case Command::MoveLeft:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x01, 0x03);
            break;
        case Command::MoveRight:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x02, 0x03);
            break;
        case Command::MoveUpLeft:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x01, 0x01);
            break;
        case Command::MoveUpRight:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x02, 0x01);
            break;
        case Command::MoveDownLeft:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x01, 0x02);
            break;
        case Command::MoveDownRight:
            ok = visca.panTiltDrive(panSpeed, tiltSpeed, 0x02, 0x02);
            break;
        case Command::StopPanTilt:
            ok = visca.panTiltStop(panSpeed, tiltSpeed);
            break;
        case Command::Home:
            (void)visca.panTiltStop(panSpeed, tiltSpeed);
            ok = visca.home();
            break;
        case Command::ZoomTele:
            ok = visca.zoomTele();
            break;
        case Command::ZoomWide:
            ok = visca.zoomWide();
            break;
        case Command::StopZoom:
            ok = visca.zoomStop();
            break;
        }

        if (!ok) {
            emit commandError(QString::fromStdString(visca.lastError()));
        }
        (void)visca.drainPackets();
    }

    if (visca.isOpen()) {
        (void)visca.panTiltStop(m_panSpeed.load(), m_tiltSpeed.load());
        (void)visca.zoomStop();
        usleep(100000);
        visca.closePort();
    }

    m_ready.store(false);
    m_running.store(false);
    emit stateChanged(false, "RS232 VISCA已停止");
}
