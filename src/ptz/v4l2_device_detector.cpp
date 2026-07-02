#include "ptz/v4l2_device_detector.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

bool xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret == 0;
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

bool parseHex16(const QString &text, quint16 *value)
{
    if (!value) {
        return false;
    }
    bool ok = false;
    const uint parsed = text.trimmed().toUInt(&ok, 16);
    if (!ok || parsed > 0xffffU) {
        return false;
    }
    *value = static_cast<quint16>(parsed);
    return true;
}

struct UsbIdentity
{
    bool valid = false;
    quint16 vendorId = 0;
    quint16 productId = 0;
    QString sysfsPath;
    QString product;
    QString serial;
};

UsbIdentity queryUsbIdentity(const QString &videoNode)
{
    UsbIdentity identity;
    const QString nodeName = QFileInfo(videoNode).fileName();
    QString path = QFileInfo(QStringLiteral("/sys/class/video4linux/%1/device")
                             .arg(nodeName)).canonicalFilePath();

    while (!path.isEmpty() && path.startsWith(QStringLiteral("/sys/"))) {
        const QString vendorText = readTextFile(QDir(path).filePath(QStringLiteral("idVendor")));
        const QString productText = readTextFile(QDir(path).filePath(QStringLiteral("idProduct")));
        quint16 vendorId = 0;
        quint16 productId = 0;
        if (parseHex16(vendorText, &vendorId)
                && parseHex16(productText, &productId)) {
            identity.valid = true;
            identity.vendorId = vendorId;
            identity.productId = productId;
            identity.sysfsPath = path;
            identity.product = readTextFile(QDir(path).filePath(QStringLiteral("product")));
            identity.serial = readTextFile(QDir(path).filePath(QStringLiteral("serial")));
            return identity;
        }

        QDir parent(path);
        if (!parent.cdUp()) {
            break;
        }
        const QString parentPath = parent.canonicalPath();
        if (parentPath == path) {
            break;
        }
        path = parentPath;
    }

    return identity;
}

bool supportsFrameInterval(int fd,
                           uint32_t pixelFormat,
                           uint32_t width,
                           uint32_t height,
                           int fps)
{
    bool sawInterval = false;
    for (uint32_t index = 0; ; ++index) {
        v4l2_frmivalenum interval {};
        interval.index = index;
        interval.pixel_format = pixelFormat;
        interval.width = width;
        interval.height = height;

        if (!xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval)) {
            if (errno == EINVAL) {
                break;
            }
            return false;
        }

        sawInterval = true;
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            const uint32_t numerator = interval.discrete.numerator;
            const uint32_t denominator = interval.discrete.denominator;
            if (numerator > 0
                    && denominator == static_cast<uint32_t>(fps) * numerator) {
                return true;
            }
        } else {
            // 连续/步进帧率由驱动在S_PARM时裁剪，保留该候选。
            return true;
        }
    }

    // 有些UVC驱动不实现FRAMEINTERVALS枚举，交给S_PARM做最终校验。
    return !sawInterval;
}

bool supportsProfile(int fd, const PtzCameraProfile &profile)
{
    for (uint32_t formatIndex = 0; ; ++formatIndex) {
        v4l2_fmtdesc format {};
        format.index = formatIndex;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (!xioctl(fd, VIDIOC_ENUM_FMT, &format)) {
            if (errno == EINVAL) {
                break;
            }
            return false;
        }
        if (format.pixelformat != profile.pixelFormat) {
            continue;
        }

        for (uint32_t sizeIndex = 0; ; ++sizeIndex) {
            v4l2_frmsizeenum size {};
            size.index = sizeIndex;
            size.pixel_format = profile.pixelFormat;
            if (!xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size)) {
                if (errno == EINVAL) {
                    break;
                }
                return false;
            }

            bool sizeMatches = false;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                sizeMatches = int(size.discrete.width) == profile.captureWidth
                        && int(size.discrete.height) == profile.captureHeight;
            } else {
                sizeMatches = profile.captureWidth >= int(size.stepwise.min_width)
                        && profile.captureWidth <= int(size.stepwise.max_width)
                        && profile.captureHeight >= int(size.stepwise.min_height)
                        && profile.captureHeight <= int(size.stepwise.max_height);
            }

            if (sizeMatches
                    && supportsFrameInterval(fd,
                                             profile.pixelFormat,
                                             static_cast<uint32_t>(profile.captureWidth),
                                             static_cast<uint32_t>(profile.captureHeight),
                                             profile.captureFps)) {
                return true;
            }
        }
    }
    return false;
}

int videoNodeNumber(const QString &path)
{
    const QRegularExpression expression(QStringLiteral("video(\\d+)$"));
    const QRegularExpressionMatch match = expression.match(path);
    return match.hasMatch() ? match.captured(1).toInt() : 1000000;
}

QStringList enumerateVideoNodes()
{
    QDir devDir(QStringLiteral("/dev"));
    QStringList names = devDir.entryList({QStringLiteral("video[0-9]*")},
                                         QDir::System | QDir::Files,
                                         QDir::Name);
    QStringList nodes;
    for (const QString &name : names) {
        const QRegularExpressionMatch match = QRegularExpression(
                    QStringLiteral("^video\\d+$")).match(name);
        if (match.hasMatch()) {
            nodes.push_back(devDir.filePath(name));
        }
    }
    std::sort(nodes.begin(), nodes.end(), [](const QString &a, const QString &b) {
        return videoNodeNumber(a) < videoNodeNumber(b);
    });
    return nodes;
}

struct Candidate
{
    PtzResolvedDevice resolved;
};

QList<Candidate> detectCandidates(QStringList *diagnostics)
{
    QList<Candidate> candidates;
    const QStringList nodes = enumerateVideoNodes();
    const QList<PtzCameraProfile> profiles = supportedPtzCameraProfiles();

    if (nodes.isEmpty() && diagnostics) {
        diagnostics->push_back(QStringLiteral("未发现/dev/video数字节点"));
    }

    for (const QString &node : nodes) {
        const QByteArray encodedNode = QFile::encodeName(node);
        const int fd = open(encodedNode.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("%1打开失败：%2")
                                       .arg(node, QString::fromLocal8Bit(strerror(errno))));
            }
            continue;
        }

        v4l2_capability capability {};
        if (!xioctl(fd, VIDIOC_QUERYCAP, &capability)) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("%1不是可查询的V4L2采集节点")
                                       .arg(node));
            }
            close(fd);
            continue;
        }

        const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                ? capability.device_caps : capability.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("%1已过滤：不支持VIDEO_CAPTURE+STREAMING")
                                       .arg(node));
            }
            close(fd);
            continue;
        }

        const UsbIdentity usb = queryUsbIdentity(node);
        if (!usb.valid) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("%1已过滤：无法读取USB VID/PID")
                                       .arg(node));
            }
            close(fd);
            continue;
        }

        bool matchedIdentity = false;
        for (const PtzCameraProfile &profile : profiles) {
            if (usb.vendorId != profile.usbVendorId
                    || usb.productId != profile.usbProductId) {
                continue;
            }
            matchedIdentity = true;

            if (!supportsProfile(fd, profile)) {
                if (diagnostics) {
                    diagnostics->push_back(QStringLiteral(
                                                "%1匹配%2，但不支持%3 %4x%5@%6")
                                           .arg(node,
                                                profile.displayName,
                                                v4l2PixelFormatName(profile.pixelFormat))
                                           .arg(profile.captureWidth)
                                           .arg(profile.captureHeight)
                                           .arg(profile.captureFps));
                }
                continue;
            }

            Candidate candidate;
            candidate.resolved.valid = true;
            candidate.resolved.profile = profile;
            candidate.resolved.videoDevice = node;
            candidate.resolved.usbSysfsPath = usb.sysfsPath;
            candidate.resolved.usbProduct = usb.product;
            candidate.resolved.usbSerial = usb.serial;
            candidates.push_back(candidate);
        }

        if (!matchedIdentity && diagnostics) {
            diagnostics->push_back(QStringLiteral("%1 USB=%2:%3，不属于已配置PTZ")
                                   .arg(node)
                                   .arg(usb.vendorId, 4, 16, QLatin1Char('0'))
                                   .arg(usb.productId, 4, 16, QLatin1Char('0')));
        }

        close(fd);
    }

    return candidates;
}

} // namespace

bool V4l2DeviceDetector::resolve(PtzProfileSelection selection,
                                 PtzResolvedDevice *resolved,
                                 QString *errorMessage,
                                 QStringList *diagnostics)
{
    if (!resolved || !errorMessage) {
        return false;
    }

    *resolved = PtzResolvedDevice();
    errorMessage->clear();
    if (diagnostics) {
        diagnostics->clear();
    }

    QStringList localDiagnostics;
    QList<Candidate> candidates = detectCandidates(&localDiagnostics);
    if (diagnostics) {
        *diagnostics = localDiagnostics;
    }

    if (selection != PtzProfileSelection::Auto) {
        QList<Candidate> selectedCandidates;
        std::set<QString> physicalDevices;
        for (const Candidate &candidate : candidates) {
            if (candidate.resolved.profile.selection != selection) {
                continue;
            }
            selectedCandidates.push_back(candidate);
            physicalDevices.insert(candidate.resolved.usbSysfsPath);
        }

        if (physicalDevices.size() > 1) {
            *errorMessage = QStringLiteral("检测到多个“%1”设备，当前版本无法自动区分，请只保留一台或后续增加设备选择")
                    .arg(ptzProfileSelectionName(selection));
            return false;
        }
        if (!selectedCandidates.isEmpty()) {
            *resolved = selectedCandidates.first().resolved;
            return true;
        }

        *errorMessage = QStringLiteral("未找到“%1”：请检查USB连接及目标格式能力")
                .arg(ptzProfileSelectionName(selection));
        if (!localDiagnostics.isEmpty()) {
            *errorMessage += QStringLiteral("；%1").arg(localDiagnostics.join(QStringLiteral("；")));
        }
        return false;
    }

    if (candidates.isEmpty()) {
        *errorMessage = QStringLiteral("自动识别失败：未发现已支持的PTZ视频设备");
        if (!localDiagnostics.isEmpty()) {
            *errorMessage += QStringLiteral("；%1").arg(localDiagnostics.join(QStringLiteral("；")));
        }
        return false;
    }

    std::set<QString> distinctPhysicalDevices;
    for (const Candidate &candidate : candidates) {
        distinctPhysicalDevices.insert(candidate.resolved.usbSysfsPath);
    }

    if (distinctPhysicalDevices.size() > 1) {
        QStringList names;
        for (const Candidate &candidate : candidates) {
            const QString name = QStringLiteral("%1(%2)")
                    .arg(candidate.resolved.profile.displayName,
                         candidate.resolved.videoDevice);
            if (!names.contains(name)) {
                names.push_back(name);
            }
        }
        *errorMessage = QStringLiteral("自动识别到多个PTZ设备（%1），请手动选择")
                .arg(names.join(QStringLiteral("、")));
        return false;
    }

    // 同一USB设备可能暴露多个video节点；按编号选择首个满足能力的节点。
    *resolved = candidates.first().resolved;
    return true;
}


bool V4l2DeviceDetector::isPresent(const PtzResolvedDevice &device,
                                   QString *reason)
{
    if (reason) {
        reason->clear();
    }
    if (!device.valid || device.videoDevice.isEmpty()
            || device.profile.id.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("活动PTZ端点无效");
        }
        return false;
    }

    const QFileInfo nodeInfo(device.videoDevice);
    if (!nodeInfo.exists()) {
        if (reason) {
            *reason = QStringLiteral("%1已不存在").arg(device.videoDevice);
        }
        return false;
    }

    const UsbIdentity usb = queryUsbIdentity(device.videoDevice);
    if (!usb.valid) {
        if (reason) {
            *reason = QStringLiteral("无法读取%1所属USB设备").arg(device.videoDevice);
        }
        return false;
    }

    if (usb.vendorId != device.profile.usbVendorId
            || usb.productId != device.profile.usbProductId) {
        if (reason) {
            *reason = QStringLiteral("%1已被其他USB设备占用：%2:%3")
                    .arg(device.videoDevice)
                    .arg(usb.vendorId, 4, 16, QLatin1Char('0'))
                    .arg(usb.productId, 4, 16, QLatin1Char('0'));
        }
        return false;
    }

    if (!device.usbSysfsPath.isEmpty()
            && usb.sysfsPath != device.usbSysfsPath) {
        if (reason) {
            *reason = QStringLiteral("%1对应USB实例已变化")
                    .arg(device.videoDevice);
        }
        return false;
    }

    return true;
}
