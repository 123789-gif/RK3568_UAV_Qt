#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

namespace {

QString plateProfileDisplayName(NpuPlateModelProfile profile)
{
    return profile == NpuPlateModelProfile::Legacy
            ? QStringLiteral("旧版基线") : QStringLiteral("v2.9.1候选");
}

QString platePostprocessDisplayName(NpuPlatePostprocess mode)
{
    return mode == NpuPlatePostprocess::Raw
            ? QStringLiteral("逐帧结果") : QStringLiteral("稳定显示");
}

enum PlateUiMode
{
    PlateUiStandard = 0,
    PlateUiStandardEnhanced = 1,
    PlateUiRule = 2,
    PlateUiRuleEnhanced = 3
};

PlateUiMode plateUiModeFromConfig(NpuPlateModelProfile profile,
                                  NpuPlateCropSource cropSource)
{
    if (profile == NpuPlateModelProfile::Legacy) {
        return cropSource == NpuPlateCropSource::OriginalSource
                ? PlateUiStandardEnhanced : PlateUiStandard;
    }
    return cropSource == NpuPlateCropSource::OriginalSource
            ? PlateUiRuleEnhanced : PlateUiRule;
}

QString plateUiModeDisplayName(PlateUiMode mode)
{
    switch (mode) {
    case PlateUiStandardEnhanced:
        return QStringLiteral("标准加强");
    case PlateUiRule:
        return QStringLiteral("规则模式");
    case PlateUiRuleEnhanced:
        return QStringLiteral("规则加强");
    case PlateUiStandard:
    default:
        return QStringLiteral("标准模式");
    }
}

QString plateCropSourceDisplayName(NpuPlateCropSource source)
{
    return source == NpuPlateCropSource::OriginalSource
            ? QStringLiteral("原始选路图") : QStringLiteral("检测中间图");
}

void applyPlateUiMode(PlateUiMode mode,
                      NpuPlateModelProfile *profile,
                      NpuPlateCropSource *cropSource)
{
    if (!profile || !cropSource) return;
    switch (mode) {
    case PlateUiStandardEnhanced:
        *profile = NpuPlateModelProfile::Legacy;
        *cropSource = NpuPlateCropSource::OriginalSource;
        break;
    case PlateUiRule:
        *profile = NpuPlateModelProfile::V291Candidate;
        *cropSource = NpuPlateCropSource::DetectorInput;
        break;
    case PlateUiRuleEnhanced:
        *profile = NpuPlateModelProfile::V291Candidate;
        *cropSource = NpuPlateCropSource::OriginalSource;
        break;
    case PlateUiStandard:
    default:
        *profile = NpuPlateModelProfile::Legacy;
        *cropSource = NpuPlateCropSource::DetectorInput;
        break;
    }
}

}  // namespace

QWidget *ControlPanelWindow::createAiPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(8);

    // 主页面只呈现系统核心的车牌检测识别任务。行人/违法后端继续保留，
    // 由独立违法检测页使用，不在这里形成并列主任务。
    auto createChoiceRow = [this](QWidget *parent,
                                  QButtonGroup **groupOut,
                                  const QList<QPair<QString, int>> &choices,
                                  const QString &objectName,
                                  int minimumHeight,
                                  const std::function<void(int)> &selected) {
        QButtonGroup *group = new QButtonGroup(this);
        group->setExclusive(true);
        QWidget *widget = new QWidget(parent);
        QHBoxLayout *layout = new QHBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        for (const auto &choice : choices) {
            QPushButton *button = new QPushButton(choice.first, widget);
            button->setObjectName(objectName);
            button->setCheckable(true);
            button->setMinimumHeight(minimumHeight);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            group->addButton(button, choice.second);
            layout->addWidget(button, 1);
            connect(button, &QPushButton::clicked, this,
                    [selected, choice]() { selected(choice.second); });
        }
        *groupOut = group;
        return widget;
    };

    QGroupBox *modeGroup = new QGroupBox(QStringLiteral("车牌识别模式"), page);
    QVBoxLayout *modeLayout = new QVBoxLayout(modeGroup);
    modeLayout->setContentsMargins(12, 18, 12, 10);
    modeLayout->setSpacing(7);

    QWidget *sourceWidget = new QWidget(modeGroup);
    QHBoxLayout *sourceLayout = new QHBoxLayout(sourceWidget);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(10);
    QLabel *sourceTitle = new QLabel(QStringLiteral("当前输入源"), sourceWidget);
    sourceTitle->setMinimumWidth(100);
    QLabel *sourceValue = new QLabel(
                QStringLiteral("视频显示页选定输入：总览全图或 CH1~CH9"),
                sourceWidget);
    sourceValue->setObjectName(QStringLiteral("ptzState"));
    sourceValue->setMinimumHeight(34);
    sourceValue->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sourceLayout->addWidget(sourceTitle);
    sourceLayout->addWidget(sourceValue, 1);
    modeLayout->addWidget(sourceWidget);

    QWidget *modeWidget = createChoiceRow(
                modeGroup,
                &m_plateModeButtonGroup,
                {{QStringLiteral("标准模式\nLegacy·检测图ROI"), PlateUiStandard},
                 {QStringLiteral("标准加强\nLegacy·原图ROI"), PlateUiStandardEnhanced},
                 {QStringLiteral("规则模式\nv2.9.1·检测图ROI"), PlateUiRule},
                 {QStringLiteral("规则加强\nv2.9.1·原图ROI"), PlateUiRuleEnhanced}},
                QStringLiteral("aiModeButton"),
                58,
                [this](int value) {
        if (m_aiTaskSwitchPending) return;
        applyPlateUiMode(static_cast<PlateUiMode>(value),
                         &m_aiRequestedPlateProfile,
                         &m_aiRequestedPlateCropSource);
        m_aiRequestedTask = NpuTaskType::PlateCascade;
        m_aiPlateOptionsDirty = true;
        clearAiCurrentResult();
        if (m_aiConfigSummaryLabel) {
            m_aiConfigSummaryLabel->setText(
                        QStringLiteral("待应用：%1｜%2｜%3")
                        .arg(plateUiModeDisplayName(static_cast<PlateUiMode>(value)))
                        .arg(plateCropSourceDisplayName(m_aiRequestedPlateCropSource))
                        .arg(platePostprocessDisplayName(
                                 m_aiRequestedPlatePostprocess)));
        }
        updateAiTaskButtons();
    });
    modeLayout->addWidget(modeWidget);

    m_aiConfigSummaryLabel = new QLabel(modeGroup);
    m_aiConfigSummaryLabel->setWordWrap(false);
    m_aiConfigSummaryLabel->setObjectName(QStringLiteral("ptzState"));
    m_aiConfigSummaryLabel->setMinimumHeight(34);
    m_aiConfigSummaryLabel->setSizePolicy(QSizePolicy::Expanding,
                                          QSizePolicy::Fixed);
    modeLayout->addWidget(m_aiConfigSummaryLabel);
    pageLayout->addWidget(modeGroup, 0);

    QHBoxLayout *contentLayout = new QHBoxLayout;
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    QWidget *leftPanel = new QWidget(page);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    QGroupBox *configGroup = new QGroupBox(QStringLiteral("识别参数"), leftPanel);
    QGridLayout *grid = new QGridLayout(configGroup);
    grid->setContentsMargins(12, 18, 12, 10);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(7);
    grid->setColumnMinimumWidth(0, 112);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    QWidget *postprocessWidget = createChoiceRow(
                configGroup,
                &m_platePostprocessButtonGroup,
                {{QStringLiteral("逐帧结果（测试）"), static_cast<int>(NpuPlatePostprocess::Raw)},
                 {QStringLiteral("稳定显示（默认）"), static_cast<int>(NpuPlatePostprocess::Stable)}},
                QStringLiteral("aiChoiceButton"),
                46,
                [this](int value) {
        if (m_aiTaskSwitchPending) return;
        m_aiRequestedPlatePostprocess = static_cast<NpuPlatePostprocess>(value);
        m_aiPlateOptionsDirty = true;
        clearAiCurrentResult();
        if (m_aiConfigSummaryLabel) {
            m_aiConfigSummaryLabel->setText(
                        QStringLiteral("待应用：%1｜%2｜%3")
                        .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                                 m_aiRequestedPlateProfile,
                                 m_aiRequestedPlateCropSource)))
                        .arg(plateCropSourceDisplayName(m_aiRequestedPlateCropSource))
                        .arg(platePostprocessDisplayName(
                                 m_aiRequestedPlatePostprocess)));
        }
        updateAiTaskButtons();
    });

    auto makeTouchButton = [configGroup](const QString &text) {
        QPushButton *button = new QPushButton(text, configGroup);
        button->setObjectName(QStringLiteral("touchAdjustButton"));
        button->setFixedSize(50, 46);
        button->setAutoRepeat(true);
        button->setAutoRepeatDelay(350);
        button->setAutoRepeatInterval(120);
        return button;
    };

    auto configureTouchSpin = [](QAbstractSpinBox *spin) {
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setReadOnly(true);
        spin->setAlignment(Qt::AlignCenter);
        spin->setObjectName(QStringLiteral("touchValueSpin"));
        spin->setMinimumSize(120, 46);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setFocusPolicy(Qt::NoFocus);
    };

    m_npuFpsSpin = new QSpinBox(configGroup);
    m_npuFpsSpin->setRange(NPU_TARGET_FPS_MIN, NPU_TARGET_FPS_MAX);
    m_npuFpsSpin->setSingleStep(1);
    m_npuFpsSpin->setValue(NPU_TARGET_FPS_DEFAULT);
    m_npuFpsSpin->setSuffix(QStringLiteral(" fps"));
    m_npuFpsSpin->setToolTip(
                QStringLiteral("可设置范围：%1～%2 fps；默认：%3 fps")
                .arg(NPU_TARGET_FPS_MIN)
                .arg(NPU_TARGET_FPS_MAX)
                .arg(NPU_TARGET_FPS_DEFAULT));
    configureTouchSpin(m_npuFpsSpin);

    m_confSpin = new QDoubleSpinBox(configGroup);
    m_confSpin->setRange(0.10, 0.95);
    m_confSpin->setSingleStep(0.05);
    m_confSpin->setDecimals(2);
    m_confSpin->setValue(0.50);
    configureTouchSpin(m_confSpin);

    m_nmsSpin = new QDoubleSpinBox(configGroup);
    m_nmsSpin->setRange(0.10, 0.90);
    m_nmsSpin->setSingleStep(0.05);
    m_nmsSpin->setDecimals(2);
    m_nmsSpin->setValue(0.45);
    configureTouchSpin(m_nmsSpin);

    auto addTouchAdjustRow = [&](int row,
                                 const QString &labelText,
                                 QAbstractSpinBox *spin,
                                 const std::function<void()> &decrease,
                                 const std::function<void()> &increase) {
        QLabel *label = new QLabel(labelText, configGroup);
        QPushButton *minusButton = makeTouchButton(QStringLiteral("−"));
        QPushButton *plusButton = makeTouchButton(QStringLiteral("+"));

        QWidget *adjustWidget = new QWidget(configGroup);
        QHBoxLayout *adjustLayout = new QHBoxLayout(adjustWidget);
        adjustLayout->setContentsMargins(0, 0, 0, 0);
        adjustLayout->setSpacing(5);
        adjustLayout->addWidget(minusButton);
        adjustLayout->addWidget(spin, 1);
        adjustLayout->addWidget(plusButton);

        grid->addWidget(label, row, 0);
        grid->addWidget(adjustWidget, row, 1);

        connect(minusButton, &QPushButton::clicked, this, decrease);
        connect(plusButton, &QPushButton::clicked, this, increase);
    };

    m_btnAiApply = new QPushButton(
                QStringLiteral("应用参数并推理"), configGroup);
    m_btnAiApply->setMinimumHeight(50);

    m_btnAiStop = new QPushButton(QStringLiteral("关闭推理"), configGroup);
    m_btnAiStop->setObjectName(QStringLiteral("aiStopButton"));
    m_btnAiStop->setMinimumHeight(50);

    grid->addWidget(new QLabel(QStringLiteral("结果策略"), configGroup), 0, 0);
    grid->addWidget(postprocessWidget, 0, 1);

    addTouchAdjustRow(1,
                      QStringLiteral("目标推理帧率"),
                      m_npuFpsSpin,
                      [this]() { m_npuFpsSpin->stepDown(); },
                      [this]() { m_npuFpsSpin->stepUp(); });
    addTouchAdjustRow(2,
                      QStringLiteral("检测置信度"),
                      m_confSpin,
                      [this]() { m_confSpin->stepDown(); },
                      [this]() { m_confSpin->stepUp(); });
    addTouchAdjustRow(3,
                      QStringLiteral("NMS阈值"),
                      m_nmsSpin,
                      [this]() { m_nmsSpin->stepDown(); },
                      [this]() { m_nmsSpin->stepUp(); });

    QWidget *actionWidget = new QWidget(configGroup);
    QHBoxLayout *actionLayout = new QHBoxLayout(actionWidget);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(10);
    actionLayout->addWidget(m_btnAiApply, 1);
    actionLayout->addWidget(m_btnAiStop, 1);
    grid->addWidget(actionWidget, 4, 0, 1, 2);

    QLabel *touchHint = new QLabel(
                QStringLiteral("精度对比使用“逐帧结果”；实际演示使用“稳定显示”。"),
                configGroup);
    touchHint->setObjectName(QStringLiteral("navHint"));
    touchHint->setWordWrap(true);
    grid->addWidget(touchHint, 5, 0, 1, 2);

    leftLayout->addWidget(configGroup, 1);
    contentLayout->addWidget(leftPanel, 9);

    QWidget *rightPanel = new QWidget(page);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    QGroupBox *runtimeGroup = new QGroupBox(QStringLiteral("运行状态"), rightPanel);
    runtimeGroup->setMinimumHeight(190);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(10, 18, 10, 8);
    m_aiRuntimeLabel = new QLabel(QStringLiteral("AI未启用"), runtimeGroup);
    m_aiRuntimeLabel->setObjectName(QStringLiteral("aiRuntimeStatus"));
    m_aiRuntimeLabel->setWordWrap(true);
    m_aiRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_aiRuntimeLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    runtimeLayout->addWidget(m_aiRuntimeLabel);
    rightLayout->addWidget(runtimeGroup, 1);

    QGroupBox *resultGroup = new QGroupBox(QStringLiteral("当前结果"), rightPanel);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(10, 18, 10, 10);
    resultLayout->setSpacing(5);

    m_aiCurrentResultLabel = new QLabel(
                QStringLiteral("等待识别结果……"), resultGroup);
    m_aiCurrentResultLabel->setObjectName(QStringLiteral("aiCurrentResult"));
    m_aiCurrentResultLabel->setWordWrap(true);
    m_aiCurrentResultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_aiCurrentResultLabel->setSizePolicy(QSizePolicy::Expanding,
                                          QSizePolicy::Expanding);
    resultLayout->addWidget(m_aiCurrentResultLabel, 1);
    rightLayout->addWidget(resultGroup, 1);

    contentLayout->addWidget(rightPanel, 11);
    pageLayout->addLayout(contentLayout, 1);

    connect(m_btnAiApply, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyAiConfigClicked);
    connect(m_btnAiStop, &QPushButton::clicked,
            this, &ControlPanelWindow::onStopAiInferenceClicked);

    updateAiTaskButtons();
    return page;
}
void ControlPanelWindow::onApplyAiConfigClicked()
{
    if (!m_display || !m_plateModeButtonGroup ||
        !m_platePostprocessButtonGroup || !m_npuFpsSpin ||
        !m_confSpin || !m_nmsSpin) {
        return;
    }

    if (m_violationRunning) {
        stopViolationDetection(false);
    }

    const NpuTaskConfig previous = m_display->npuConfig();
    NpuTaskConfig config = previous;
    m_aiRequestedTask = NpuTaskType::PlateCascade;
    config.enabled = true;
    config.task = NpuTaskType::PlateCascade;
    config.plateModelProfile = m_aiRequestedPlateProfile;
    config.platePostprocess = m_aiRequestedPlatePostprocess;
    config.plateCropSource = m_aiRequestedPlateCropSource;
    config.targetFps = m_npuFpsSpin->value();
    config.confidence = static_cast<float>(m_confSpin->value());
    config.nmsThreshold = static_cast<float>(m_nmsSpin->value());
    config.resultHistoryDepth = 50;
    config = sanitizeNpuTaskConfig(config);

    const bool executionChanged = !m_aiEnabled ||
            config.task != previous.task ||
            config.plateModelProfile != previous.plateModelProfile ||
            config.platePostprocess != previous.platePostprocess ||
            config.plateCropSource != previous.plateCropSource;
    m_aiTaskSwitchPending = executionChanged;

    m_display->setNpuConfig(config);
    m_aiRequestedConfigRevision = m_display->npuConfig().revision;
    m_aiPlateOptionsDirty = false;
    saveAiSettings(config);

    m_aiEnabled = true;
    m_aiReady = false;
    m_aiStatus = executionChanged
            ? QStringLiteral("switching")
            : QStringLiteral("pending");

    clearAiCurrentResult();
    updateAiTaskButtons();

    if (m_aiConfigSummaryLabel) {
        m_aiConfigSummaryLabel->setText(
                    QStringLiteral("正在应用：%1｜%2｜%3｜%4 fps｜conf=%5｜NMS=%6")
                    .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                             config.plateModelProfile, config.plateCropSource)))
                    .arg(plateProfileDisplayName(config.plateModelProfile))
                    .arg(platePostprocessDisplayName(config.platePostprocess))
                    .arg(config.targetFps)
                    .arg(config.confidence, 0, 'f', 2)
                    .arg(config.nmsThreshold, 0, 'f', 2));
    }

    m_lastAction = QStringLiteral("启动车牌识别：mode=%1，fps=%2，conf=%3，nms=%4")
            .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                     config.plateModelProfile, config.plateCropSource)))
            .arg(config.targetFps)
            .arg(config.confidence, 0, 'f', 2)
            .arg(config.nmsThreshold, 0, 'f', 2);
    m_lastError = QStringLiteral("none");
    updateStatusText();
}
void ControlPanelWindow::onStopAiInferenceClicked()
{
    if (!m_display || !m_npuFpsSpin || !m_confSpin || !m_nmsSpin) {
        return;
    }

    if (m_violationRunning) {
        stopViolationDetection(false);
    }

    NpuTaskConfig config = m_display->npuConfig();
    config.enabled = false;
    config.task = NpuTaskType::PlateCascade;
    config.plateModelProfile = m_aiRequestedPlateProfile;
    config.platePostprocess = m_aiRequestedPlatePostprocess;
    config.plateCropSource = m_aiRequestedPlateCropSource;
    config.targetFps = m_npuFpsSpin->value();
    config.confidence = static_cast<float>(m_confSpin->value());
    config.nmsThreshold = static_cast<float>(m_nmsSpin->value());
    config.resultHistoryDepth = 50;
    config = sanitizeNpuTaskConfig(config);

    m_display->setNpuConfig(config);
    m_aiRequestedConfigRevision = m_display->npuConfig().revision;
    m_aiPlateOptionsDirty = false;
    saveAiSettings(config);

    m_aiEnabled = false;
    m_aiReady = false;
    m_aiTaskSwitchPending = false;
    m_aiFps = 0.0;
    m_aiPlateCount = 0;
    m_aiPedestrianCount = 0;
    m_aiVehicleCount = 0;
    m_aiLastMs = 0.0;
    m_aiPreMs = 0.0;
    m_aiStatus = QStringLiteral("off");

    clearAiCurrentResult();
    updateAiTaskButtons();

    if (m_aiConfigSummaryLabel) {
        m_aiConfigSummaryLabel->setText(
                    QStringLiteral("推理已关闭｜待执行：%1｜%2｜%3｜%4 fps｜conf=%5｜NMS=%6")
                    .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                             config.plateModelProfile, config.plateCropSource)))
                    .arg(plateProfileDisplayName(config.plateModelProfile))
                    .arg(platePostprocessDisplayName(config.platePostprocess))
                    .arg(config.targetFps)
                    .arg(config.confidence, 0, 'f', 2)
                    .arg(config.nmsThreshold, 0, 'f', 2));
    }

    m_lastAction = QStringLiteral("关闭NPU推理");
    m_lastError = QStringLiteral("none");
    updateStatusText();
}
void ControlPanelWindow::onAiUiTimer()
{
    if (!m_display) {
        return;
    }

    const NpuFrameResult result = m_display->latestNpuResult();
    updateAiPanel(result);
}
void ControlPanelWindow::updateAiPanel(const NpuFrameResult &result)
{
    m_aiEnabled = result.enabled;
    m_aiReady = result.ready;
    m_aiTask = result.task;
    m_aiFps = result.fps;
    m_aiPlateCount = static_cast<int>(result.plates.size());
    m_aiPedestrianCount = static_cast<int>(result.pedestrians.size());
    m_aiVehicleCount = static_cast<int>(result.vehicles.size());
    m_aiLastMs = result.lastMs;
    m_aiPreMs = result.preMs;
    m_aiProcessed = result.processed;
    m_aiDropped = result.dropped;
    m_aiActivePlateProfileValid = result.activePlateModelProfileValid;
    if (result.activePlateModelProfileValid) {
        m_aiActivePlateProfile = result.activePlateModelProfile;
    }

    // UI没有未应用的本地选择时，以后台已接受（包括失败回滚后）的配置为准。
    if (!m_aiPlateOptionsDirty &&
        result.configRevision >= m_aiRequestedConfigRevision) {
        m_aiRequestedPlateProfile = result.requestedPlateModelProfile;
        m_aiRequestedPlatePostprocess = result.platePostprocess;
        m_aiRequestedPlateCropSource = result.requestedPlateCropSource;
        m_aiRequestedConfigRevision = result.configRevision;
    }

    if (m_aiTaskSwitchPending &&
        result.configRevision >= m_aiRequestedConfigRevision &&
        (!result.enabled ||
         !result.error.empty() ||
         (result.ready && result.modelsReady && !result.modelSwitching &&
           result.task == m_aiRequestedTask &&
           (result.task != NpuTaskType::PlateCascade ||
           (result.activePlateModelProfileValid &&
            result.activePlateModelProfile == m_aiRequestedPlateProfile &&
            result.requestedPlateCropSource == m_aiRequestedPlateCropSource))))) {
        // 第一帧已经由目标配置处理，或后台明确返回失败/回滚，解除按钮锁定。
        m_aiTaskSwitchPending = false;
    }

    const VideoRoute route = m_display ? m_display->videoRoute() : VideoRoute::Overview;

    if (!result.enabled) {
        m_aiStatus = QStringLiteral("off");
    } else if (!result.error.empty()) {
        m_aiStatus = QString::fromStdString(result.error);
    } else if (m_aiTaskSwitchPending || result.modelSwitching) {
        m_aiStatus = QStringLiteral("switching");
    } else if (result.ready) {
        m_aiStatus = QStringLiteral("ready");
    } else if (!result.modelsReady) {
        m_aiStatus = QStringLiteral("loading model");
    } else {
        m_aiStatus = QStringLiteral("pending");
    }

    if (m_aiRuntimeLabel) {
        const int resultCount = m_aiPlateCount + m_aiPedestrianCount + m_aiVehicleCount;
        m_aiRuntimeLabel->setText(
                    QStringLiteral("状态：%1｜输入：%2｜模式：%3\n"
                                   "模型：请求%4｜实际%5｜结果策略%6\n"
                                   "ROI：请求%7｜实际%8｜源图%9×%10｜回退%11\n"
                                   "实际FPS：%12 / 目标%13｜总耗时：%14 ms｜预处理：%15 ms\n"
                                   "RKNN运行：%16 ms｜后处理：%17 ms｜runs=%18\n"
                                   "本帧目标：%19｜已处理：%20｜覆盖丢帧：%21\n"
                                   "最近错误：%22")
                    .arg(m_aiStatus)
                    .arg(isSingleChannelRoute(route)
                         ? QStringLiteral("CH%1").arg(videoRouteChannel(route))
                         : QStringLiteral("总览全图"))
                    .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                             result.requestedPlateModelProfile,
                             result.requestedPlateCropSource)))
                    .arg(plateProfileDisplayName(result.requestedPlateModelProfile))
                    .arg(result.activePlateModelProfileValid
                         ? plateProfileDisplayName(result.activePlateModelProfile)
                         : QStringLiteral("未加载"))
                    .arg(platePostprocessDisplayName(result.platePostprocess))
                    .arg(plateCropSourceDisplayName(result.requestedPlateCropSource))
                    .arg(plateCropSourceDisplayName(result.actualPlateCropSource))
                    .arg(result.originalSourceWidth)
                    .arg(result.originalSourceHeight)
                    .arg(result.originalRoiFallbackCount)
                    .arg(result.fps, 0, 'f', 1)
                    .arg(result.targetFps)
                    .arg(result.lastMs, 0, 'f', 1)
                    .arg(result.preMs, 0, 'f', 1)
                    .arg(result.rknnRunMs, 0, 'f', 1)
                    .arg(result.rknnPostMs, 0, 'f', 1)
                    .arg(static_cast<qulonglong>(result.rknnRunCount))
                    .arg(resultCount)
                    .arg(result.processed)
                    .arg(result.dropped)
                    .arg(result.error.empty()
                         ? QStringLiteral("无")
                         : QString::fromStdString(result.error)));
    }

    updateAiCurrentResult(result);
    updateViolationPanel(result);
    updateWrongWayPanel(result);
    updateAiTaskButtons();
}
void ControlPanelWindow::updateAiCurrentResult(const NpuFrameResult &result)
{
    if (!m_aiCurrentResultLabel) {
        return;
    }

    if (!result.enabled) {
        m_aiCurrentResultLabel->setText(QStringLiteral("NPU推理未启用。"));
        return;
    }
    if (!result.error.empty()) {
        m_aiCurrentResultLabel->setText(
                    QStringLiteral("推理异常：%1")
                    .arg(QString::fromStdString(result.error)));
        return;
    }
    if (!result.ready) {
        m_aiCurrentResultLabel->setText(
                    m_aiTaskSwitchPending
                    ? QStringLiteral("正在切换到%1，请稍候……")
                      .arg(aiTaskDisplayName(m_aiRequestedTask))
                    : QStringLiteral("模型正在加载或等待第一帧结果……"));
        return;
    }

    const QString timeText = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString sourceText = result.sourceChannel > 0
            ? QStringLiteral("CH%1").arg(result.sourceChannel)
            : QStringLiteral("总览全图");

    QStringList lines;
    lines << QStringLiteral("更新时间：%1｜帧号：%2｜输入：%3")
             .arg(timeText)
             .arg(static_cast<qulonglong>(result.frameId))
             .arg(sourceText);
    lines << QStringLiteral("当前任务：%1").arg(aiTaskDisplayName(result.task));

    if (result.task == NpuTaskType::PlateCascade) {
        lines << QStringLiteral("模式：%1｜模型：%2｜结果：%3")
                 .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                          result.requestedPlateModelProfile,
                          result.requestedPlateCropSource)))
                 .arg(result.activePlateModelProfileValid
                      ? plateProfileDisplayName(result.activePlateModelProfile)
                      : QStringLiteral("未加载"))
                 .arg(platePostprocessDisplayName(result.platePostprocess));
        lines << QStringLiteral("ROI：%1｜源图：%2×%3｜本帧原图裁剪：%4｜回退：%5")
                 .arg(plateCropSourceDisplayName(result.actualPlateCropSource))
                 .arg(result.originalSourceWidth)
                 .arg(result.originalSourceHeight)
                 .arg(result.originalRoiCropCount)
                 .arg(result.originalRoiFallbackCount);
        if (result.plates.empty()) {
            lines << QStringLiteral("本帧未识别到车牌");
        } else {
            lines << QStringLiteral("识别到车牌：%1个").arg(static_cast<int>(result.plates.size()));
            const int showCount = qMin(4, static_cast<int>(result.plates.size()));
            for (int i = 0; i < showCount; ++i) {
                const NpuBox &box = result.plates[static_cast<std::size_t>(i)];
                const QString plate = !box.text.empty()
                        ? QString::fromStdString(box.text)
                        : (!box.label.empty()
                           ? QString::fromStdString(box.label)
                           : QStringLiteral("未识别字符"));
                lines << QStringLiteral("%1. %2｜置信度 %3")
                         .arg(i + 1)
                         .arg(plate)
                         .arg(box.score, 0, 'f', 2);
            }
        }
    } else if (result.task == NpuTaskType::Pedestrian) {
        lines << QStringLiteral("检测到行人：%1个").arg(static_cast<int>(result.pedestrians.size()));
        if (!result.pedestrians.empty()) {
            float maxScore = 0.0f;
            for (const NpuBox &box : result.pedestrians) {
                maxScore = qMax(maxScore, box.score);
            }
            lines << QStringLiteral("最高置信度：%1").arg(maxScore, 0, 'f', 2);
        }
    } else if (result.task == NpuTaskType::VehicleWrongWay) {
        lines << QStringLiteral("检测到车辆：%1个｜有效轨迹：%2")
                 .arg(static_cast<int>(result.vehicles.size()))
                 .arg(result.activeVehicleTrackCount);
        lines << QStringLiteral("当前逆行：%1｜累计事件：%2")
                 .arg(result.activeWrongWayCount)
                 .arg(static_cast<qulonglong>(result.wrongWayEventCount));
    } else {
        lines << QStringLiteral("检测到行人：%1个｜有效轨迹：%2")
                 .arg(static_cast<int>(result.pedestrians.size()))
                 .arg(result.activeTrackCount);
        lines << QStringLiteral("信号灯：%1｜区域：%2")
                 .arg(violationLightText(result.trafficLight))
                 .arg(result.violationRegionsConfigured
                      ? QStringLiteral("已配置")
                      : QStringLiteral("未完成"));
        lines << QStringLiteral("当前违法轨迹：%1｜累计事件：%2")
                 .arg(result.activeViolationCount)
                 .arg(static_cast<qulonglong>(result.violationEventCount));
        if (result.hasRecentViolation) {
            lines << QStringLiteral("最近事件：行人 #%1 闯红灯，持续%2 ms")
                     .arg(result.recentViolation.trackId)
                     .arg(result.recentViolation.accumulatedMs, 0, 'f', 0);
        }
    }

    m_aiCurrentResultLabel->setText(lines.join(QLatin1Char('\n')));
}
void ControlPanelWindow::clearAiCurrentResult()
{
    if (m_aiCurrentResultLabel) {
        m_aiCurrentResultLabel->setText(QStringLiteral("等待识别结果……"));
    }
}
void ControlPanelWindow::requestAiTask(NpuTaskType task)
{
    if (m_aiTaskSwitchPending) {
        return;
    }

    m_aiRequestedTask = task;
    m_lastAction = QStringLiteral("选择AI任务：%1，等待应用")
            .arg(aiTaskDisplayName(task));
    clearAiCurrentResult();
    updateAiTaskButtons();

    if (m_aiConfigSummaryLabel) {
        m_aiConfigSummaryLabel->setText(
                    QStringLiteral("待应用：%1｜%2｜%3｜%4 fps｜conf=%5｜NMS=%6")
                    .arg(plateUiModeDisplayName(plateUiModeFromConfig(
                             m_aiRequestedPlateProfile,
                             m_aiRequestedPlateCropSource)))
                    .arg(plateCropSourceDisplayName(m_aiRequestedPlateCropSource))
                    .arg(platePostprocessDisplayName(m_aiRequestedPlatePostprocess))
                    .arg(m_npuFpsSpin ? m_npuFpsSpin->value()
                                     : NPU_TARGET_FPS_DEFAULT)
                    .arg(m_confSpin ? m_confSpin->value() : 0.50, 0, 'f', 2)
                    .arg(m_nmsSpin ? m_nmsSpin->value() : 0.45, 0, 'f', 2));
    }
    updateStatusText();
}
void ControlPanelWindow::updateAiTaskButtons()
{
    if (m_npuTaskButtonGroup) {
        const QList<QAbstractButton *> buttons = m_npuTaskButtonGroup->buttons();
        for (QAbstractButton *button : buttons) {
            if (!button) continue;
            const NpuTaskType task = static_cast<NpuTaskType>(
                        m_npuTaskButtonGroup->id(button));
            const bool active = m_aiEnabled &&
                    !m_aiTaskSwitchPending && task == m_aiTask;
            const bool pending = task == m_aiRequestedTask && !active;
            button->setText(aiTaskButtonText(task, active, pending));
            button->setProperty("aiActive", active);
            button->setProperty("aiPending", pending);
            button->setEnabled(!m_aiTaskSwitchPending);
            button->style()->unpolish(button);
            button->style()->polish(button);
            button->update();
        }
    }

    if (m_btnAiApply) {
        m_btnAiApply->setEnabled(!m_aiTaskSwitchPending);
    }
    if (m_btnAiStop) {
        m_btnAiStop->setEnabled(m_aiEnabled || m_aiTaskSwitchPending);
    }

    if (m_plateModeButtonGroup) {
        const PlateUiMode requestedMode = plateUiModeFromConfig(
                    m_aiRequestedPlateProfile, m_aiRequestedPlateCropSource);
        for (QAbstractButton *button : m_plateModeButtonGroup->buttons()) {
            if (!button) continue;
            button->setChecked(m_plateModeButtonGroup->id(button) ==
                               static_cast<int>(requestedMode));
            button->setEnabled(!m_aiTaskSwitchPending);
        }
    }
    if (m_platePostprocessButtonGroup) {
        for (QAbstractButton *button : m_platePostprocessButtonGroup->buttons()) {
            if (!button) continue;
            button->setChecked(m_platePostprocessButtonGroup->id(button) ==
                               static_cast<int>(m_aiRequestedPlatePostprocess));
            button->setEnabled(!m_aiTaskSwitchPending);
        }
    }
}
void ControlPanelWindow::loadAiSettings()
{
    if (!m_plateModeButtonGroup || !m_platePostprocessButtonGroup || !m_npuFpsSpin ||
        !m_confSpin || !m_nmsSpin) {
        return;
    }

    QSettings settings(aiSettingsPath(), QSettings::IniFormat);
    const int fps = settings.value(QStringLiteral("ai/target_fps"),
                                   NPU_TARGET_FPS_DEFAULT).toInt();
    const double confidence = settings.value(
                QStringLiteral("ai/confidence"), 0.50).toDouble();
    const double nms = settings.value(
                QStringLiteral("ai/nms"), 0.45).toDouble();

    // 实验版本每次启动都回到已验证的标准模式；原图ROI不持久化，
    // 防止重启后意外持续复制1080P源图。
    const NpuTaskType savedTask = NpuTaskType::PlateCascade;
    m_aiRequestedTask = savedTask;
    m_aiTask = savedTask;
    m_aiEnabled = false;
    m_aiReady = false;
    m_aiTaskSwitchPending = false;
    const NpuTaskConfig runtimeConfig = m_display
            ? m_display->npuConfig() : NpuTaskConfig();
    m_aiRequestedPlateProfile = NpuPlateModelProfile::Legacy;
    m_aiActivePlateProfile = NpuPlateModelProfile::Legacy;
    m_aiRequestedPlateCropSource = NpuPlateCropSource::DetectorInput;
    m_aiRequestedPlatePostprocess = NpuPlatePostprocess::Stable;
    m_aiActivePlateProfileValid = false;
    m_aiPlateOptionsDirty = false;
    m_aiRequestedConfigRevision = runtimeConfig.revision;

    m_npuFpsSpin->setValue(qBound(NPU_TARGET_FPS_MIN,
                                  fps,
                                  NPU_TARGET_FPS_MAX));
    m_confSpin->setValue(qBound(0.10, confidence, 0.95));
    m_nmsSpin->setValue(qBound(0.10, nms, 0.90));

    if (m_aiConfigSummaryLabel) {
        m_aiConfigSummaryLabel->setText(
                    QStringLiteral("推理未启动｜待执行：%1｜%2｜%3｜%4 fps｜conf=%5｜NMS=%6")
                    .arg(plateUiModeDisplayName(PlateUiStandard))
                    .arg(plateCropSourceDisplayName(m_aiRequestedPlateCropSource))
                    .arg(platePostprocessDisplayName(m_aiRequestedPlatePostprocess))
                    .arg(m_npuFpsSpin->value())
                    .arg(m_confSpin->value(), 0, 'f', 2)
                    .arg(m_nmsSpin->value(), 0, 'f', 2));
    }
    updateAiTaskButtons();
}
void ControlPanelWindow::saveAiSettings(const NpuTaskConfig &config)
{
    QSettings settings(aiSettingsPath(), QSettings::IniFormat);
    // 新交互要求每次程序启动时NPU默认关闭，只恢复任务和参数。
    settings.setValue(QStringLiteral("ai/enabled"), false);
    settings.setValue(QStringLiteral("ai/task"),
                      static_cast<int>(NpuTaskType::PlateCascade));
    settings.setValue(QStringLiteral("ai/target_fps"), config.targetFps);
    settings.setValue(QStringLiteral("ai/confidence"), config.confidence);
    settings.setValue(QStringLiteral("ai/nms"), config.nmsThreshold);
    settings.sync();
}
void ControlPanelWindow::onDisplayAiStatsUpdated(bool enabled,
                                                 bool ready,
                                                 double fps,
                                                 int plateCount,
                                                 int pedestrianCount,
                                                 int vehicleCount,
                                                 QString status)
{
    m_aiEnabled = enabled;
    m_aiReady = ready;
    m_aiFps = fps;
    m_aiPlateCount = plateCount;
    m_aiPedestrianCount = pedestrianCount;
    m_aiVehicleCount = vehicleCount;
    m_aiStatus = status;

    updateStatusText();
}
