#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createViolationPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(10);

    QWidget *leftPanel = new QWidget(page);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(7);

    QGroupBox *previewGroup = new QGroupBox(QStringLiteral("场景预览与区域配置"), leftPanel);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(9, 18, 9, 8);
    previewLayout->setSpacing(6);

    m_violationPreview = new ViolationPreviewWidget(previewGroup);
    previewLayout->addWidget(m_violationPreview, 1);

    QWidget *regionButtons = new QWidget(previewGroup);
    QGridLayout *regionGrid = new QGridLayout(regionButtons);
    regionGrid->setContentsMargins(0, 0, 0, 0);
    regionGrid->setHorizontalSpacing(6);
    regionGrid->setVerticalSpacing(5);

    QPushButton *editLight = new QPushButton(QStringLiteral("交通灯区域"), regionButtons);
    QPushButton *editCrosswalk = new QPushButton(QStringLiteral("斑马线区域"), regionButtons);
    QPushButton *undo = new QPushButton(QStringLiteral("撤销"), regionButtons);
    QPushButton *clear = new QPushButton(QStringLiteral("清除区域"), regionButtons);
    QPushButton *save = new QPushButton(QStringLiteral("保存配置"), regionButtons);
    for (QPushButton *button : {editLight, editCrosswalk, undo, clear, save}) {
        button->setObjectName(QStringLiteral("violationCompactButton"));
        button->setMinimumHeight(42);
    }

    regionGrid->addWidget(editLight, 0, 0);
    regionGrid->addWidget(editCrosswalk, 0, 1);
    regionGrid->addWidget(undo, 0, 2);
    regionGrid->addWidget(clear, 1, 0);
    regionGrid->addWidget(save, 1, 1, 1, 2);
    previewLayout->addWidget(regionButtons);

    m_violationEditStatusLabel = new QLabel(
                QStringLiteral("先启动PCIe视频，再在预览中划定交通灯和斑马线区域。"),
                previewGroup);
    m_violationEditStatusLabel->setObjectName(QStringLiteral("violationEditStatus"));
    m_violationEditStatusLabel->setWordWrap(true);
    m_violationEditStatusLabel->setMinimumHeight(38);
    previewLayout->addWidget(m_violationEditStatusLabel);
    leftLayout->addWidget(previewGroup, 1);

    QWidget *rightPanel = new QWidget(page);
    rightPanel->setFixedWidth(385);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(7);

    QGroupBox *controlGroup = new QGroupBox(QStringLiteral("运行控制"), rightPanel);
    QGridLayout *controlGrid = new QGridLayout(controlGroup);
    controlGrid->setContentsMargins(10, 18, 10, 9);
    controlGrid->setHorizontalSpacing(6);
    controlGrid->setVerticalSpacing(6);

    m_btnViolationStart = new QPushButton(QStringLiteral("开始检测"), controlGroup);
    m_btnViolationStop = new QPushButton(QStringLiteral("停止检测"), controlGroup);
    m_btnViolationStart->setObjectName(QStringLiteral("violationRunButton"));
    m_btnViolationStop->setObjectName(QStringLiteral("violationRunButton"));
    m_btnViolationStart->setMinimumHeight(48);
    m_btnViolationStop->setMinimumHeight(48);

    QLabel *lightModeLabel = new QLabel(QStringLiteral("信号灯状态"), controlGroup);
    m_violationLightModeGroup = new QButtonGroup(this);
    m_violationLightModeGroup->setExclusive(true);
    QWidget *lightModeWidget = new QWidget(controlGroup);
    QHBoxLayout *lightModeLayout = new QHBoxLayout(lightModeWidget);
    lightModeLayout->setContentsMargins(0, 0, 0, 0);
    lightModeLayout->setSpacing(5);

    const auto addLightModeButton = [this, lightModeWidget, lightModeLayout](
            const QString &text, ViolationLightMode mode) {
        QPushButton *button = new QPushButton(text, lightModeWidget);
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("violationLightButton"));
        button->setMinimumHeight(40);
        m_violationLightModeGroup->addButton(button, static_cast<int>(mode));
        lightModeLayout->addWidget(button, 1);
    };
    addLightModeButton(QStringLiteral("自动"), ViolationLightMode::Auto);
    addLightModeButton(QStringLiteral("红灯"), ViolationLightMode::ForceRed);
    addLightModeButton(QStringLiteral("绿灯"), ViolationLightMode::ForceGreen);

    QLabel *ruleHint = new QLabel(
                QStringLiteral("规则：稳定红灯 + 行人脚点进入斑马线并持续600 ms，判定闯红灯。"),
                controlGroup);
    ruleHint->setObjectName(QStringLiteral("navHint"));
    ruleHint->setWordWrap(true);

    controlGrid->addWidget(m_btnViolationStart, 0, 0);
    controlGrid->addWidget(m_btnViolationStop, 0, 1);
    controlGrid->addWidget(lightModeLabel, 1, 0, 1, 2);
    controlGrid->addWidget(lightModeWidget, 2, 0, 1, 2);
    controlGrid->addWidget(ruleHint, 3, 0, 1, 2);
    rightLayout->addWidget(controlGroup, 0);

    QGroupBox *runtimeGroup = new QGroupBox(QStringLiteral("运行状态"), rightPanel);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(9, 18, 9, 8);
    m_violationRuntimeLabel = new QLabel(QStringLiteral("闯红灯检测未启动"), runtimeGroup);
    m_violationRuntimeLabel->setObjectName(QStringLiteral("violationRuntime"));
    m_violationRuntimeLabel->setWordWrap(true);
    m_violationRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    runtimeLayout->addWidget(m_violationRuntimeLabel);
    rightLayout->addWidget(runtimeGroup, 0);

    QGroupBox *resultGroup = new QGroupBox(QStringLiteral("当前结果"), rightPanel);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(9, 18, 9, 8);
    m_violationResultLabel = new QLabel(
                QStringLiteral("当前未发现行人违法行为。"), resultGroup);
    m_violationResultLabel->setObjectName(QStringLiteral("violationResult"));
    m_violationResultLabel->setWordWrap(true);
    m_violationResultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_violationResultLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    resultLayout->addWidget(m_violationResultLabel, 1);
    rightLayout->addWidget(resultGroup, 1);

    pageLayout->addWidget(leftPanel, 1);
    pageLayout->addWidget(rightPanel, 0);

    connect(editLight, &QPushButton::clicked,
            m_violationPreview, &ViolationPreviewWidget::beginTrafficLightEdit);
    connect(editCrosswalk, &QPushButton::clicked,
            m_violationPreview, &ViolationPreviewWidget::beginCrosswalkEdit);
    connect(undo, &QPushButton::clicked,
            m_violationPreview, &ViolationPreviewWidget::undoLastPoint);
    connect(clear, &QPushButton::clicked,
            m_violationPreview, &ViolationPreviewWidget::clearRegions);
    connect(save, &QPushButton::clicked, this, [this]() {
        if (saveViolationSettings()) {
            m_violationConfigDirty = false;
            if (m_violationEditStatusLabel) {
                m_violationEditStatusLabel->setText(
                            QStringLiteral("区域与规则配置已保存到 pedestrian_violation.ini"));
            }
        }
    });

    connect(m_violationPreview, &ViolationPreviewWidget::configEdited,
            this, [this](ViolationRuleConfig config) {
        m_violationConfig = sanitizeViolationRuleConfig(config);
        m_violationConfigDirty = true;
        if (m_display) {
            m_display->setViolationConfig(m_violationConfig);
        }
        updateViolationButtons();
    });
    connect(m_violationPreview, &ViolationPreviewWidget::statusMessage,
            this, [this](const QString &message) {
        if (m_violationEditStatusLabel) {
            m_violationEditStatusLabel->setText(message);
        }
    });

    connect(m_btnViolationStart, &QPushButton::clicked,
            this, &ControlPanelWindow::startViolationDetection);
    connect(m_btnViolationStop, &QPushButton::clicked,
            this, [this]() { stopViolationDetection(true); });

    connect(m_violationLightModeGroup,
            static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked),
            this, [this](int id) {
        m_violationConfig.lightMode = static_cast<ViolationLightMode>(id);
        ++m_violationConfig.revision;
        m_violationConfig = sanitizeViolationRuleConfig(m_violationConfig);
        m_violationConfigDirty = true;
        if (m_violationPreview) {
            m_violationPreview->setConfig(m_violationConfig);
        }
        if (m_display) {
            m_display->setViolationConfig(m_violationConfig);
        }
        updateViolationButtons();
    });

    updateViolationButtons();
    return page;
}
void ControlPanelWindow::loadViolationSettings()
{
    QString error;
    ViolationRuleConfig config;
    if (!loadViolationRuleConfig(violationSettingsPath(), &config, &error)) {
        m_lastError = error;
        config = ViolationRuleConfig();
    }
    m_violationConfig = sanitizeViolationRuleConfig(config);
    m_violationConfigDirty = false;

    if (m_violationPreview) {
        m_violationPreview->setConfig(m_violationConfig);
    }
    if (m_display) {
        m_display->setViolationConfig(m_violationConfig);
    }
    updateViolationButtons();

    if (m_violationEditStatusLabel) {
        m_violationEditStatusLabel->setText(m_violationConfig.regionsValid()
                ? QStringLiteral("已加载交通灯和斑马线区域，可直接开始检测。")
                : QStringLiteral("尚未完成区域配置：请先框选交通灯并点击4个斑马线顶点。"));
    }
}
bool ControlPanelWindow::saveViolationSettings()
{
    ++m_violationConfig.revision;
    m_violationConfig = sanitizeViolationRuleConfig(m_violationConfig);
    QString error;
    if (!saveViolationRuleConfig(violationSettingsPath(), m_violationConfig, &error)) {
        m_lastError = error;
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return false;
    }
    if (m_violationPreview) {
        m_violationPreview->setConfig(m_violationConfig);
    }
    if (m_display) {
        m_display->setViolationConfig(m_violationConfig);
    }
    m_violationConfigDirty = false;
    m_lastAction = QStringLiteral("已保存行人闯红灯区域与规则配置");
    m_lastError = QStringLiteral("none");
    updateStatusText();
    return true;
}
void ControlPanelWindow::startViolationDetection()
{
    if (!m_display || !m_display->isRunning()) {
        m_lastError = QStringLiteral("请先在“视频显示”页面启动PCIe视频");
        if (m_violationRuntimeLabel) {
            m_violationRuntimeLabel->setText(QStringLiteral("启动失败：%1").arg(m_lastError));
        }
        updateStatusText();
        return;
    }
    if (!m_violationConfig.regionsValid()) {
        m_lastError = QStringLiteral("交通灯区域或斑马线区域尚未配置完整");
        if (m_violationRuntimeLabel) {
            m_violationRuntimeLabel->setText(QStringLiteral("启动失败：%1").arg(m_lastError));
        }
        updateViolationButtons();
        updateStatusText();
        return;
    }

    NpuTaskConfig previous = m_display->npuConfig();
    if (m_wrongWayRunning && m_wrongWayPreviousAiConfigValid) {
        previous = m_wrongWayPreviousAiConfig;
    }
    if (m_wrongWayRunning) {
        stopWrongWayDetection(false);
    }
    if (!m_violationRunning) {
        m_violationPreviousAiConfig = previous;
        m_violationPreviousAiConfigValid = true;
    }

    if (m_violationConfigDirty) {
        (void)saveViolationSettings();
    }
    m_display->setViolationConfig(m_violationConfig);

    NpuTaskConfig config = m_display->npuConfig();
    config.enabled = true;
    config.task = NpuTaskType::TrafficViolation;
    config.targetFps = m_npuFpsSpin ? m_npuFpsSpin->value() : NPU_TARGET_FPS_DEFAULT;
    config.confidence = m_confSpin ? static_cast<float>(m_confSpin->value()) : 0.50f;
    config.nmsThreshold = m_nmsSpin ? static_cast<float>(m_nmsSpin->value()) : 0.45f;
    config = sanitizeNpuTaskConfig(config);
    m_display->setNpuConfig(config);

    m_violationRunning = true;
    m_violationLastEventId = 0;
    m_lastAction = QStringLiteral("启动行人闯红灯检测");
    m_lastError = QStringLiteral("none");
    if (m_violationRuntimeLabel) {
        m_violationRuntimeLabel->setText(QStringLiteral("正在加载行人模型并等待第一帧规则结果……"));
    }
    updateViolationButtons();
    updateStatusText();
}
void ControlPanelWindow::stopViolationDetection(bool restorePreviousConfig)
{
    if (!m_display) {
        return;
    }

    if (restorePreviousConfig && m_violationPreviousAiConfigValid) {
        m_display->setNpuConfig(m_violationPreviousAiConfig);
        m_aiRequestedTask = m_violationPreviousAiConfig.task == NpuTaskType::TrafficViolation
                ? NpuTaskType::Pedestrian
                : m_violationPreviousAiConfig.task;
    } else if (m_display->npuConfig().task == NpuTaskType::TrafficViolation) {
        NpuTaskConfig config = m_display->npuConfig();
        config.enabled = false;
        config.task = NpuTaskType::Pedestrian;
        m_display->setNpuConfig(config);
    }

    m_violationRunning = false;
    m_violationPreviousAiConfigValid = false;
    m_lastAction = QStringLiteral("停止行人闯红灯检测");
    if (m_violationRuntimeLabel) {
        m_violationRuntimeLabel->setText(QStringLiteral("闯红灯检测已停止，区域配置仍保留。"));
    }
    updateViolationButtons();
    updateAiTaskButtons();
    updateStatusText();
}
void ControlPanelWindow::updateViolationButtons()
{
    if (m_btnViolationStart) {
        m_btnViolationStart->setEnabled(!m_violationRunning && m_violationConfig.regionsValid());
        m_btnViolationStart->setProperty("violationActive", m_violationRunning);
        m_btnViolationStart->style()->unpolish(m_btnViolationStart);
        m_btnViolationStart->style()->polish(m_btnViolationStart);
    }
    if (m_btnViolationStop) {
        m_btnViolationStop->setEnabled(m_violationRunning);
    }

    if (m_violationLightModeGroup) {
        if (QAbstractButton *button = m_violationLightModeGroup->button(
                    static_cast<int>(m_violationConfig.lightMode))) {
            const QSignalBlocker blocker(m_violationLightModeGroup);
            button->setChecked(true);
        }
    }
}
void ControlPanelWindow::updateViolationPanel(const NpuFrameResult &result)
{
    if (m_violationPreview) {
        m_violationPreview->setResult(result);
    }

    if (result.task != NpuTaskType::TrafficViolation || !result.enabled) {
        if (m_violationRunning && result.task != NpuTaskType::TrafficViolation) {
            m_violationRunning = false;
            m_violationPreviousAiConfigValid = false;
            updateViolationButtons();
        }
        if (!m_violationRunning && m_violationRuntimeLabel) {
            m_violationRuntimeLabel->setText(QStringLiteral("闯红灯检测未启动｜区域：%1")
                    .arg(m_violationConfig.regionsValid()
                         ? QStringLiteral("已配置")
                         : QStringLiteral("未完成")));
        }
        return;
    }

    m_violationRunning = true;
    updateViolationButtons();

    const QString regionText = result.violationRegionsConfigured
            ? QStringLiteral("已配置") : QStringLiteral("未完成");
    if (m_violationRuntimeLabel) {
        m_violationRuntimeLabel->setText(
                    QStringLiteral("状态：%1｜区域：%2\n"
                                   "信号灯：%3｜行人：%4｜轨迹：%5\n"
                                   "推理FPS：%6｜单帧耗时：%7 ms")
                    .arg(result.ready ? QStringLiteral("运行中") : QStringLiteral("准备中"))
                    .arg(regionText)
                    .arg(violationLightText(result.trafficLight))
                    .arg(static_cast<int>(result.pedestrians.size()))
                    .arg(result.activeTrackCount)
                    .arg(result.fps, 0, 'f', 1)
                    .arg(result.lastMs, 0, 'f', 1));
    }

    if (!m_violationResultLabel) {
        return;
    }
    if (!result.error.empty()) {
        m_violationResultLabel->setText(
                    QStringLiteral("闯红灯检测异常：%1")
                    .arg(QString::fromStdString(result.error)));
        return;
    }
    if (!result.ready) {
        m_violationResultLabel->setText(QStringLiteral("等待行人模型和规则引擎输出……"));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("当前违法轨迹：%1")
             .arg(result.activeViolationCount);
    lines << QStringLiteral("累计违法事件：%1")
             .arg(static_cast<qulonglong>(result.violationEventCount));

    int accumulating = 0;
    for (const NpuBox &box : result.pedestrians) {
        if (box.pedestrianState == NpuPedestrianState::RedAccumulating) {
            ++accumulating;
        }
    }
    lines << QStringLiteral("红灯区域累计中：%1人").arg(accumulating);

    if (result.hasRecentViolation) {
        const NpuViolationEvent &event = result.recentViolation;
        const QDateTime eventTime = QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(event.wallClockMs));
        lines << QStringLiteral("最近事件：行人 #%1 闯红灯")
                 .arg(event.trackId);
        lines << QStringLiteral("判定持续：%1 ms｜时间：%2")
                 .arg(event.accumulatedMs, 0, 'f', 0)
                 .arg(eventTime.toString(QStringLiteral("HH:mm:ss")));
        if (event.eventId != m_violationLastEventId) {
            m_violationLastEventId = event.eventId;
            m_lastAction = QStringLiteral("检测到行人 #%1 闯红灯").arg(event.trackId);
        }
    } else {
        lines << QStringLiteral("当前未发现行人违法行为。");
    }

    m_violationResultLabel->setText(lines.join(QLatin1Char('\n')));
}
