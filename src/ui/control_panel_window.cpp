#include "ui/control_panel_window.h"
#include "ui_control_panel_window.h"
#include "ptz/ptz_capture_worker.h"
#include "ptz/ptz_control_worker.h"
#include "system/fan_controller.h"

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
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSpinBox>
#include <QTableWidget>
#include <QScrollArea>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QSettings>
#include <QCoreApplication>
#include <QFrame>
#include <QButtonGroup>
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
    return task == NpuTaskType::PlateCascade
            ? QStringLiteral("车牌识别")
            : QStringLiteral("行人检测");
}

QString aiSettingsPath()
{
    QDir binDir(QCoreApplication::applicationDirPath());
    const QString configDir = QDir::cleanPath(binDir.filePath(QStringLiteral("../config")));
    QDir().mkpath(configDir);
    return QDir(configDir).filePath(QStringLiteral("ai_panel.ini"));
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

QString aiBoxText(const NpuBox &box)
{
    return QStringLiteral("%1,%2,%3,%4")
            .arg(qRound(box.x))
            .arg(qRound(box.y))
            .arg(qRound(box.w))
            .arg(qRound(box.h));
}

} // namespace

ControlPanelWindow::ControlPanelWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::ControlPanelWindow)
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("智能交通感知系统控制台"));

    m_display = new DrmRgaDisplay(this);
    m_ptzCapture = new PtzCaptureWorker(this);
    m_ptzControl = new PtzControlWorker(this);
    m_fanController = new FanController(this);

    connect(m_display, &DrmRgaDisplay::stateChanged,
            this, &ControlPanelWindow::onDisplayStateChanged,
            Qt::QueuedConnection);

    connect(m_display, &DrmRgaDisplay::errorOccurred,
            this, &ControlPanelWindow::onDisplayError,
            Qt::QueuedConnection);

    connect(m_display, &DrmRgaDisplay::statsUpdated,
            this, &ControlPanelWindow::onDisplayStatsUpdated,
            Qt::QueuedConnection);

    connect(m_display, &DrmRgaDisplay::aiStatsUpdated,
            this, &ControlPanelWindow::onDisplayAiStatsUpdated,
            Qt::QueuedConnection);

    connect(m_ptzCapture, &PtzCaptureWorker::stateChanged,
            this, &ControlPanelWindow::onPtzCaptureStateChanged,
            Qt::QueuedConnection);

    connect(m_ptzCapture, &PtzCaptureWorker::statsUpdated,
            this, &ControlPanelWindow::onPtzCaptureStatsUpdated,
            Qt::QueuedConnection);

    connect(m_ptzCapture, &PtzCaptureWorker::deviceLost,
            this, &ControlPanelWindow::onPtzCaptureDeviceLost,
            Qt::QueuedConnection);

    connect(m_ptzControl, &PtzControlWorker::stateChanged,
            this, &ControlPanelWindow::onPtzControlStateChanged,
            Qt::QueuedConnection);

    connect(m_ptzControl, &PtzControlWorker::commandError,
            this, &ControlPanelWindow::onPtzControlError,
            Qt::QueuedConnection);

    setupControlUi();
    applyStyle();
    loadAiSettings();
    onApplyAiConfigClicked();
    loadPtzSettings();

    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout,
            this, &ControlPanelWindow::onStatusTimer);
    m_statusTimer->start(1000);

    // AI结果只轮询最新快照，不从推理线程向UI排队。5 Hz足以更新状态和结果表，
    // 同时避免DSI界面频繁重绘影响主显示链路。
    m_aiUiTimer = new QTimer(this);
    connect(m_aiUiTimer, &QTimer::timeout,
            this, &ControlPanelWindow::onAiUiTimer);
    m_aiUiTimer->start(200);

    // Qt按20 fps取最新帧。当前低CPU版本的PTZ采集同样为20 fps，
    // 不向主线程排队，也不影响PCIe HDMI显示链路。
    m_ptzFrameTimer = new QTimer(this);
    connect(m_ptzFrameTimer, &QTimer::timeout,
            this, &ControlPanelWindow::onPtzFrameTimer);
    m_ptzFrameTimer->start(50);

    // 运行期间轮询V4L2设备集合。1秒周期只在设备变化或服务异常时
    // 执行重配置，不影响PCIe/HDMI主视频链路。
    m_ptzDeviceMonitorTimer = new QTimer(this);
    connect(m_ptzDeviceMonitorTimer, &QTimer::timeout,
            this, &ControlPanelWindow::onPtzDeviceMonitorTimer);
    m_ptzDeviceMonitorTimer->start(1000);

    updateStatusText();

    // 界面进入事件循环后自动启动PTZ视频和RS232控制。
    // 若设备被其他程序占用，界面会显示错误，原PCIe/HDMI功能不受影响。
    QTimer::singleShot(0, this, [this]() {
        if (m_fanController) {
            if (!m_fanController->initialize()) {
                m_lastError = QStringLiteral("风扇控制：%1")
                        .arg(m_fanController->lastError());
            }
            updateFanUi();
        }
        // 保持原版本“程序启动后自动开启预览和控制”的行为。
        // 若设备尚未插入，则进入等待态，后续热插拔监测会自动恢复。
        m_ptzPreviewDesired = true;
        m_ptzControlDesired = true;
        (void)applyPtzProfileSelection(false);
        updatePtzServiceButtons();
    });
}

ControlPanelWindow::~ControlPanelWindow()
{
    if (m_aiUiTimer) {
        m_aiUiTimer->stop();
    }

    if (m_ptzFrameTimer) {
        m_ptzFrameTimer->stop();
    }

    if (m_ptzDeviceMonitorTimer) {
        m_ptzDeviceMonitorTimer->stop();
    }

    if (m_ptzCapture) {
        m_ptzCapture->stopCapture();
    }

    if (m_ptzControl) {
        // stopControl内部会先发送云台停止和变焦停止，再关闭串口。
        m_ptzControl->stopControl();
    }

    if (m_display) {
        m_display->stopDisplay();
    }

    delete ui;
}

void ControlPanelWindow::showWindows()
{
    setGeometry(0, 0, 1280, 720);
    show();

    qDebug() << "ControlPanelWindow geometry:" << geometry();

    const QList<QScreen *> screens = QApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        qDebug() << "Screen" << i
                 << "name =" << screens[i]->name()
                 << "geometry =" << screens[i]->geometry()
                 << "available =" << screens[i]->availableGeometry();
    }
}

void ControlPanelWindow::setupControlUi()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 10);
    root->setSpacing(8);

    QLabel *title = new QLabel(QStringLiteral("智能交通感知系统控制台"), central);
    title->setObjectName("titleLabel");
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    QHBoxLayout *body = new QHBoxLayout();
    body->setSpacing(10);

    body->addWidget(createNavPanel(), 0);

    m_stack = new QStackedWidget(central);
    m_stack->addWidget(createVideoPage());
    m_stack->addWidget(createPtzPage());
    m_stack->addWidget(createFpgaPage());
    m_stack->addWidget(createCpuPage());
    m_stack->addWidget(createAiPage());
    m_stack->addWidget(createStatusPage());
    body->addWidget(m_stack, 1);

    root->addLayout(body, 1);

    m_bottomStatus = new QLabel(central);
    m_bottomStatus->setObjectName("bottomStatus");
    m_bottomStatus->setMinimumHeight(42);
    m_bottomStatus->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    root->addWidget(m_bottomStatus);
}

QWidget *ControlPanelWindow::createNavPanel()
{
    QFrame *nav = new QFrame(this);
    nav->setObjectName("navPanel");
    nav->setFixedWidth(178);

    QVBoxLayout *layout = new QVBoxLayout(nav);
    layout->setContentsMargins(10, 12, 10, 12);
    layout->setSpacing(10);

    QStringList names;
    names << "视频显示" << "PTZ控制" << "FPGA配置" << "CPU配置" << "AI推理" << "系统状态";

    QButtonGroup *group = new QButtonGroup(nav);
    group->setExclusive(true);

    for (int i = 0; i < names.size(); ++i) {
        QPushButton *btn = new QPushButton(names[i], nav);
        btn->setCheckable(true);
        btn->setMinimumHeight(54);
        btn->setObjectName("navButton");
        group->addButton(btn, i);
        layout->addWidget(btn);
        if (i == 0) {
            btn->setChecked(true);
        }
    }

    connect(group,
            static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked),
            this,
            &ControlPanelWindow::setCurrentPage);

    layout->addStretch(1);

    QLabel *hint = new QLabel("DSI：Qt + PTZ\nHDMI：PCIe视频", nav);
    hint->setObjectName("navHint");
    hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(hint);

    return nav;
}

QWidget *ControlPanelWindow::createVideoPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(8, 6, 8, 6);
    pageLayout->setSpacing(10);

    QWidget *leftPanel = new QWidget(page);
    leftPanel->setFixedWidth(390);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    QGroupBox *displayGroup = new QGroupBox("HDMI 输出控制", leftPanel);
    QGridLayout *grid = new QGridLayout(displayGroup);
    grid->setContentsMargins(14, 20, 14, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(9);

    m_btnStartStop = new QPushButton("启动/停止 HDMI", displayGroup);
    m_btnOsd = new QPushButton("AI结果OSD 开/关", displayGroup);

    m_resolutionCombo = new QComboBox(displayGroup);
    m_resolutionCombo->addItem("1920x1080@60", QSize(1920, 1080));
    m_resolutionCombo->addItem("1280x720@60", QSize(1280, 720));

    m_outputModeCombo = new QComboBox(displayGroup);
    m_outputModeCombo->addItem("原图显示", 0);
    m_outputModeCombo->addItem("测试框叠加", 1);
    m_outputModeCombo->addItem("推理框预留", 2);
    m_outputModeCombo->addItem("BEV预留", 3);

    QPushButton *applyVideo = new QPushButton("应用显示与选路", displayGroup);
    applyVideo->setMinimumHeight(50);

    grid->addWidget(m_btnStartStop, 0, 0);
    grid->addWidget(m_btnOsd, 0, 1);
    grid->addWidget(new QLabel("输出分辨率", displayGroup), 1, 0);
    grid->addWidget(m_resolutionCombo, 1, 1);
    grid->addWidget(new QLabel("输出模式", displayGroup), 2, 0);
    grid->addWidget(m_outputModeCombo, 2, 1);
    grid->addWidget(applyVideo, 3, 0, 1, 2);
    leftLayout->addWidget(displayGroup);

    QGroupBox *noteGroup = new QGroupBox("当前策略", leftPanel);
    QVBoxLayout *noteLayout = new QVBoxLayout(noteGroup);
    noteLayout->setContentsMargins(10, 18, 10, 8);
    QLabel *note = new QLabel(
                "总览：HDMI显示完整九宫格，并将完整九宫格画面送入NPU。\n"
                "CH1~CH9：裁出对应640×360单路，放大到HDMI全屏，并仅将该路送入NPU。",
                noteGroup);
    note->setWordWrap(true);
    note->setObjectName("navHint");
    noteLayout->addWidget(note);
    leftLayout->addWidget(noteGroup);
    leftLayout->addStretch(1);
    pageLayout->addWidget(leftPanel, 0);

    QGroupBox *routeGroup = new QGroupBox("九宫格通道选择", page);
    QVBoxLayout *routeLayout = new QVBoxLayout(routeGroup);
    routeLayout->setContentsMargins(14, 20, 14, 12);
    routeLayout->setSpacing(8);

    m_videoRouteGroup = new QButtonGroup(routeGroup);
    m_videoRouteGroup->setExclusive(true);

    QPushButton *overviewButton = new QPushButton(QStringLiteral("九宫格总览（NPU全图）"), routeGroup);
    overviewButton->setObjectName("videoRouteButton");
    overviewButton->setCheckable(true);
    overviewButton->setChecked(true);
    overviewButton->setMinimumHeight(54);
    m_videoRouteGroup->addButton(overviewButton, static_cast<int>(VideoRoute::Overview));
    routeLayout->addWidget(overviewButton);

    QGridLayout *channelGrid = new QGridLayout();
    channelGrid->setHorizontalSpacing(8);
    channelGrid->setVerticalSpacing(8);
    for (int channel = 1; channel <= 9; ++channel) {
        QPushButton *button = new QPushButton(QString("CH%1").arg(channel), routeGroup);
        button->setObjectName("videoRouteButton");
        button->setCheckable(true);
        button->setMinimumSize(150, 82);
        m_videoRouteGroup->addButton(button, channel);
        const int index = channel - 1;
        channelGrid->addWidget(button, index / 3, index % 3);
    }
    routeLayout->addLayout(channelGrid, 1);

    m_videoRouteSummaryLabel = new QLabel(QStringLiteral("当前应用：九宫格总览；NPU全图"), routeGroup);
    m_videoRouteSummaryLabel->setObjectName("ptzState");
    m_videoRouteSummaryLabel->setAlignment(Qt::AlignCenter);
    m_videoRouteSummaryLabel->setMinimumHeight(40);
    routeLayout->addWidget(m_videoRouteSummaryLabel);

    pageLayout->addWidget(routeGroup, 1);

    connect(m_btnStartStop, &QPushButton::clicked,
            this, &ControlPanelWindow::onStartStopClicked);
    connect(m_btnOsd, &QPushButton::clicked,
            this, &ControlPanelWindow::onOsdClicked);
    connect(applyVideo, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyVideoConfigClicked);

    return page;
}

QWidget *ControlPanelWindow::createPtzPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(8, 6, 8, 6);
    pageLayout->setSpacing(10);

    QGroupBox *previewGroup = new QGroupBox("PTZ相机预览与配置", page);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(10, 20, 10, 10);
    previewLayout->setSpacing(6);

    QHBoxLayout *profileRow = new QHBoxLayout();
    QLabel *profileLabel = new QLabel(QStringLiteral("相机配置"), previewGroup);
    m_ptzProfileCombo = new QComboBox(previewGroup);
    m_ptzProfileCombo->addItem(QStringLiteral("自动识别"),
                               static_cast<int>(PtzProfileSelection::Auto));
    m_ptzProfileCombo->addItem(QStringLiteral("当前HD USB PTZ"),
                               static_cast<int>(PtzProfileSelection::HdUsbPtz));
    m_ptzProfileCombo->addItem(QStringLiteral("Sony EVI-D100 + MS210x"),
                               static_cast<int>(PtzProfileSelection::SonyEviD100));
    m_ptzProfileCombo->setMinimumHeight(42);
    m_ptzProfileCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_btnApplyPtzProfile = new QPushButton(QStringLiteral("应用配置"), previewGroup);
    m_btnApplyPtzProfile->setObjectName("ptzButton");
    m_btnApplyPtzProfile->setMinimumHeight(42);
    m_btnApplyPtzProfile->setMinimumWidth(120);

    profileRow->addWidget(profileLabel);
    profileRow->addWidget(m_ptzProfileCombo, 1);
    profileRow->addWidget(m_btnApplyPtzProfile);
    previewLayout->addLayout(profileRow);

    m_ptzProfileSummaryLabel = new QLabel(
                QStringLiteral("尚未解析PTZ相机配置"), previewGroup);
    m_ptzProfileSummaryLabel->setObjectName("ptzState");
    m_ptzProfileSummaryLabel->setWordWrap(true);
    previewLayout->addWidget(m_ptzProfileSummaryLabel);

    m_ptzPreviewLabel = new QLabel("等待自动识别PTZ视频设备...", previewGroup);
    m_ptzPreviewLabel->setObjectName("ptzPreview");
    m_ptzPreviewLabel->setAlignment(Qt::AlignCenter);
    m_ptzPreviewLabel->setMinimumSize(600, 285);
    m_ptzPreviewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewLayout->addWidget(m_ptzPreviewLabel, 1);

    m_ptzVideoStatusLabel = new QLabel("PTZ视频未启动", previewGroup);
    m_ptzVideoStatusLabel->setObjectName("ptzState");
    m_ptzVideoStatusLabel->setWordWrap(true);
    previewLayout->addWidget(m_ptzVideoStatusLabel);

    m_ptzStatsLabel = new QLabel("CAP:0.0  DEC:0.0  JPEG:0 us  FAIL:0", previewGroup);
    m_ptzStatsLabel->setObjectName("ptzStats");
    previewLayout->addWidget(m_ptzStatsLabel);

    pageLayout->addWidget(previewGroup, 1);

    QGroupBox *controlGroup = new QGroupBox("RS232 VISCA 控制", page);
    controlGroup->setFixedWidth(340);
    QVBoxLayout *controlLayout = new QVBoxLayout(controlGroup);
    controlLayout->setContentsMargins(10, 20, 10, 10);
    controlLayout->setSpacing(5);

    QHBoxLayout *serviceRow = new QHBoxLayout();
    m_btnPtzPreview = new QPushButton("启动预览", controlGroup);
    m_btnPtzControl = new QPushButton("连接控制", controlGroup);
    m_btnPtzPreview->setObjectName("ptzButton");
    m_btnPtzControl->setObjectName("ptzButton");
    serviceRow->addWidget(m_btnPtzPreview);
    serviceRow->addWidget(m_btnPtzControl);
    controlLayout->addLayout(serviceRow);

    // 水平和俯仰速度范围不同，因此分别设置。
    // 水平VISCA速度等级：1～24；俯仰VISCA速度等级：1～20。
    QGridLayout *speedGrid = new QGridLayout();
    speedGrid->setHorizontalSpacing(7);
    speedGrid->setVerticalSpacing(4);

    QLabel *panSpeedLabel = new QLabel("水平速度", controlGroup);
    QLabel *tiltSpeedLabel = new QLabel("俯仰速度", controlGroup);

    m_ptzPanSpeedSlider = new QSlider(Qt::Horizontal, controlGroup);
    m_ptzPanSpeedSlider->setRange(1, 24);
    m_ptzPanSpeedSlider->setValue(5);
    m_ptzPanSpeedSlider->setSingleStep(1);
    m_ptzPanSpeedSlider->setPageStep(4);

    m_ptzPanSpeedSpin = new QSpinBox(controlGroup);
    m_ptzPanSpeedSpin->setRange(1, 24);
    m_ptzPanSpeedSpin->setValue(5);
    m_ptzPanSpeedSpin->setFixedWidth(68);
    m_ptzPanSpeedSpin->setAlignment(Qt::AlignCenter);

    m_ptzTiltSpeedSlider = new QSlider(Qt::Horizontal, controlGroup);
    m_ptzTiltSpeedSlider->setRange(1, 20);
    m_ptzTiltSpeedSlider->setValue(5);
    m_ptzTiltSpeedSlider->setSingleStep(1);
    m_ptzTiltSpeedSlider->setPageStep(4);

    m_ptzTiltSpeedSpin = new QSpinBox(controlGroup);
    m_ptzTiltSpeedSpin->setRange(1, 20);
    m_ptzTiltSpeedSpin->setValue(5);
    m_ptzTiltSpeedSpin->setFixedWidth(68);
    m_ptzTiltSpeedSpin->setAlignment(Qt::AlignCenter);

    speedGrid->addWidget(panSpeedLabel, 0, 0);
    speedGrid->addWidget(m_ptzPanSpeedSlider, 0, 1);
    speedGrid->addWidget(m_ptzPanSpeedSpin, 0, 2);
    speedGrid->addWidget(tiltSpeedLabel, 1, 0);
    speedGrid->addWidget(m_ptzTiltSpeedSlider, 1, 1);
    speedGrid->addWidget(m_ptzTiltSpeedSpin, 1, 2);
    speedGrid->setColumnStretch(1, 1);
    controlLayout->addLayout(speedGrid);

    // 标准九宫格：八方向持续运动，中间为紧急停止。
    QGridLayout *directionGrid = new QGridLayout();
    directionGrid->setSpacing(5);

    QPushButton *upLeft = new QPushButton("左上", controlGroup);
    QPushButton *up = new QPushButton("上", controlGroup);
    QPushButton *upRight = new QPushButton("右上", controlGroup);
    QPushButton *left = new QPushButton("左", controlGroup);
    QPushButton *stop = new QPushButton("停止", controlGroup);
    QPushButton *right = new QPushButton("右", controlGroup);
    QPushButton *downLeft = new QPushButton("左下", controlGroup);
    QPushButton *down = new QPushButton("下", controlGroup);
    QPushButton *downRight = new QPushButton("右下", controlGroup);

    const QList<QPushButton *> directionButtons = {
        upLeft, up, upRight,
        left, stop, right,
        downLeft, down, downRight
    };
    for (QPushButton *button : directionButtons) {
        button->setObjectName("ptzButton");
    }

    directionGrid->addWidget(upLeft, 0, 0);
    directionGrid->addWidget(up, 0, 1);
    directionGrid->addWidget(upRight, 0, 2);
    directionGrid->addWidget(left, 1, 0);
    directionGrid->addWidget(stop, 1, 1);
    directionGrid->addWidget(right, 1, 2);
    directionGrid->addWidget(downLeft, 2, 0);
    directionGrid->addWidget(down, 2, 1);
    directionGrid->addWidget(downRight, 2, 2);
    controlLayout->addLayout(directionGrid);

    QPushButton *home = new QPushButton("云台回中", controlGroup);
    home->setObjectName("ptzButton");
    controlLayout->addWidget(home);

    QGridLayout *zoomGrid = new QGridLayout();
    zoomGrid->setSpacing(5);
    QPushButton *zoomTele = new QPushButton("变焦 +", controlGroup);
    QPushButton *zoomWide = new QPushButton("变焦 -", controlGroup);
    zoomTele->setObjectName("ptzButton");
    zoomWide->setObjectName("ptzButton");
    zoomGrid->addWidget(zoomTele, 0, 0);
    zoomGrid->addWidget(zoomWide, 0, 1);
    controlLayout->addLayout(zoomGrid);

    QPushButton *saveSnapshot = new QPushButton("保存PTZ截图", controlGroup);
    QPushButton *viewSnapshots = new QPushButton("查看抓拍", controlGroup);
    saveSnapshot->setObjectName("ptzButton");
    viewSnapshots->setObjectName("ptzButton");
    controlLayout->addWidget(saveSnapshot);
    controlLayout->addWidget(viewSnapshots);

    controlLayout->addStretch(1);

    m_ptzControlStatusLabel = new QLabel("RS232控制未启动", controlGroup);
    m_ptzControlStatusLabel->setObjectName("ptzState");
    m_ptzControlStatusLabel->setWordWrap(true);
    controlLayout->addWidget(m_ptzControlStatusLabel);

    pageLayout->addWidget(controlGroup, 0);

    connect(m_btnApplyPtzProfile, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyPtzProfileClicked);
    connect(m_btnPtzPreview, &QPushButton::clicked,
            this, &ControlPanelWindow::onPtzPreviewClicked);
    connect(m_btnPtzControl, &QPushButton::clicked,
            this, &ControlPanelWindow::onPtzControlClicked);
    connect(saveSnapshot, &QPushButton::clicked,
            this, &ControlPanelWindow::onSavePtzSnapshotClicked);
    connect(viewSnapshots, &QPushButton::clicked,
            this, &ControlPanelWindow::onViewPtzSnapshotsClicked);

    // 滑块和数值框双向同步；数值变化立即更新控制线程中的速度参数。
    connect(m_ptzPanSpeedSlider, &QSlider::valueChanged,
            m_ptzPanSpeedSpin, &QSpinBox::setValue);
    connect(m_ptzPanSpeedSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_ptzPanSpeedSlider, &QSlider::setValue);
    connect(m_ptzPanSpeedSlider, &QSlider::valueChanged,
            m_ptzControl, &PtzControlWorker::setPanSpeed);

    connect(m_ptzTiltSpeedSlider, &QSlider::valueChanged,
            m_ptzTiltSpeedSpin, &QSpinBox::setValue);
    connect(m_ptzTiltSpeedSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_ptzTiltSpeedSlider, &QSlider::setValue);
    connect(m_ptzTiltSpeedSlider, &QSlider::valueChanged,
            m_ptzControl, &PtzControlWorker::setTiltSpeed);

    // 八方向均采用“按下运动、松开停止”，避免触摸按键产生持续运动。
    connect(up, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveUp);
    connect(up, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(down, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveDown);
    connect(down, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(left, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveLeft);
    connect(left, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(right, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveRight);
    connect(right, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);

    connect(upLeft, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveUpLeft);
    connect(upLeft, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(upRight, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveUpRight);
    connect(upRight, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(downLeft, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveDownLeft);
    connect(downLeft, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(downRight, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveDownRight);
    connect(downRight, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);

    connect(stop, &QPushButton::clicked,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(home, &QPushButton::clicked,
            m_ptzControl, &PtzControlWorker::home);

    connect(zoomTele, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::zoomTele);
    connect(zoomTele, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopZoom);
    connect(zoomWide, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::zoomWide);
    connect(zoomWide, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopZoom);

    return page;
}

QWidget *ControlPanelWindow::createFpgaPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    QGroupBox *group = new QGroupBox("FPGA 功能配置", page);
    QGridLayout *grid = new QGridLayout(group);
    grid->setContentsMargins(18, 22, 18, 16);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(12);

    m_sourceCombo = new QComboBox(group);
    m_sourceCombo->addItems(QStringList() << "HDMI1" << "HDMI2" << "HDMI3" << "CMOS" << "SFP" << "测试图");

    QStringList pp;
    pp << "无" << "Gamma增强" << "边缘增强" << "车牌定位" << "Resize" << "BEV预留";
    m_ch1PreprocCombo = new QComboBox(group);
    m_ch2PreprocCombo = new QComboBox(group);
    m_ch3PreprocCombo = new QComboBox(group);
    m_ch4PreprocCombo = new QComboBox(group);
    m_ch1PreprocCombo->addItems(pp);
    m_ch2PreprocCombo->addItems(pp);
    m_ch3PreprocCombo->addItems(pp);
    m_ch4PreprocCombo->addItems(pp);

    QPushButton *apply = new QPushButton("应用 FPGA 配置（预留 BAR/ioctl）", group);

    grid->addWidget(new QLabel("主输入源", group), 0, 0);
    grid->addWidget(m_sourceCombo, 0, 1);
    grid->addWidget(new QLabel("CH1预处理", group), 1, 0);
    grid->addWidget(m_ch1PreprocCombo, 1, 1);
    grid->addWidget(new QLabel("CH2预处理", group), 2, 0);
    grid->addWidget(m_ch2PreprocCombo, 2, 1);
    grid->addWidget(new QLabel("CH3预处理", group), 3, 0);
    grid->addWidget(m_ch3PreprocCombo, 3, 1);
    grid->addWidget(new QLabel("CH4预处理", group), 4, 0);
    grid->addWidget(m_ch4PreprocCombo, 4, 1);
    grid->addWidget(apply, 5, 0, 1, 2);

    layout->addWidget(group);
    layout->addStretch(1);

    connect(apply, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyFpgaConfigClicked);

    return page;
}

QWidget *ControlPanelWindow::createCpuPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    QGroupBox *group = new QGroupBox("CPU与散热配置", page);
    QGridLayout *grid = new QGridLayout(group);
    grid->setContentsMargins(18, 22, 18, 16);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(14);

    m_btnFanPower = new QPushButton("风扇关闭", group);
    m_btnFanPower->setCheckable(true);

    m_fanPwmSlider = new QSlider(Qt::Horizontal, group);
    m_fanPwmSlider->setRange(0, 100);
    m_fanPwmSlider->setSingleStep(5);
    m_fanPwmSlider->setPageStep(10);
    m_fanPwmSlider->setValue(30);

    m_fanPwmSpin = new QSpinBox(group);
    m_fanPwmSpin->setRange(0, 100);
    m_fanPwmSpin->setSingleStep(5);
    m_fanPwmSpin->setValue(30);
    m_fanPwmSpin->setSuffix(" %");

    QPushButton *apply = new QPushButton("应用风扇设置", group);

    m_fanStatusLabel = new QLabel("正在检测PWM风扇...", group);
    m_fanStatusLabel->setObjectName("ptzState");
    m_fanStatusLabel->setWordWrap(true);

    QLabel *note = new QLabel(
                "PWM节点：/sys/class/pwm/pwmchip0/pwm0\n"
                "周期：200000 ns（约5 kHz）；Qt程序需具有PWM节点写权限。",
                group);
    note->setWordWrap(true);

    grid->addWidget(new QLabel("风扇开关", group), 0, 0);
    grid->addWidget(m_btnFanPower, 0, 1, 1, 2);
    grid->addWidget(new QLabel("PWM占空比", group), 1, 0);
    grid->addWidget(m_fanPwmSlider, 1, 1);
    grid->addWidget(m_fanPwmSpin, 1, 2);
    grid->addWidget(apply, 2, 0, 1, 3);
    grid->addWidget(m_fanStatusLabel, 3, 0, 1, 3);
    grid->addWidget(note, 4, 0, 1, 3);

    layout->addWidget(group);
    layout->addStretch(1);

    connect(m_btnFanPower, &QPushButton::clicked,
            this, &ControlPanelWindow::onFanPowerClicked);
    connect(m_fanPwmSlider, &QSlider::valueChanged,
            this, &ControlPanelWindow::onFanPwmChanged);
    connect(m_fanPwmSlider, &QSlider::valueChanged,
            m_fanPwmSpin, &QSpinBox::setValue);
    connect(m_fanPwmSpin,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            m_fanPwmSlider, &QSlider::setValue);
    connect(apply, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyFanConfigClicked);

    return page;
}

QWidget *ControlPanelWindow::createAiPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(8);

    QWidget *leftPanel = new QWidget(page);
    leftPanel->setFixedWidth(400);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    QGroupBox *configGroup = new QGroupBox("AI / NPU 单任务配置", leftPanel);
    QGridLayout *grid = new QGridLayout(configGroup);
    grid->setContentsMargins(12, 18, 12, 10);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(7);
    // 压缩标签列，为三个触控数值框保留稳定且完整的显示宽度。
    grid->setColumnMinimumWidth(0, 104);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    m_npuEnableCheck = new QCheckBox("启用 NPU 推理", configGroup);
    m_npuEnableCheck->setMinimumHeight(42);

    m_npuTaskCombo = new QComboBox(configGroup);
    m_npuTaskCombo->addItem("行人检测", static_cast<int>(NpuTaskType::Pedestrian));
    m_npuTaskCombo->addItem("车牌识别（检测→识别）",
                            static_cast<int>(NpuTaskType::PlateCascade));
    m_npuTaskCombo->setMinimumHeight(46);

    QLabel *sourceValue = new QLabel(QStringLiteral("视频显示页选定输入：总览全图或CH1~CH9"), configGroup);
    sourceValue->setObjectName("ptzState");
    sourceValue->setMinimumHeight(38);

    auto makeTouchButton = [configGroup](const QString &text) {
        QPushButton *button = new QPushButton(text, configGroup);
        button->setObjectName("touchAdjustButton");
        button->setFixedSize(50, 46);
        button->setAutoRepeat(true);
        button->setAutoRepeatDelay(350);
        button->setAutoRepeatInterval(120);
        return button;
    };

    auto configureTouchSpin = [](QAbstractSpinBox *spin) {
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setReadOnly(true);
        spin->setAlignment(Qt::AlignCenter);
        spin->setObjectName("touchValueSpin");
        spin->setMinimumSize(120, 46);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setFocusPolicy(Qt::NoFocus);
    };

    m_npuFpsSpin = new QSpinBox(configGroup);
    m_npuFpsSpin->setRange(NPU_TARGET_FPS_MIN, NPU_TARGET_FPS_MAX);
    m_npuFpsSpin->setSingleStep(1);
    m_npuFpsSpin->setValue(NPU_TARGET_FPS_DEFAULT);
    m_npuFpsSpin->setSuffix(" fps");
    m_npuFpsSpin->setToolTip(
                QStringLiteral("可设置范围：%1～%2 fps；默认：%3 fps")
                .arg(NPU_TARGET_FPS_MIN)
                .arg(NPU_TARGET_FPS_MAX)
                .arg(NPU_TARGET_FPS_DEFAULT));
    configureTouchSpin(m_npuFpsSpin);

    m_confSpin = new QDoubleSpinBox(configGroup);
    m_confSpin->setRange(0.10, 0.95);
    m_confSpin->setSingleStep(0.05);
    m_confSpin->setDecimals(2);
    m_confSpin->setValue(0.50);
    configureTouchSpin(m_confSpin);

    m_nmsSpin = new QDoubleSpinBox(configGroup);
    m_nmsSpin->setRange(0.10, 0.90);
    m_nmsSpin->setSingleStep(0.05);
    m_nmsSpin->setDecimals(2);
    m_nmsSpin->setValue(0.45);
    configureTouchSpin(m_nmsSpin);

    auto addTouchAdjustRow = [&](int row,
                                 const QString &labelText,
                                 QAbstractSpinBox *spin,
                                 const std::function<void()> &decrease,
                                 const std::function<void()> &increase) {
        QLabel *label = new QLabel(labelText, configGroup);
        QPushButton *minusButton = makeTouchButton(QStringLiteral("−"));
        QPushButton *plusButton = makeTouchButton(QStringLiteral("+"));

        QWidget *adjustWidget = new QWidget(configGroup);
        QHBoxLayout *adjustLayout = new QHBoxLayout(adjustWidget);
        adjustLayout->setContentsMargins(0, 0, 0, 0);
        adjustLayout->setSpacing(5);
        adjustLayout->addWidget(minusButton);
        adjustLayout->addWidget(spin, 1);
        adjustLayout->addWidget(plusButton);

        grid->addWidget(label, row, 0);
        grid->addWidget(adjustWidget, row, 1);

        connect(minusButton, &QPushButton::clicked, this, decrease);
        connect(plusButton, &QPushButton::clicked, this, increase);
    };

    QPushButton *apply = new QPushButton("应用 AI 配置", configGroup);
    apply->setMinimumHeight(50);

    grid->addWidget(m_npuEnableCheck, 0, 0, 1, 2);
    grid->addWidget(new QLabel("推理任务", configGroup), 1, 0);
    grid->addWidget(m_npuTaskCombo, 1, 1);
    grid->addWidget(new QLabel("输入源", configGroup), 2, 0);
    grid->addWidget(sourceValue, 2, 1);

    addTouchAdjustRow(3,
                      QStringLiteral("目标推理帧率"),
                      m_npuFpsSpin,
                      [this]() { m_npuFpsSpin->stepDown(); },
                      [this]() { m_npuFpsSpin->stepUp(); });
    addTouchAdjustRow(4,
                      QStringLiteral("检测置信度"),
                      m_confSpin,
                      [this]() { m_confSpin->stepDown(); },
                      [this]() { m_confSpin->stepUp(); });
    addTouchAdjustRow(5,
                      QStringLiteral("NMS阈值"),
                      m_nmsSpin,
                      [this]() { m_nmsSpin->stepDown(); },
                      [this]() { m_nmsSpin->stepUp(); });

    grid->addWidget(apply, 6, 0, 1, 2);

    m_aiConfigSummaryLabel = new QLabel(configGroup);
    m_aiConfigSummaryLabel->setWordWrap(true);
    m_aiConfigSummaryLabel->setObjectName("ptzState");
    m_aiConfigSummaryLabel->setMaximumHeight(52);
    grid->addWidget(m_aiConfigSummaryLabel, 7, 0, 1, 2);

    QLabel *touchHint = new QLabel("参数使用大号 − / + 调节，支持长按连续调整。", configGroup);
    touchHint->setObjectName("navHint");
    touchHint->setWordWrap(true);
    grid->addWidget(touchHint, 8, 0, 1, 2);

    leftLayout->addWidget(configGroup);
    leftLayout->addStretch(1);
    pageLayout->addWidget(leftPanel, 0);

    QWidget *rightPanel = new QWidget(page);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    QGroupBox *runtimeGroup = new QGroupBox("运行状态", rightPanel);
    runtimeGroup->setMaximumHeight(156);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(10, 18, 10, 8);
    m_aiRuntimeLabel = new QLabel("AI未启用", runtimeGroup);
    m_aiRuntimeLabel->setObjectName("aiRuntimeStatus");
    m_aiRuntimeLabel->setWordWrap(true);
    m_aiRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_aiRuntimeLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    runtimeLayout->addWidget(m_aiRuntimeLabel);
    rightLayout->addWidget(runtimeGroup, 0);

    QGroupBox *resultGroup = new QGroupBox("最近识别结果（最多50条）", rightPanel);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(8, 18, 8, 8);
    resultLayout->setSpacing(5);

    m_aiResultTable = new QTableWidget(0, 7, resultGroup);
    m_aiResultTable->setObjectName("aiResultTable");
    m_aiResultTable->setHorizontalHeaderLabels(
                QStringList() << "时间" << "帧号" << "任务" << "结果"
                              << "置信度" << "目标框 x,y,w,h" << "耗时/ms");
    m_aiResultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_aiResultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_aiResultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_aiResultTable->setAlternatingRowColors(true);
    m_aiResultTable->verticalHeader()->setVisible(false);
    m_aiResultTable->verticalHeader()->setDefaultSectionSize(34);
    m_aiResultTable->horizontalHeader()->setStretchLastSection(false);
    m_aiResultTable->horizontalHeader()->setMinimumSectionSize(54);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_aiResultTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    resultLayout->addWidget(m_aiResultTable, 1);

    QPushButton *clear = new QPushButton("清空结果", resultGroup);
    clear->setObjectName("compactActionButton");
    clear->setMinimumWidth(130);
    resultLayout->addWidget(clear, 0, Qt::AlignRight);
    rightLayout->addWidget(resultGroup, 1);

    pageLayout->addWidget(rightPanel, 1);

    connect(apply, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyAiConfigClicked);
    connect(clear, &QPushButton::clicked,
            this, &ControlPanelWindow::clearAiResultTable);

    return page;
}

QWidget *ControlPanelWindow::createStatusPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    QGroupBox *group = new QGroupBox("详细状态", page);
    QVBoxLayout *v = new QVBoxLayout(group);
    v->setContentsMargins(8, 18, 8, 8);

    QScrollArea *scrollArea = new QScrollArea(group);
    scrollArea->setObjectName("statusScrollArea");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    QWidget *scrollContent = new QWidget(scrollArea);
    QVBoxLayout *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    m_statusDetail = new QLabel(scrollContent);
    m_statusDetail->setObjectName("statusDetail");
    m_statusDetail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_statusDetail->setWordWrap(true);
    m_statusDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusDetail->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    contentLayout->addWidget(m_statusDetail);
    contentLayout->addStretch(1);

    scrollArea->setWidget(scrollContent);
    v->addWidget(scrollArea, 1);
    layout->addWidget(group, 1);
    return page;
}

void ControlPanelWindow::setCurrentPage(int index)
{
    if (!m_stack) {
        return;
    }
    if (index < 0 || index >= m_stack->count()) {
        return;
    }

    m_currentPage = index;
    m_stack->setCurrentIndex(index);
    updateStatusText();
}

void ControlPanelWindow::onStartStopClicked()
{
    if (!m_display) {
        return;
    }

    if (!m_display->isRunning()) {
        onApplyVideoConfigClicked();
        onApplyAiConfigClicked();
        m_display->startDisplay();
    } else {
        m_display->stopDisplay();
    }

    m_displayRunning = m_display->isRunning();
    updateStatusText();
}

void ControlPanelWindow::onOsdClicked()
{
    m_osdEnabled = !m_osdEnabled;

    if (m_display) {
        m_display->setOsdEnabled(m_osdEnabled);
    }

    m_lastAction = QString("AI结果OSD %1").arg(m_osdEnabled ? "ON" : "OFF");
    updateStatusText();
}

void ControlPanelWindow::onApplyVideoConfigClicked()
{
    if (!m_display) {
        return;
    }

    QSize size = m_resolutionCombo->currentData().toSize();
    int mode = m_outputModeCombo->currentData().toInt();
    const int checkedRoute = m_videoRouteGroup ? m_videoRouteGroup->checkedId() : 0;
    const VideoRoute route = sanitizeVideoRoute(checkedRoute);

    if ((m_display->outputWidth() != size.width() ||
         m_display->outputHeight() != size.height()) &&
        !m_display->setOutputResolution(size.width(), size.height())) {
        m_lastError = m_display->lastErrorString();
    }
    m_display->setDisplayMode(mode);
    m_display->setOsdEnabled(m_osdEnabled);

    const VideoRoute oldRoute = m_display->videoRoute();
    m_display->setVideoRoute(route);
    if (oldRoute != route) {
        clearAiResultTable();
        m_lastAiResultFrameId = 0;
    }

    if (m_videoRouteSummaryLabel) {
        m_videoRouteSummaryLabel->setText(isSingleChannelRoute(route)
                ? QString("当前应用：CH%1；HDMI全屏 + NPU单路")
                  .arg(videoRouteChannel(route))
                : QStringLiteral("当前应用：九宫格总览；NPU全图"));
    }

    m_lastAction = QString("应用显示配置: %1x%2 mode=%3 route=%4")
            .arg(size.width()).arg(size.height()).arg(mode)
            .arg(QString::fromLatin1(videoRouteKey(route)));
    updateStatusText();
}

void ControlPanelWindow::onApplyFpgaConfigClicked()
{
    m_lastAction = QString("FPGA配置预留: src=%1 CH1=%2 CH2=%3 CH3=%4 CH4=%5")
            .arg(m_sourceCombo->currentText())
            .arg(m_ch1PreprocCombo->currentText())
            .arg(m_ch2PreprocCombo->currentText())
            .arg(m_ch3PreprocCombo->currentText())
            .arg(m_ch4PreprocCombo->currentText());
    updateStatusText();
}

void ControlPanelWindow::onApplyAiConfigClicked()
{
    if (!m_display || !m_npuEnableCheck || !m_npuTaskCombo ||
        !m_npuFpsSpin || !m_confSpin || !m_nmsSpin) {
        return;
    }

    NpuTaskConfig config;
    config.enabled = m_npuEnableCheck->isChecked();
    config.task = static_cast<NpuTaskType>(m_npuTaskCombo->currentData().toInt());
    config.targetFps = m_npuFpsSpin->value();
    config.confidence = static_cast<float>(m_confSpin->value());
    config.nmsThreshold = static_cast<float>(m_nmsSpin->value());
    config.resultHistoryDepth = 50;
    config = sanitizeNpuTaskConfig(config);

    m_display->setNpuConfig(config);
    saveAiSettings(config);

    m_aiEnabled = config.enabled;
    m_aiTask = config.task;
    m_aiStatus = config.enabled ? "pending" : "off";
    m_aiHistoryDepth = config.resultHistoryDepth;
    m_lastAiResultFrameId = 0;
    clearAiResultTable();

    if (!config.enabled) {
        m_aiReady = false;
        m_aiFps = 0.0;
        m_aiPlateCount = 0;
        m_aiPedestrianCount = 0;
        m_aiLastMs = 0.0;
        m_aiPreMs = 0.0;
    }

    if (m_aiConfigSummaryLabel) {
        m_aiConfigSummaryLabel->setText(
                    QStringLiteral("当前：%1｜%2 fps｜conf=%3｜NMS=%4")
                    .arg(aiTaskDisplayName(config.task))
                    .arg(config.targetFps)
                    .arg(config.confidence, 0, 'f', 2)
                    .arg(config.nmsThreshold, 0, 'f', 2));
    }

    m_lastAction = QStringLiteral("AI配置：%1，task=%2，fps=%3，conf=%4，nms=%5")
            .arg(config.enabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(aiTaskDisplayName(config.task))
            .arg(config.targetFps)
            .arg(config.confidence, 0, 'f', 2)
            .arg(config.nmsThreshold, 0, 'f', 2);
    updateStatusText();
}

void ControlPanelWindow::onFanPowerClicked(bool checked)
{
    if (!m_fanController || !m_fanPwmSlider || !m_btnFanPower) {
        return;
    }

    const bool oldEnabled = m_fanController->isEnabled();
    if (!m_fanController->apply(checked, m_fanPwmSlider->value())) {
        m_lastError = QStringLiteral("风扇控制：%1")
                .arg(m_fanController->lastError());
        const QSignalBlocker blocker(m_btnFanPower);
        m_btnFanPower->setChecked(oldEnabled);
    } else {
        m_lastAction = QStringLiteral("风扇%1，PWM=%2%")
                .arg(checked ? QStringLiteral("开启") : QStringLiteral("关闭"))
                .arg(m_fanPwmSlider->value());
        m_lastError = QStringLiteral("none");
    }

    updateFanUi();
    updateStatusText();
}

void ControlPanelWindow::onFanPwmChanged(int value)
{
    if (m_fanStatusLabel) {
        const QString runText = (m_btnFanPower && m_btnFanPower->isChecked())
                ? QStringLiteral("开启") : QStringLiteral("关闭");
        m_fanStatusLabel->setText(
                    QStringLiteral("待应用：风扇%1，PWM占空比%2%")
                    .arg(runText)
                    .arg(value));
    }
}

void ControlPanelWindow::onApplyFanConfigClicked()
{
    if (!m_fanController || !m_fanPwmSlider || !m_btnFanPower) {
        return;
    }

    const bool requestedEnabled = m_btnFanPower->isChecked();
    const int requestedDuty = m_fanPwmSlider->value();

    if (!m_fanController->apply(requestedEnabled, requestedDuty)) {
        m_lastError = QStringLiteral("风扇控制：%1")
                .arg(m_fanController->lastError());
    } else {
        m_lastAction = QStringLiteral("应用风扇设置：%1，PWM=%2%")
                .arg(requestedEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
                .arg(requestedDuty);
        m_lastError = QStringLiteral("none");
    }

    updateFanUi();
    updateStatusText();
}

void ControlPanelWindow::onStatusTimer()
{
    m_seconds++;

    if (m_display) {
        m_displayRunning = m_display->isRunning();
    }
    if (m_ptzCapture) {
        m_ptzCaptureRunning = m_ptzCapture->isRunning();
    }
    if (m_ptzControl) {
        m_ptzControlReady = m_ptzControl->isReady();
    }

    updateStatusText();
}

void ControlPanelWindow::onAiUiTimer()
{
    if (!m_display) {
        return;
    }

    const NpuFrameResult result = m_display->latestNpuResult();
    updateAiPanel(result);

    const std::vector<NpuFrameResult> newResults =
            m_display->npuResultsAfter(m_lastAiResultFrameId, 64);
    for (const NpuFrameResult &item : newResults) {
        if (item.frameId > m_lastAiResultFrameId) {
            m_lastAiResultFrameId = item.frameId;
        }
        if (item.ready) {
            appendAiResultRows(item);
        }
    }
}

void ControlPanelWindow::updateAiPanel(const NpuFrameResult &result)
{
    m_aiEnabled = result.enabled;
    m_aiReady = result.ready;
    m_aiTask = result.task;
    m_aiFps = result.fps;
    m_aiPlateCount = static_cast<int>(result.plates.size());
    m_aiPedestrianCount = static_cast<int>(result.pedestrians.size());
    m_aiLastMs = result.lastMs;
    m_aiPreMs = result.preMs;
    m_aiProcessed = result.processed;
    m_aiDropped = result.dropped;

    const VideoRoute route = m_display ? m_display->videoRoute() : VideoRoute::Overview;

    if (!result.enabled) {
        m_aiStatus = QStringLiteral("off");
    } else if (!result.error.empty()) {
        m_aiStatus = QString::fromStdString(result.error);
    } else if (result.ready) {
        m_aiStatus = QStringLiteral("ready");
    } else if (!result.modelsReady) {
        m_aiStatus = QStringLiteral("loading model");
    } else {
        m_aiStatus = QStringLiteral("pending");
    }

    if (m_aiRuntimeLabel) {
        const int resultCount = m_aiPlateCount + m_aiPedestrianCount;
        m_aiRuntimeLabel->setText(
                    QStringLiteral("状态：%1｜输入：%2｜任务：%3\n"
                                   "实际FPS：%4 / 目标%5｜总耗时：%6 ms｜拷贝：%7 ms｜预处理：%8 ms\n"
                                   "RKNN in/run/out/post：%9/%10/%11/%12 ms｜runs=%13\n"
                                   "本帧结果：%14｜已处理：%15｜覆盖丢帧：%16\n"
                                   "最近错误：%17")
                    .arg(m_aiStatus)
                    .arg(isSingleChannelRoute(route)
                         ? QString("CH%1").arg(videoRouteChannel(route))
                         : QStringLiteral("总览全图"))
                    .arg(aiTaskDisplayName(result.task))
                    .arg(result.fps, 0, 'f', 1)
                    .arg(result.targetFps)
                    .arg(result.lastMs, 0, 'f', 1)
                    .arg(result.copyMs, 0, 'f', 2)
                    .arg(result.preMs, 0, 'f', 1)
                    .arg(result.rknnInputMs, 0, 'f', 1)
                    .arg(result.rknnRunMs, 0, 'f', 1)
                    .arg(result.rknnOutputMs, 0, 'f', 1)
                    .arg(result.rknnPostMs, 0, 'f', 1)
                    .arg(static_cast<qulonglong>(result.rknnRunCount))
                    .arg(resultCount)
                    .arg(result.processed)
                    .arg(result.dropped)
                    .arg(result.error.empty()
                         ? QStringLiteral("none")
                         : QString::fromStdString(result.error)));
    }
}

void ControlPanelWindow::appendAiResultRows(const NpuFrameResult &result)
{
    if (!m_aiResultTable) {
        return;
    }

    struct RowData
    {
        QString resultText;
        QString confidenceText;
        QString boxText;
    };

    QList<RowData> rows;
    if (result.task == NpuTaskType::PlateCascade) {
        for (const NpuBox &box : result.plates) {
            const QString plate = !box.text.empty()
                    ? QString::fromStdString(box.text)
                    : QString::fromStdString(box.label);
            RowData row;
            row.resultText = plate.isEmpty() ? QStringLiteral("车牌") : plate;
            row.confidenceText = box.score >= 0.0f
                    ? QString::number(box.score, 'f', 2)
                    : QStringLiteral("--");
            row.boxText = aiBoxText(box);
            rows.push_back(row);
        }
    } else {
        for (const NpuBox &box : result.pedestrians) {
            RowData row;
            row.resultText = QStringLiteral("person");
            row.confidenceText = QString::number(box.score, 'f', 2);
            row.boxText = aiBoxText(box);
            rows.push_back(row);
        }
    }

    // 无目标帧不写入历史表，避免5~15 fps下用“无目标”快速淹没有效记录。
    if (rows.isEmpty()) {
        return;
    }

    const QString timeText = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString frameText = QString::number(result.frameId);
    const QString taskText = result.sourceChannel > 0
            ? QString("CH%1 %2").arg(result.sourceChannel).arg(aiTaskDisplayName(result.task))
            : QStringLiteral("ALL %1").arg(aiTaskDisplayName(result.task));
    const QString latencyText = QString::number(result.lastMs, 'f', 1);

    for (const RowData &row : rows) {
        m_aiResultTable->insertRow(0);
        const QStringList values {
            timeText,
            frameText,
            taskText,
            row.resultText,
            row.confidenceText,
            row.boxText,
            latencyText
        };
        for (int column = 0; column < values.size(); ++column) {
            QTableWidgetItem *item = new QTableWidgetItem(values[column]);
            const int alignment = column == 3
                    ? static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter)
                    : static_cast<int>(Qt::AlignCenter);
            item->setTextAlignment(alignment);
            m_aiResultTable->setItem(0, column, item);
        }
    }

    while (m_aiResultTable->rowCount() > m_aiHistoryDepth) {
        m_aiResultTable->removeRow(m_aiResultTable->rowCount() - 1);
    }
}

void ControlPanelWindow::clearAiResultTable()
{
    if (m_aiResultTable) {
        m_aiResultTable->setRowCount(0);
    }
}

void ControlPanelWindow::loadAiSettings()
{
    if (!m_npuEnableCheck || !m_npuTaskCombo || !m_npuFpsSpin ||
        !m_confSpin || !m_nmsSpin) {
        return;
    }

    QSettings settings(aiSettingsPath(), QSettings::IniFormat);
    const bool enabled = settings.value(QStringLiteral("ai/enabled"), false).toBool();
    const int taskValue = settings.value(
                QStringLiteral("ai/task"),
                static_cast<int>(NpuTaskType::Pedestrian)).toInt();
    const int fps = settings.value(QStringLiteral("ai/target_fps"),
                                   NPU_TARGET_FPS_DEFAULT).toInt();
    const double confidence = settings.value(QStringLiteral("ai/confidence"), 0.50).toDouble();
    const double nms = settings.value(QStringLiteral("ai/nms"), 0.45).toDouble();

    m_npuEnableCheck->setChecked(enabled);
    int taskIndex = m_npuTaskCombo->findData(taskValue);
    if (taskIndex < 0) {
        taskIndex = 0;
    }
    m_npuTaskCombo->setCurrentIndex(taskIndex);
    m_npuFpsSpin->setValue(qBound(NPU_TARGET_FPS_MIN,
                                      fps,
                                      NPU_TARGET_FPS_MAX));
    m_confSpin->setValue(qBound(0.10, confidence, 0.95));
    m_nmsSpin->setValue(qBound(0.10, nms, 0.90));
}

void ControlPanelWindow::saveAiSettings(const NpuTaskConfig &config)
{
    QSettings settings(aiSettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ai/enabled"), config.enabled);
    settings.setValue(QStringLiteral("ai/task"), static_cast<int>(config.task));
    settings.setValue(QStringLiteral("ai/target_fps"), config.targetFps);
    settings.setValue(QStringLiteral("ai/confidence"), config.confidence);
    settings.setValue(QStringLiteral("ai/nms"), config.nmsThreshold);
    settings.sync();
}

void ControlPanelWindow::onDisplayStateChanged(QString state)
{
    m_displayState = state;

    if (m_display) {
        m_displayRunning = m_display->isRunning();
    }

    updateStatusText();
}

void ControlPanelWindow::onDisplayError(QString msg)
{
    m_lastError = msg;
    updateStatusText();
}

void ControlPanelWindow::onDisplayStatsUpdated(double readFps,
                                               double validFps,
                                               double rgaFps,
                                               double flipFps,
                                               double rgaAvgUs,
                                               int failCount,
                                               int cpuLoad,
                                               int npuLoad,
                                               double tempC,
                                               double osdAvgUs,
                                               double plateOsdAvgUs,
                                               double npuPrepareAvgUs)
{
    m_readFps = readFps;
    m_validFps = validFps;
    m_rgaFps = rgaFps;
    m_flipFps = flipFps;
    m_rgaAvgUs = rgaAvgUs;
    m_failCount = failCount;
    m_cpuLoad = cpuLoad;
    m_npuLoad = npuLoad;
    m_tempC = tempC;
    m_osdAvgUs = osdAvgUs;
    m_plateOsdAvgUs = plateOsdAvgUs;
    m_npuPrepareAvgUs = npuPrepareAvgUs;

    updateStatusText();
}

void ControlPanelWindow::onDisplayAiStatsUpdated(bool enabled,
                                                 bool ready,
                                                 double fps,
                                                 int plateCount,
                                                 int pedestrianCount,
                                                 QString status)
{
    m_aiEnabled = enabled;
    m_aiReady = ready;
    m_aiFps = fps;
    m_aiPlateCount = plateCount;
    m_aiPedestrianCount = pedestrianCount;
    m_aiStatus = status;

    updateStatusText();
}


void ControlPanelWindow::loadPtzSettings()
{
    if (!m_ptzProfileCombo) {
        return;
    }

    QSettings settings(ptzSettingsPath(), QSettings::IniFormat);
    const int selectionValue = settings.value(
                QStringLiteral("ptz/profile_selection"),
                static_cast<int>(PtzProfileSelection::Auto)).toInt();
    const int lastSuccessfulValue = settings.value(
                QStringLiteral("ptz/last_successful_profile"),
                static_cast<int>(PtzProfileSelection::Auto)).toInt();

    int index = m_ptzProfileCombo->findData(selectionValue);
    if (index < 0) {
        index = m_ptzProfileCombo->findData(
                    static_cast<int>(PtzProfileSelection::Auto));
    }
    m_ptzProfileCombo->setCurrentIndex(qMax(0, index));
    m_requestedPtzSelection = static_cast<PtzProfileSelection>(
                m_ptzProfileCombo->currentData().toInt());

    if (lastSuccessfulValue >= static_cast<int>(PtzProfileSelection::HdUsbPtz)
            && lastSuccessfulValue <= static_cast<int>(PtzProfileSelection::SonyEviD100)) {
        m_lastSuccessfulPtzSelection = static_cast<PtzProfileSelection>(lastSuccessfulValue);
    } else {
        m_lastSuccessfulPtzSelection = PtzProfileSelection::Auto;
    }

    updatePtzProfileSummary(QStringLiteral("等待应用“%1”")
                            .arg(m_ptzProfileCombo->currentText()));
}

void ControlPanelWindow::savePtzSettings(PtzProfileSelection selection)
{
    QSettings settings(ptzSettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ptz/profile_selection"),
                      static_cast<int>(selection));
    settings.sync();
}

void ControlPanelWindow::clearPtzPreview(const QString &message)
{
    if (m_ptzCapture) {
        m_ptzCapture->clearLatestFrame();
    }
    m_lastPtzFrame = QImage();
    if (m_ptzPreviewLabel) {
        m_ptzPreviewLabel->clear();
        m_ptzPreviewLabel->setText(message);
    }
}

void ControlPanelWindow::updatePtzServiceButtons()
{
    if (m_btnPtzPreview) {
        if (m_ptzCapture && m_ptzCapture->isRunning()) {
            m_btnPtzPreview->setText(QStringLiteral("停止预览"));
        } else if (m_ptzPreviewDesired) {
            m_btnPtzPreview->setText(QStringLiteral("取消等待"));
        } else {
            m_btnPtzPreview->setText(QStringLiteral("启动预览"));
        }
    }

    if (m_btnPtzControl) {
        if (m_ptzControl && m_ptzControl->isRunning()) {
            m_btnPtzControl->setText(QStringLiteral("断开控制"));
        } else if (m_ptzControlDesired) {
            m_btnPtzControl->setText(QStringLiteral("取消等待"));
        } else {
            m_btnPtzControl->setText(QStringLiteral("连接控制"));
        }
    }
}

void ControlPanelWindow::enterPtzWaitingState(const QString &message,
                                               bool clearActiveDevice)
{
    m_ptzWaitingForDevice = true;

    if (clearActiveDevice) {
        m_ptzSwitchInProgress = true;
        if (m_ptzControl && m_ptzControl->isRunning()) {
            m_ptzControl->stopControl();
        }
        if (m_ptzCapture && m_ptzCapture->isRunning()) {
            m_ptzCapture->stopCapture();
        }
        m_ptzSwitchInProgress = false;
        m_activePtzDevice = PtzResolvedDevice();
        clearPtzPreview(message);
    }

    if (m_ptzVideoStatusLabel) {
        m_ptzVideoStatusLabel->setText(message);
    }
    updatePtzProfileSummary(message);
    updatePtzServiceButtons();
    updateStatusText();
}

void ControlPanelWindow::updatePtzProfileSummary(const QString &overrideText)
{
    if (!m_ptzProfileSummaryLabel) {
        return;
    }

    if (!overrideText.isEmpty()) {
        m_ptzProfileSummaryLabel->setText(overrideText);
        return;
    }

    if (!m_activePtzDevice.valid) {
        const QString requested = ptzProfileSelectionName(m_requestedPtzSelection);
        m_ptzProfileSummaryLabel->setText(
                    m_ptzWaitingForDevice
                    ? QStringLiteral("等待PTZ设备：%1").arg(requested)
                    : QStringLiteral("尚未解析PTZ相机配置"));
        return;
    }

    const PtzCameraProfile &profile = m_activePtzDevice.profile;
    const QString usbText = QStringLiteral("%1:%2")
            .arg(profile.usbVendorId, 4, 16, QLatin1Char('0'))
            .arg(profile.usbProductId, 4, 16, QLatin1Char('0'));
    QString text = QStringLiteral("当前：%1 | %2 | USB %3 | %4 %5x%6@%7 | 显示%8:%9")
            .arg(profile.displayName,
                 m_activePtzDevice.videoDevice,
                 usbText,
                 v4l2PixelFormatName(profile.pixelFormat))
            .arg(profile.captureWidth)
            .arg(profile.captureHeight)
            .arg(profile.captureFps)
            .arg(profile.displayAspectWidth)
            .arg(profile.displayAspectHeight);

    if (m_requestedPtzSelection != PtzProfileSelection::Auto
            && m_requestedPtzSelection != profile.selection) {
        text += QStringLiteral(" | 等待切换到：%1")
                .arg(ptzProfileSelectionName(m_requestedPtzSelection));
    }
    m_ptzProfileSummaryLabel->setText(text);
}

bool ControlPanelWindow::switchToResolvedPtzDevice(const PtzResolvedDevice &resolved,
                                                    bool forceRestart,
                                                    QString *errorMessage)
{
    if (!resolved.valid || !m_ptzCapture || !m_ptzControl) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无效的PTZ切换目标");
        }
        return false;
    }

    const bool sameEndpoint = samePtzEndpoint(m_activePtzDevice, resolved);
    if (sameEndpoint && !forceRestart) {
        m_activePtzDevice = resolved;
        m_ptzWaitingForDevice = false;
        m_ptzDisplayAspectWidth = qMax(1, resolved.profile.displayAspectWidth);
        m_ptzDisplayAspectHeight = qMax(1, resolved.profile.displayAspectHeight);

        if (m_ptzPreviewDesired && !m_ptzCapture->isRunning()) {
            m_ptzCapture->startCapture();
        }
        if (m_ptzControlDesired && !m_ptzControl->isRunning()) {
            m_ptzControl->startControl();
        }
        updatePtzProfileSummary();
        updatePtzServiceButtons();
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    m_ptzSwitchInProgress = true;
    const PtzResolvedDevice previousDevice = m_activePtzDevice;

    PtzResolvedDevice previousAvailable = previousDevice;
    QString previousResolveError;
    const bool previousStillPresent = previousDevice.valid
            && V4l2DeviceDetector::isPresent(previousDevice,
                                              &previousResolveError);

    // 切换前先停止VISCA，stopControl会发送Pan/Tilt Stop与Zoom Stop。
    if (m_ptzControl->isRunning()) {
        m_ptzControl->stopControl();
    }
    if (m_ptzCapture->isRunning()) {
        m_ptzCapture->stopCapture();
    }

    clearPtzPreview(QStringLiteral("正在切换到%1……")
                    .arg(resolved.profile.displayName));

    QString configError;
    const bool captureConfigured = m_ptzCapture->setCaptureConfig(
                resolved, &configError);
    const bool controlConfigured = captureConfigured
            && m_ptzControl->setCameraProfile(resolved.profile, &configError);

    if (!captureConfigured || !controlConfigured) {
        if (previousStillPresent) {
            QString rollbackError;
            (void)m_ptzCapture->setCaptureConfig(previousAvailable, &rollbackError);
            (void)m_ptzControl->setCameraProfile(previousAvailable.profile,
                                                  &rollbackError);
            m_activePtzDevice = previousAvailable;
            if (m_ptzPreviewDesired) {
                m_ptzCapture->startCapture();
            }
            if (m_ptzControlDesired) {
                m_ptzControl->startControl();
            }
        } else {
            m_activePtzDevice = PtzResolvedDevice();
        }

        m_ptzSwitchInProgress = false;
        const QString text = QStringLiteral("PTZ配置应用失败：%1").arg(configError);
        if (errorMessage) {
            *errorMessage = text;
        }
        m_lastError = text;
        updatePtzProfileSummary(text);
        updatePtzServiceButtons();
        updateStatusText();
        return false;
    }

    m_activePtzDevice = resolved;
    m_ptzWaitingForDevice = false;
    m_ptzDisplayAspectWidth = qMax(1, resolved.profile.displayAspectWidth);
    m_ptzDisplayAspectHeight = qMax(1, resolved.profile.displayAspectHeight);
    m_lastSuccessfulPtzSelection = resolved.profile.selection;

    QSettings settings(ptzSettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ptz/last_successful_profile"),
                      static_cast<int>(m_lastSuccessfulPtzSelection));
    settings.sync();

    qInfo() << "PTZ hot-switch applied:"
            << ptzEndpointText(resolved)
            << "previewDesired=" << m_ptzPreviewDesired
            << "controlDesired=" << m_ptzControlDesired;

    clearPtzPreview(QStringLiteral("等待%1视频帧……")
                    .arg(resolved.profile.displayName));

    if (m_ptzPreviewDesired) {
        m_ptzCapture->startCapture();
    }
    if (m_ptzControlDesired) {
        m_ptzControl->startControl();
    }

    m_ptzSwitchInProgress = false;
    m_lastAction = QStringLiteral("切换PTZ视频源：%1")
            .arg(resolved.profile.displayName);
    m_lastError = QStringLiteral("none");
    updatePtzProfileSummary();
    updatePtzServiceButtons();
    updateStatusText();
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool ControlPanelWindow::reconcilePtzDevice(bool userInitiated,
                                            bool forceRestart)
{
    if (m_ptzSwitchInProgress || !m_ptzCapture || !m_ptzControl) {
        return false;
    }

    PtzResolvedDevice refreshedActive = m_activePtzDevice;
    QString activeError;
    const bool activePresent = m_activePtzDevice.valid
            && V4l2DeviceDetector::isPresent(m_activePtzDevice, &activeError);

    PtzResolvedDevice target;
    QString targetError;
    bool targetFound = false;

    if (m_requestedPtzSelection == PtzProfileSelection::Auto) {
        // 自动模式下，只要当前源仍在线就保持当前源，避免第二台设备插入时
        // 无故跳转。当前源消失后，再按上次成功源和唯一在线源进行选择。
        if (activePresent) {
            target = refreshedActive;
            targetFound = true;
        } else if (m_lastSuccessfulPtzSelection != PtzProfileSelection::Auto) {
            targetFound = V4l2DeviceDetector::resolve(
                        m_lastSuccessfulPtzSelection,
                        &target,
                        &targetError,
                        nullptr);
        }

        if (!targetFound) {
            targetFound = V4l2DeviceDetector::resolve(
                        PtzProfileSelection::Auto,
                        &target,
                        &targetError,
                        nullptr);
        }
    } else if (activePresent
               && m_activePtzDevice.profile.selection == m_requestedPtzSelection) {
        target = refreshedActive;
        targetFound = true;
    } else {
        targetFound = V4l2DeviceDetector::resolve(
                    m_requestedPtzSelection,
                    &target,
                    &targetError,
                    nullptr);
    }

    if (!targetFound) {
        QString waitText;
        if (activePresent) {
            waitText = QStringLiteral("等待“%1”接入；当前保持%2")
                    .arg(ptzProfileSelectionName(m_requestedPtzSelection),
                         refreshedActive.profile.displayName);
            m_activePtzDevice = refreshedActive;
            m_ptzWaitingForDevice = true;
            updatePtzProfileSummary(waitText);
            if (m_ptzVideoStatusLabel) {
                m_ptzVideoStatusLabel->setText(waitText);
            }
        } else {
            waitText = QStringLiteral("等待PTZ设备：%1")
                    .arg(ptzProfileSelectionName(m_requestedPtzSelection));
            enterPtzWaitingState(waitText, true);
        }

        if (userInitiated) {
            m_lastAction = waitText;
            m_lastError = targetError.isEmpty()
                    ? QStringLiteral("none")
                    : QStringLiteral("PTZ配置：%1").arg(targetError);
        }
        if (m_lastPtzMonitorMessage != waitText) {
            qInfo() << "PTZ hotplug waiting:" << waitText
                    << "detail=" << targetError;
            m_lastPtzMonitorMessage = waitText;
        }
        updatePtzServiceButtons();
        updateStatusText();
        return false;
    }

    m_lastPtzMonitorMessage.clear();
    QString switchError;
    const bool ok = switchToResolvedPtzDevice(target,
                                               forceRestart,
                                               &switchError);
    if (!ok && userInitiated && !switchError.isEmpty()) {
        m_lastError = switchError;
    }
    return ok;
}

bool ControlPanelWindow::applyPtzProfileSelection(bool restartActiveServices)
{
    if (!m_ptzProfileCombo || !m_ptzCapture || !m_ptzControl) {
        return false;
    }

    m_requestedPtzSelection = static_cast<PtzProfileSelection>(
                m_ptzProfileCombo->currentData().toInt());
    savePtzSettings(m_requestedPtzSelection);
    m_lastAction = QStringLiteral("请求PTZ配置：%1")
            .arg(ptzProfileSelectionName(m_requestedPtzSelection));

    return reconcilePtzDevice(true, restartActiveServices);
}

void ControlPanelWindow::onApplyPtzProfileClicked()
{
    const bool anyServiceRunning = (m_ptzCapture && m_ptzCapture->isRunning())
            || (m_ptzControl && m_ptzControl->isRunning());
    (void)applyPtzProfileSelection(anyServiceRunning);
}

void ControlPanelWindow::onPtzPreviewClicked()
{
    if (!m_ptzCapture) {
        return;
    }

    if (m_ptzPreviewDesired) {
        m_ptzPreviewDesired = false;
        if (m_ptzCapture->isRunning()) {
            m_ptzCapture->stopCapture();
        }
        clearPtzPreview(QStringLiteral("PTZ视频预览已停止"));
        m_lastAction = QStringLiteral("停止PTZ视频预览/取消自动恢复");
    } else {
        m_ptzPreviewDesired = true;
        m_lastAction = QStringLiteral("启动PTZ视频预览/等待设备自动恢复");
        (void)reconcilePtzDevice(true, false);
    }
    updatePtzServiceButtons();
    updateStatusText();
}

void ControlPanelWindow::onPtzControlClicked()
{
    if (!m_ptzControl) {
        return;
    }

    if (m_ptzControlDesired) {
        m_ptzControlDesired = false;
        if (m_ptzControl->isRunning()) {
            m_ptzControl->stopControl();
        }
        m_lastAction = QStringLiteral("断开RS232 PTZ控制/取消自动恢复");
    } else {
        m_ptzControlDesired = true;
        m_lastAction = QStringLiteral("连接RS232 PTZ控制/等待设备自动恢复");
        (void)reconcilePtzDevice(true, false);
    }
    updatePtzServiceButtons();
    updateStatusText();
}

void ControlPanelWindow::onPtzDeviceMonitorTimer()
{
    if (m_ptzSwitchInProgress) {
        return;
    }

    // 只有用户仍希望预览/控制，或当前已有活动配置时才持续监测。
    if (!m_ptzPreviewDesired && !m_ptzControlDesired
            && !m_activePtzDevice.valid) {
        return;
    }
    (void)reconcilePtzDevice(false, false);
}

void ControlPanelWindow::onPtzCaptureDeviceLost(QString videoDevice,
                                                 QString reason)
{
    if (m_ptzSwitchInProgress || !m_ptzPreviewDesired) {
        return;
    }

    m_lastError = QStringLiteral("PTZ视频设备断开：%1").arg(reason);
    clearPtzPreview(QStringLiteral("检测到%1断开，等待重新枚举PTZ设备……")
                    .arg(videoDevice));
    updatePtzServiceButtons();
    updateStatusText();

    // 给udev/V4L2节点留出稳定时间，随后由统一重协调流程恢复。
    QTimer::singleShot(500, this, [this]() {
        (void)reconcilePtzDevice(false, false);
    });
}

void ControlPanelWindow::onPtzFrameTimer()
{
    if (!m_ptzCapture || !m_ptzPreviewLabel) {
        return;
    }

    QImage frame;
    if (!m_ptzCapture->takeLatestFrame(&frame) || frame.isNull()) {
        return;
    }

    // 保存原始预览帧，用于截图；界面缩放后的QPixmap不作为截图源。
    m_lastPtzFrame = frame.copy();

    const QSize target = m_ptzPreviewLabel->contentsRect().size();
    if (target.width() <= 0 || target.height() <= 0) {
        return;
    }

    QSize displaySize = target;
    const qreal sourceAspect = qreal(m_ptzDisplayAspectWidth)
            / qreal(m_ptzDisplayAspectHeight);
    const qreal targetAspect = qreal(target.width()) / qreal(target.height());
    if (targetAspect > sourceAspect) {
        displaySize.setWidth(qMax(1, qRound(target.height() * sourceAspect)));
    } else {
        displaySize.setHeight(qMax(1, qRound(target.width() / sourceAspect)));
    }

    // 720x480模拟视频使用非方形像素，按Profile的4:3比例显示；
    // HD USB PTZ仍按16:9显示。QLabel居中后自动形成必要黑边。
    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
                displaySize,
                Qt::IgnoreAspectRatio,
                Qt::FastTransformation);
    m_ptzPreviewLabel->setPixmap(pixmap);
}

void ControlPanelWindow::onSavePtzSnapshotClicked()
{
    if (m_lastPtzFrame.isNull()) {
        m_lastError = QStringLiteral("PTZ截图：当前没有可保存的预览帧");
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        updateStatusText();
        return;
    }

    QString storageError;
    if (!prepareSnapshotStorage(m_snapshotMountPoint,
                                m_manualSnapshotRoot,
                                m_detectedSnapshotRoot,
                                &storageError)) {
        m_lastError = QStringLiteral("PTZ截图：%1").arg(storageError);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        updateStatusText();
        return;
    }

    const QString fileName = QStringLiteral("PTZ_%1.jpg")
            .arg(QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString filePath = QDir(m_manualSnapshotRoot).filePath(fileName);

    QString errorMessage;
    if (!saveImageAsJpeg(m_lastPtzFrame, filePath, 92, &errorMessage)) {
        m_lastError = QStringLiteral("PTZ截图：%1").arg(errorMessage);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        updateStatusText();
        return;
    }

    m_lastSnapshotPath = filePath;
    m_lastAction = QStringLiteral("PTZ截图已保存：%1").arg(filePath);
    m_lastError = QStringLiteral("none");
    if (m_ptzVideoStatusLabel) {
        m_ptzVideoStatusLabel->setText(
                    QStringLiteral("截图已保存：%1").arg(filePath));
    }
    updateStatusText();
}

void ControlPanelWindow::onViewPtzSnapshotsClicked()
{
    QString storageError;
    if (!prepareSnapshotStorage(m_snapshotMountPoint,
                                m_manualSnapshotRoot,
                                m_detectedSnapshotRoot,
                                &storageError)) {
        m_lastError = QStringLiteral("查看抓拍：%1").arg(storageError);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        QMessageBox::warning(this,
                             QStringLiteral("查看抓拍"),
                             m_lastError);
        updateStatusText();
        return;
    }

    const QFileInfoList snapshots = collectManualSnapshotFiles(
                m_manualSnapshotRoot,
                m_snapshotRoot);
    if (snapshots.isEmpty()) {
        const QString message = QStringLiteral(
                    "尚未找到手动抓拍图片。\n保存目录：%1")
                .arg(m_manualSnapshotRoot);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(message);
        }
        QMessageBox::information(this,
                                 QStringLiteral("查看抓拍"),
                                 message);
        m_lastAction = QStringLiteral("查看手动抓拍：目录为空");
        updateStatusText();
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("查看手动抓拍"));
    dialog.resize(1040, 650);
    dialog.setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    QLabel *imageLabel = new QLabel(QStringLiteral("正在加载图片..."), &dialog);
    imageLabel->setObjectName("ptzPreview");
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(820, 460);
    imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(imageLabel, 1);

    QLabel *fileLabel = new QLabel(&dialog);
    fileLabel->setObjectName("ptzState");
    fileLabel->setAlignment(Qt::AlignCenter);
    fileLabel->setWordWrap(true);
    layout->addWidget(fileLabel);

    QHBoxLayout *buttonRow = new QHBoxLayout();
    QPushButton *previousButton = new QPushButton(QStringLiteral("上一张"), &dialog);
    QPushButton *nextButton = new QPushButton(QStringLiteral("下一张"), &dialog);
    QPushButton *closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    previousButton->setObjectName("ptzButton");
    nextButton->setObjectName("ptzButton");
    closeButton->setObjectName("ptzButton");
    buttonRow->addWidget(previousButton);
    buttonRow->addWidget(nextButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    int currentIndex = 0;
    QImage currentImage;

    const auto refreshViewer = [&]() {
        if (currentIndex < 0 || currentIndex >= snapshots.size()) {
            return;
        }

        const QFileInfo &fileInfo = snapshots.at(currentIndex);
        QString loadError;
        QImage loaded;
        if (!loadJpegImage(fileInfo.absoluteFilePath(), &loaded, &loadError)) {
            currentImage = QImage();
            imageLabel->setPixmap(QPixmap());
            imageLabel->setText(QStringLiteral("图片读取失败\n%1")
                                .arg(loadError));
        } else {
            currentImage = loaded;
            QSize targetSize = imageLabel->contentsRect().size();
            if (targetSize.width() < 100 || targetSize.height() < 100) {
                targetSize = QSize(900, 500);
            }
            imageLabel->setText(QString());
            imageLabel->setPixmap(QPixmap::fromImage(currentImage).scaled(
                                      targetSize,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
        }

        fileLabel->setText(QStringLiteral(
                               "%1 / %2    %3\n拍摄时间：%4")
                           .arg(currentIndex + 1)
                           .arg(snapshots.size())
                           .arg(fileInfo.fileName())
                           .arg(fileInfo.lastModified().toString(
                                    QStringLiteral("yyyy-MM-dd HH:mm:ss"))));

        previousButton->setEnabled(currentIndex > 0);
        nextButton->setEnabled(currentIndex + 1 < snapshots.size());
    };

    connect(previousButton, &QPushButton::clicked, &dialog, [&]() {
        if (currentIndex > 0) {
            --currentIndex;
            refreshViewer();
        }
    });
    connect(nextButton, &QPushButton::clicked, &dialog, [&]() {
        if (currentIndex + 1 < snapshots.size()) {
            ++currentIndex;
            refreshViewer();
        }
    });
    connect(closeButton, &QPushButton::clicked,
            &dialog, &QDialog::accept);

    dialog.layout()->activate();
    refreshViewer();
    dialog.exec();

    m_lastAction = QStringLiteral("查看手动抓拍：共%1张")
            .arg(snapshots.size());
    m_lastError = QStringLiteral("none");
    updateStatusText();
}

void ControlPanelWindow::onPtzCaptureStateChanged(bool running, QString message)
{
    m_ptzCaptureRunning = running;
    m_ptzCaptureState = message;

    if (m_ptzVideoStatusLabel) {
        m_ptzVideoStatusLabel->setText(message);
    }
    if (!running && m_ptzPreviewLabel && message != "PTZ视频已停止") {
        m_ptzPreviewLabel->setText(message);
    }

    updatePtzServiceButtons();
    updateStatusText();

    // 非人工停止、非切换过程中的异常退出，由热插拔协调器重新解析和恢复。
    if (!running && m_ptzPreviewDesired && !m_ptzSwitchInProgress
            && message != QStringLiteral("PTZ视频已停止")) {
        QTimer::singleShot(700, this, [this]() {
            (void)reconcilePtzDevice(false, false);
        });
    }
}

void ControlPanelWindow::onPtzCaptureStatsUpdated(double captureFps,
                                                  double decodeFps,
                                                  double decodeAvgUs,
                                                  int failCount)
{
    m_ptzCaptureFps = captureFps;
    m_ptzDecodeFps = decodeFps;
    m_ptzDecodeAvgUs = decodeAvgUs;
    m_ptzDecodeFail = failCount;

    if (m_ptzStatsLabel) {
        m_ptzStatsLabel->setText(
                    QString("CAP:%1  DEC:%2  JPEG:%3 us  FAIL:%4")
                    .arg(captureFps, 0, 'f', 1)
                    .arg(decodeFps, 0, 'f', 1)
                    .arg(decodeAvgUs, 0, 'f', 0)
                    .arg(failCount));
    }

    updateStatusText();
}

void ControlPanelWindow::onPtzControlStateChanged(bool ready, QString message)
{
    m_ptzControlReady = ready;
    m_ptzControlState = message;

    if (m_ptzControlStatusLabel) {
        m_ptzControlStatusLabel->setText(message);
    }
    updatePtzServiceButtons();
    updateStatusText();
}

void ControlPanelWindow::onPtzControlError(QString message)
{
    m_lastError = QString("PTZ控制：%1").arg(message);
    if (m_ptzControlStatusLabel) {
        m_ptzControlStatusLabel->setText(m_lastError);
    }
    updateStatusText();
}

void ControlPanelWindow::updateFanUi()
{
    if (!m_fanController) {
        return;
    }

    const bool available = m_fanController->isAvailable();
    const bool enabled = available && m_fanController->isEnabled();
    const int duty = available ? m_fanController->dutyPercent()
                               : (m_fanPwmSlider ? m_fanPwmSlider->value() : 30);

    if (m_btnFanPower) {
        const QSignalBlocker blocker(m_btnFanPower);
        m_btnFanPower->setEnabled(available);
        m_btnFanPower->setChecked(enabled);
        m_btnFanPower->setText(enabled ? QStringLiteral("风扇开启")
                                           : QStringLiteral("风扇关闭"));
    }

    if (m_fanPwmSlider) {
        const QSignalBlocker blocker(m_fanPwmSlider);
        m_fanPwmSlider->setEnabled(available);
        m_fanPwmSlider->setValue(duty);
    }

    if (m_fanPwmSpin) {
        const QSignalBlocker blocker(m_fanPwmSpin);
        m_fanPwmSpin->setEnabled(available);
        m_fanPwmSpin->setValue(duty);
    }

    if (m_fanStatusLabel) {
        m_fanStatusLabel->setText(m_fanController->statusText());
    }
}

void ControlPanelWindow::updateStatusText()
{
    QString cpuText = (m_cpuLoad >= 0) ? QString::number(m_cpuLoad) + "%" : "NA";
    QString npuText = (m_npuLoad >= 0) ? QString::number(m_npuLoad) + "%" : "NA";
    QString tempText = (m_tempC >= 0.0) ? QString::number(m_tempC, 'f', 1) + "C" : "NA";

    QString fanText = QStringLiteral("NA");
    if (m_fanController && m_fanController->isAvailable()) {
        fanText = m_fanController->isEnabled()
                ? QStringLiteral("%1%").arg(m_fanController->dutyPercent())
                : QStringLiteral("OFF");
    }

    const VideoRoute route = m_display ? m_display->videoRoute() : VideoRoute::Overview;
    const QString routeText = QString::fromLatin1(videoRouteKey(route));
    const QString aiShort = m_aiEnabled
            ? (m_aiReady ? "ON" : "WAIT")
            : "OFF";
    const QString aiTaskShort = m_aiTask == NpuTaskType::PlateCascade ? "PLATE" : "PED";

    QString bottom = QString("HDMI:%1 VIEW:%2  PTZ-V:%3  PTZ-C:%4  FAN:%5  RX:%6  FLIP:%7  PTZ:%8  AI:%9/%10 P:%11 D:%12  TEMP:%13  FAIL:%14")
            .arg(m_displayRunning ? "ON" : "OFF")
            .arg(routeText)
            .arg(m_ptzCaptureRunning ? "ON" : "OFF")
            .arg(m_ptzControlReady ? "ON" : "OFF")
            .arg(fanText)
            .arg(m_validFps, 0, 'f', 1)
            .arg(m_flipFps, 0, 'f', 1)
            .arg(m_ptzDecodeFps, 0, 'f', 1)
            .arg(aiShort)
            .arg(aiTaskShort)
            .arg(m_aiPlateCount)
            .arg(m_aiPedestrianCount)
            .arg(tempText)
            .arg(m_failCount + m_ptzDecodeFail);

    if (m_bottomStatus) {
        m_bottomStatus->setText(bottom);
    }

    QString detail;
    detail += QString("HDMI运行 : %1\n").arg(m_displayRunning ? "RUN" : "STOP");
    detail += QString("显示后端 : PCIe + RGA + DRM/KMS\n");
    detail += QString("显示状态 : %1\n").arg(m_displayState);
    detail += QString("显示规格 : %1x%2 RGB565\n")
            .arg(m_display ? m_display->outputWidth() : 0)
            .arg(m_display ? m_display->outputHeight() : 0);
    detail += QString("视频选路 : %1\n").arg(routeText);
    detail += QString("NPU输入  : %1\n")
            .arg(isSingleChannelRoute(route) ? routeText : QStringLiteral("ALL"));
    detail += QString("性能HUD  : ON\n");
    detail += QString("AI结果OSD: %1\n").arg(m_osdEnabled ? "ON" : "OFF");
    detail += QString("当前页面 : %1\n").arg(m_currentPage);
    detail += QString("运行时间 : %1 s\n\n").arg(m_seconds);

    detail += QString("PCIe read_fps  : %1\n").arg(m_readFps, 0, 'f', 1);
    detail += QString("PCIe valid_fps : %1\n").arg(m_validFps, 0, 'f', 1);
    detail += QString("HDMI rga_fps   : %1\n").arg(m_rgaFps, 0, 'f', 1);
    detail += QString("HDMI flip_fps  : %1\n").arg(m_flipFps, 0, 'f', 1);
    detail += QString("HDMI rga_avg_us: %1\n").arg(m_rgaAvgUs, 0, 'f', 1);
    detail += QString("HDMI osd_avg_us: %1\n").arg(m_osdAvgUs, 0, 'f', 0);
    detail += QString("HDMI plate_us  : %1\n").arg(m_plateOsdAvgUs, 0, 'f', 0);
    detail += QString("NPU prepare_us : %1\n").arg(m_npuPrepareAvgUs, 0, 'f', 0);
    detail += QString("HDMI fail_cnt  : %1\n\n").arg(m_failCount);

    detail += QString("PTZ配置     : %1\n")
            .arg(m_activePtzDevice.valid
                 ? QStringLiteral("%1 | %2 | %3 %4x%5@%6 | 显示%7:%8")
                   .arg(m_activePtzDevice.profile.displayName,
                        m_activePtzDevice.videoDevice,
                        v4l2PixelFormatName(m_activePtzDevice.profile.pixelFormat))
                   .arg(m_activePtzDevice.profile.captureWidth)
                   .arg(m_activePtzDevice.profile.captureHeight)
                   .arg(m_activePtzDevice.profile.captureFps)
                   .arg(m_ptzDisplayAspectWidth)
                   .arg(m_ptzDisplayAspectHeight)
                 : QStringLiteral("未解析"));
    detail += QString("PTZ视频状态 : %1\n").arg(m_ptzCaptureState);
    detail += QString("PTZ控制状态 : %1\n").arg(m_ptzControlState);
    detail += QString("PTZ capture_fps: %1\n").arg(m_ptzCaptureFps, 0, 'f', 1);
    detail += QString("PTZ decode_fps : %1\n").arg(m_ptzDecodeFps, 0, 'f', 1);
    detail += QString("PTZ jpeg_avg_us: %1\n").arg(m_ptzDecodeAvgUs, 0, 'f', 0);
    detail += QString("PTZ decode_fail: %1\n").arg(m_ptzDecodeFail);
    detail += QString("PTZ last_snapshot: %1\n\n").arg(m_lastSnapshotPath);

    if (m_fanController) {
        detail += QString("fan_status : %1\n")
                .arg(m_fanController->statusText());
    } else {
        detail += QString("fan_status : NA\n");
    }

    detail += QString("ai_enable : %1\n").arg(m_aiEnabled ? "ON" : "OFF");
    detail += QString("ai_ready  : %1\n").arg(m_aiReady ? "YES" : "NO");
    detail += QString("ai_fps    : %1\n").arg(m_aiFps, 0, 'f', 1);
    detail += QString("ai_plate  : %1\n").arg(m_aiPlateCount);
    detail += QString("ai_ped    : %1\n").arg(m_aiPedestrianCount);
    detail += QString("ai_status : %1\n").arg(m_aiStatus);

    detail += QString("cpu_load  : %1\n").arg(cpuText);
    detail += QString("npu_load  : %1\n").arg(npuText);
    detail += QString("temp      : %1\n").arg(tempText);
    detail += QString("最近操作 : %1\n").arg(m_lastAction);
    detail += QString("错误信息 : %1\n").arg(m_lastError);

    if (m_statusDetail) {
        m_statusDetail->setText(detail);
    }
}

void ControlPanelWindow::applyStyle()
{
    setStyleSheet(
        "QMainWindow { background: #101418; }"
        "QWidget { background: #101418; color: #E8EEF2; font-family: 'Sans'; font-size: 18px; }"
        "#titleLabel { font-size: 28px; font-weight: 700; color: #FFFFFF; padding: 2px; }"
        "#navPanel { background: #18202A; border-radius: 14px; }"
        "#navButton { background: #243040; color: #DDE7EF; border: 1px solid #34475D; border-radius: 12px; font-size: 19px; font-weight: 600; }"
        "#navButton:checked { background: #0A84FF; color: white; border: 1px solid #4CA3FF; }"
        "#navButton:pressed { background: #1260B3; }"
        "#navHint { color: #AAB7C4; font-size: 15px; }"
        "QGroupBox { background: #18202A; border: 1px solid #2A3A4A; border-radius: 14px; margin-top: 16px; font-size: 20px; font-weight: 700; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 6px; color: #85C7FF; }"
        "QPushButton { background: #26384B; color: #FFFFFF; border: 1px solid #3F5871; border-radius: 12px; min-height: 54px; font-size: 19px; font-weight: 600; }"
        "QPushButton:pressed { background: #0A84FF; }"
        "#videoRouteButton { min-height: 54px; font-size: 22px; font-weight: 700; }"
        "#videoRouteButton:checked { background: #0A84FF; color: white; border: 2px solid #85C7FF; }"
        "#videoRouteButton:pressed { background: #1260B3; }"
        "#touchAdjustButton { min-width: 54px; max-width: 54px; min-height: 46px; max-height: 46px; border-radius: 10px; font-size: 28px; font-weight: 700; padding: 0px; }"
        "#touchValueSpin { min-height: 46px; font-size: 20px; font-weight: 700; padding: 0px 4px; }"
        "#compactActionButton { min-height: 40px; max-height: 42px; font-size: 17px; padding: 0 12px; }"
        "#ptzButton { min-height: 44px; font-size: 18px; }"
        "QComboBox, QSpinBox, QDoubleSpinBox { background: #0F151C; color: #FFFFFF; border: 1px solid #3F5871; border-radius: 8px; min-height: 42px; padding-left: 8px; }"
        "QSlider::groove:horizontal { height: 10px; background: #0F151C; border: 1px solid #3F5871; border-radius: 5px; }"
        "QSlider::sub-page:horizontal { background: #0A84FF; border-radius: 5px; }"
        "QSlider::handle:horizontal { width: 24px; margin: -8px 0; background: #FFFFFF; border: 2px solid #0A84FF; border-radius: 12px; }"
        "QCheckBox { font-size: 19px; min-height: 42px; }"
        "QLabel { color: #E8EEF2; }"
        "#ptzPreview { background: #000000; color: #90A0AE; border: 1px solid #3F5871; border-radius: 8px; font-size: 20px; }"
        "#ptzState { background: #0F151C; color: #85C7FF; border-radius: 6px; padding: 5px; font-size: 15px; }"
        "#ptzStats { background: #06111C; color: #00FF66; border-radius: 6px; padding: 5px; font-family: 'Monospace'; font-size: 15px; }"
        "#bottomStatus { background: #06111C; color: #00FF66; border: 1px solid #17482A; border-radius: 10px; padding-left: 12px; font-size: 16px; font-weight: 700; }"
        "#aiRuntimeStatus { background: #06111C; color: #DCEEFF; border-radius: 8px; padding: 6px 8px; font-size: 15px; }"
        "#aiResultTable { font-size: 14px; }"
        "#statusScrollArea { background: #06111C; border-radius: 10px; }"
        "#statusScrollArea QWidget { background: #06111C; }"
        "#statusDetail { background: #06111C; color: #00FF66; border-radius: 10px; padding: 8px; font-family: 'Monospace'; font-size: 14px; }"
    );
}
