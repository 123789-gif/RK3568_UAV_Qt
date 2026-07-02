#include "ptz/ptz_camera_profile.h"

namespace {

PtzCameraProfile makeHdUsbProfile()
{
    PtzCameraProfile profile;
    profile.selection = PtzProfileSelection::HdUsbPtz;
    profile.id = QStringLiteral("hd_usb_ptz_33f1_1003");
    profile.displayName = QStringLiteral("当前HD USB PTZ");
    profile.usbVendorId = 0x33f1;
    profile.usbProductId = 0x1003;
    profile.usbProductHint = QStringLiteral("HD Camera");
    profile.pixelFormat = V4L2_PIX_FMT_MJPEG;
    profile.captureWidth = 960;
    profile.captureHeight = 540;
    profile.captureFps = 20;
    profile.displayAspectWidth = 16;
    profile.displayAspectHeight = 9;
    profile.serialDevice = QStringLiteral("/dev/ttyS0");
    profile.serialBaudRate = 9600;
    profile.viscaEnabled = true;
    return profile;
}

PtzCameraProfile makeSonyEviD100Profile()
{
    PtzCameraProfile profile;
    profile.selection = PtzProfileSelection::SonyEviD100;
    profile.id = QStringLiteral("sony_evi_d100_ms210x");
    profile.displayName = QStringLiteral("Sony EVI-D100 + MS210x");
    profile.usbVendorId = 0x534d;
    profile.usbProductId = 0x0021;
    profile.usbProductHint = QStringLiteral("AV TO USB2.0");
    profile.usbSerialHint = QStringLiteral("20200909");
    profile.pixelFormat = V4L2_PIX_FMT_MJPEG;
    profile.captureWidth = 720;
    profile.captureHeight = 480;
    profile.captureFps = 25;
    profile.displayAspectWidth = 4;
    profile.displayAspectHeight = 3;
    profile.serialDevice = QStringLiteral("/dev/ttyS0");
    profile.serialBaudRate = 9600;
    profile.viscaEnabled = true;
    return profile;
}

} // namespace

QList<PtzCameraProfile> supportedPtzCameraProfiles()
{
    return {
        makeHdUsbProfile(),
        makeSonyEviD100Profile()
    };
}

PtzCameraProfile ptzCameraProfile(PtzProfileSelection selection)
{
    const QList<PtzCameraProfile> profiles = supportedPtzCameraProfiles();
    for (const PtzCameraProfile &profile : profiles) {
        if (profile.selection == selection) {
            return profile;
        }
    }
    return PtzCameraProfile();
}

QString ptzProfileSelectionName(PtzProfileSelection selection)
{
    switch (selection) {
    case PtzProfileSelection::Auto:
        return QStringLiteral("自动识别");
    case PtzProfileSelection::HdUsbPtz:
        return QStringLiteral("当前HD USB PTZ");
    case PtzProfileSelection::SonyEviD100:
        return QStringLiteral("Sony EVI-D100 + MS210x");
    }
    return QStringLiteral("未知配置");
}

QString v4l2PixelFormatName(uint32_t pixelFormat)
{
    char text[5] = {
        static_cast<char>(pixelFormat & 0xff),
        static_cast<char>((pixelFormat >> 8) & 0xff),
        static_cast<char>((pixelFormat >> 16) & 0xff),
        static_cast<char>((pixelFormat >> 24) & 0xff),
        '\0'
    };
    return QString::fromLatin1(text);
}
