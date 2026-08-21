#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

namespace {

QString wrongWaySourceDisplayName(int sourceChannel)
{
    return sourceChannel == 0
            ? QStringLiteral("总览")
            : QStringLiteral("CH%1").arg(sourceChannel);
}

} // namespace

QWidget *ControlPanelWindow::createWrongWayPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(10);

    QWidget *leftPanel = new QWidget(page);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(7);

    QGroupBox *previewGroup = new QGroupBox(QStringLiteral("车道预览与方向配置"), leftPanel);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(9, 18, 9, 8);
    previewLayout->setSpacing(6);

    m_wrongWayPreview = new WrongWayPreviewWidget(previewGroup);
    previewLayout->addWidget(m_wrongWayPreview, 1);

    QWidget *buttons = new QWidget(previewGroup);
    QGridLayout *grid = new QGridLayout(buttons);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(5);
    QPushButton *addLane = new QPushButton(QStringLiteral("新增车道"), buttons);
    QPushButton *editLane = new QPushButton(QStringLiteral("重画车道"), buttons);
    QPushButton *editDirection = new QPushButton(QStringLiteral("设置方向"), buttons);
    QPushButton *undo = new QPushButton(QStringLiteral("撤销"), buttons);
    QPushButton *deleteLane = new QPushButton(QStringLiteral("删除车道"), buttons);
    QPushButton *clear = new QPushButton(QStringLiteral("清除全部"), buttons);
    QPushButton *save = new QPushButton(QStringLiteral("保存当前输入配置"), buttons);
    for (QPushButton *button : {addLane, editLane, editDirection, undo,
                                deleteLane, clear, save}) {
        button->setObjectName(QStringLiteral("violationCompactButton"));
        button->setMinimumHeight(40);
    }
    m_wrongWayLaneCombo = new QComboBox(buttons);
    m_wrongWayLaneCombo->setMinimumHeight(40);
    m_wrongWayLaneCombo->addItem(QStringLiteral("暂无车道"), -1);
    grid->addWidget(new QLabel(QStringLiteral("当前车道"), buttons), 0, 0);
    grid->addWidget(m_wrongWayLaneCombo, 0, 1, 1, 3);
    grid->addWidget(addLane, 1, 0);
    grid->addWidget(editLane, 1, 1);
    grid->addWidget(editDirection, 1, 2);
    grid->addWidget(save, 1, 3);
    grid->addWidget(undo, 2, 0);
    grid->addWidget(deleteLane, 2, 1);
    grid->addWidget(clear, 2, 2, 1, 2);
    previewLayout->addWidget(buttons);

    m_wrongWayEditStatusLabel = new QLabel(
                QStringLiteral("可在总览或CH1～CH9输入上配置车道及正常方向。"),
                previewGroup);
    m_wrongWayEditStatusLabel->setObjectName(QStringLiteral("violationEditStatus"));
    m_wrongWayEditStatusLabel->setWordWrap(true);
    m_wrongWayEditStatusLabel->setMinimumHeight(36);
    previewLayout->addWidget(m_wrongWayEditStatusLabel);
    leftLayout->addWidget(previewGroup, 1);

    QWidget *rightPanel = new QWidget(page);
    rightPanel->setMinimumWidth(350);
    rightPanel->setMaximumWidth(390);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(7);

    QGroupBox *controlGroup = new QGroupBox(QStringLiteral("运行控制"), rightPanel);
    QGridLayout *controlGrid = new QGridLayout(controlGroup);
    controlGrid->setContentsMargins(10, 18, 10, 9);
    controlGrid->setHorizontalSpacing(6);
    controlGrid->setVerticalSpacing(6);
    m_btnWrongWayStart = new QPushButton(QStringLiteral("开启车辆模型"), controlGroup);
    m_btnWrongWayStop = new QPushButton(QStringLiteral("关闭车辆模型"), controlGroup);
    m_btnWrongWayRule = new QPushButton(QStringLiteral("逆行判定：关闭"), controlGroup);
    m_btnWrongWayStart->setObjectName(QStringLiteral("violationRunButton"));
    m_btnWrongWayStop->setObjectName(QStringLiteral("violationRunButton"));
    m_btnWrongWayRule->setObjectName(QStringLiteral("violationLightButton"));
    m_btnWrongWayRule->setCheckable(true);
    m_btnWrongWayStart->setMinimumHeight(48);
    m_btnWrongWayStop->setMinimumHeight(48);
    m_btnWrongWayRule->setMinimumHeight(44);

    QLabel *presetLabel = new QLabel(QStringLiteral("状态稳定策略"), controlGroup);
    QWidget *presetWidget = new QWidget(controlGroup);
    QHBoxLayout *presetLayout = new QHBoxLayout(presetWidget);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(5);
    m_wrongWayPresetGroup = new QButtonGroup(this);
    m_wrongWayPresetGroup->setExclusive(true);
    const QStringList presetNames = {QStringLiteral("灵敏"), QStringLiteral("均衡"), QStringLiteral("稳健")};
    for (int i = 0; i < presetNames.size(); ++i) {
        QPushButton *button = new QPushButton(presetNames[i], presetWidget);
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("violationLightButton"));
        button->setMinimumHeight(40);
        m_wrongWayPresetGroup->addButton(button, i);
        presetLayout->addWidget(button, 1);
        if (i == 1) button->setChecked(true);
    }

    QLabel *hint = new QLabel(
                QStringLiteral("规则：一次车辆模型推理后，由CPU按车道轨迹、方向余弦及连续时间判定；总览按完整一路1080P连续跟踪。"),
                controlGroup);
    hint->setObjectName(QStringLiteral("navHint"));
    hint->setWordWrap(true);
    controlGrid->addWidget(m_btnWrongWayStart, 0, 0);
    controlGrid->addWidget(m_btnWrongWayStop, 0, 1);
    controlGrid->addWidget(m_btnWrongWayRule, 1, 0, 1, 2);
    controlGrid->addWidget(presetLabel, 2, 0, 1, 2);
    controlGrid->addWidget(presetWidget, 3, 0, 1, 2);
    controlGrid->addWidget(hint, 4, 0, 1, 2);
    rightLayout->addWidget(controlGroup, 0);

    QGroupBox *runtimeGroup = new QGroupBox(QStringLiteral("运行状态"), rightPanel);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(9, 18, 9, 8);
    m_wrongWayRuntimeLabel = new QLabel(QStringLiteral("逆行检测未启动"), runtimeGroup);
    m_wrongWayRuntimeLabel->setObjectName(QStringLiteral("violationRuntime"));
    m_wrongWayRuntimeLabel->setWordWrap(true);
    m_wrongWayRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    runtimeLayout->addWidget(m_wrongWayRuntimeLabel);
    rightLayout->addWidget(runtimeGroup, 0);

    QGroupBox *resultGroup = new QGroupBox(QStringLiteral("当前结果"), rightPanel);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(9, 18, 9, 8);
    m_wrongWayResultLabel = new QLabel(QStringLiteral("当前未发现车辆逆行。"), resultGroup);
    m_wrongWayResultLabel->setObjectName(QStringLiteral("violationResult"));
    m_wrongWayResultLabel->setWordWrap(true);
    m_wrongWayResultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_wrongWayResultLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    resultLayout->addWidget(m_wrongWayResultLabel, 1);
    rightLayout->addWidget(resultGroup, 1);

    pageLayout->addWidget(leftPanel, 1);
    pageLayout->addWidget(rightPanel, 0);

    connect(addLane, &QPushButton::clicked, m_wrongWayPreview, &WrongWayPreviewWidget::addLane);
    connect(editLane, &QPushButton::clicked, m_wrongWayPreview, &WrongWayPreviewWidget::beginLaneQuadEdit);
    connect(editDirection, &QPushButton::clicked, m_wrongWayPreview, &WrongWayPreviewWidget::beginDirectionEdit);
    connect(undo, &QPushButton::clicked, m_wrongWayPreview, &WrongWayPreviewWidget::undoLastPoint);
    connect(deleteLane, &QPushButton::clicked, m_wrongWayPreview, &WrongWayPreviewWidget::deleteSelectedLane);
    connect(clear, &QPushButton::clicked, m_wrongWayPreview, &WrongWayPreviewWidget::clearAllLanes);
    connect(save, &QPushButton::clicked, this, [this]() { (void)saveWrongWaySettings(); });
    connect(m_wrongWayLaneCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (!m_wrongWayPreview || !m_wrongWayLaneCombo || index < 0) return;
        m_wrongWayPreview->setSelectedLane(
                    m_wrongWayLaneCombo->itemData(index).toInt());
    });
    connect(m_wrongWayPreview, &WrongWayPreviewWidget::selectedLaneChanged,
            this, [this](int laneIndex) {
        if (!m_wrongWayLaneCombo) return;
        const QSignalBlocker blocker(m_wrongWayLaneCombo);
        const int index = m_wrongWayLaneCombo->findData(laneIndex);
        if (index >= 0) m_wrongWayLaneCombo->setCurrentIndex(index);
    });
    connect(m_wrongWayPreview, &WrongWayPreviewWidget::configEdited,
            this, [this](VehicleDirectionConfig config) {
        m_wrongWayConfig = sanitizeVehicleDirectionConfig(config);
        m_wrongWayConfigDirty = true;
        if (m_display) m_display->setWrongWayConfig(m_wrongWayConfig);
        updateWrongWayLaneSelector();
        updateWrongWayButtons();
    });
    connect(m_wrongWayPreview, &WrongWayPreviewWidget::statusMessage,
            this, [this](const QString& message) {
        if (m_wrongWayEditStatusLabel) m_wrongWayEditStatusLabel->setText(message);
    });
    connect(m_btnWrongWayStart, &QPushButton::clicked,
            this, &ControlPanelWindow::startWrongWayDetection);
    connect(m_btnWrongWayStop, &QPushButton::clicked,
            this, &ControlPanelWindow::stopWrongWayDetection);
    connect(m_btnWrongWayRule, &QPushButton::toggled,
            this, [this](bool enabled) {
        if (!m_display || !m_wrongWayRunning ||
            (enabled && !m_wrongWayConfig.regionsValid())) {
            const QSignalBlocker blocker(m_btnWrongWayRule);
            m_btnWrongWayRule->setChecked(false);
            m_wrongWayRuleEnabled = false;
            if (enabled && m_wrongWayEditStatusLabel) {
                m_wrongWayEditStatusLabel->setText(
                            QStringLiteral("请先开启车辆模型并完成车道和方向配置。"));
            }
            updateWrongWayButtons();
            return;
        }
        if (enabled && m_wrongWayConfigDirty && !saveWrongWaySettings()) {
            const QSignalBlocker blocker(m_btnWrongWayRule);
            m_btnWrongWayRule->setChecked(false);
            m_wrongWayRuleEnabled = false;
            updateWrongWayButtons();
            return;
        }
        m_display->setWrongWayConfig(m_wrongWayConfig);
        NpuTaskConfig config = m_display->npuConfig();
        config.enabled = true;
        config.task = NpuTaskType::VehicleWrongWay;
        config.wrongWayRuleEnabled = enabled;
        m_display->setNpuConfig(sanitizeNpuTaskConfig(config));
        m_wrongWayRuleEnabled = enabled;
        m_lastAction = enabled
                ? QStringLiteral("开启CPU逆行判定规则")
                : QStringLiteral("关闭CPU逆行判定规则，保留车辆模型");
        updateWrongWayButtons();
        updateStatusText();
    });
    connect(m_wrongWayPresetGroup,
            static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked),
            this, [this](int id) {
        if (id == 0) {
            m_wrongWayConfig.directionWindowMs = 450;
            m_wrongWayConfig.wrongWayHoldMs = 250;
            m_wrongWayConfig.wrongWayClearMs = 650;
            m_wrongWayConfig.motionThreshold = 0.015f;
            m_wrongWayConfig.directionCosine = 0.45f;
        } else if (id == 2) {
            m_wrongWayConfig.directionWindowMs = 800;
            m_wrongWayConfig.wrongWayHoldMs = 500;
            m_wrongWayConfig.wrongWayClearMs = 1200;
            m_wrongWayConfig.motionThreshold = 0.030f;
            m_wrongWayConfig.directionCosine = 0.65f;
        } else {
            m_wrongWayConfig.directionWindowMs = 600;
            m_wrongWayConfig.wrongWayHoldMs = 350;
            m_wrongWayConfig.wrongWayClearMs = 800;
            m_wrongWayConfig.motionThreshold = 0.020f;
            m_wrongWayConfig.directionCosine = 0.50f;
        }
        ++m_wrongWayConfig.revision;
        m_wrongWayConfig = sanitizeVehicleDirectionConfig(m_wrongWayConfig);
        m_wrongWayConfigDirty = true;
        if (m_wrongWayPreview) m_wrongWayPreview->setConfig(m_wrongWayConfig);
        if (m_display) m_display->setWrongWayConfig(m_wrongWayConfig);
        if (m_wrongWayEditStatusLabel)
            m_wrongWayEditStatusLabel->setText(QStringLiteral("稳定策略已更新，启动前请保存当前输入配置。"));
    });

    updateWrongWayButtons();
    return page;
}

void ControlPanelWindow::updateWrongWayLaneSelector()
{
    if (!m_wrongWayLaneCombo) return;
    const int selected = m_wrongWayPreview
            ? m_wrongWayPreview->selectedLane() : -1;
    const QSignalBlocker blocker(m_wrongWayLaneCombo);
    m_wrongWayLaneCombo->clear();
    if (m_wrongWayConfig.lanes.empty()) {
        m_wrongWayLaneCombo->addItem(QStringLiteral("暂无车道"), -1);
        return;
    }

    for (std::size_t laneIndex = 0;
         laneIndex < m_wrongWayConfig.lanes.size(); ++laneIndex) {
        QString label = QStringLiteral("车道%1").arg(laneIndex + 1);
        if (m_wrongWaySourceChannel == 0) {
            float centerX = 0.0f;
            float centerY = 0.0f;
            for (const auto& point : m_wrongWayConfig.lanes[laneIndex].quad) {
                centerX += point.x;
                centerY += point.y;
            }
            const int column = qBound(0, static_cast<int>(centerX * 0.75f), 2);
            const int row = qBound(0, static_cast<int>(centerY * 0.75f), 2);
            label = QStringLiteral("CH%1 · 车道%2")
                    .arg(row * 3 + column + 1)
                    .arg(laneIndex + 1);
        }
        m_wrongWayLaneCombo->addItem(label, static_cast<int>(laneIndex));
    }
    const int selectedIndex = m_wrongWayLaneCombo->findData(selected);
    m_wrongWayLaneCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
}

void ControlPanelWindow::loadWrongWaySettings(int sourceChannel)
{
    if (sourceChannel < 0 || sourceChannel > 9) sourceChannel = 0;
    if (sourceChannel == m_wrongWaySourceChannel) return;
    if (m_wrongWayConfigDirty && m_wrongWaySourceChannel >= 0)
        (void)saveWrongWaySettings();

    m_wrongWaySourceChannel = sourceChannel;
    QString error;
    VehicleDirectionConfig config;
    if (!loadVehicleDirectionConfig(wrongWaySettingsPath(sourceChannel), &config, &error)) {
        m_lastError = error;
        config = VehicleDirectionConfig();
    }
    m_wrongWayConfig = sanitizeVehicleDirectionConfig(config);
    m_wrongWayConfigDirty = false;
    if (m_wrongWayPreview) m_wrongWayPreview->setConfig(m_wrongWayConfig);
    if (m_display) m_display->setWrongWayConfig(m_wrongWayConfig);
    updateWrongWayLaneSelector();
    if (m_wrongWayEditStatusLabel) {
        m_wrongWayEditStatusLabel->setText(m_wrongWayConfig.regionsValid()
                ? QStringLiteral("%1已加载%2条车道配置。")
                  .arg(wrongWaySourceDisplayName(sourceChannel))
                  .arg(static_cast<int>(m_wrongWayConfig.lanes.size()))
                : QStringLiteral("%1尚未配置车道，请点击“新增车道”。")
                  .arg(wrongWaySourceDisplayName(sourceChannel)));
    }
    updateWrongWayButtons();
}

bool ControlPanelWindow::saveWrongWaySettings()
{
    if (m_wrongWaySourceChannel < 0 || m_wrongWaySourceChannel > 9) {
        m_lastError = QStringLiteral("逆行配置输入源无效");
        return false;
    }
    ++m_wrongWayConfig.revision;
    const QString settingsPath = wrongWaySettingsPath(m_wrongWaySourceChannel);
    if (m_wrongWayConfig.lanes.empty()) {
        QFile settingsFile(settingsPath);
        if (settingsFile.exists() && !settingsFile.remove()) {
            m_lastError = QStringLiteral("清除逆行配置失败：%1")
                    .arg(settingsFile.errorString());
            QMessageBox::warning(this, QStringLiteral("清除失败"), m_lastError);
            return false;
        }
        m_wrongWayConfig = sanitizeVehicleDirectionConfig(m_wrongWayConfig);
        m_wrongWayConfigDirty = false;
        if (m_display) m_display->setWrongWayConfig(m_wrongWayConfig);
        if (m_wrongWayEditStatusLabel) {
            m_wrongWayEditStatusLabel->setText(QStringLiteral("%1逆行配置已清除。")
                    .arg(wrongWaySourceDisplayName(m_wrongWaySourceChannel)));
        }
        m_lastAction = QStringLiteral("清除%1车辆逆行配置")
                .arg(wrongWaySourceDisplayName(m_wrongWaySourceChannel));
        m_lastError = QStringLiteral("none");
        updateWrongWayButtons();
        updateStatusText();
        return true;
    }
    QString error;
    if (!saveVehicleDirectionConfig(settingsPath,
                                    m_wrongWayConfig, &error)) {
        m_lastError = error;
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return false;
    }
    m_wrongWayConfig = sanitizeVehicleDirectionConfig(m_wrongWayConfig);
    m_wrongWayConfigDirty = false;
    if (m_display) m_display->setWrongWayConfig(m_wrongWayConfig);
    if (m_wrongWayEditStatusLabel)
        m_wrongWayEditStatusLabel->setText(QStringLiteral("%1逆行配置已保存。")
                .arg(wrongWaySourceDisplayName(m_wrongWaySourceChannel)));
    m_lastAction = QStringLiteral("保存%1车辆逆行配置")
            .arg(wrongWaySourceDisplayName(m_wrongWaySourceChannel));
    m_lastError = QStringLiteral("none");
    updateWrongWayButtons();
    updateStatusText();
    return true;
}

void ControlPanelWindow::startWrongWayDetection()
{
    if (m_display) {
        const int activeChannel = videoRouteChannel(m_display->videoRoute());
        if (activeChannel != m_wrongWaySourceChannel) {
            loadWrongWaySettings(activeChannel);
        }
    }
    if (!m_display || !m_display->isRunning()) {
        m_lastError = QStringLiteral("请先启动PCIe视频");
    } else {
        m_display->setWrongWayConfig(m_wrongWayConfig);
        NpuTaskConfig config = m_display->npuConfig();
        config.enabled = true;
        config.task = NpuTaskType::VehicleWrongWay;
        config.wrongWayRuleEnabled = false;
        config.targetFps = m_npuFpsSpin ? m_npuFpsSpin->value() : NPU_TARGET_FPS_DEFAULT;
        config.confidence = 0.25f;
        config.nmsThreshold = 0.45f;
        m_display->setNpuConfig(sanitizeNpuTaskConfig(config));
        m_aiRequestedConfigRevision = m_display->npuConfig().revision;
        m_wrongWayRunning = true;
        m_wrongWayRuleEnabled = false;
        m_pedestrianRunning = false;
        m_pedestrianViolationRuleEnabled = false;
        m_aiRequestedTask = NpuTaskType::VehicleWrongWay;
        m_aiTaskSwitchPending = true;
        m_wrongWayLastEventId = 0;
        m_lastAction = QStringLiteral("启动%1车辆检测模型")
                .arg(wrongWaySourceDisplayName(m_wrongWaySourceChannel));
        m_lastError = QStringLiteral("none");
        if (m_wrongWayRuntimeLabel)
            m_wrongWayRuntimeLabel->setText(QStringLiteral("正在加载车辆模型并等待检测结果……"));
        updateWrongWayButtons();
        updatePedestrianButtons();
        updateStatusText();
        return;
    }
    if (m_wrongWayRuntimeLabel)
        m_wrongWayRuntimeLabel->setText(QStringLiteral("启动失败：%1").arg(m_lastError));
    updateWrongWayButtons();
    updateStatusText();
}

void ControlPanelWindow::stopWrongWayDetection()
{
    if (!m_display) return;
    if (m_display->npuConfig().task == NpuTaskType::VehicleWrongWay) {
        NpuTaskConfig config = m_display->npuConfig();
        config.enabled = false;
        config.wrongWayRuleEnabled = false;
        m_display->setNpuConfig(config);
    }
    m_wrongWayRunning = false;
    m_wrongWayRuleEnabled = false;
    m_lastAction = QStringLiteral("关闭车辆检测模型");
    if (m_wrongWayRuntimeLabel)
        m_wrongWayRuntimeLabel->setText(QStringLiteral("车辆模型已关闭，当前输入配置仍保留。"));
    updateWrongWayButtons();
    updateAiTaskButtons();
    updateStatusText();
}

void ControlPanelWindow::updateWrongWayButtons()
{
    const bool routeReady = m_display != nullptr;
    const bool switching = m_aiTaskSwitchPending ||
            (m_display && m_display->latestNpuResult().modelSwitching);
    if (m_btnWrongWayStart) {
        m_btnWrongWayStart->setEnabled(!m_wrongWayRunning && routeReady && !switching);
        if (m_btnWrongWayStart->property("violationActive").toBool() !=
                m_wrongWayRunning) {
            m_btnWrongWayStart->setProperty("violationActive", m_wrongWayRunning);
            m_btnWrongWayStart->style()->unpolish(m_btnWrongWayStart);
            m_btnWrongWayStart->style()->polish(m_btnWrongWayStart);
        }
    }
    if (m_btnWrongWayStop)
        m_btnWrongWayStop->setEnabled(m_wrongWayRunning && !switching);
    if (m_btnWrongWayRule) {
        const QSignalBlocker blocker(m_btnWrongWayRule);
        m_btnWrongWayRule->setChecked(m_wrongWayRuleEnabled);
        m_btnWrongWayRule->setText(m_wrongWayRuleEnabled
                ? QStringLiteral("逆行判定：开启")
                : QStringLiteral("逆行判定：关闭"));
        m_btnWrongWayRule->setEnabled(m_wrongWayRunning && !switching &&
                                      m_wrongWayConfig.regionsValid());
    }
}

void ControlPanelWindow::updateWrongWayPanel(const NpuFrameResult& result)
{
    if (m_wrongWayPreview) m_wrongWayPreview->setResult(result);
    if (result.task != NpuTaskType::VehicleWrongWay || !result.enabled) {
        if (m_wrongWayRunning && result.task != NpuTaskType::VehicleWrongWay) {
            m_wrongWayRunning = false;
            m_wrongWayRuleEnabled = false;
            updateWrongWayButtons();
        }
        if (!m_wrongWayRunning && m_wrongWayRuntimeLabel)
            m_wrongWayRuntimeLabel->setText(QStringLiteral("车辆模型未启动｜%1｜车道：%2")
                                            .arg(wrongWaySourceDisplayName(
                                                     m_wrongWaySourceChannel))
                                            .arg(static_cast<int>(m_wrongWayConfig.lanes.size())));
        return;
    }
    m_wrongWayRunning = true;
    m_wrongWayRuleEnabled = result.wrongWayRuleEnabled;
    updateWrongWayButtons();
    if (m_wrongWayRuntimeLabel) {
        m_wrongWayRuntimeLabel->setText(
                    QStringLiteral("模型：%1｜逆行规则：%2｜输入：%3\n"
                                   "车道：%4｜车辆：%5｜有效轨迹：%6｜逆行：%7\n"
                                   "推理FPS：%8｜单帧耗时：%9 ms")
                    .arg(result.ready ? QStringLiteral("运行中") : QStringLiteral("准备中"))
                    .arg(result.wrongWayRuleEnabled
                         ? QStringLiteral("开启") : QStringLiteral("关闭"))
                    .arg(wrongWaySourceDisplayName(result.sourceChannel))
                    .arg(static_cast<int>(result.wrongWayConfig.lanes.size()))
                    .arg(static_cast<int>(result.vehicles.size()))
                    .arg(result.activeVehicleTrackCount)
                    .arg(result.activeWrongWayCount)
                    .arg(result.fps, 0, 'f', 1)
                    .arg(result.lastMs, 0, 'f', 1));
    }
    if (!m_wrongWayResultLabel) return;
    if (!result.error.empty()) {
        m_wrongWayResultLabel->setText(QStringLiteral("逆行检测异常：%1")
                                       .arg(QString::fromStdString(result.error)));
        return;
    }
    if (!result.ready) {
        m_wrongWayResultLabel->setText(QStringLiteral("等待车辆模型输出……"));
        return;
    }
    QStringList lines;
    lines << QStringLiteral("检测到车辆：%1个")
             .arg(static_cast<int>(result.vehicles.size()));
    if (!result.wrongWayRuleEnabled) {
        lines << QStringLiteral("车辆模型验证模式：未运行轨迹和逆行规则。");
        m_wrongWayResultLabel->setText(lines.join(QLatin1Char('\n')));
        return;
    }
    lines << QStringLiteral("当前逆行车辆：%1").arg(result.activeWrongWayCount);
    lines << QStringLiteral("累计逆行事件：%1").arg(static_cast<qulonglong>(result.wrongWayEventCount));
    if (result.hasRecentWrongWay) {
        const NpuWrongWayEvent& event = result.recentWrongWay;
        const QDateTime time = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(event.wallClockMs));
        lines << QStringLiteral("最近事件：车辆 #%1｜车道%2").arg(event.trackId).arg(event.laneId + 1);
        lines << QStringLiteral("方向余弦：%1｜运动量：%2｜时间：%3")
                 .arg(event.directionCosine, 0, 'f', 2)
                 .arg(event.normalizedMotion, 0, 'f', 3)
                 .arg(time.toString(QStringLiteral("HH:mm:ss")));
        if (event.eventId != m_wrongWayLastEventId) {
            m_wrongWayLastEventId = event.eventId;
            m_lastAction = QStringLiteral("检测到车辆 #%1 逆行").arg(event.trackId);
        }
    } else {
        lines << QStringLiteral("当前未发现车辆逆行。" );
    }
    m_wrongWayResultLabel->setText(lines.join(QLatin1Char('\n')));
}
