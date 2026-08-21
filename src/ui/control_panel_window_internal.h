#pragma once

// ControlPanelWindow 各实现文件共享的私有依赖与辅助函数；不属于公共接口。
#include "ptz/ptz_capture_worker.h"
#include "ptz/ptz_control_worker.h"
#include "system/fan_controller.h"
#include "ui/violation_preview_widget.h"
#include "ui/wrong_way_preview_widget.h"
#include "npu/violation_rule_config.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QDebug>
#include <QScreen>
#include <QStackedWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QCoreApplication>
#include <QFrame>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QStyle>
#include <QPixmap>
#include <QSizePolicy>
#include <QSlider>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QDialog>
#include <QMessageBox>
#include <QTemporaryFile>
#include <QFileInfoList>
#include <QSet>
#include <QList>

#include <cstdio>
#include <csetjmp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>
#include <functional>
#include <sys/statvfs.h>

extern "C" {
#include <jpeglib.h>
}

namespace {

struct JpegSaveErrorManager
{
    jpeg_error_mgr base;
    jmp_buf jumpBuffer;
    char message[JMSG_LENGTH_MAX] = {0};
};

void jpegSaveErrorExit(j_common_ptr cinfo)
{
    auto *error = reinterpret_cast<JpegSaveErrorManager *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message);
    longjmp(error->jumpBuffer, 1);
}

bool saveImageAsJpeg(const QImage &source,
                     const QString &filePath,
                     int quality,
                     QString *errorMessage)
{
    if (source.isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("截图图像为空");
        }
        return false;
    }

    const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("截图转换为RGB888失败");
        }
        return false;
    }

    const QByteArray encodedPath = QFile::encodeName(filePath);
    FILE *output = std::fopen(encodedPath.constData(), "wb");
    if (!output) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建截图文件：%1").arg(filePath);
        }
        return false;
    }

    jpeg_compress_struct cinfo {};
    JpegSaveErrorManager jerr {};
    bool created = false;

    cinfo.err = jpeg_std_error(&jerr.base);
    jerr.base.error_exit = jpegSaveErrorExit;

    if (setjmp(jerr.jumpBuffer)) {
        if (created) {
            jpeg_destroy_compress(&cinfo);
        }
        std::fclose(output);
        if (errorMessage) {
            *errorMessage = QStringLiteral("JPEG编码失败：%1")
                    .arg(QString::fromLocal8Bit(jerr.message));
        }
        return false;
    }

    jpeg_create_compress(&cinfo);
    created = true;
    jpeg_stdio_dest(&cinfo, output);

    cinfo.image_width = static_cast<JDIMENSION>(rgb.width());
    cinfo.image_height = static_cast<JDIMENSION>(rgb.height());
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, qBound(1, quality, 100), TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        uchar *rowData = const_cast<uchar *>(
                    rgb.constScanLine(static_cast<int>(cinfo.next_scanline)));
        JSAMPROW row = reinterpret_cast<JSAMPROW>(rowData);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(output);
    return true;
}

struct JpegLoadErrorManager
{
    jpeg_error_mgr base;
    jmp_buf jumpBuffer;
    char message[JMSG_LENGTH_MAX] = {0};
};

void jpegLoadErrorExit(j_common_ptr cinfo)
{
    auto *error = reinterpret_cast<JpegLoadErrorManager *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message);
    longjmp(error->jumpBuffer, 1);
}

bool loadJpegImage(const QString &filePath,
                   QImage *image,
                   QString *errorMessage)
{
    if (!image) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("输出图像指针为空");
        }
        return false;
    }

    image->detach();
    *image = QImage();

    const QByteArray encodedPath = QFile::encodeName(filePath);
    FILE *input = std::fopen(encodedPath.constData(), "rb");
    if (!input) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法打开图片：%1").arg(filePath);
        }
        return false;
    }

    jpeg_decompress_struct cinfo {};
    JpegLoadErrorManager jerr {};
    bool created = false;

    cinfo.err = jpeg_std_error(&jerr.base);
    jerr.base.error_exit = jpegLoadErrorExit;

    if (setjmp(jerr.jumpBuffer)) {
        if (created) {
            jpeg_destroy_decompress(&cinfo);
        }
        std::fclose(input);
        if (errorMessage) {
            *errorMessage = QStringLiteral("JPEG解码失败：%1")
                    .arg(QString::fromLocal8Bit(jerr.message));
        }
        return false;
    }

    jpeg_create_decompress(&cinfo);
    created = true;
    jpeg_stdio_src(&cinfo, input);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(input);
        if (errorMessage) {
            *errorMessage = QStringLiteral("JPEG文件头无效：%1").arg(filePath);
        }
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width == 0 || cinfo.output_height == 0
            || cinfo.output_components != 3) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        std::fclose(input);
        if (errorMessage) {
            *errorMessage = QStringLiteral("JPEG输出格式不受支持：%1").arg(filePath);
        }
        return false;
    }

    QImage decoded(static_cast<int>(cinfo.output_width),
                   static_cast<int>(cinfo.output_height),
                   QImage::Format_RGB888);
    if (decoded.isNull()) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        std::fclose(input);
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法为截图分配图像内存");
        }
        return false;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = reinterpret_cast<JSAMPROW>(
                    decoded.scanLine(static_cast<int>(cinfo.output_scanline)));
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(input);

    *image = decoded;
    return true;
}

QString decodeMountInfoField(QString value)
{
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return value;
}

bool queryMountPoint(const QString &mountPoint,
                     bool *readOnly,
                     QString *source,
                     QString *errorMessage)
{
    if (readOnly) {
        *readOnly = false;
    }
    if (source) {
        source->clear();
    }

    QFile mountInfo(QStringLiteral("/proc/self/mountinfo"));
    if (!mountInfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法读取/proc/self/mountinfo");
        }
        return false;
    }

    /*
     * /proc 下的虚拟文件经常报告 size=0。部分 Qt 5.12 环境中，
     * 在第一次读取前使用 atEnd() 可能直接得到 true，因此先 readAll()
     * 再逐行解析，避免把已挂载的 SD 卡误判为未挂载。
     */
    const QByteArray content = mountInfo.readAll();
    mountInfo.close();

    const QString expected = QDir::cleanPath(mountPoint);
    const QList<QByteArray> lines = content.split('\n');

    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 10) {
            continue;
        }

        const QString currentMount = QDir::cleanPath(
                    decodeMountInfoField(QString::fromUtf8(fields.at(4))));
        if (currentMount != expected) {
            continue;
        }

        // mountinfo 第 6 个字段是挂载选项，只精确匹配独立的 ro。
        const QList<QByteArray> mountOptions = fields.at(5).split(',');
        if (readOnly) {
            *readOnly = mountOptions.contains(QByteArrayLiteral("ro"));
        }

        // “-”后依次是文件系统类型、设备源、超级块选项。
        const int separator = fields.indexOf(QByteArrayLiteral("-"));
        if (source && separator >= 0 && separator + 2 < fields.size()) {
            *source = decodeMountInfoField(
                        QString::fromUtf8(fields.at(separator + 2)));
        }

        qInfo() << "Snapshot storage mounted:"
                << "source=" << (source ? *source : QString())
                << "target=" << currentMount
                << "readOnly=" << (readOnly ? *readOnly : false);
        return true;
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("SD卡未挂载到%1").arg(mountPoint);
    }
    qWarning() << "Snapshot storage mount point not found:" << expected;
    return false;
}

bool prepareSnapshotStorage(const QString &mountPoint,
                            const QString &manualDirectory,
                            const QString &detectedDirectory,
                            QString *errorMessage)
{
    bool readOnly = false;
    QString source;
    QString mountError;
    if (!queryMountPoint(mountPoint, &readOnly, &source, &mountError)) {
        if (errorMessage) {
            *errorMessage = mountError;
        }
        return false;
    }

    if (readOnly) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("%1为只读挂载").arg(mountPoint);
        }
        return false;
    }

    QDir dir;
    if (!dir.mkpath(manualDirectory)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建手动抓拍目录：%1")
                    .arg(manualDirectory);
        }
        return false;
    }
    if (!dir.mkpath(detectedDirectory)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建目标抓拍目录：%1")
                    .arg(detectedDirectory);
        }
        return false;
    }

    QTemporaryFile writeProbe(QDir(manualDirectory).filePath(
                                  QStringLiteral(".qt_snapshot_write_test_XXXXXX")));
    writeProbe.setAutoRemove(true);
    if (!writeProbe.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("抓拍目录不可写：%1")
                    .arg(manualDirectory);
        }
        return false;
    }
    if (writeProbe.write("ok", 2) != 2 || !writeProbe.flush()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("抓拍目录写入测试失败：%1")
                    .arg(manualDirectory);
        }
        return false;
    }
    writeProbe.close();

    struct statvfs storageStats {};
    const QByteArray encodedMountPoint = QFile::encodeName(mountPoint);
    if (::statvfs(encodedMountPoint.constData(), &storageStats) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法读取SD卡剩余空间：%1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return false;
    }

    const quint64 availableBytes = static_cast<quint64>(storageStats.f_bavail)
            * static_cast<quint64>(storageStats.f_frsize);
    if (availableBytes < 10ULL * 1024ULL * 1024ULL) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("SD卡剩余空间不足10 MB");
        }
        return false;
    }

    Q_UNUSED(source)
    return true;
}

QFileInfoList collectManualSnapshotFiles(const QString &manualDirectory,
                                         const QString &legacyDirectory)
{
    const QStringList filters = {
        QStringLiteral("*.jpg"),
        QStringLiteral("*.jpeg"),
        QStringLiteral("*.JPG"),
        QStringLiteral("*.JPEG")
    };

    QFileInfoList result;
    QSet<QString> seenPaths;

    const auto appendDirectory = [&](const QString &directoryPath) {
        QDir directory(directoryPath);
        const QFileInfoList entries = directory.entryInfoList(
                    filters,
                    QDir::Files | QDir::Readable | QDir::NoSymLinks,
                    QDir::Time);
        for (const QFileInfo &entry : entries) {
            const QString canonical = entry.canonicalFilePath().isEmpty()
                    ? entry.absoluteFilePath()
                    : entry.canonicalFilePath();
            if (seenPaths.contains(canonical)) {
                continue;
            }
            seenPaths.insert(canonical);
            result.append(entry);
        }
    };

    appendDirectory(manualDirectory);
    if (QDir::cleanPath(legacyDirectory) != QDir::cleanPath(manualDirectory)) {
        appendDirectory(legacyDirectory);
    }

    std::sort(result.begin(), result.end(),
              [](const QFileInfo &left, const QFileInfo &right) {
        if (left.lastModified() == right.lastModified()) {
            return left.fileName() > right.fileName();
        }
        return left.lastModified() > right.lastModified();
    });

    return result;
}

QString aiTaskDisplayName(NpuTaskType task)
{
    switch (task) {
    case NpuTaskType::PlateCascade:
        return QStringLiteral("车牌识别");
    case NpuTaskType::TrafficViolation:
        return QStringLiteral("闯红灯检测");
    case NpuTaskType::VehicleWrongWay:
        return QStringLiteral("逆行检测");
    case NpuTaskType::Pedestrian:
    default:
        return QStringLiteral("行人检测");
    }
}

QString aiTaskButtonText(NpuTaskType task, bool active, bool pending)
{
    const QString name = aiTaskDisplayName(task);
    if (pending) {
        return QStringLiteral("● %1").arg(name);
    }
    if (active) {
        return QStringLiteral("✓ %1").arg(name);
    }
    return name;
}


QString aiSettingsPath()
{
    QDir binDir(QCoreApplication::applicationDirPath());
    const QString configDir = QDir::cleanPath(binDir.filePath(QStringLiteral("../config")));
    QDir().mkpath(configDir);
    return QDir(configDir).filePath(QStringLiteral("ai_panel.ini"));
}

QString violationSettingsPath()
{
    QDir binDir(QCoreApplication::applicationDirPath());
    const QString configDir = QDir::cleanPath(binDir.filePath(QStringLiteral("../config")));
    QDir().mkpath(configDir);
    return QDir(configDir).filePath(QStringLiteral("pedestrian_violation.ini"));
}

QString wrongWaySettingsPath(int sourceChannel)
{
    QDir binDir(QCoreApplication::applicationDirPath());
    const QString configDir = QDir::cleanPath(binDir.filePath(QStringLiteral("../config")));
    QDir().mkpath(configDir);
    if (sourceChannel == 0) {
        return QDir(configDir).filePath(
                    QStringLiteral("vehicle_wrong_way_overview.ini"));
    }
    return QDir(configDir).filePath(
                QStringLiteral("vehicle_wrong_way_ch%1.ini").arg(sourceChannel));
}

QString violationLightText(NpuTrafficLightState state)
{
    switch (state) {
    case NpuTrafficLightState::Red:
        return QStringLiteral("红灯");
    case NpuTrafficLightState::Green:
        return QStringLiteral("绿灯");
    case NpuTrafficLightState::Unknown:
    default:
        return QStringLiteral("未知");
    }
}

QString violationPedestrianStateText(NpuPedestrianState state)
{
    switch (state) {
    case NpuPedestrianState::RedAccumulating:
        return QStringLiteral("红灯区域累计中");
    case NpuPedestrianState::RedViolation:
        return QStringLiteral("闯红灯");
    case NpuPedestrianState::Normal:
    default:
        return QStringLiteral("正常");
    }
}

QString ptzSettingsPath()
{
    QDir binDir(QCoreApplication::applicationDirPath());
    const QString configDir = QDir::cleanPath(binDir.filePath(QStringLiteral("../config")));
    QDir().mkpath(configDir);
    return QDir(configDir).filePath(QStringLiteral("ptz_camera.ini"));
}

bool samePtzEndpoint(const PtzResolvedDevice &a, const PtzResolvedDevice &b)
{
    return a.valid && b.valid
            && a.profile.id == b.profile.id
            && a.videoDevice == b.videoDevice
            && a.usbSysfsPath == b.usbSysfsPath;
}

QString ptzEndpointText(const PtzResolvedDevice &device)
{
    if (!device.valid) {
        return QStringLiteral("none");
    }
    return QStringLiteral("%1 %2 USB %3:%4")
            .arg(device.profile.displayName,
                 device.videoDevice)
            .arg(device.profile.usbVendorId, 4, 16, QLatin1Char('0'))
            .arg(device.profile.usbProductId, 4, 16, QLatin1Char('0'));
}


QString fpgaModeName(quint32 mode)
{
    switch (mode) {
    case 0U: return QStringLiteral("原始画面");
    case 1U: return QStringLiteral("反色诊断");
    case 2U: return QStringLiteral("固定色诊断");
    case 3U: return QStringLiteral("暗光增强");
    case 4U: return QStringLiteral("边缘检测");
    case 5U: return QStringLiteral("图像去雾");
    default: return QStringLiteral("未知");
    }
}

QString fpgaModeButtonText(quint32 mode, bool active, bool pending)
{
    QString title;
    QString subtitle;
    switch (mode) {
    case 0U:
        title = QStringLiteral("原始画面");
        subtitle = QStringLiteral("Bypass");
        break;
    case 3U:
        title = QStringLiteral("暗光增强");
        subtitle = QStringLiteral("Gamma");
        break;
    case 4U:
        title = QStringLiteral("边缘检测");
        subtitle = QStringLiteral("Sobel");
        break;
    case 5U:
        title = QStringLiteral("图像去雾");
        subtitle = QStringLiteral("Dehaze");
        break;
    default:
        title = fpgaModeName(mode);
        subtitle = QStringLiteral("Mode %1").arg(mode);
        break;
    }

    // 按钮始终保持单行固定文字；切换中状态由橙色样式和下方状态栏表示，
    // 避免文字换行或变长引起按钮高度、页面布局跳动。
    Q_UNUSED(pending)
    const QString text = QStringLiteral("%1 %2").arg(title, subtitle);
    return active ? QStringLiteral("✓ %1").arg(text) : text;
}

QString fpgaIpVersionText(quint32 version)
{
    return QStringLiteral("%1.%2")
            .arg((version >> 16) & 0xffffU)
            .arg(version & 0xffffU);
}

QString ethernetOutputButtonText(bool enable, bool active, bool pending)
{
    const QString title = enable ? QStringLiteral("开启")
                                 : QStringLiteral("关闭");
    if (pending) {
        return enable ? QStringLiteral("正在开启……")
                      : QStringLiteral("正在关闭……");
    }
    if (active) {
        return QStringLiteral("✓ %1").arg(title);
    }
    return title;
}

QString ipv4Text(quint32 ipv4)
{
    return QStringLiteral("%1.%2.%3.%4")
            .arg((ipv4 >> 24) & 0xffU)
            .arg((ipv4 >> 16) & 0xffU)
            .arg((ipv4 >> 8) & 0xffU)
            .arg(ipv4 & 0xffU);
}

} // namespace
