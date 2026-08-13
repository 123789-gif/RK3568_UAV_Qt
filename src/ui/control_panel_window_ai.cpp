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
            ? QStringLiteral("原始结果") : QStringLiteral("稳定结果");
}

}  // namespace

QWidget *ControlPanelWindow::createAiPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(6, 4, 6, 4);
    pageLayout->setSpacing(10);

    // 左侧保留原有参数配置，只适当加宽并把任务下拉框改为直达按钮。
    QWidget *leftPanel = new QWidget(page);
    leftPanel->setFixedWidth(455);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    QGroupBox *configGroup = new QGroupBox(QStringLiteral("AI / NPU 单任务配置"), leftPanel);
    QGridLayout *grid = new QGridLayout(configGroup);
    grid->setContentsMargins(12, 18, 12, 10);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnMinimumWidth(0, 94);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    m_npuTaskButtonGroup = new QButtonGroup(this);
    m_npuTaskButtonGroup->setExclusive(false);

    QWidget *taskWidget = new QWidget(configGroup);
    QHBoxLayout *taskLayout = new QHBoxLayout(taskWidget);
    taskLayout->setContentsMargins(0, 0, 0, 0);
    taskLayout->setSpacing(6);

    auto createTaskButton = [this, configGroup, taskLayout](NpuTaskType task) {
        QPushButton *button = new QPushButton(aiTaskDisplayName(task), configGroup);
        button->setObjectName(QStringLiteral("aiTaskButton"));
        button->setMinimumHeight(48);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("aiActive", false);
        button->setProperty("aiPending", false);
        m_npuTaskButtonGroup->addButton(button, static_cast<int>(task));
        taskLayout->addWidget(button, 1);
        connect(button, &QPushButton::clicked, this,
                [this, task]() { requestAiTask(task); });
    };

    createTaskButton(NpuTaskType::PlateCascade);
    createTaskButton(NpuTaskType::Pedestrian);

    auto createChoiceRow = [this, configGroup](QButtonGroup **groupOut,
                                                const QList<QPair<QString, int>> &choices,
                                                const std::function<void(int)> &selected) {
        QButtonGroup *group = new QButtonGroup(this);
        group->setExclusive(true);
        QWidget *widget = new QWidget(configGroup);
        QHBoxLayout *layout = new QHBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        for (const auto &choice : choices) {
            QPushButton *button = new QPushButton(choice.first, widget);
            button->setObjectName(QStringLiteral("aiChoiceButton"));
            button->setCheckable(true);
            button->setMinimumHeight(42);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            group->addButton(button, choice.second);
            layout->addWidget(button, 1);
            connect(button, &QPushButton::clicked, this,
                    [selected, choice]() { selected(choice.second); });
        }
        *groupOut = group;
        return widget;
    };

    QWidget *modelWidget = createChoiceRow(
                &m_plateModelButtonGroup,
                {{QStringLiteral("旧版基线"), static_cast<int>(NpuPlateModelProfile::Legacy)},
                 {QStringLiteral("v2.9.1候选"), static_cast<int>(NpuPlateModelProfile::V291Candidate)}},
                [this](int value) {
        if (m_aiTaskSwitchPending) return;
        m_aiRequestedPlateProfile = static_cast<NpuPlateModelProfile>(value);
        m_aiPlateOptionsDirty = true;
        clearAiCurrentResult();
        if (m_aiConfigSummaryLabel) {
            m_aiConfigSummaryLabel->setText(
                        QStringLiteral("待应用：%1｜%2｜%3")
                        .arg(aiTaskDisplayName(m_aiRequestedTask))
                        .arg(plateProfileDisplayName(m_aiRequestedPlateProfile))
                        .arg(platePostprocessDisplayName(
                                 m_aiRequestedPlatePostprocess)));
        }
        updateAiTaskButtons();
    });

    QWidget *postprocessWidget = createChoiceRow(
                &m_platePostprocessButtonGroup,
                {{QStringLiteral("原始结果"), static_cast<int>(NpuPlatePostprocess::Raw)},
                 {QStringLiteral("稳定结果"), static_cast<int>(NpuPlatePostprocess::Stable)}},
                [this](int value) {
        if (m_aiTaskSwitchPending) return;
        m_aiRequestedPlatePostprocess = static_cast<NpuPlatePostprocess>(value);
        m_aiPlateOptionsDirty = true;
        clearAiCurrentResult();
        if (m_aiConfigSummaryLabel) {
            m_aiConfigSummaryLabel->setText(
                        QStringLiteral("待应用：%1｜%2｜%3")
                        .arg(aiTaskDisplayName(m_aiRequestedTask))
                        .arg(plateProfileDisplayName(m_aiRequestedPlateProfile))
                        .arg(platePostprocessDisplayName(
                                 m_aiRequestedPlatePostprocess)));
        }
        updateAiTaskButtons();
    });

    QLabel *sourceValue = new QLabel(
                QStringLiteral("视频显示页选定输入：总览全图或CH1~CH9"), configGroup);
    sourceValue->setObjectName(QStringLiteral("ptzState"));
    sourceValue->setMinimumHeight(38);

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

    grid->addWidget(new QLabel(QStringLiteral("推理任务"), configGroup), 0, 0);
    grid->addWidget(taskWidget, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("输入源"), configGroup), 1, 0);
    grid->addWidget(sourceValue, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("车牌模型"), configGroup), 2, 0);
    grid->addWidget(modelWidget, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("后处理"), configGroup), 3, 0);
    grid->addWidget(postprocessWidget, 3, 1);

    addTouchAdjustRow(4,
                      QStringLiteral("目标推理帧率"),
                      m_npuFpsSpin,
                      [this]() { m_npuFpsSpin->stepDown(); },
                      [this]() { m_npuFpsSpin->stepUp(); });
    addTouchAdjustRow(5,
                      QStringLiteral("检测置信度"),
                      m_confSpin,
                      [this]() { m_confSpin->stepDown(); },
                      [this]() { m_confSpin->stepUp(); });
    addTouchAdjustRow(6,
                      QStringLiteral("NMS阈值"),
                      m_nmsSpin,
                      [this]() { m_nmsSpin->stepDown(); },
                      [this]() { m_nmsSpin->stepUp(); });

    grid->addWidget(m_btnAiApply, 7, 0, 1, 2);
    grid->addWidget(m_btnAiStop, 8, 0, 1, 2);

    m_aiConfigSummaryLabel = new QLabel(configGroup);
    m_aiConfigSummaryLabel->setWordWrap(true);
    m_aiConfigSummaryLabel->setObjectName(QStringLiteral("ptzState"));
    m_aiConfigSummaryLabel->setMaximumHeight(52);
    grid->addWidget(m_aiConfigSummaryLabel, 9, 0, 1, 2);

    QLabel *touchHint = new QLabel(
                QStringLiteral("先选择任务和参数，再点击“应用参数并推理”；"
                               "行人违法规则请使用独立“违法检测”页面。"),
                configGroup);
    touchHint->setObjectName(QStringLiteral("navHint"));
    touchHint->setWordWrap(true);
    grid->addWidget(touchHint, 10, 0, 1, 2);

    leftLayout->addWidget(configGroup);
    leftLayout->addStretch(1);
    pageLayout->addWidget(leftPanel, 0);

    QWidget *rightPanel = new QWidget(page);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    QGroupBox *runtimeGroup = new QGroupBox(QStringLiteral("运行状态"), rightPanel);
    runtimeGroup->setMaximumHeight(220);
    QVBoxLayout *runtimeLayout = new QVBoxLayout(runtimeGroup);
    runtimeLayout->setContentsMargins(10, 18, 10, 8);
    m_aiRuntimeLabel = new QLabel(QStringLiteral("AI未启用"), runtimeGroup);
    m_aiRuntimeLabel->setObjectName(QStringLiteral("aiRuntimeStatus"));
    m_aiRuntimeLabel->setWordWrap(true);
    m_aiRuntimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_aiRuntimeLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    runtimeLayout->addWidget(m_aiRuntimeLabel);
    rightLayout->addWidget(runtimeGroup, 0);

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

    pageLayout->addWidget(rightPanel, 1);

    connect(m_btnAiApply, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyAiConfigClicked);
    connect(m_btnAiStop, &QPushButton::clicked,
            this, &ControlPanelWindow::onStopAiInferenceClicked);

    updateAiTaskButtons();
    return page;
}
void ControlPanelWindow::onApplyAiConfigClicked()
{
    if (!m_display || !m_npuTaskButtonGroup ||
        !m_npuFpsSpin || !m_confSpin || !m_nmsSpin) {
        return;
    }

    if (m_violationRunning) {
        stopViolationDetection(false);
    }

    const NpuTaskConfig previous = m_display->npuConfig();
    NpuTaskConfig config = previous;
    config.enabled = true;
    config.task = m_aiRequestedTask;
    config.plateModelProfile = m_aiRequestedPlateProfile;
    config.platePostprocess = m_aiRequestedPlatePostprocess;
    config.targetFps = m_npuFpsSpin->value();
    config.confidence = static_cast<float>(m_confSpin->value());
    config.nmsThreshold = static_cast<float>(m_nmsSpin->value());
    config.resultHistoryDepth = 50;
    config = sanitizeNpuTaskConfig(config);

    const bool executionChanged = !m_aiEnabled ||
            config.task != previous.task ||
            config.plateModelProfile != previous.plateModelProfile ||
            config.platePostprocess != previous.platePostprocess;
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
                    .arg(aiTaskDisplayName(config.task))
                    .arg(plateProfileDisplayName(config.plateModelProfile))
                    .arg(platePostprocessDisplayName(config.platePostprocess))
                    .arg(config.targetFps)
                    .arg(config.confidence, 0, 'f', 2)
                    .arg(config.nmsThreshold, 0, 'f', 2));
    }

    m_lastAction = QStringLiteral("启动AI推理：task=%1，fps=%2，conf=%3，nms=%4")
            .arg(aiTaskDisplayName(config.task))
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
    config.task = m_aiRequestedTask;
    config.plateModelProfile = m_aiRequestedPlateProfile;
    config.platePostprocess = m_aiRequestedPlatePostprocess;
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
                    .arg(aiTaskDisplayName(config.task))
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
            result.activePlateModelProfile == m_aiRequestedPlateProfile))))) {
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
                    QStringLiteral("状态：%1｜输入：%2｜任务：%3\n"
                                   "模型：请求%4｜实际%5｜后处理%6\n"
                                   "实际FPS：%7 / 目标%8｜总耗时：%9 ms｜预处理：%10 ms\n"
                                   "RKNN运行：%11 ms｜后处理：%12 ms｜runs=%13\n"
                                   "本帧目标：%14｜已处理：%15｜覆盖丢帧：%16\n"
                                   "最近错误：%17")
                    .arg(m_aiStatus)
                    .arg(isSingleChannelRoute(route)
                         ? QStringLiteral("CH%1").arg(videoRouteChannel(route))
                         : QStringLiteral("总览全图"))
                    .arg(aiTaskDisplayName(result.task))
                    .arg(plateProfileDisplayName(result.requestedPlateModelProfile))
                    .arg(result.activePlateModelProfileValid
                         ? plateProfileDisplayName(result.activePlateModelProfile)
                         : QStringLiteral("未加载"))
                    .arg(platePostprocessDisplayName(result.platePostprocess))
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
        lines << QStringLiteral("模型：%1｜后处理：%2")
                 .arg(result.activePlateModelProfileValid
                      ? plateProfileDisplayName(result.activePlateModelProfile)
                      : QStringLiteral("未加载"))
                 .arg(platePostprocessDisplayName(result.platePostprocess));
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
    if (!m_npuTaskButtonGroup) {
        return;
    }

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
                    .arg(aiTaskDisplayName(task))
                    .arg(plateProfileDisplayName(m_aiRequestedPlateProfile))
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
    if (!m_npuTaskButtonGroup) {
        return;
    }

    const QList<QAbstractButton *> buttons = m_npuTaskButtonGroup->buttons();
    for (QAbstractButton *button : buttons) {
        if (!button) {
            continue;
        }

        const NpuTaskType task = static_cast<NpuTaskType>(
                    m_npuTaskButtonGroup->id(button));
        const bool active = m_aiEnabled &&
                !m_aiTaskSwitchPending &&
                task == m_aiTask;
        const bool pending = task == m_aiRequestedTask && !active;

        button->setText(aiTaskButtonText(task, active, pending));
        button->setProperty("aiActive", active);
        button->setProperty("aiPending", pending);
        button->setEnabled(!m_aiTaskSwitchPending);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }

    if (m_btnAiApply) {
        m_btnAiApply->setEnabled(!m_aiTaskSwitchPending);
    }
    if (m_btnAiStop) {
        m_btnAiStop->setEnabled(m_aiEnabled || m_aiTaskSwitchPending);
    }

    if (m_plateModelButtonGroup) {
        for (QAbstractButton *button : m_plateModelButtonGroup->buttons()) {
            if (!button) continue;
            button->setChecked(m_plateModelButtonGroup->id(button) ==
                               static_cast<int>(m_aiRequestedPlateProfile));
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
    if (!m_npuTaskButtonGroup || !m_plateModelButtonGroup ||
        !m_platePostprocessButtonGroup || !m_npuFpsSpin ||
        !m_confSpin || !m_nmsSpin) {
        return;
    }

    QSettings settings(aiSettingsPath(), QSettings::IniFormat);
    const int taskValue = settings.value(
                QStringLiteral("ai/task"),
                static_cast<int>(NpuTaskType::Pedestrian)).toInt();
    const int fps = settings.value(QStringLiteral("ai/target_fps"),
                                   NPU_TARGET_FPS_DEFAULT).toInt();
    const double confidence = settings.value(
                QStringLiteral("ai/confidence"), 0.50).toDouble();
    const double nms = settings.value(
                QStringLiteral("ai/nms"), 0.45).toDouble();

    const NpuTaskType savedTask = m_npuTaskButtonGroup->button(taskValue)
            ? static_cast<NpuTaskType>(taskValue)
            : NpuTaskType::Pedestrian;
    m_aiRequestedTask = savedTask;
    m_aiTask = savedTask;
    m_aiEnabled = false;
    m_aiReady = false;
    m_aiTaskSwitchPending = false;
    const NpuTaskConfig runtimeConfig = m_display
            ? m_display->npuConfig() : NpuTaskConfig();
    m_aiRequestedPlateProfile = runtimeConfig.plateModelProfile;
    m_aiActivePlateProfile = runtimeConfig.plateModelProfile;
    m_aiRequestedPlatePostprocess = runtimeConfig.platePostprocess;
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
                    .arg(aiTaskDisplayName(savedTask))
                    .arg(plateProfileDisplayName(m_aiRequestedPlateProfile))
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
    settings.setValue(QStringLiteral("ai/task"), static_cast<int>(config.task));
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
