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
                "总览：HDMI显示前端输出的完整1080P画面，并将该画面送入NPU。\n"
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

    QPushButton *overviewButton = new QPushButton(QStringLiteral("总览1080P（NPU全图）"), routeGroup);
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

    m_videoRouteSummaryLabel = new QLabel(QStringLiteral("当前应用：总览1080P；NPU全图"), routeGroup);
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

    // 以用户意图为准，而不是只看worker瞬时状态。这样在自动恢复等待期
    // 再次点击可可靠取消恢复，不会把已经退出的会话误当成“再次启动”。
    if (!m_displayDesired) {
        m_displayDesired = true;
        m_displayRestartAttempts = 0;
        m_displayHealthySamples = 0;
        if (m_displayRestartTimer) {
            m_displayRestartTimer->stop();
        }
        onApplyVideoConfigClicked();
        if (!m_display->startDisplay()) {
            m_lastError = m_display->lastErrorString();
            scheduleDisplayRestart(m_lastError);
        }
    } else {
        m_displayDesired = false;
        m_displayRestartAttempts = 0;
        m_displayHealthySamples = 0;
        if (m_displayRestartTimer) {
            m_displayRestartTimer->stop();
        }
        m_display->stopDisplay();
        m_lastAction = QStringLiteral("用户停止HDMI显示");
    }

    m_displayRunning = m_display->isRunning();
    updateStatusText();
}

void ControlPanelWindow::scheduleDisplayRestart(const QString &reason)
{
    constexpr int kMaxRestartAttempts = 2;
    if (!m_display || !m_displayDesired || m_display->isRunning() ||
        !m_displayRestartTimer || m_displayRestartTimer->isActive()) {
        return;
    }

    if (m_displayRestartAttempts >= kMaxRestartAttempts) {
        m_displayDesired = false;
        m_displayHealthySamples = 0;
        m_lastError = QStringLiteral("HDMI显示连续恢复%1次失败：%2；已停止自动重试")
                .arg(kMaxRestartAttempts)
                .arg(reason.isEmpty() ? QStringLiteral("未知错误") : reason);
        qCritical() << "[HDMI-RECOVERY] exhausted" << m_lastError;
        updateStatusText();
        return;
    }

    ++m_displayRestartAttempts;
    m_displayHealthySamples = 0;
    const int delayMs = (m_displayRestartAttempts == 1) ? 400 : 1000;
    m_displayState = QStringLiteral("HDMI异常退出，%1 ms后恢复（%2/%3）")
            .arg(delayMs)
            .arg(m_displayRestartAttempts)
            .arg(kMaxRestartAttempts);
    qWarning() << "[HDMI-RECOVERY] scheduled"
               << "attempt=" << m_displayRestartAttempts
               << "delay_ms=" << delayMs
               << "reason=" << reason;
    m_displayRestartTimer->start(delayMs);
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
                : QStringLiteral("当前应用：总览1080P；NPU全图"));
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

    if (state == QStringLiteral("RGA/DRM display running")) {
        m_displayHealthySamples = 0;
    } else if (state == QStringLiteral("RGA/DRM exited") &&
               m_displayDesired) {
        scheduleDisplayRestart(m_lastError);
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
                                               double labelOsdAvgUs,
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
    m_labelOsdAvgUs = labelOsdAvgUs;
    m_npuPrepareAvgUs = npuPrepareAvgUs;

    // 连续三个统计周期有有效page flip，才视为恢复成功并重新开放
    // 后续两次自动恢复额度，避免启动瞬间抖动形成无限重启环。
    if (m_displayDesired && m_displayRunning && flipFps > 1.0) {
        if (++m_displayHealthySamples >= 3) {
            if (m_displayRestartAttempts > 0) {
                qInfo() << "[HDMI-RECOVERY] session stable"
                        << "recoveries_total=" << m_displayRecoveryTotal;
            }
            m_displayHealthySamples = 3;
            m_displayRestartAttempts = 0;
        }
    } else {
        m_displayHealthySamples = 0;
    }

    updateStatusText();
}
