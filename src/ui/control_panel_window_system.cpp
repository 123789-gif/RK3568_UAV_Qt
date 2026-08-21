#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createCpuPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    QGroupBox *group = new QGroupBox(QStringLiteral("CPU与散热配置"), page);
    QGridLayout *grid = new QGridLayout(group);
    grid->setContentsMargins(18, 22, 18, 16);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(16);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    m_btnFanPower = new QPushButton(QStringLiteral("风扇关闭"), group);
    m_btnFanPower->setCheckable(true);
    m_btnFanPower->setMinimumHeight(58);

    auto makePwmButton = [group](const QString &text) {
        QPushButton *button = new QPushButton(text, group);
        button->setObjectName(QStringLiteral("fanPwmButton"));
        button->setFixedSize(72, 58);
        button->setAutoRepeat(true);
        button->setAutoRepeatDelay(450);
        button->setAutoRepeatInterval(180);
        return button;
    };

    m_btnFanPwmMinus = makePwmButton(QStringLiteral("−"));
    m_btnFanPwmPlus = makePwmButton(QStringLiteral("+"));

    m_fanPwmSpin = new QSpinBox(group);
    m_fanPwmSpin->setRange(0, 100);
    m_fanPwmSpin->setSingleStep(5);
    m_fanPwmSpin->setValue(30);
    m_fanPwmSpin->setSuffix(QStringLiteral(" %"));
    m_fanPwmSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_fanPwmSpin->setReadOnly(true);
    m_fanPwmSpin->setAlignment(Qt::AlignCenter);
    m_fanPwmSpin->setFocusPolicy(Qt::NoFocus);
    m_fanPwmSpin->setObjectName(QStringLiteral("fanPwmValue"));
    m_fanPwmSpin->setMinimumSize(150, 58);

    QWidget *pwmAdjust = new QWidget(group);
    QHBoxLayout *pwmLayout = new QHBoxLayout(pwmAdjust);
    pwmLayout->setContentsMargins(0, 0, 0, 0);
    pwmLayout->setSpacing(10);
    pwmLayout->addWidget(m_btnFanPwmMinus);
    pwmLayout->addWidget(m_fanPwmSpin, 1);
    pwmLayout->addWidget(m_btnFanPwmPlus);

    m_fanStatusLabel = new QLabel(QStringLiteral("正在检测PWM风扇..."), group);
    m_fanStatusLabel->setObjectName(QStringLiteral("ptzState"));
    m_fanStatusLabel->setWordWrap(true);

    QLabel *hint = new QLabel(
                QStringLiteral("点击 − / + 后立即写入PWM，占空比每次调整5%，无需再次确认。"),
                group);
    hint->setObjectName(QStringLiteral("navHint"));
    hint->setWordWrap(true);

    QLabel *note = new QLabel(
                QStringLiteral("PWM节点：/sys/class/pwm/pwmchip0/pwm0\n"
                               "周期：200000 ns（约5 kHz）；Qt程序需具有PWM节点写权限。"),
                group);
    note->setWordWrap(true);

    grid->addWidget(new QLabel(QStringLiteral("风扇开关"), group), 0, 0);
    grid->addWidget(m_btnFanPower, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("PWM占空比"), group), 1, 0);
    grid->addWidget(pwmAdjust, 1, 1);
    grid->addWidget(hint, 2, 1);
    grid->addWidget(m_fanStatusLabel, 3, 0, 1, 2);
    grid->addWidget(note, 4, 0, 1, 2);

    layout->addWidget(group);
    layout->addStretch(1);

    connect(m_btnFanPower, &QPushButton::clicked,
            this, &ControlPanelWindow::onFanPowerClicked);
    connect(m_btnFanPwmMinus, &QPushButton::clicked,
            m_fanPwmSpin, &QSpinBox::stepDown);
    connect(m_btnFanPwmPlus, &QPushButton::clicked,
            m_fanPwmSpin, &QSpinBox::stepUp);
    connect(m_fanPwmSpin,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &ControlPanelWindow::onFanPwmChanged);

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
void ControlPanelWindow::onFanPowerClicked(bool checked)
{
    if (!m_fanController || !m_fanPwmSpin || !m_btnFanPower) {
        return;
    }

    const bool oldEnabled = m_fanController->isEnabled();
    if (!m_fanController->apply(checked, m_fanPwmSpin->value())) {
        m_lastError = QStringLiteral("风扇控制：%1")
                .arg(m_fanController->lastError());
        const QSignalBlocker blocker(m_btnFanPower);
        m_btnFanPower->setChecked(oldEnabled);
    } else {
        m_lastAction = QStringLiteral("风扇%1，PWM=%2%")
                .arg(checked ? QStringLiteral("开启") : QStringLiteral("关闭"))
                .arg(m_fanPwmSpin->value());
        m_lastError = QStringLiteral("none");
    }

    updateFanUi();
    updateStatusText();
}
void ControlPanelWindow::onFanPwmChanged(int value)
{
    if (!m_fanController || !m_btnFanPower || !m_fanPwmSpin) {
        return;
    }

    const bool requestedEnabled = m_btnFanPower->isChecked();
    if (!m_fanController->apply(requestedEnabled, value)) {
        m_lastError = QStringLiteral("PWM占空比设置失败：%1")
                .arg(m_fanController->lastError());
    } else {
        m_lastAction = QStringLiteral("PWM占空比已设为%1%（立即生效）").arg(value);
        m_lastError = QStringLiteral("none");
    }

    // 成功时显示硬件回读值；失败时恢复到上一次有效值。
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

    updateFpgaUiAvailability();
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
                               : (m_fanPwmSpin ? m_fanPwmSpin->value() : 30);

    if (m_btnFanPower) {
        const QSignalBlocker blocker(m_btnFanPower);
        m_btnFanPower->setEnabled(available);
        m_btnFanPower->setChecked(enabled);
        m_btnFanPower->setText(enabled ? QStringLiteral("风扇开启")
                                       : QStringLiteral("风扇关闭"));
    }

    if (m_fanPwmSpin) {
        const QSignalBlocker blocker(m_fanPwmSpin);
        m_fanPwmSpin->setEnabled(available);
        m_fanPwmSpin->setValue(duty);
    }

    if (m_btnFanPwmMinus) {
        m_btnFanPwmMinus->setEnabled(available && duty > 0);
    }
    if (m_btnFanPwmPlus) {
        m_btnFanPwmPlus->setEnabled(available && duty < 100);
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
    const QString aiTaskShort = m_aiTask == NpuTaskType::PlateCascade
            ? "PLATE"
            : (m_aiTask == NpuTaskType::TrafficViolation
               ? "RULE"
               : (m_aiTask == NpuTaskType::VehicleWrongWay
                  ? (m_wrongWayRuleEnabled ? "WRONG" : "VEH")
                  : (m_pedestrianViolationRuleEnabled ? "PED-RULE" : "PED")));

    const QString ethernetShort = m_ethernetControlAvailable
            ? (m_ethernetActiveEnable ? QStringLiteral("ON") : QStringLiteral("OFF"))
            : QStringLiteral("NA");

    QString bottom = QString("HDMI:%1 VIEW:%2  NET:%3  PTZ-V:%4  PTZ-C:%5  FAN:%6  RX:%7  FLIP:%8  PTZ:%9  AI:%10/%11 P:%12 D:%13 V:%14  TEMP:%15  FAIL:%16")
            .arg(m_displayRunning ? "ON" : "OFF")
            .arg(routeText)
            .arg(ethernetShort)
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
            .arg(m_aiVehicleCount)
            .arg(tempText)
            .arg(m_failCount + m_ptzDecodeFail);

    if (m_bottomStatus) {
        m_bottomStatus->setText(bottom);
    }

    QString detail;
    detail += QString("HDMI运行 : %1\n").arg(m_displayRunning ? "RUN" : "STOP");
    detail += QString("显示后端 : PCIe + RGA + DRM/KMS\n");
    detail += QString("显示状态 : %1\n").arg(m_displayState);
    detail += QString("显示意图 : %1｜恢复次数：%2\n")
            .arg(m_displayDesired ? QStringLiteral("RUN") : QStringLiteral("STOP"))
            .arg(m_displayRecoveryTotal);
    detail += QString("显示规格 : %1x%2 RGB565\n")
            .arg(m_display ? m_display->outputWidth() : 0)
            .arg(m_display ? m_display->outputHeight() : 0);
    detail += QString("视频选路 : %1\n").arg(routeText);
    detail += QString("NPU输入  : %1\n")
            .arg(isSingleChannelRoute(route) ? routeText : QStringLiteral("ALL"));
    detail += QString("性能HUD  : ON\n");
    detail += QString("AI结果OSD: %1\n").arg(m_osdEnabled ? "ON" : "OFF");
    detail += QString("当前页面 : %1\n").arg(m_currentPage);
    detail += QString("运行时间 : %1 s\n").arg(m_seconds);
    detail += QString("FPGA控制 : %1\n").arg(m_fpgaControlAvailable ? "READY" : "OFFLINE");
    detail += QString("FPGA IP   : %1\n").arg(fpgaIpVersionText(m_fpgaIpVersion));
    detail += QString("FPGA模式  : %1\n").arg(fpgaModeName(m_fpgaActiveMode));
    detail += QString("FPGA bitmap: 0x%1\n").arg(m_fpgaAlgorithmBitmap, 8, 16, QLatin1Char('0'));
    detail += QString("FPGA ack/frame: %1 / %2\n\n")
            .arg(m_fpgaAckSequence)
            .arg(m_fpgaFrameCount);
    detail += QString("Ethernet控制: %1\n")
            .arg(m_ethernetControlAvailable ? "READY" : "OFFLINE");
    detail += QString("Ethernet状态: %1\n")
            .arg(m_ethernetActiveEnable ? "ON" : "OFF");
    detail += QString("Ethernet输出: %1x%2@%3 -> %4:%5\n")
            .arg(m_ethernetOutputWidth)
            .arg(m_ethernetOutputHeight)
            .arg(m_ethernetOutputFps)
            .arg(ipv4Text(m_ethernetDestinationIpv4))
            .arg(m_ethernetDestinationPort);
    detail += QString("Ethernet ack/frame: %1 / %2\n\n")
            .arg(m_ethernetAckSequence)
            .arg(m_ethernetFrameCount);

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
    detail += QString("ai_vehicle: %1\n").arg(m_aiVehicleCount);
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
