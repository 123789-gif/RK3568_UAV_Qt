#ifndef PTZ_CAMERA_PROFILE_H
#define PTZ_CAMERA_PROFILE_H

#include <QString>
#include <QList>
#include <QtGlobal>

#include <cstdint>

#include <linux/videodev2.h>

enum class PtzProfileSelection : int
{
    Auto = 0,
    HdUsbPtz = 1,
    SonyEviD100 = 2
};

struct PtzCameraProfile
{
    PtzProfileSelection selection = PtzProfileSelection::Auto;
    QString id;
    QString displayName;

    quint16 usbVendorId = 0;
    quint16 usbProductId = 0;
    QString usbProductHint;
    QString usbSerialHint;

    uint32_t pixelFormat = V4L2_PIX_FMT_MJPEG;
    int captureWidth = 0;
    int captureHeight = 0;
    int captureFps = 0;

    // 预览显示比例。EVI-D100 的720x480为非方形像素，按4:3显示。
    int displayAspectWidth = 0;
    int displayAspectHeight = 0;

    QString serialDevice;
    int serialBaudRate = 9600;
    bool viscaEnabled = true;
};

QList<PtzCameraProfile> supportedPtzCameraProfiles();
PtzCameraProfile ptzCameraProfile(PtzProfileSelection selection);
QString ptzProfileSelectionName(PtzProfileSelection selection);
QString v4l2PixelFormatName(uint32_t pixelFormat);

#endif // PTZ_CAMERA_PROFILE_H
