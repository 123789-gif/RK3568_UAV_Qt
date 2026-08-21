#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createFpgaPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(6);

    QGroupBox *group = new QGroupBox(QStringLiteral("FPGA整帧预处理"), page);
    QGridLayout *grid = new QGridLayout(group);
    grid->setContentsMargins(16, 20, 16, 10);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    m_fpgaPreprocButtonGroup = new QButtonGroup(this);
    m_fpgaPreprocButtonGroup->setExclusive(false);

    auto createModeButton = [this, group](const QString &text, quint32 mode) {
        QPushButton *button = new QPushButton(text, group);
        button->setObjectName(QStringLiteral("fpgaModeButton"));
        button->setFixedHeight(58);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("fpgaActive", false);
        button->setProperty("fpgaPending", false);
        m_fpgaPreprocButtonGroup->addButton(button, static_cast<int>(mode));
        connect(button, &QPushButton::clicked, this,
                [this, mode]() { requestFpgaMode(mode); });
        return button;
    };

    QPushButton *bypassButton = createModeButton(
                fpgaModeButtonText(0U, false, false), 0U);
    QPushButton *gammaButton = createModeButton(
                fpgaModeButtonText(3U, false, false), 3U);
    QPushButton *sobelButton = createModeButton(
                fpgaModeButtonText(4U, false, false), 4U);
    QPushButton *dehazeButton = createModeButton(
                fpgaModeButtonText(5U, false, false), 5U);

    m_fpgaCapabilityLabel = new QLabel(
                QStringLiteral("硬件能力：等待启动PCIe显示并读取FPGA……"), group);
    m_fpgaCapabilityLabel->setWordWrap(true);
    m_fpgaCapabilityLabel->setObjectName(QStringLiteral("fpgaInfoLabel"));

    m_fpgaCurrentModeLabel = new QLabel(QStringLiteral("当前模式：未知"), group);
    m_fpgaCurrentModeLabel->setWordWrap(true);
    m_fpgaCurrentModeLabel->setObjectName(QStringLiteral("fpgaCurrentModeLabel"));

    grid->addWidget(bypassButton, 0, 0);
    grid->addWidget(gammaButton, 0, 1);
    grid->addWidget(sobelButton, 1, 0);
    grid->addWidget(dehazeButton, 1, 1);
    grid->addWidget(m_fpgaCurrentModeLabel, 2, 0, 1, 2);
    grid->addWidget(m_fpgaCapabilityLabel, 3, 0, 1, 2);

    QGroupBox *ethernetGroup = new QGroupBox(
                QStringLiteral("网络视频输出"), page);
    QGridLayout *ethernetGrid = new QGridLayout(ethernetGroup);
    ethernetGrid->setContentsMargins(18, 22, 18, 14);
    ethernetGrid->setHorizontalSpacing(14);
    ethernetGrid->setVerticalSpacing(8);
    ethernetGrid->setColumnStretch(0, 1);
    ethernetGrid->setColumnStretch(1, 1);

    m_ethernetOutputButtonGroup = new QButtonGroup(this);
    m_ethernetOutputButtonGroup->setExclusive(false);

    auto createEthernetButton = [this, ethernetGroup](bool enable) {
        QPushButton *button = new QPushButton(
                    ethernetOutputButtonText(enable, false, false),
                    ethernetGroup);
        button->setObjectName(QStringLiteral("ethOutputButton"));
        button->setMinimumHeight(58);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setProperty("ethActive", false);
        button->setProperty("ethPending", false);
        m_ethernetOutputButtonGroup->addButton(button, enable ? 1 : 0);
        connect(button, &QPushButton::clicked, this,
                [this, enable]() { requestEthernetOutput(enable); });
        return button;
    };

    QPushButton *ethernetOffButton = createEthernetButton(false);
    QPushButton *ethernetOnButton = createEthernetButton(true);

    m_ethernetCurrentStateLabel = new QLabel(
                QStringLiteral("当前状态：未知"), ethernetGroup);
    m_ethernetCurrentStateLabel->setObjectName(
                QStringLiteral("ethernetCurrentStateLabel"));

    m_ethernetInfoLabel = new QLabel(
                QStringLiteral("输出信息：等待读取FPGA……"), ethernetGroup);
    m_ethernetInfoLabel->setWordWrap(true);
    m_ethernetInfoLabel->setObjectName(QStringLiteral("ethernetInfoLabel"));

    ethernetGrid->addWidget(ethernetOffButton, 0, 0);
    ethernetGrid->addWidget(ethernetOnButton, 0, 1);
    ethernetGrid->addWidget(m_ethernetCurrentStateLabel, 1, 0, 1, 2);
    ethernetGrid->addWidget(m_ethernetInfoLabel, 2, 0, 1, 2);

    layout->addWidget(group);
    layout->addWidget(ethernetGroup);
    layout->addStretch(1);

    updateFpgaModeButtons();
    updateEthernetButtons();
    return page;
}
void ControlPanelWindow::requestFpgaMode(quint32 mode)
{
    if (!m_display) {
        return;
    }

    if (!m_display->isRunning()) {
        m_lastError = QStringLiteral("请先启动HDMI/PCIe显示，再切换FPGA预处理");
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    if (!m_fpgaControlAvailable) {
        m_lastError = QStringLiteral("FPGA预处理控制尚未就绪，请稍候");
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    if (mode >= 32U || !(m_fpgaAlgorithmBitmap & (1U << mode))) {
        m_lastError = QStringLiteral("当前FPGA比特流不支持%1").arg(fpgaModeName(mode));
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    if (m_fpgaApplyPending) {
        return;
    }

    if (mode == m_fpgaActiveMode) {
        m_lastAction = QStringLiteral("FPGA预处理保持当前模式：%1")
                .arg(fpgaModeName(mode));
        m_lastError = QStringLiteral("none");
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    m_fpgaRequestedMode = mode;
    m_fpgaApplyPending = true;
    m_lastAction = QStringLiteral("请求FPGA预处理：%1").arg(fpgaModeName(mode));
    m_lastError = QStringLiteral("none");
    updateFpgaUiAvailability();
    updateStatusText();

    m_display->requestFpgaPreprocess(static_cast<int>(mode), 0U);
}
void ControlPanelWindow::onFpgaControlInfoUpdated(bool available,
                                                        quint32 ipVersion,
                                                        quint32 capability,
                                                        quint32 algorithmBitmap,
                                                        quint32 streamCount,
                                                        quint32 activeMode,
                                                        quint32 ackSequence,
                                                        quint32 status,
                                                        quint32 errorStatus,
                                                        quint32 frameCount,
                                                        quint32 lastApplyFrame,
                                                        QString message)
{
    m_fpgaControlAvailable = available;
    if (!available) {
        m_fpgaApplyPending = false;
    }
    m_fpgaIpVersion = ipVersion;
    m_fpgaCapability = capability;
    m_fpgaAlgorithmBitmap = algorithmBitmap;
    m_fpgaActiveMode = activeMode;
    if (!m_fpgaApplyPending) {
        m_fpgaRequestedMode = activeMode;
    }
    m_fpgaAckSequence = ackSequence;
    m_fpgaStatus = status;
    m_fpgaErrorStatus = errorStatus;
    m_fpgaFrameCount = frameCount;
    m_fpgaLastApplyFrame = lastApplyFrame;

    if (m_fpgaCapabilityLabel) {
        m_fpgaCapabilityLabel->setText(available
                ? QStringLiteral("硬件能力：IP %1｜caps 0x%2｜algorithms 0x%3｜streams %4")
                  .arg(fpgaIpVersionText(ipVersion))
                  .arg(capability, 8, 16, QLatin1Char('0'))
                  .arg(algorithmBitmap, 8, 16, QLatin1Char('0'))
                  .arg(streamCount)
                : QStringLiteral("硬件能力：不可用｜%1").arg(message));
    }

    if (m_fpgaCurrentModeLabel) {
        // 当前模式只显示简短中文，避免状态字段过长引发布局跳动。
        m_fpgaCurrentModeLabel->setText(available
                ? QStringLiteral("当前模式：%1").arg(fpgaModeName(activeMode))
                : QStringLiteral("当前模式：未知"));
    }

    updateFpgaUiAvailability();
    updateStatusText();
}
void ControlPanelWindow::onFpgaApplyFinished(bool success,
                                               quint32 requestedMode,
                                               quint32 activeMode,
                                               quint32 sequence,
                                               quint32 ackSequence,
                                               quint32 status,
                                               quint32 errorStatus,
                                               quint32 latencyUs,
                                               quint32 lastApplyFrame,
                                               QString message)
{
    Q_UNUSED(sequence)
    Q_UNUSED(ackSequence)
    Q_UNUSED(status)
    Q_UNUSED(errorStatus)
    Q_UNUSED(latencyUs)
    Q_UNUSED(lastApplyFrame)

    m_fpgaApplyPending = false;
    m_fpgaActiveMode = activeMode;
    m_fpgaRequestedMode = activeMode;

    if (success) {
        m_lastAction = QStringLiteral("FPGA预处理已切换：%1")
                .arg(fpgaModeName(activeMode));
        m_lastError = QStringLiteral("none");
    } else {
        m_lastAction = QStringLiteral("FPGA预处理切换失败：请求%1，保持%2")
                .arg(fpgaModeName(requestedMode), fpgaModeName(activeMode));
        m_lastError = message;
    }

    updateFpgaUiAvailability();
    updateStatusText();
}
void ControlPanelWindow::onEthernetControlInfoUpdated(bool available,
                                                        quint32 ipVersion,
                                                        quint32 capability,
                                                        quint32 outputWidth,
                                                        quint32 outputHeight,
                                                        quint32 outputFps,
                                                        quint32 destinationIpv4,
                                                        quint32 destinationPort,
                                                        bool requestedEnable,
                                                        bool activeEnable,
                                                        quint32 ackSequence,
                                                        quint32 status,
                                                        quint32 errorStatus,
                                                        quint32 frameCount,
                                                        quint32 lastApplyFrame,
                                                        QString message)
{
    m_ethernetControlAvailable = available;
    if (!available) {
        m_ethernetApplyPending = false;
    }
    m_ethernetIpVersion = ipVersion;
    m_ethernetCapability = capability;
    m_ethernetOutputWidth = outputWidth;
    m_ethernetOutputHeight = outputHeight;
    m_ethernetOutputFps = outputFps;
    m_ethernetDestinationIpv4 = destinationIpv4;
    m_ethernetDestinationPort = destinationPort;
    m_ethernetRequestedEnable = requestedEnable;
    m_ethernetActiveEnable = activeEnable;
    m_ethernetAckSequence = ackSequence;
    m_ethernetStatus = status;
    m_ethernetErrorStatus = errorStatus;
    m_ethernetFrameCount = frameCount;
    m_ethernetLastApplyFrame = lastApplyFrame;

    if (m_ethernetCurrentStateLabel) {
        m_ethernetCurrentStateLabel->setText(available
                ? QStringLiteral("当前状态：%1")
                  .arg(activeEnable ? QStringLiteral("已开启")
                                    : QStringLiteral("已关闭"))
                : QStringLiteral("当前状态：未知"));
    }

    if (m_ethernetInfoLabel) {
        m_ethernetInfoLabel->setText(available
                ? QStringLiteral("输出信息：IP %1｜caps 0x%2｜%3×%4@%5｜目标 %6:%7｜发送帧 %8")
                  .arg(fpgaIpVersionText(ipVersion))
                  .arg(capability, 8, 16, QLatin1Char('0'))
                  .arg(outputWidth)
                  .arg(outputHeight)
                  .arg(outputFps)
                  .arg(ipv4Text(destinationIpv4))
                  .arg(destinationPort)
                  .arg(frameCount)
                : QStringLiteral("输出信息：不可用｜%1").arg(message));
    }

    updateFpgaUiAvailability();
    updateStatusText();
}
void ControlPanelWindow::onEthernetApplyFinished(bool success,
                                                 bool requestedEnable,
                                                 bool activeEnable,
                                                 quint32 sequence,
                                                 quint32 ackSequence,
                                                 quint32 status,
                                                 quint32 errorStatus,
                                                 quint32 latencyUs,
                                                 quint32 lastApplyFrame,
                                                 QString message)
{
    Q_UNUSED(sequence)
    Q_UNUSED(ackSequence)
    Q_UNUSED(status)
    Q_UNUSED(errorStatus)
    Q_UNUSED(latencyUs)
    Q_UNUSED(lastApplyFrame)

    m_ethernetApplyPending = false;
    m_ethernetActiveEnable = activeEnable;
    m_ethernetRequestedEnable = activeEnable;

    if (success) {
        m_lastAction = activeEnable
                ? QStringLiteral("网络视频输出已开启")
                : QStringLiteral("网络视频输出已关闭");
        m_lastError = QStringLiteral("none");
    } else {
        m_lastAction = QStringLiteral("网络视频输出切换失败：请求%1，保持%2")
                .arg(requestedEnable ? QStringLiteral("开启") : QStringLiteral("关闭"),
                     activeEnable ? QStringLiteral("开启") : QStringLiteral("关闭"));
        m_lastError = message;
    }

    updateFpgaUiAvailability();
    updateStatusText();
}
void ControlPanelWindow::requestEthernetOutput(bool enable)
{
    if (!m_display || !m_display->isRunning()) {
        m_lastError = QStringLiteral("请先启动HDMI/PCIe显示，再切换网络视频输出");
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    if (!m_ethernetControlAvailable) {
        m_lastError = QStringLiteral("网络视频输出控制尚未就绪，请稍候");
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    if (m_ethernetApplyPending) {
        return;
    }

    if (enable == m_ethernetActiveEnable) {
        m_lastAction = enable
                ? QStringLiteral("网络视频输出已经开启")
                : QStringLiteral("网络视频输出已经关闭");
        updateFpgaUiAvailability();
        updateStatusText();
        return;
    }

    m_ethernetRequestedEnable = enable;
    m_ethernetApplyPending = true;
    m_lastAction = enable
            ? QStringLiteral("请求开启网络视频输出")
            : QStringLiteral("请求关闭网络视频输出");
    m_lastError = QStringLiteral("none");

    updateFpgaUiAvailability();
    updateStatusText();
    m_display->requestEthernetOutput(enable);
}
void ControlPanelWindow::updateFpgaModeButtons()
{
    if (!m_fpgaPreprocButtonGroup) {
        return;
    }

    const bool displayRunning = m_display && m_display->isRunning();
    const QList<QAbstractButton *> buttons = m_fpgaPreprocButtonGroup->buttons();
    for (QAbstractButton *button : buttons) {
        if (!button) {
            continue;
        }

        const int buttonId = m_fpgaPreprocButtonGroup->id(button);
        if (buttonId < 0) {
            continue;
        }

        const quint32 mode = static_cast<quint32>(buttonId);
        const bool supported = mode < 32U
                && (m_fpgaAlgorithmBitmap & (1U << mode));
        const bool active = m_fpgaControlAvailable && mode == m_fpgaActiveMode;
        const bool pending = m_fpgaApplyPending && mode == m_fpgaRequestedMode;

        button->setEnabled(displayRunning
                           && m_fpgaControlAvailable
                           && supported
                           && !m_fpgaApplyPending);
        button->setText(fpgaModeButtonText(mode, active, pending));
        button->setProperty("fpgaActive", active);
        button->setProperty("fpgaPending", pending);

        // 动态属性改变后立即刷新QSS，使ACK前后高亮状态准确同步。
        if (button->style()) {
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
        button->update();
    }
}
void ControlPanelWindow::updateEthernetButtons()
{
    if (!m_ethernetOutputButtonGroup) {
        return;
    }

    const bool displayRunning = m_display && m_display->isRunning();
    const QList<QAbstractButton *> buttons = m_ethernetOutputButtonGroup->buttons();
    for (QAbstractButton *button : buttons) {
        if (!button) {
            continue;
        }

        const int buttonId = m_ethernetOutputButtonGroup->id(button);
        if (buttonId != 0 && buttonId != 1) {
            continue;
        }

        const bool enable = (buttonId == 1);
        const bool active = m_ethernetControlAvailable
                && enable == m_ethernetActiveEnable;
        const bool pending = m_ethernetApplyPending
                && enable == m_ethernetRequestedEnable;

        button->setEnabled(displayRunning
                           && m_ethernetControlAvailable
                           && !m_ethernetApplyPending);
        button->setText(ethernetOutputButtonText(enable, active, pending));
        button->setProperty("ethActive", active);
        button->setProperty("ethPending", pending);

        if (button->style()) {
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
        button->update();
    }
}
void ControlPanelWindow::updateFpgaUiAvailability()
{
    updateFpgaModeButtons();
    updateEthernetButtons();
}
