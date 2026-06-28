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

#include <cstdio>
#include <csetjmp>
#include <algorithm>
#include <cerrno>
#include <cstring>
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


} // namespace

ControlPanelWindow::ControlPanelWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::ControlPanelWindow)
{
    ui->setupUi(this);

    setWindowTitle("RK3568 Dual Display + PTZ Console");

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

    connect(m_ptzControl, &PtzControlWorker::stateChanged,
            this, &ControlPanelWindow::onPtzControlStateChanged,
            Qt::QueuedConnection);

    connect(m_ptzControl, &PtzControlWorker::commandError,
            this, &ControlPanelWindow::onPtzControlError,
            Qt::QueuedConnection);

    setupControlUi();
    applyStyle();

    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout,
            this, &ControlPanelWindow::onStatusTimer);
    m_statusTimer->start(1000);

    // Qt按20 fps取最新帧。当前低CPU版本的PTZ采集同样为20 fps，
    // 不向主线程排队，也不影响PCIe HDMI显示链路。
    m_ptzFrameTimer = new QTimer(this);
    connect(m_ptzFrameTimer, &QTimer::timeout,
            this, &ControlPanelWindow::onPtzFrameTimer);
    m_ptzFrameTimer->start(50);

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
        if (m_ptzControl) {
            m_ptzControl->startControl();
        }
        if (m_ptzCapture) {
            m_ptzCapture->startCapture();
        }
    });
}

ControlPanelWindow::~ControlPanelWindow()
{
    if (m_ptzFrameTimer) {
        m_ptzFrameTimer->stop();
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

    QLabel *title = new QLabel("RK3568 双屏异构显示与 PTZ 控制台", central);
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
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    QGroupBox *displayGroup = new QGroupBox("HDMI 输出控制", page);
    QGridLayout *grid = new QGridLayout(displayGroup);
    grid->setContentsMargins(18, 22, 18, 16);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(14);

    m_btnStartStop = new QPushButton("启动/停止 HDMI", displayGroup);
    m_btnOsd = new QPushButton("OSD 开/关", displayGroup);

    m_resolutionCombo = new QComboBox(displayGroup);
    m_resolutionCombo->addItem("1920x1080@60", QSize(1920, 1080));
    m_resolutionCombo->addItem("1280x720@60", QSize(1280, 720));

    m_outputModeCombo = new QComboBox(displayGroup);
    m_outputModeCombo->addItem("原图显示", 0);
    m_outputModeCombo->addItem("测试框叠加", 1);
    m_outputModeCombo->addItem("推理框预留", 2);
    m_outputModeCombo->addItem("BEV预留", 3);

    QPushButton *applyVideo = new QPushButton("应用显示配置", displayGroup);

    grid->addWidget(m_btnStartStop, 0, 0);
    grid->addWidget(m_btnOsd, 0, 1);
    grid->addWidget(new QLabel("输出分辨率", displayGroup), 1, 0);
    grid->addWidget(m_resolutionCombo, 1, 1);
    grid->addWidget(new QLabel("输出模式", displayGroup), 2, 0);
    grid->addWidget(m_outputModeCombo, 2, 1);
    grid->addWidget(applyVideo, 3, 0, 1, 2);

    layout->addWidget(displayGroup);

    QGroupBox *noteGroup = new QGroupBox("当前策略", page);
    QVBoxLayout *noteLayout = new QVBoxLayout(noteGroup);
    QLabel *note = new QLabel("HDMI 主显示流走 PCIe + RGA + DRM/KMS，Qt 不参与每帧图像搬运。\n"
                              "PTZ USB视频仅进入DSI上的Qt预览，不访问DRM，不改变原PCIe显示链路。",
                              noteGroup);
    note->setWordWrap(true);
    noteLayout->addWidget(note);
    layout->addWidget(noteGroup);
    layout->addStretch(1);

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

    QGroupBox *previewGroup = new QGroupBox("PTZ USB 高清预览", page);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(10, 20, 10, 10);
    previewLayout->setSpacing(6);

    m_ptzPreviewLabel = new QLabel("等待 /dev/video0 ...", previewGroup);
    m_ptzPreviewLabel->setObjectName("ptzPreview");
    m_ptzPreviewLabel->setAlignment(Qt::AlignCenter);
    m_ptzPreviewLabel->setMinimumSize(600, 338);
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
    m_btnPtzPreview = new QPushButton("停止预览", controlGroup);
    m_btnPtzControl = new QPushButton("断开控制", controlGroup);
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
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    QGroupBox *group = new QGroupBox("AI / NPU 推理配置", page);
    QGridLayout *grid = new QGridLayout(group);
    grid->setContentsMargins(18, 22, 18, 16);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(12);

    m_npuEnableCheck = new QCheckBox("启用 NPU 推理", group);

    m_npuFpsSpin = new QSpinBox(group);
    m_npuFpsSpin->setRange(1, 30);
    m_npuFpsSpin->setValue(8);
    m_npuFpsSpin->setSuffix(" fps");

    m_confSpin = new QDoubleSpinBox(group);
    m_confSpin->setRange(0.05, 0.95);
    m_confSpin->setSingleStep(0.05);
    m_confSpin->setValue(0.35);

    QPushButton *apply = new QPushButton("应用 AI 配置", group);

    grid->addWidget(m_npuEnableCheck, 0, 0, 1, 2);
    grid->addWidget(new QLabel("推理频率", group), 1, 0);
    grid->addWidget(m_npuFpsSpin, 1, 1);
    grid->addWidget(new QLabel("置信度阈值", group), 2, 0);
    grid->addWidget(m_confSpin, 2, 1);
    grid->addWidget(apply, 3, 0, 1, 2);

    layout->addWidget(group);
    layout->addStretch(1);

    connect(apply, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyAiConfigClicked);

    return page;
}

QWidget *ControlPanelWindow::createStatusPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    QGroupBox *group = new QGroupBox("详细状态", page);
    QVBoxLayout *v = new QVBoxLayout(group);

    m_statusDetail = new QLabel(group);
    m_statusDetail->setObjectName("statusDetail");
    m_statusDetail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_statusDetail->setMinimumHeight(420);
    v->addWidget(m_statusDetail);

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

    m_lastAction = QString("OSD %1").arg(m_osdEnabled ? "ON" : "OFF");
    updateStatusText();
}

void ControlPanelWindow::onApplyVideoConfigClicked()
{
    if (!m_display) {
        return;
    }

    QSize size = m_resolutionCombo->currentData().toSize();
    int mode = m_outputModeCombo->currentData().toInt();

    if (!m_display->setOutputResolution(size.width(), size.height())) {
        m_lastError = m_display->lastErrorString();
    }
    m_display->setDisplayMode(mode);
    m_display->setOsdEnabled(m_osdEnabled);

    m_lastAction = QString("应用显示配置: %1x%2 mode=%3")
            .arg(size.width()).arg(size.height()).arg(mode);
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
    if (!m_display || !m_npuEnableCheck || !m_npuFpsSpin || !m_confSpin) {
        return;
    }

    const bool enabled = m_npuEnableCheck->isChecked();
    const int fps = m_npuFpsSpin->value();
    const double conf = m_confSpin->value();

    m_display->setNpuTargetFps(fps);
    m_display->setNpuConfidence(static_cast<float>(conf));
    m_display->setNpuEnabled(enabled);

    m_aiEnabled = enabled;
    m_aiStatus = enabled ? "pending" : "off";
    if (!enabled) {
        m_aiReady = false;
        m_aiFps = 0.0;
        m_aiPlateCount = 0;
        m_aiPedestrianCount = 0;
    }
    m_lastAction = QString("AI配置: enable=%1 fps=%2 conf=%3")
            .arg(enabled ? "ON" : "OFF")
            .arg(fps)
            .arg(conf, 0, 'f', 2);
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
                                               int npuLoad,
                                               double tempC)
{
    m_readFps = readFps;
    m_validFps = validFps;
    m_rgaFps = rgaFps;
    m_flipFps = flipFps;
    m_rgaAvgUs = rgaAvgUs;
    m_failCount = failCount;
    m_npuLoad = npuLoad;
    m_tempC = tempC;

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

void ControlPanelWindow::onPtzPreviewClicked()
{
    if (!m_ptzCapture) {
        return;
    }

    if (m_ptzCapture->isRunning()) {
        m_ptzCapture->stopCapture();
        m_lastAction = "停止PTZ视频预览";
    } else {
        m_ptzCapture->startCapture();
        m_lastAction = "启动PTZ视频预览";
    }
    updateStatusText();
}

void ControlPanelWindow::onPtzControlClicked()
{
    if (!m_ptzControl) {
        return;
    }

    if (m_ptzControl->isRunning()) {
        m_ptzControl->stopControl();
        m_lastAction = "断开RS232 PTZ控制";
    } else {
        m_ptzControl->startControl();
        m_lastAction = "连接RS232 PTZ控制";
    }
    updateStatusText();
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

    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
                target,
                Qt::KeepAspectRatio,
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
    if (m_btnPtzPreview) {
        m_btnPtzPreview->setText(running ? "停止预览" : "启动预览");
    }
    if (!running && m_ptzPreviewLabel && message != "PTZ视频已停止") {
        m_ptzPreviewLabel->setText(message);
    }

    updateStatusText();
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
    if (m_btnPtzControl) {
        const bool running = m_ptzControl && m_ptzControl->isRunning();
        m_btnPtzControl->setText(running ? "断开控制" : "连接控制");
    }

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
    QString npuText = (m_npuLoad >= 0) ? QString::number(m_npuLoad) + "%" : "NA";
    QString tempText = (m_tempC >= 0.0) ? QString::number(m_tempC, 'f', 1) + "C" : "NA";

    QString fanText = QStringLiteral("NA");
    if (m_fanController && m_fanController->isAvailable()) {
        fanText = m_fanController->isEnabled()
                ? QStringLiteral("%1%").arg(m_fanController->dutyPercent())
                : QStringLiteral("OFF");
    }

    const QString aiShort = m_aiEnabled ? (m_aiReady ? "ON" : "WAIT") : "OFF";

    QString bottom = QString("HDMI:%1  PTZ-V:%2  PTZ-C:%3  FAN:%4  RX:%5  FLIP:%6  PTZ:%7  AI:%8 P:%9 D:%10  TEMP:%11  FAIL:%12")
            .arg(m_displayRunning ? "ON" : "OFF")
            .arg(m_ptzCaptureRunning ? "ON" : "OFF")
            .arg(m_ptzControlReady ? "ON" : "OFF")
            .arg(fanText)
            .arg(m_validFps, 0, 'f', 1)
            .arg(m_flipFps, 0, 'f', 1)
            .arg(m_ptzDecodeFps, 0, 'f', 1)
            .arg(aiShort)
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
    detail += QString("OSD显示  : %1\n").arg(m_osdEnabled ? "ON" : "OFF");
    detail += QString("当前页面 : %1\n").arg(m_currentPage);
    detail += QString("运行时间 : %1 s\n\n").arg(m_seconds);

    detail += QString("PCIe read_fps  : %1\n").arg(m_readFps, 0, 'f', 1);
    detail += QString("PCIe valid_fps : %1\n").arg(m_validFps, 0, 'f', 1);
    detail += QString("HDMI rga_fps   : %1\n").arg(m_rgaFps, 0, 'f', 1);
    detail += QString("HDMI flip_fps  : %1\n").arg(m_flipFps, 0, 'f', 1);
    detail += QString("HDMI rga_avg_us: %1\n").arg(m_rgaAvgUs, 0, 'f', 1);
    detail += QString("HDMI fail_cnt  : %1\n\n").arg(m_failCount);

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
        "#statusDetail { background: #06111C; color: #00FF66; border-radius: 10px; padding: 12px; font-family: 'Monospace'; font-size: 16px; }"
    );
}
