#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

ControlPanelWindow::ControlPanelWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("智能交通感知控制台｜BUILD-CHECK-09"));

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

    connect(m_display, &DrmRgaDisplay::fpgaControlInfoUpdated,
            this, &ControlPanelWindow::onFpgaControlInfoUpdated,
            Qt::QueuedConnection);

    connect(m_display, &DrmRgaDisplay::fpgaApplyFinished,
            this, &ControlPanelWindow::onFpgaApplyFinished,
            Qt::QueuedConnection);

    connect(m_display, &DrmRgaDisplay::ethernetControlInfoUpdated,
            this, &ControlPanelWindow::onEthernetControlInfoUpdated,
            Qt::QueuedConnection);

    connect(m_display, &DrmRgaDisplay::ethernetApplyFinished,
            this, &ControlPanelWindow::onEthernetApplyFinished,
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
    loadPedestrianViolationSettings();
    loadWrongWaySettings(videoRouteChannel(m_display->videoRoute()));
    loadPtzSettings();

    connect(m_display, &DrmRgaDisplay::previewFrameReady,
            this,
            [this](const QImage &image,
                   quint64 frameId,
                   int sourceChannel,
                   quint64 sourceRevision) {
        Q_UNUSED(frameId)
        Q_UNUSED(sourceRevision)
        if (m_currentPage == PedestrianPage && m_pedestrianPreview) {
            m_pedestrianPreview->setFrame(image);
        }
        if (m_currentPage == WrongWayPage && m_wrongWayPreview) {
            m_wrongWayPreview->setFrame(image);
        }
        if (sourceChannel != m_wrongWaySourceChannel) {
            loadWrongWaySettings(sourceChannel);
        }
    },
    Qt::QueuedConnection);

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

    m_displayRestartTimer = new QTimer(this);
    m_displayRestartTimer->setSingleShot(true);
    connect(m_displayRestartTimer, &QTimer::timeout, this, [this]() {
        if (!m_display || !m_displayDesired || m_display->isRunning()) {
            return;
        }
        ++m_displayRecoveryTotal;
        m_lastAction = QStringLiteral("自动重建HDMI显示会话：第%1次")
                .arg(m_displayRestartAttempts);
        qWarning() << "[HDMI-RECOVERY] restart attempt"
                   << m_displayRestartAttempts
                   << "total=" << m_displayRecoveryTotal;
        if (!m_display->startDisplay()) {
            scheduleDisplayRestart(m_display->lastErrorString());
        }
        updateStatusText();
    });

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

    if (m_displayRestartTimer) {
        m_displayRestartTimer->stop();
    }
    m_displayDesired = false;

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

}

void ControlPanelWindow::showWindows()
{
    // DSI与HDMI组成扩展桌面时，不能让窗口管理器的标题栏继续占用
    // 1280x720控制屏的有效高度。显式选择包含原点的DSI屏幕，并使用
    // 无边框窗口贴合availableGeometry，避免页面底部被裁出屏幕。
    QScreen *controlScreen = nullptr;
    const QList<QScreen *> screens = QApplication::screens();
    for (QScreen *screen : screens) {
        if (screen && screen->geometry().contains(QPoint(0, 0))) {
            controlScreen = screen;
            break;
        }
    }
    if (!controlScreen) {
        controlScreen = QApplication::primaryScreen();
    }

    setWindowFlag(Qt::FramelessWindowHint, true);
    const QRect target = controlScreen
            ? controlScreen->availableGeometry() : QRect(0, 0, 1280, 720);
    // 页面内容不得通过minimumSizeHint把顶层窗口推入右侧HDMI桌面。
    // 固定DSI窗口几何；局部低分辨率兼容由各页面viewport负责。
    setFixedSize(target.size());
    move(target.topLeft());
    show();
    QTimer::singleShot(0, this, [this]() { logCurrentPageGeometry(); });

    qDebug() << "ControlPanelWindow geometry:" << geometry();

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
    root->setContentsMargins(10, 6, 10, 6);
    root->setSpacing(5);

    QLabel *title = new QLabel(QStringLiteral("智能交通感知控制台"), central);
    title->setObjectName("titleLabel");
    title->setAlignment(Qt::AlignCenter);
    title->setFixedHeight(42);
    root->addWidget(title);

    QHBoxLayout *body = new QHBoxLayout();
    body->setSpacing(7);

    body->addWidget(createNavPanel(), 0);

    m_stack = new QStackedWidget(central);
    const auto addPageViewport = [this](QWidget *page,
                                        Qt::ScrollBarPolicy verticalPolicy) {
        QScrollArea *scroll = new QScrollArea(m_stack);
        scroll->setObjectName(QStringLiteral("pageScrollArea"));
        scroll->setWidgetResizable(true);
        scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(
                    verticalPolicy == Qt::ScrollBarAlwaysOff
                    ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(verticalPolicy);
        scroll->setWidget(page);
        m_stack->addWidget(scroll);
    };
    addPageViewport(createVideoPage(), Qt::ScrollBarAsNeeded);
    addPageViewport(createPtzPage(), Qt::ScrollBarAsNeeded);
    addPageViewport(createFpgaPage(), Qt::ScrollBarAsNeeded);
    addPageViewport(createCpuPage(), Qt::ScrollBarAsNeeded);
    // 车牌页仍使用viewport隔离sizeHint，但明确关闭竖向滚动条。
    // 页面内部按1280x720有效高度固定布局，不允许滚动掩盖布局错误。
    addPageViewport(createAiPage(), Qt::ScrollBarAlwaysOff);
    addPageViewport(createPedestrianPage(), Qt::ScrollBarAsNeeded);
    addPageViewport(createWrongWayPage(), Qt::ScrollBarAsNeeded);
    addPageViewport(createStatusPage(), Qt::ScrollBarAsNeeded);
    body->addWidget(m_stack, 1);

    root->addLayout(body, 1);

    m_bottomStatus = new QLabel(central);
    m_bottomStatus->setObjectName("bottomStatus");
    m_bottomStatus->setFixedHeight(30);
    m_bottomStatus->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    root->addWidget(m_bottomStatus);
}
QWidget *ControlPanelWindow::createNavPanel()
{
    QFrame *nav = new QFrame(this);
    nav->setObjectName("navPanel");
    nav->setFixedWidth(172);

    QVBoxLayout *layout = new QVBoxLayout(nav);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    QStringList names;
    names << "视频显示" << "PTZ控制" << "FPGA配置" << "CPU配置"
          << "车牌识别" << "行人检测" << "逆行检测" << "系统状态";

    QButtonGroup *group = new QButtonGroup(nav);
    group->setExclusive(true);

    for (int i = 0; i < names.size(); ++i) {
        QPushButton *btn = new QPushButton(names[i], nav);
        btn->setCheckable(true);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        btn->setObjectName("navButton");
        group->addButton(btn, i);
        layout->addWidget(btn, 1);
        if (i == 0) {
            btn->setChecked(true);
        }
    }

    connect(group,
            static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked),
            this,
            &ControlPanelWindow::setCurrentPage);

    return nav;
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
    if (m_display) {
        updateAiPanel(m_display->latestNpuResult());
    }
    updateStatusText();
    QTimer::singleShot(0, this, [this]() { logCurrentPageGeometry(); });
}

void ControlPanelWindow::logCurrentPageGeometry() const
{
    if (!m_stack || m_currentPage < 0 || m_currentPage >= m_stack->count()) {
        return;
    }
    QWidget *page = m_stack->widget(m_currentPage);
    QScrollArea *scroll = qobject_cast<QScrollArea *>(page);
    QWidget *content = scroll ? scroll->widget() : page;
    QRect screenGeometry;
    for (QScreen *candidate : QApplication::screens()) {
        if (candidate && candidate->geometry().contains(frameGeometry().center())) {
            screenGeometry = candidate->geometry();
            break;
        }
    }
    qInfo() << "[UI-GEOMETRY] page=" << m_currentPage
            << "window=" << geometry()
            << "stack=" << m_stack->geometry()
            << "page=" << (page ? page->geometry() : QRect())
            << "content=" << (content ? content->geometry() : QRect())
            << "content_min_hint=" << (content ? content->minimumSizeHint() : QSize())
            << "h_scroll=" << (scroll && scroll->horizontalScrollBar()->isVisible())
            << "v_scroll=" << (scroll && scroll->verticalScrollBar()->isVisible())
            << "screen=" << screenGeometry
            << "display_running=" << (m_display && m_display->isRunning());
}
