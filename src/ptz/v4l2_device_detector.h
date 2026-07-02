#ifndef V4L2_DEVICE_DETECTOR_H
#define V4L2_DEVICE_DETECTOR_H

#include "ptz/ptz_camera_profile.h"

#include <QString>
#include <QStringList>

struct PtzResolvedDevice
{
    bool valid = false;
    PtzCameraProfile profile;
    QString videoDevice;
    QString usbSysfsPath;
    QString usbProduct;
    QString usbSerial;
};

class V4l2DeviceDetector
{
public:
    static bool resolve(PtzProfileSelection selection,
                        PtzResolvedDevice *resolved,
                        QString *errorMessage,
                        QStringList *diagnostics = nullptr);

    // 轻量检查当前已解析端点是否仍指向同一USB设备，不重新枚举格式。
    static bool isPresent(const PtzResolvedDevice &device,
                          QString *reason = nullptr);
};

#endif // V4L2_DEVICE_DETECTOR_H
