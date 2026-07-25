#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

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
void ControlPanelWindow::onStartStopClicked()
{
    if (!m_display) {
        return;
    }

    if (!m_display->isRunning()) {
        onApplyVideoConfigClicked();
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
        clearAiCurrentResult();
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
void ControlPanelWindow::onDisplayStateChanged(QString state)
{
    m_displayState = state;

    if (m_display) {
        m_displayRunning = m_display->isRunning();
    }

    updateFpgaUiAvailability();
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
