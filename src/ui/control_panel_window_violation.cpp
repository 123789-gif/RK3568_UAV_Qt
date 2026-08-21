#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createPedestrianPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(10);

    QGroupBox *previewGroup = new QGroupBox(
                QStringLiteral("行人模型检测预览"), page);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(9, 18, 9, 8);
    previewLayout->setSpacing(6);

    m_pedestrianPreview = new ViolationPreviewWidget(previewGroup);
    previewLayout->addWidget(m_pedestrianPreview, 1);

    QLabel *previewHint = new QLabel(
                QStringLiteral("显示行人模型经置信度阈值和NMS后的原始框；"
                               "不叠加站立姿态过滤、交通灯、斑马线或违法规则。"),
                previewGroup);
    previewHint->setObjectName(QStringLiteral("navHint"));
    previewHint->setWordWrap(true);
    previewLayout->addWidget(previewHint);
    pageLayout->addWidget(previewGroup, 1);

    QWidget *rightPanel = new QWidget(page);
    rightPanel->setMaximumWidth(380);
    rightPanel->setMinimumWidth(330);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(7);

    QGroupBox *controlGroup = new QGroupBox(QStringLiteral("行人模型控制"), rightPanel);
    QGridLayout *controlGrid = new QGridLayout(controlGroup);
    controlGrid->setContentsMargins(10, 18, 10, 9);
    controlGrid->setHorizontalSpacing(7);
    controlGrid->setVerticalSpacing(7);

    m_btnPedestrianStart = new QPushButton(QStringLiteral("开启行人模型"), controlGroup);
    m_btnPedestrianStop = new QPushButton(QStringLiteral("关闭行人模型"), controlGroup);
    m_btnPedestrianStart->setObjectName(QStringLiteral("violationRunButton"));
    m_btnPedestrianStop->setObjectName(QStringLiteral("violationRunButton"));
    m_btnPedestrianStart->setMinimumHeight(48);
    m_btnPedestrianStop->setMinimumHeight(48);

    m_pedestrianConfSpin = new QDoubleSpinBox(controlGroup);
    m_pedestrianConfSpin->setRange(0.10, 0.95);
    m_pedestrianConfSpin->setSingleStep(0.05);
    m_pedestrianConfSpin->setDecimals(2);
    m_pedestrianConfSpin->setValue(0.50);
    m_pedestrianConfSpin->setMinimumHeight(44);

    m_pedestrianNmsSpin = new QDoubleSpinBox(controlGroup);
    m_pedestrianNmsSpin->setRange(0.10, 0.90);
    m_pedestrianNmsSpin->setSingleStep(0.05);
    m_pedestrianNmsSpin->setDecimals(2);
    m_pedestrianNmsSpin->setValue(0.45);
    m_pedestrianNmsSpin->setMinimumHeight(44);

    controlGrid->addWidget(m_btnPedestrianStart, 0, 0, 1, 2);
    controlGrid->addWidget(m_btnPedestrianStop, 1, 0, 1, 2);
    controlGrid->addWidget(new QLabel(QStringLiteral("置信度阈值"), controlGroup), 2, 0);
    controlGrid->addWidget(m_pedestrianConfSpin, 2, 1);
    controlGrid->addWidget(new QLabel(QStringLiteral("NMS阈值"), controlGroup), 3, 0);
    controlGrid->addWidget(m_pedestrianNmsSpin, 3, 1);
    rightLayout->addWidget(controlGroup);

    QGroupBox *runtimeGroup = new QGroupBox(QStringLiteral("运行状态"), rightPanel);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(9, 18, 9, 8);
    m_pedestrianRuntimeLabel = new QLabel(
                QStringLiteral("行人模型未启动"), runtimeGroup);
    m_pedestrianRuntimeLabel->setObjectName(QStringLiteral("violationRuntime"));
    m_pedestrianRuntimeLabel->setWordWrap(true);
    m_pedestrianRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    runtimeLayout->addWidget(m_pedestrianRuntimeLabel);
    rightLayout->addWidget(runtimeGroup);

    QGroupBox *resultGroup = new QGroupBox(QStringLiteral("模型输出"), rightPanel);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(9, 18, 9, 8);
    m_pedestrianResultLabel = new QLabel(
                QStringLiteral("等待行人检测结果。"), resultGroup);
    m_pedestrianResultLabel->setObjectName(QStringLiteral("violationResult"));
    m_pedestrianResultLabel->setWordWrap(true);
    m_pedestrianResultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_pedestrianResultLabel->setSizePolicy(QSizePolicy::Expanding,
                                           QSizePolicy::Expanding);
    resultLayout->addWidget(m_pedestrianResultLabel, 1);
    rightLayout->addWidget(resultGroup, 1);
    pageLayout->addWidget(rightPanel);

    connect(m_btnPedestrianStart, &QPushButton::clicked,
            this, &ControlPanelWindow::startPedestrianDetection);
    connect(m_btnPedestrianStop, &QPushButton::clicked,
            this, &ControlPanelWindow::stopPedestrianDetection);

    updatePedestrianButtons();
    return page;
}

void ControlPanelWindow::startPedestrianDetection()
{
    if (!m_display || !m_display->isRunning()) {
        m_lastError = QStringLiteral("请先在“视频显示”页面启动PCIe视频");
        if (m_pedestrianRuntimeLabel) {
            m_pedestrianRuntimeLabel->setText(
                        QStringLiteral("启动失败：%1").arg(m_lastError));
        }
        updateStatusText();
        return;
    }

    NpuTaskConfig config = m_display->npuConfig();
    config.enabled = true;
    config.task = NpuTaskType::Pedestrian;
    config.wrongWayRuleEnabled = false;
    config.targetFps = m_npuFpsSpin
            ? m_npuFpsSpin->value() : NPU_TARGET_FPS_DEFAULT;
    config.confidence = m_pedestrianConfSpin
            ? static_cast<float>(m_pedestrianConfSpin->value()) : 0.50f;
    config.nmsThreshold = m_pedestrianNmsSpin
            ? static_cast<float>(m_pedestrianNmsSpin->value()) : 0.45f;
    m_display->setNpuConfig(sanitizeNpuTaskConfig(config));
    m_aiRequestedConfigRevision = m_display->npuConfig().revision;

    m_pedestrianRunning = true;
    m_wrongWayRunning = false;
    m_wrongWayRuleEnabled = false;
    m_aiRequestedTask = NpuTaskType::Pedestrian;
    m_aiTaskSwitchPending = true;
    m_lastAction = QStringLiteral("开启独立行人检测模型");
    m_lastError = QStringLiteral("none");
    if (m_pedestrianRuntimeLabel) {
        m_pedestrianRuntimeLabel->setText(
                    QStringLiteral("正在加载行人模型并等待第一帧结果……"));
    }
    updatePedestrianButtons();
    updateWrongWayButtons();
    updateAiTaskButtons();
    updateStatusText();
}

void ControlPanelWindow::stopPedestrianDetection()
{
    if (!m_display) return;
    NpuTaskConfig config = m_display->npuConfig();
    if (config.task == NpuTaskType::Pedestrian) {
        config.enabled = false;
        m_display->setNpuConfig(config);
    }
    m_pedestrianRunning = false;
    m_lastAction = QStringLiteral("关闭独立行人检测模型");
    if (m_pedestrianRuntimeLabel) {
        m_pedestrianRuntimeLabel->setText(QStringLiteral("行人模型已关闭。"));
    }
    updatePedestrianButtons();
    updateAiTaskButtons();
    updateStatusText();
}

void ControlPanelWindow::updatePedestrianButtons()
{
    const bool switching = m_aiTaskSwitchPending ||
            (m_display && m_display->latestNpuResult().modelSwitching);
    if (m_btnPedestrianStart) {
        m_btnPedestrianStart->setEnabled(!m_pedestrianRunning && !switching);
        m_btnPedestrianStart->setProperty("violationActive", m_pedestrianRunning);
        m_btnPedestrianStart->style()->unpolish(m_btnPedestrianStart);
        m_btnPedestrianStart->style()->polish(m_btnPedestrianStart);
    }
    if (m_btnPedestrianStop) {
        m_btnPedestrianStop->setEnabled(m_pedestrianRunning && !switching);
    }
}

void ControlPanelWindow::updatePedestrianPanel(const NpuFrameResult &result)
{
    if (m_pedestrianPreview) m_pedestrianPreview->setResult(result);

    const bool active = result.enabled && result.task == NpuTaskType::Pedestrian;
    m_pedestrianRunning = active;
    updatePedestrianButtons();

    if (!active) {
        if (m_pedestrianRuntimeLabel) {
            m_pedestrianRuntimeLabel->setText(QStringLiteral("行人模型未启动"));
        }
        return;
    }

    if (m_pedestrianRuntimeLabel) {
        m_pedestrianRuntimeLabel->setText(
                    QStringLiteral("状态：%1｜输入：%2\n"
                                   "推理FPS：%3 / %4｜单帧：%5 ms\n"
                                   "RKNN：%6 ms｜后处理：%7 ms")
                    .arg(result.ready ? QStringLiteral("运行中")
                                      : QStringLiteral("准备中"))
                    .arg(result.sourceChannel > 0
                         ? QStringLiteral("CH%1").arg(result.sourceChannel)
                         : QStringLiteral("总览全图"))
                    .arg(result.fps, 0, 'f', 1)
                    .arg(result.targetFps)
                    .arg(result.lastMs, 0, 'f', 1)
                    .arg(result.rknnRunMs, 0, 'f', 1)
                    .arg(result.rknnPostMs, 0, 'f', 1));
    }

    if (!m_pedestrianResultLabel) return;
    if (!result.error.empty()) {
        m_pedestrianResultLabel->setText(
                    QStringLiteral("行人模型异常：%1")
                    .arg(QString::fromStdString(result.error)));
        return;
    }
    if (!result.ready) {
        m_pedestrianResultLabel->setText(QStringLiteral("等待行人模型输出……"));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("检测到行人：%1个")
             .arg(static_cast<int>(result.pedestrians.size()));
    const int showCount = qMin(6, static_cast<int>(result.pedestrians.size()));
    for (int i = 0; i < showCount; ++i) {
        const NpuBox &box = result.pedestrians[static_cast<std::size_t>(i)];
        lines << QStringLiteral("%1. conf=%2｜%3×%4")
                 .arg(i + 1)
                 .arg(box.score, 0, 'f', 2)
                 .arg(box.w, 0, 'f', 0)
                 .arg(box.h, 0, 'f', 0);
    }
    m_pedestrianResultLabel->setText(lines.join(QLatin1Char('\n')));
}
