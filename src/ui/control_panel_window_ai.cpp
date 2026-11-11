#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createAiPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(12);
    auto *group = new QGroupBox(QStringLiteral("无人机检测 · 触屏参数 v2.1"), page);
    auto *grid = new QGridLayout(group);
    m_npuFpsSpin = new QSpinBox(group);
    m_npuFpsSpin->setRange(1, 30);
    m_npuFpsSpin->setValue(5);
    m_confSpin = new QDoubleSpinBox(group);
    m_confSpin->setRange(0.01, 0.99);
    m_confSpin->setSingleStep(0.05);
    m_confSpin->setValue(0.35);
    m_nmsSpin = new QDoubleSpinBox(group);
    m_nmsSpin->setRange(0.01, 0.99);
    m_nmsSpin->setSingleStep(0.05);
    m_nmsSpin->setValue(0.45);
    // Read-only values avoid popping up a keyboard; large buttons retain the original touch workflow.
    const auto configureTouchSpin = [](QAbstractSpinBox *spin) {
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setReadOnly(true);
        spin->setAlignment(Qt::AlignCenter);
        spin->setObjectName(QStringLiteral("touchValueSpin"));
        spin->setMinimumSize(120, 46);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setFocusPolicy(Qt::NoFocus);
    };
    configureTouchSpin(m_npuFpsSpin);
    configureTouchSpin(m_confSpin);
    configureTouchSpin(m_nmsSpin);
    m_npuFpsSpin->setSingleStep(1);
    m_npuFpsSpin->setSuffix(QStringLiteral(" fps"));
    m_confSpin->setDecimals(2);
    m_nmsSpin->setDecimals(2);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);

    const auto addAdjustRow = [this, group, grid](int row, const QString &text,
                                                QAbstractSpinBox *spin,
                                                const std::function<void()> &decrease,
                                                const std::function<void()> &increase) {
        auto *container = new QWidget(group);
        auto *line = new QHBoxLayout(container);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        const auto makeButton = [container](const QString &label) {
            auto *button = new QPushButton(label, container);
            button->setObjectName(QStringLiteral("touchAdjustButton"));
            button->setFixedSize(54, 46);
            button->setFocusPolicy(Qt::NoFocus);
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(350);
            button->setAutoRepeatInterval(120);
            return button;
        };
        auto *minus = makeButton(QStringLiteral("−"));
        auto *plus = makeButton(QStringLiteral("+"));
        line->addWidget(minus);
        line->addWidget(spin, 1);
        line->addWidget(plus);
        grid->addWidget(new QLabel(text, group), row, 0);
        grid->addWidget(container, row, 1);
        connect(minus, &QPushButton::clicked, this, decrease);
        connect(plus, &QPushButton::clicked, this, increase);
    };
    addAdjustRow(0, QStringLiteral("目标推理帧率"), m_npuFpsSpin,
                 [this]() { m_npuFpsSpin->stepDown(); },
                 [this]() { m_npuFpsSpin->stepUp(); });
    addAdjustRow(1, QStringLiteral("置信度阈值"), m_confSpin,
                 [this]() { m_confSpin->stepDown(); },
                 [this]() { m_confSpin->stepUp(); });
    addAdjustRow(2, QStringLiteral("NMS阈值"), m_nmsSpin,
                 [this]() { m_nmsSpin->stepDown(); },
                 [this]() { m_nmsSpin->stepUp(); });
    m_btnAiApply = new QPushButton(QStringLiteral("应用参数并推理"), group);
    m_btnAiStop = new QPushButton(QStringLiteral("停止检测"), group);
    m_btnAiApply->setMinimumHeight(50);
    m_btnAiStop->setMinimumHeight(50);
    grid->addWidget(m_btnAiApply, 3, 0);
    grid->addWidget(m_btnAiStop, 3, 1);
    connect(m_btnAiApply, &QPushButton::clicked, this, &ControlPanelWindow::onApplyAiConfigClicked);
    connect(m_btnAiStop, &QPushButton::clicked, this,
            &ControlPanelWindow::onStopAiInferenceClicked);
    layout->addWidget(group);
    auto *applyHint = new QLabel(QStringLiteral("点击 − / + 调整数值，支持长按连续调节；修改后点击“应用参数并推理”生效。"), page);
    applyHint->setWordWrap(true);
    layout->addWidget(applyHint);
    auto *note =
        new QLabel(QStringLiteral("输入：视频显示页选定的PCIe总览或CH1～CH9；PTZ预览暂不送入检测。"
                                  "\n模型：无人机专用YOLOv8n，640×640；先选单路验证，九宫格总览的小"
                                  "目标更难检出。\n检测与PTZ手动控制独立，本版不自动转动云台。"),
                   page);
    note->setWordWrap(true);
    layout->addWidget(note);
    m_aiRuntimeLabel = new QLabel(page);
    m_aiRuntimeLabel->setWordWrap(true);
    layout->addWidget(m_aiRuntimeLabel);
    m_aiCurrentResultLabel = new QLabel(page);
    m_aiCurrentResultLabel->setWordWrap(true);
    layout->addWidget(m_aiCurrentResultLabel);
    layout->addStretch();
    return page;
}
void ControlPanelWindow::loadAiSettings()
{
    QSettings s(aiSettingsPath(), QSettings::IniFormat);
    m_npuFpsSpin->setValue(s.value("drone/fps", 5).toInt());
    m_confSpin->setValue(s.value("drone/confidence", 0.35).toDouble());
    m_nmsSpin->setValue(s.value("drone/nms", 0.45).toDouble());
    // Safe startup: video/PTZ controls remain available before model validation.
    NpuTaskConfig c;
    c.enabled = false;
    c.targetFps = m_npuFpsSpin->value();
    c.confidence = m_confSpin->value();
    c.nmsThreshold = m_nmsSpin->value();
    m_display->setNpuConfig(c);
}
void ControlPanelWindow::saveAiSettings(const NpuTaskConfig &c)
{
    QSettings s(aiSettingsPath(), QSettings::IniFormat);
    s.setValue("drone/fps", c.targetFps);
    s.setValue("drone/confidence", c.confidence);
    s.setValue("drone/nms", c.nmsThreshold);
}
void ControlPanelWindow::onApplyAiConfigClicked()
{
    if (!m_display)
        return;
    if (!m_display->isRunning())
    {
        m_lastError = QStringLiteral("请先在视频显示页启动HDMI视频链路");
        updateStatusText();
        return;
    }
    auto c = m_display->npuConfig();
    c.enabled = true;
    c.targetFps = m_npuFpsSpin->value();
    c.confidence = m_confSpin->value();
    c.nmsThreshold = m_nmsSpin->value();
    m_display->setNpuConfig(c);
    saveAiSettings(c);
    clearAiCurrentResult();
    m_lastAction = QStringLiteral("启动无人机检测");
    onAiUiTimer();
}
void ControlPanelWindow::onStopAiInferenceClicked()
{
    if (m_display)
        m_display->setNpuEnabled(false);
    clearAiCurrentResult();
    onAiUiTimer();
}
void ControlPanelWindow::clearAiCurrentResult()
{
    if (m_aiCurrentResultLabel)
        m_aiCurrentResultLabel->setText(QStringLiteral("当前无检测结果"));
}
void ControlPanelWindow::updateAiCurrentResult(const NpuFrameResult &r)
{
    if (!m_aiCurrentResultLabel)
        return;
    if (!r.enabled || !r.ready)
    {
        clearAiCurrentResult();
        return;
    }
    QString text = QStringLiteral("当前帧：%1｜无人机：%2（坐标为选路640×360）")
                       .arg(r.frameId)
                       .arg(r.drones.size());
    for (std::size_t i = 0; i < std::min<std::size_t>(r.drones.size(), 5); ++i)
    {
        const auto &b = r.drones[i];
        text += QStringLiteral("\nDRONE %1  x:%2 y:%3 w:%4 h:%5")
                    .arg(b.score, 0, 'f', 2)
                    .arg(b.x, 0, 'f', 0)
                    .arg(b.y, 0, 'f', 0)
                    .arg(b.w, 0, 'f', 0)
                    .arg(b.h, 0, 'f', 0);
    }
    m_aiCurrentResultLabel->setText(text);
}
void ControlPanelWindow::updateAiPanel(const NpuFrameResult &r)
{
    m_aiEnabled = r.enabled;
    m_aiReady = r.ready;
    m_aiFps = r.fps;
    m_aiDroneCount = int(r.drones.size());
    m_aiStatus = QString::fromStdString(r.error.empty() ? r.status : r.error);
    m_aiLastMs = r.lastMs;
    m_aiPreMs = r.preMs;
    m_aiProcessed = r.processed;
    m_aiDropped = r.dropped;
    if (m_aiRuntimeLabel)
        m_aiRuntimeLabel->setText(
            QStringLiteral("状态：%1｜通道：%2｜实际/目标FPS：%3/%4\n预处理：%5 ms｜NPU：%6 "
                           "ms｜后处理：%7 ms｜处理合计：%8 ms\n完成：%9｜覆盖丢帧：%10")
                .arg(m_aiStatus)
                .arg(r.sourceChannel)
                .arg(r.fps, 0, 'f', 1)
                .arg(r.targetFps)
                .arg(r.preMs, 0, 'f', 1)
                .arg(r.rknnRunMs, 0, 'f', 1)
                .arg(r.postMs, 0, 'f', 1)
                .arg(r.lastMs, 0, 'f', 1)
                .arg(r.processed)
                .arg(r.dropped));
    updateAiCurrentResult(r);
}
void ControlPanelWindow::onAiUiTimer()
{
    if (m_display)
        updateAiPanel(m_display->latestNpuResult());
    updateStatusText();
}
void ControlPanelWindow::onDisplayAiStatsUpdated(bool enabled, bool ready, double fps,
                                                 int droneCount, QString status)
{
    m_aiEnabled = enabled;
    m_aiReady = ready;
    m_aiFps = fps;
    m_aiDroneCount = droneCount;
    m_aiStatus = status;
    updateStatusText();
}
