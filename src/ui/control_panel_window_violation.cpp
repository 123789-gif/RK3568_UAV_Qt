#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createPedestrianPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(9);

    QGroupBox *previewGroup = new QGroupBox(
                QStringLiteral("总览1080P预览与违法区域"), page);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(9, 18, 9, 8);
    previewLayout->setSpacing(5);

    m_pedestrianPreview = new ViolationPreviewWidget(previewGroup);
    previewLayout->addWidget(m_pedestrianPreview, 1);

    QWidget *regionButtons = new QWidget(previewGroup);
    QHBoxLayout *regionLayout = new QHBoxLayout(regionButtons);
    regionLayout->setContentsMargins(0, 0, 0, 0);
    regionLayout->setSpacing(5);
    QPushButton *editLight = new QPushButton(QStringLiteral("交通灯区域"), regionButtons);
    QPushButton *editCrosswalk = new QPushButton(QStringLiteral("斑马线区域"), regionButtons);
    QPushButton *undo = new QPushButton(QStringLiteral("撤销"), regionButtons);
    QPushButton *clear = new QPushButton(QStringLiteral("清除区域"), regionButtons);
    QPushButton *save = new QPushButton(QStringLiteral("保存配置"), regionButtons);
    for (QPushButton *button : {editLight, editCrosswalk, undo, clear, save}) {
        button->setObjectName(QStringLiteral("violationCompactButton"));
        button->setFixedHeight(40);
        regionLayout->addWidget(button, 1);
    }
    previewLayout->addWidget(regionButtons);

    m_pedestrianEditStatusLabel = new QLabel(
                QStringLiteral("在总览1080P画面中配置交通灯和斑马线区域。"),
                previewGroup);
    m_pedestrianEditStatusLabel->setObjectName(QStringLiteral("violationEditStatus"));
    m_pedestrianEditStatusLabel->setWordWrap(false);
    m_pedestrianEditStatusLabel->setFixedHeight(34);
    previewLayout->addWidget(m_pedestrianEditStatusLabel);
    pageLayout->addWidget(previewGroup, 1);

    QWidget *rightPanel = new QWidget(page);
    rightPanel->setFixedWidth(400);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    QGroupBox *modelGroup = new QGroupBox(QStringLiteral("行人模型控制"), rightPanel);
    modelGroup->setFixedHeight(150);
    QGridLayout *modelGrid = new QGridLayout(modelGroup);
    modelGrid->setContentsMargins(9, 18, 9, 8);
    modelGrid->setHorizontalSpacing(6);
    modelGrid->setVerticalSpacing(5);
    m_btnPedestrianStart = new QPushButton(QStringLiteral("开启行人模型"), modelGroup);
    m_btnPedestrianStop = new QPushButton(QStringLiteral("关闭行人模型"), modelGroup);
    m_btnPedestrianStart->setObjectName(QStringLiteral("violationRunButton"));
    m_btnPedestrianStop->setObjectName(QStringLiteral("violationRunButton"));
    m_btnPedestrianStart->setFixedHeight(44);
    m_btnPedestrianStop->setFixedHeight(44);

    m_pedestrianConfSpin = new QDoubleSpinBox(modelGroup);
    m_pedestrianConfSpin->setRange(0.10, 0.95);
    m_pedestrianConfSpin->setSingleStep(0.05);
    m_pedestrianConfSpin->setDecimals(2);
    m_pedestrianConfSpin->setValue(0.50);
    m_pedestrianConfSpin->setFixedHeight(40);
    m_pedestrianNmsSpin = new QDoubleSpinBox(modelGroup);
    m_pedestrianNmsSpin->setRange(0.10, 0.90);
    m_pedestrianNmsSpin->setSingleStep(0.05);
    m_pedestrianNmsSpin->setDecimals(2);
    m_pedestrianNmsSpin->setValue(0.45);
    m_pedestrianNmsSpin->setFixedHeight(40);

    modelGrid->addWidget(m_btnPedestrianStart, 0, 0, 1, 2);
    modelGrid->addWidget(m_btnPedestrianStop, 0, 2, 1, 2);
    modelGrid->addWidget(new QLabel(QStringLiteral("置信度"), modelGroup), 1, 0);
    modelGrid->addWidget(m_pedestrianConfSpin, 1, 1);
    modelGrid->addWidget(new QLabel(QStringLiteral("NMS"), modelGroup), 1, 2);
    modelGrid->addWidget(m_pedestrianNmsSpin, 1, 3);
    rightLayout->addWidget(modelGroup);

    QGroupBox *ruleGroup = new QGroupBox(QStringLiteral("行人违法判定"), rightPanel);
    ruleGroup->setFixedHeight(176);
    QGridLayout *ruleGrid = new QGridLayout(ruleGroup);
    ruleGrid->setContentsMargins(9, 18, 9, 8);
    ruleGrid->setHorizontalSpacing(5);
    ruleGrid->setVerticalSpacing(5);
    m_btnPedestrianViolationRule = new QPushButton(
                QStringLiteral("违法判定：关闭"), ruleGroup);
    m_btnPedestrianViolationRule->setCheckable(true);
    m_btnPedestrianViolationRule->setObjectName(QStringLiteral("violationLightButton"));
    m_btnPedestrianViolationRule->setFixedHeight(42);

    m_pedestrianLightModeGroup = new QButtonGroup(this);
    m_pedestrianLightModeGroup->setExclusive(true);
    QWidget *lightModeWidget = new QWidget(ruleGroup);
    QHBoxLayout *lightModeLayout = new QHBoxLayout(lightModeWidget);
    lightModeLayout->setContentsMargins(0, 0, 0, 0);
    lightModeLayout->setSpacing(5);
    const auto addLightMode = [this, lightModeWidget, lightModeLayout](
            const QString& text, ViolationLightMode mode) {
        QPushButton *button = new QPushButton(text, lightModeWidget);
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("violationLightButton"));
        button->setFixedHeight(38);
        m_pedestrianLightModeGroup->addButton(button, static_cast<int>(mode));
        lightModeLayout->addWidget(button, 1);
    };
    addLightMode(QStringLiteral("自动"), ViolationLightMode::Auto);
    addLightMode(QStringLiteral("强制红灯"), ViolationLightMode::ForceRed);
    addLightMode(QStringLiteral("强制绿灯"), ViolationLightMode::ForceGreen);

    QLabel *ruleHint = new QLabel(
                QStringLiteral("红灯下脚点进入斑马线持续600 ms判定违法。"),
                ruleGroup);
    ruleHint->setObjectName(QStringLiteral("navHint"));
    ruleHint->setWordWrap(false);
    ruleGrid->addWidget(m_btnPedestrianViolationRule, 0, 0, 1, 2);
    ruleGrid->addWidget(lightModeWidget, 1, 0, 1, 2);
    ruleGrid->addWidget(ruleHint, 2, 0, 1, 2);
    rightLayout->addWidget(ruleGroup);

    QGroupBox *runtimeGroup = new QGroupBox(QStringLiteral("运行状态"), rightPanel);
    runtimeGroup->setFixedHeight(118);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(9, 18, 9, 8);
    m_pedestrianRuntimeLabel = new QLabel(QStringLiteral("行人模型未启动"), runtimeGroup);
    m_pedestrianRuntimeLabel->setObjectName(QStringLiteral("violationRuntime"));
    m_pedestrianRuntimeLabel->setWordWrap(false);
    m_pedestrianRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    runtimeLayout->addWidget(m_pedestrianRuntimeLabel);
    rightLayout->addWidget(runtimeGroup);

    QGroupBox *resultGroup = new QGroupBox(QStringLiteral("模型与违法结果"), rightPanel);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(9, 18, 9, 8);
    m_pedestrianResultLabel = new QLabel(QStringLiteral("等待行人检测结果。"), resultGroup);
    m_pedestrianResultLabel->setObjectName(QStringLiteral("violationResult"));
    m_pedestrianResultLabel->setWordWrap(true);
    m_pedestrianResultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_pedestrianResultLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    resultLayout->addWidget(m_pedestrianResultLabel, 1);
    rightLayout->addWidget(resultGroup, 1);
    pageLayout->addWidget(rightPanel);

    connect(editLight, &QPushButton::clicked,
            m_pedestrianPreview, &ViolationPreviewWidget::beginTrafficLightEdit);
    connect(editCrosswalk, &QPushButton::clicked,
            m_pedestrianPreview, &ViolationPreviewWidget::beginCrosswalkEdit);
    connect(undo, &QPushButton::clicked,
            m_pedestrianPreview, &ViolationPreviewWidget::undoLastPoint);
    connect(clear, &QPushButton::clicked,
            m_pedestrianPreview, &ViolationPreviewWidget::clearRegions);
    connect(save, &QPushButton::clicked, this, [this]() {
        (void)savePedestrianViolationSettings();
    });
    connect(m_pedestrianPreview, &ViolationPreviewWidget::configEdited,
            this, [this](ViolationRuleConfig config) {
        m_pedestrianViolationConfig = sanitizeViolationRuleConfig(config);
        m_pedestrianViolationConfigDirty = true;
        if (m_display) m_display->setViolationConfig(m_pedestrianViolationConfig);
        updatePedestrianButtons();
    });
    connect(m_pedestrianPreview, &ViolationPreviewWidget::statusMessage,
            this, [this](const QString& message) {
        if (m_pedestrianEditStatusLabel)
            m_pedestrianEditStatusLabel->setText(message);
    });
    connect(m_btnPedestrianStart, &QPushButton::clicked,
            this, &ControlPanelWindow::startPedestrianDetection);
    connect(m_btnPedestrianStop, &QPushButton::clicked,
            this, &ControlPanelWindow::stopPedestrianDetection);
    connect(m_btnPedestrianViolationRule, &QPushButton::toggled,
            this, [this](bool enabled) {
        const bool overview = m_display &&
                m_display->videoRoute() == VideoRoute::Overview;
        if (!m_display || !m_pedestrianRunning || !overview ||
            (enabled && !m_pedestrianViolationConfig.regionsValid())) {
            const QSignalBlocker blocker(m_btnPedestrianViolationRule);
            m_btnPedestrianViolationRule->setChecked(false);
            m_pedestrianViolationRuleEnabled = false;
            if (enabled && m_pedestrianEditStatusLabel) {
                m_pedestrianEditStatusLabel->setText(
                            QStringLiteral("请先在总览1080P下开启行人模型，并完成两个区域配置。"));
            }
            updatePedestrianButtons();
            return;
        }
        if (enabled && m_pedestrianViolationConfigDirty &&
            !savePedestrianViolationSettings()) {
            const QSignalBlocker blocker(m_btnPedestrianViolationRule);
            m_btnPedestrianViolationRule->setChecked(false);
            m_pedestrianViolationRuleEnabled = false;
            updatePedestrianButtons();
            return;
        }

        m_display->setViolationConfig(m_pedestrianViolationConfig);
        NpuTaskConfig config = m_display->npuConfig();
        config.enabled = true;
        config.task = NpuTaskType::Pedestrian;
        config.pedestrianViolationRuleEnabled = enabled;
        config.wrongWayRuleEnabled = false;
        m_display->setNpuConfig(sanitizeNpuTaskConfig(config));
        m_aiRequestedConfigRevision = m_display->npuConfig().revision;
        m_pedestrianViolationRuleEnabled = enabled;
        m_pedestrianViolationLastEventId = 0;
        m_lastAction = enabled
                ? QStringLiteral("开启CPU行人违法判定")
                : QStringLiteral("关闭CPU行人违法判定，保留行人模型");
        updatePedestrianButtons();
        updateStatusText();
    });
    connect(m_pedestrianLightModeGroup,
            static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked),
            this, [this](int id) {
        m_pedestrianViolationConfig.lightMode =
                static_cast<ViolationLightMode>(id);
        ++m_pedestrianViolationConfig.revision;
        m_pedestrianViolationConfig = sanitizeViolationRuleConfig(
                    m_pedestrianViolationConfig);
        m_pedestrianViolationConfigDirty = true;
        if (m_pedestrianPreview)
            m_pedestrianPreview->setConfig(m_pedestrianViolationConfig);
        if (m_display)
            m_display->setViolationConfig(m_pedestrianViolationConfig);
        updatePedestrianButtons();
    });

    updatePedestrianButtons();
    return page;
}

void ControlPanelWindow::loadPedestrianViolationSettings()
{
    QString error;
    ViolationRuleConfig config;
    if (!loadViolationRuleConfig(violationSettingsPath(), &config, &error)) {
        m_lastError = error;
        config = ViolationRuleConfig();
    }
    m_pedestrianViolationConfig = sanitizeViolationRuleConfig(config);
    m_pedestrianViolationConfigDirty = false;
    if (m_pedestrianPreview)
        m_pedestrianPreview->setConfig(m_pedestrianViolationConfig);
    if (m_display)
        m_display->setViolationConfig(m_pedestrianViolationConfig);
    if (m_pedestrianEditStatusLabel) {
        m_pedestrianEditStatusLabel->setText(
                    m_pedestrianViolationConfig.regionsValid()
                    ? QStringLiteral("已加载总览1080P交通灯和斑马线配置。")
                    : QStringLiteral("请配置交通灯矩形和斑马线四点区域。"));
    }
    updatePedestrianButtons();
}

bool ControlPanelWindow::savePedestrianViolationSettings()
{
    ++m_pedestrianViolationConfig.revision;
    m_pedestrianViolationConfig = sanitizeViolationRuleConfig(
                m_pedestrianViolationConfig);
    QString error;
    if (!saveViolationRuleConfig(violationSettingsPath(),
                                 m_pedestrianViolationConfig, &error)) {
        m_lastError = error;
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return false;
    }
    m_pedestrianViolationConfigDirty = false;
    if (m_pedestrianPreview)
        m_pedestrianPreview->setConfig(m_pedestrianViolationConfig);
    if (m_display)
        m_display->setViolationConfig(m_pedestrianViolationConfig);
    if (m_pedestrianEditStatusLabel)
        m_pedestrianEditStatusLabel->setText(
                    QStringLiteral("总览1080P行人违法配置已保存。"));
    m_lastAction = QStringLiteral("保存总览行人违法配置");
    m_lastError = QStringLiteral("none");
    updatePedestrianButtons();
    updateStatusText();
    return true;
}

void ControlPanelWindow::startPedestrianDetection()
{
    if (!m_display || !m_display->isRunning()) {
        m_lastError = QStringLiteral("请先在“视频显示”页面启动PCIe视频");
    } else if (m_display->videoRoute() != VideoRoute::Overview) {
        m_lastError = QStringLiteral("行人检测与违法规则统一使用总览1080P输入");
    } else {
        m_display->setViolationConfig(m_pedestrianViolationConfig);
        NpuTaskConfig config = m_display->npuConfig();
        config.enabled = true;
        config.task = NpuTaskType::Pedestrian;
        config.pedestrianViolationRuleEnabled = false;
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
        m_pedestrianViolationRuleEnabled = false;
        m_wrongWayRunning = false;
        m_wrongWayRuleEnabled = false;
        m_aiRequestedTask = NpuTaskType::Pedestrian;
        m_aiTaskSwitchPending = true;
        m_pedestrianViolationLastEventId = 0;
        m_lastAction = QStringLiteral("启动总览1080P行人模型");
        m_lastError = QStringLiteral("none");
        if (m_pedestrianRuntimeLabel)
            m_pedestrianRuntimeLabel->setText(
                        QStringLiteral("正在加载行人模型……\n输入：总览1080P\n违法规则：关闭"));
        updatePedestrianButtons();
        updateWrongWayButtons();
        updateAiTaskButtons();
        updateStatusText();
        return;
    }
    if (m_pedestrianRuntimeLabel)
        m_pedestrianRuntimeLabel->setText(QStringLiteral("启动失败：%1").arg(m_lastError));
    updatePedestrianButtons();
    updateStatusText();
}

void ControlPanelWindow::stopPedestrianDetection()
{
    if (!m_display) return;
    NpuTaskConfig config = m_display->npuConfig();
    if (config.task == NpuTaskType::Pedestrian ||
        config.task == NpuTaskType::TrafficViolation) {
        config.enabled = false;
        config.task = NpuTaskType::Pedestrian;
        config.pedestrianViolationRuleEnabled = false;
        m_display->setNpuConfig(sanitizeNpuTaskConfig(config));
    }
    m_pedestrianRunning = false;
    m_pedestrianViolationRuleEnabled = false;
    m_aiTaskSwitchPending = false;
    m_lastAction = QStringLiteral("关闭行人模型与违法规则");
    if (m_pedestrianRuntimeLabel)
        m_pedestrianRuntimeLabel->setText(QStringLiteral("行人模型已关闭"));
    updatePedestrianButtons();
    updateAiTaskButtons();
    updateStatusText();
}

void ControlPanelWindow::updatePedestrianButtons()
{
    const bool switching = m_aiTaskSwitchPending ||
            (m_display && m_display->latestNpuResult().modelSwitching);
    const bool overview = m_display &&
            m_display->videoRoute() == VideoRoute::Overview;
    if (m_btnPedestrianStart) {
        m_btnPedestrianStart->setEnabled(!m_pedestrianRunning && overview && !switching);
        m_btnPedestrianStart->setProperty("violationActive", m_pedestrianRunning);
        m_btnPedestrianStart->style()->unpolish(m_btnPedestrianStart);
        m_btnPedestrianStart->style()->polish(m_btnPedestrianStart);
    }
    if (m_btnPedestrianStop)
        m_btnPedestrianStop->setEnabled(m_pedestrianRunning && !switching);
    if (m_btnPedestrianViolationRule) {
        const QSignalBlocker blocker(m_btnPedestrianViolationRule);
        m_btnPedestrianViolationRule->setChecked(
                    m_pedestrianViolationRuleEnabled);
        m_btnPedestrianViolationRule->setText(
                    m_pedestrianViolationRuleEnabled
                    ? QStringLiteral("违法判定：开启")
                    : QStringLiteral("违法判定：关闭"));
        m_btnPedestrianViolationRule->setEnabled(
                    m_pedestrianRunning && overview && !switching &&
                    m_pedestrianViolationConfig.regionsValid());
    }
    if (m_pedestrianLightModeGroup) {
        if (QAbstractButton *button = m_pedestrianLightModeGroup->button(
                    static_cast<int>(m_pedestrianViolationConfig.lightMode))) {
            const QSignalBlocker blocker(m_pedestrianLightModeGroup);
            button->setChecked(true);
        }
    }
}

void ControlPanelWindow::updatePedestrianPanel(const NpuFrameResult &result)
{
    if (m_pedestrianPreview) m_pedestrianPreview->setResult(result);
    const bool active = result.enabled &&
            (result.task == NpuTaskType::Pedestrian ||
             result.task == NpuTaskType::TrafficViolation);
    m_pedestrianRunning = active;
    m_pedestrianViolationRuleEnabled = active &&
            result.pedestrianViolationRuleEnabled;
    updatePedestrianButtons();

    if (!active) {
        if (m_pedestrianRuntimeLabel)
            m_pedestrianRuntimeLabel->setText(QStringLiteral("行人模型未启动"));
        return;
    }
    if (m_pedestrianRuntimeLabel) {
        m_pedestrianRuntimeLabel->setText(
                    QStringLiteral("模型：%1｜规则：%2｜输入：总览1080P\n"
                                   "FPS：%3/%4｜单帧：%5 ms\n"
                                   "RKNN：%6 ms｜模型后处理：%7 ms")
                    .arg(result.ready ? QStringLiteral("运行中")
                                      : QStringLiteral("准备中"))
                    .arg(result.pedestrianViolationRuleEnabled
                         ? QStringLiteral("开启") : QStringLiteral("关闭"))
                    .arg(result.fps, 0, 'f', 1)
                    .arg(result.targetFps)
                    .arg(result.lastMs, 0, 'f', 1)
                    .arg(result.rknnRunMs, 0, 'f', 1)
                    .arg(result.rknnPostMs, 0, 'f', 1));
    }
    if (!m_pedestrianResultLabel) return;
    if (!result.error.empty()) {
        m_pedestrianResultLabel->setText(QStringLiteral("行人模型异常：%1")
                .arg(QString::fromStdString(result.error)));
        return;
    }
    if (!result.ready) {
        m_pedestrianResultLabel->setText(QStringLiteral("等待行人模型输出……"));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("原始行人：%1个")
             .arg(static_cast<int>(result.pedestrians.size()));
    if (!result.pedestrianViolationRuleEnabled) {
        float maxScore = 0.0f;
        for (const NpuBox& box : result.pedestrians)
            maxScore = qMax(maxScore, box.score);
        lines << QStringLiteral("模型验证模式｜最高置信度：%1")
                 .arg(maxScore, 0, 'f', 2);
        lines << QStringLiteral("违法规则未运行。" );
        m_pedestrianResultLabel->setText(lines.join(QLatin1Char('\n')));
        return;
    }

    lines << QStringLiteral("规则候选：%1｜有效轨迹：%2")
             .arg(static_cast<int>(result.violationPedestrians.size()))
             .arg(result.activeTrackCount);
    lines << QStringLiteral("信号灯：%1｜当前违法：%2｜累计事件：%3")
             .arg(violationLightText(result.trafficLight))
             .arg(result.activeViolationCount)
             .arg(static_cast<qulonglong>(result.violationEventCount));
    if (result.hasRecentViolation) {
        const NpuViolationEvent& event = result.recentViolation;
        const QDateTime time = QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(event.wallClockMs));
        lines << QStringLiteral("最近：行人#%1｜%2 ms｜%3")
                 .arg(event.trackId)
                 .arg(event.accumulatedMs, 0, 'f', 0)
                 .arg(time.toString(QStringLiteral("HH:mm:ss")));
        if (event.eventId != m_pedestrianViolationLastEventId) {
            m_pedestrianViolationLastEventId = event.eventId;
            m_lastAction = QStringLiteral("检测到行人 #%1 违法").arg(event.trackId);
        }
    } else {
        lines << QStringLiteral("当前未发现行人违法行为。" );
    }
    m_pedestrianResultLabel->setText(lines.join(QLatin1Char('\n')));
}
