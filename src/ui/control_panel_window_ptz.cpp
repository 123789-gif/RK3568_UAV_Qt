#include "ui/control_panel_window.h"
#include "ui/control_panel_window_internal.h"

QWidget *ControlPanelWindow::createPtzPage()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(8, 6, 8, 6);
    pageLayout->setSpacing(10);

    QGroupBox *previewGroup = new QGroupBox("PTZ相机预览与配置", page);
    QVBoxLayout *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(10, 20, 10, 10);
    previewLayout->setSpacing(6);

    QHBoxLayout *profileRow = new QHBoxLayout();
    QLabel *profileLabel = new QLabel(QStringLiteral("相机配置"), previewGroup);
    m_ptzProfileCombo = new QComboBox(previewGroup);
    m_ptzProfileCombo->addItem(QStringLiteral("自动识别"),
                               static_cast<int>(PtzProfileSelection::Auto));
    m_ptzProfileCombo->addItem(QStringLiteral("当前HD USB PTZ"),
                               static_cast<int>(PtzProfileSelection::HdUsbPtz));
    m_ptzProfileCombo->addItem(QStringLiteral("Sony EVI-D100 + MS210x"),
                               static_cast<int>(PtzProfileSelection::SonyEviD100));
    m_ptzProfileCombo->setMinimumHeight(42);
    m_ptzProfileCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_btnApplyPtzProfile = new QPushButton(QStringLiteral("应用配置"), previewGroup);
    m_btnApplyPtzProfile->setObjectName("ptzButton");
    m_btnApplyPtzProfile->setMinimumHeight(42);
    m_btnApplyPtzProfile->setMinimumWidth(120);

    profileRow->addWidget(profileLabel);
    profileRow->addWidget(m_ptzProfileCombo, 1);
    profileRow->addWidget(m_btnApplyPtzProfile);
    previewLayout->addLayout(profileRow);

    m_ptzProfileSummaryLabel = new QLabel(
                QStringLiteral("尚未解析PTZ相机配置"), previewGroup);
    m_ptzProfileSummaryLabel->setObjectName("ptzState");
    m_ptzProfileSummaryLabel->setWordWrap(true);
    previewLayout->addWidget(m_ptzProfileSummaryLabel);

    m_ptzPreviewLabel = new QLabel("等待自动识别PTZ视频设备...", previewGroup);
    m_ptzPreviewLabel->setObjectName("ptzPreview");
    m_ptzPreviewLabel->setAlignment(Qt::AlignCenter);
    m_ptzPreviewLabel->setMinimumSize(600, 285);
    m_ptzPreviewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewLayout->addWidget(m_ptzPreviewLabel, 1);

    m_ptzVideoStatusLabel = new QLabel("PTZ视频未启动", previewGroup);
    m_ptzVideoStatusLabel->setObjectName("ptzState");
    m_ptzVideoStatusLabel->setWordWrap(true);
    previewLayout->addWidget(m_ptzVideoStatusLabel);

    m_ptzStatsLabel = new QLabel("CAP:0.0  DEC:0.0  JPEG:0 us  FAIL:0", previewGroup);
    m_ptzStatsLabel->setObjectName("ptzStats");
    previewLayout->addWidget(m_ptzStatsLabel);

    pageLayout->addWidget(previewGroup, 1);

    QGroupBox *controlGroup = new QGroupBox("RS232 VISCA 控制", page);
    controlGroup->setFixedWidth(340);
    QVBoxLayout *controlLayout = new QVBoxLayout(controlGroup);
    controlLayout->setContentsMargins(10, 20, 10, 10);
    controlLayout->setSpacing(5);

    QHBoxLayout *serviceRow = new QHBoxLayout();
    m_btnPtzPreview = new QPushButton("启动预览", controlGroup);
    m_btnPtzControl = new QPushButton("连接控制", controlGroup);
    m_btnPtzPreview->setObjectName("ptzButton");
    m_btnPtzControl->setObjectName("ptzButton");
    serviceRow->addWidget(m_btnPtzPreview);
    serviceRow->addWidget(m_btnPtzControl);
    controlLayout->addLayout(serviceRow);

    // 水平和俯仰速度范围不同，因此分别设置。
    // 水平VISCA速度等级：1～24；俯仰VISCA速度等级：1～20。
    QGridLayout *speedGrid = new QGridLayout();
    speedGrid->setHorizontalSpacing(7);
    speedGrid->setVerticalSpacing(4);

    QLabel *panSpeedLabel = new QLabel("水平速度", controlGroup);
    QLabel *tiltSpeedLabel = new QLabel("俯仰速度", controlGroup);

    m_ptzPanSpeedSlider = new QSlider(Qt::Horizontal, controlGroup);
    m_ptzPanSpeedSlider->setRange(1, 24);
    m_ptzPanSpeedSlider->setValue(5);
    m_ptzPanSpeedSlider->setSingleStep(1);
    m_ptzPanSpeedSlider->setPageStep(4);

    m_ptzPanSpeedSpin = new QSpinBox(controlGroup);
    m_ptzPanSpeedSpin->setRange(1, 24);
    m_ptzPanSpeedSpin->setValue(5);
    m_ptzPanSpeedSpin->setFixedWidth(68);
    m_ptzPanSpeedSpin->setAlignment(Qt::AlignCenter);

    m_ptzTiltSpeedSlider = new QSlider(Qt::Horizontal, controlGroup);
    m_ptzTiltSpeedSlider->setRange(1, 20);
    m_ptzTiltSpeedSlider->setValue(5);
    m_ptzTiltSpeedSlider->setSingleStep(1);
    m_ptzTiltSpeedSlider->setPageStep(4);

    m_ptzTiltSpeedSpin = new QSpinBox(controlGroup);
    m_ptzTiltSpeedSpin->setRange(1, 20);
    m_ptzTiltSpeedSpin->setValue(5);
    m_ptzTiltSpeedSpin->setFixedWidth(68);
    m_ptzTiltSpeedSpin->setAlignment(Qt::AlignCenter);

    speedGrid->addWidget(panSpeedLabel, 0, 0);
    speedGrid->addWidget(m_ptzPanSpeedSlider, 0, 1);
    speedGrid->addWidget(m_ptzPanSpeedSpin, 0, 2);
    speedGrid->addWidget(tiltSpeedLabel, 1, 0);
    speedGrid->addWidget(m_ptzTiltSpeedSlider, 1, 1);
    speedGrid->addWidget(m_ptzTiltSpeedSpin, 1, 2);
    speedGrid->setColumnStretch(1, 1);
    controlLayout->addLayout(speedGrid);

    // 标准九宫格：八方向持续运动，中间为紧急停止。
    QGridLayout *directionGrid = new QGridLayout();
    directionGrid->setSpacing(5);

    QPushButton *upLeft = new QPushButton("左上", controlGroup);
    QPushButton *up = new QPushButton("上", controlGroup);
    QPushButton *upRight = new QPushButton("右上", controlGroup);
    QPushButton *left = new QPushButton("左", controlGroup);
    QPushButton *stop = new QPushButton("停止", controlGroup);
    QPushButton *right = new QPushButton("右", controlGroup);
    QPushButton *downLeft = new QPushButton("左下", controlGroup);
    QPushButton *down = new QPushButton("下", controlGroup);
    QPushButton *downRight = new QPushButton("右下", controlGroup);

    const QList<QPushButton *> directionButtons = {
        upLeft, up, upRight,
        left, stop, right,
        downLeft, down, downRight
    };
    for (QPushButton *button : directionButtons) {
        button->setObjectName("ptzButton");
    }

    directionGrid->addWidget(upLeft, 0, 0);
    directionGrid->addWidget(up, 0, 1);
    directionGrid->addWidget(upRight, 0, 2);
    directionGrid->addWidget(left, 1, 0);
    directionGrid->addWidget(stop, 1, 1);
    directionGrid->addWidget(right, 1, 2);
    directionGrid->addWidget(downLeft, 2, 0);
    directionGrid->addWidget(down, 2, 1);
    directionGrid->addWidget(downRight, 2, 2);
    controlLayout->addLayout(directionGrid);

    QPushButton *home = new QPushButton("云台回中", controlGroup);
    home->setObjectName("ptzButton");
    controlLayout->addWidget(home);

    QGridLayout *zoomGrid = new QGridLayout();
    zoomGrid->setSpacing(5);
    QPushButton *zoomTele = new QPushButton("变焦 +", controlGroup);
    QPushButton *zoomWide = new QPushButton("变焦 -", controlGroup);
    zoomTele->setObjectName("ptzButton");
    zoomWide->setObjectName("ptzButton");
    zoomGrid->addWidget(zoomTele, 0, 0);
    zoomGrid->addWidget(zoomWide, 0, 1);
    controlLayout->addLayout(zoomGrid);

    QPushButton *saveSnapshot = new QPushButton("保存PTZ截图", controlGroup);
    QPushButton *viewSnapshots = new QPushButton("查看抓拍", controlGroup);
    saveSnapshot->setObjectName("ptzButton");
    viewSnapshots->setObjectName("ptzButton");
    controlLayout->addWidget(saveSnapshot);
    controlLayout->addWidget(viewSnapshots);

    controlLayout->addStretch(1);

    m_ptzControlStatusLabel = new QLabel("RS232控制未启动", controlGroup);
    m_ptzControlStatusLabel->setObjectName("ptzState");
    m_ptzControlStatusLabel->setWordWrap(true);
    controlLayout->addWidget(m_ptzControlStatusLabel);

    pageLayout->addWidget(controlGroup, 0);

    connect(m_btnApplyPtzProfile, &QPushButton::clicked,
            this, &ControlPanelWindow::onApplyPtzProfileClicked);
    connect(m_btnPtzPreview, &QPushButton::clicked,
            this, &ControlPanelWindow::onPtzPreviewClicked);
    connect(m_btnPtzControl, &QPushButton::clicked,
            this, &ControlPanelWindow::onPtzControlClicked);
    connect(saveSnapshot, &QPushButton::clicked,
            this, &ControlPanelWindow::onSavePtzSnapshotClicked);
    connect(viewSnapshots, &QPushButton::clicked,
            this, &ControlPanelWindow::onViewPtzSnapshotsClicked);

    // 滑块和数值框双向同步；数值变化立即更新控制线程中的速度参数。
    connect(m_ptzPanSpeedSlider, &QSlider::valueChanged,
            m_ptzPanSpeedSpin, &QSpinBox::setValue);
    connect(m_ptzPanSpeedSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_ptzPanSpeedSlider, &QSlider::setValue);
    connect(m_ptzPanSpeedSlider, &QSlider::valueChanged,
            m_ptzControl, &PtzControlWorker::setPanSpeed);

    connect(m_ptzTiltSpeedSlider, &QSlider::valueChanged,
            m_ptzTiltSpeedSpin, &QSpinBox::setValue);
    connect(m_ptzTiltSpeedSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_ptzTiltSpeedSlider, &QSlider::setValue);
    connect(m_ptzTiltSpeedSlider, &QSlider::valueChanged,
            m_ptzControl, &PtzControlWorker::setTiltSpeed);

    // 八方向均采用“按下运动、松开停止”，避免触摸按键产生持续运动。
    connect(up, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveUp);
    connect(up, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(down, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveDown);
    connect(down, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(left, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveLeft);
    connect(left, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(right, &QPushButton::pressed, m_ptzControl, &PtzControlWorker::moveRight);
    connect(right, &QPushButton::released, m_ptzControl, &PtzControlWorker::stopPanTilt);

    connect(upLeft, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveUpLeft);
    connect(upLeft, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(upRight, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveUpRight);
    connect(upRight, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(downLeft, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveDownLeft);
    connect(downLeft, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(downRight, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::moveDownRight);
    connect(downRight, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopPanTilt);

    connect(stop, &QPushButton::clicked,
            m_ptzControl, &PtzControlWorker::stopPanTilt);
    connect(home, &QPushButton::clicked,
            m_ptzControl, &PtzControlWorker::home);

    connect(zoomTele, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::zoomTele);
    connect(zoomTele, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopZoom);
    connect(zoomWide, &QPushButton::pressed,
            m_ptzControl, &PtzControlWorker::zoomWide);
    connect(zoomWide, &QPushButton::released,
            m_ptzControl, &PtzControlWorker::stopZoom);

    return page;
}
void ControlPanelWindow::loadPtzSettings()
{
    if (!m_ptzProfileCombo) {
        return;
    }

    QSettings settings(ptzSettingsPath(), QSettings::IniFormat);
    const int selectionValue = settings.value(
                QStringLiteral("ptz/profile_selection"),
                static_cast<int>(PtzProfileSelection::Auto)).toInt();
    const int lastSuccessfulValue = settings.value(
                QStringLiteral("ptz/last_successful_profile"),
                static_cast<int>(PtzProfileSelection::Auto)).toInt();

    int index = m_ptzProfileCombo->findData(selectionValue);
    if (index < 0) {
        index = m_ptzProfileCombo->findData(
                    static_cast<int>(PtzProfileSelection::Auto));
    }
    m_ptzProfileCombo->setCurrentIndex(qMax(0, index));
    m_requestedPtzSelection = static_cast<PtzProfileSelection>(
                m_ptzProfileCombo->currentData().toInt());

    if (lastSuccessfulValue >= static_cast<int>(PtzProfileSelection::HdUsbPtz)
            && lastSuccessfulValue <= static_cast<int>(PtzProfileSelection::SonyEviD100)) {
        m_lastSuccessfulPtzSelection = static_cast<PtzProfileSelection>(lastSuccessfulValue);
    } else {
        m_lastSuccessfulPtzSelection = PtzProfileSelection::Auto;
    }

    updatePtzProfileSummary(QStringLiteral("等待应用“%1”")
                            .arg(m_ptzProfileCombo->currentText()));
}
void ControlPanelWindow::savePtzSettings(PtzProfileSelection selection)
{
    QSettings settings(ptzSettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ptz/profile_selection"),
                      static_cast<int>(selection));
    settings.sync();
}
void ControlPanelWindow::clearPtzPreview(const QString &message)
{
    if (m_ptzCapture) {
        m_ptzCapture->clearLatestFrame();
    }
    m_lastPtzFrame = QImage();
    if (m_ptzPreviewLabel) {
        m_ptzPreviewLabel->clear();
        m_ptzPreviewLabel->setText(message);
    }
}
void ControlPanelWindow::updatePtzServiceButtons()
{
    if (m_btnPtzPreview) {
        if (m_ptzCapture && m_ptzCapture->isRunning()) {
            m_btnPtzPreview->setText(QStringLiteral("停止预览"));
        } else if (m_ptzPreviewDesired) {
            m_btnPtzPreview->setText(QStringLiteral("取消等待"));
        } else {
            m_btnPtzPreview->setText(QStringLiteral("启动预览"));
        }
    }

    if (m_btnPtzControl) {
        if (m_ptzControl && m_ptzControl->isRunning()) {
            m_btnPtzControl->setText(QStringLiteral("断开控制"));
        } else if (m_ptzControlDesired) {
            m_btnPtzControl->setText(QStringLiteral("取消等待"));
        } else {
            m_btnPtzControl->setText(QStringLiteral("连接控制"));
        }
    }
}
void ControlPanelWindow::enterPtzWaitingState(const QString &message,
                                               bool clearActiveDevice)
{
    m_ptzWaitingForDevice = true;

    if (clearActiveDevice) {
        m_ptzSwitchInProgress = true;
        if (m_ptzControl && m_ptzControl->isRunning()) {
            m_ptzControl->stopControl();
        }
        if (m_ptzCapture && m_ptzCapture->isRunning()) {
            m_ptzCapture->stopCapture();
        }
        m_ptzSwitchInProgress = false;
        m_activePtzDevice = PtzResolvedDevice();
        clearPtzPreview(message);
    }

    if (m_ptzVideoStatusLabel) {
        m_ptzVideoStatusLabel->setText(message);
    }
    updatePtzProfileSummary(message);
    updatePtzServiceButtons();
    updateStatusText();
}
void ControlPanelWindow::updatePtzProfileSummary(const QString &overrideText)
{
    if (!m_ptzProfileSummaryLabel) {
        return;
    }

    if (!overrideText.isEmpty()) {
        m_ptzProfileSummaryLabel->setText(overrideText);
        return;
    }

    if (!m_activePtzDevice.valid) {
        const QString requested = ptzProfileSelectionName(m_requestedPtzSelection);
        m_ptzProfileSummaryLabel->setText(
                    m_ptzWaitingForDevice
                    ? QStringLiteral("等待PTZ设备：%1").arg(requested)
                    : QStringLiteral("尚未解析PTZ相机配置"));
        return;
    }

    const PtzCameraProfile &profile = m_activePtzDevice.profile;
    const QString usbText = QStringLiteral("%1:%2")
            .arg(profile.usbVendorId, 4, 16, QLatin1Char('0'))
            .arg(profile.usbProductId, 4, 16, QLatin1Char('0'));
    QString text = QStringLiteral("当前：%1 | %2 | USB %3 | %4 %5x%6@%7 | 显示%8:%9")
            .arg(profile.displayName,
                 m_activePtzDevice.videoDevice,
                 usbText,
                 v4l2PixelFormatName(profile.pixelFormat))
            .arg(profile.captureWidth)
            .arg(profile.captureHeight)
            .arg(profile.captureFps)
            .arg(profile.displayAspectWidth)
            .arg(profile.displayAspectHeight);

    if (m_requestedPtzSelection != PtzProfileSelection::Auto
            && m_requestedPtzSelection != profile.selection) {
        text += QStringLiteral(" | 等待切换到：%1")
                .arg(ptzProfileSelectionName(m_requestedPtzSelection));
    }
    m_ptzProfileSummaryLabel->setText(text);
}
bool ControlPanelWindow::switchToResolvedPtzDevice(const PtzResolvedDevice &resolved,
                                                    bool forceRestart,
                                                    QString *errorMessage)
{
    if (!resolved.valid || !m_ptzCapture || !m_ptzControl) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无效的PTZ切换目标");
        }
        return false;
    }

    const bool sameEndpoint = samePtzEndpoint(m_activePtzDevice, resolved);
    if (sameEndpoint && !forceRestart) {
        m_activePtzDevice = resolved;
        m_ptzWaitingForDevice = false;
        m_ptzDisplayAspectWidth = qMax(1, resolved.profile.displayAspectWidth);
        m_ptzDisplayAspectHeight = qMax(1, resolved.profile.displayAspectHeight);

        if (m_ptzPreviewDesired && !m_ptzCapture->isRunning()) {
            m_ptzCapture->startCapture();
        }
        if (m_ptzControlDesired && !m_ptzControl->isRunning()) {
            m_ptzControl->startControl();
        }
        updatePtzProfileSummary();
        updatePtzServiceButtons();
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    m_ptzSwitchInProgress = true;
    const PtzResolvedDevice previousDevice = m_activePtzDevice;

    PtzResolvedDevice previousAvailable = previousDevice;
    QString previousResolveError;
    const bool previousStillPresent = previousDevice.valid
            && V4l2DeviceDetector::isPresent(previousDevice,
                                              &previousResolveError);

    // 切换前先停止VISCA，stopControl会发送Pan/Tilt Stop与Zoom Stop。
    if (m_ptzControl->isRunning()) {
        m_ptzControl->stopControl();
    }
    if (m_ptzCapture->isRunning()) {
        m_ptzCapture->stopCapture();
    }

    clearPtzPreview(QStringLiteral("正在切换到%1……")
                    .arg(resolved.profile.displayName));

    QString configError;
    const bool captureConfigured = m_ptzCapture->setCaptureConfig(
                resolved, &configError);
    const bool controlConfigured = captureConfigured
            && m_ptzControl->setCameraProfile(resolved.profile, &configError);

    if (!captureConfigured || !controlConfigured) {
        if (previousStillPresent) {
            QString rollbackError;
            (void)m_ptzCapture->setCaptureConfig(previousAvailable, &rollbackError);
            (void)m_ptzControl->setCameraProfile(previousAvailable.profile,
                                                  &rollbackError);
            m_activePtzDevice = previousAvailable;
            if (m_ptzPreviewDesired) {
                m_ptzCapture->startCapture();
            }
            if (m_ptzControlDesired) {
                m_ptzControl->startControl();
            }
        } else {
            m_activePtzDevice = PtzResolvedDevice();
        }

        m_ptzSwitchInProgress = false;
        const QString text = QStringLiteral("PTZ配置应用失败：%1").arg(configError);
        if (errorMessage) {
            *errorMessage = text;
        }
        m_lastError = text;
        updatePtzProfileSummary(text);
        updatePtzServiceButtons();
        updateStatusText();
        return false;
    }

    m_activePtzDevice = resolved;
    m_ptzWaitingForDevice = false;
    m_ptzDisplayAspectWidth = qMax(1, resolved.profile.displayAspectWidth);
    m_ptzDisplayAspectHeight = qMax(1, resolved.profile.displayAspectHeight);
    m_lastSuccessfulPtzSelection = resolved.profile.selection;

    QSettings settings(ptzSettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ptz/last_successful_profile"),
                      static_cast<int>(m_lastSuccessfulPtzSelection));
    settings.sync();

    qInfo() << "PTZ hot-switch applied:"
            << ptzEndpointText(resolved)
            << "previewDesired=" << m_ptzPreviewDesired
            << "controlDesired=" << m_ptzControlDesired;

    clearPtzPreview(QStringLiteral("等待%1视频帧……")
                    .arg(resolved.profile.displayName));

    if (m_ptzPreviewDesired) {
        m_ptzCapture->startCapture();
    }
    if (m_ptzControlDesired) {
        m_ptzControl->startControl();
    }

    m_ptzSwitchInProgress = false;
    m_lastAction = QStringLiteral("切换PTZ视频源：%1")
            .arg(resolved.profile.displayName);
    m_lastError = QStringLiteral("none");
    updatePtzProfileSummary();
    updatePtzServiceButtons();
    updateStatusText();
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}
bool ControlPanelWindow::reconcilePtzDevice(bool userInitiated,
                                            bool forceRestart)
{
    if (m_ptzSwitchInProgress || !m_ptzCapture || !m_ptzControl) {
        return false;
    }

    PtzResolvedDevice refreshedActive = m_activePtzDevice;
    QString activeError;
    const bool activePresent = m_activePtzDevice.valid
            && V4l2DeviceDetector::isPresent(m_activePtzDevice, &activeError);

    PtzResolvedDevice target;
    QString targetError;
    bool targetFound = false;

    if (m_requestedPtzSelection == PtzProfileSelection::Auto) {
        // 自动模式下，只要当前源仍在线就保持当前源，避免第二台设备插入时
        // 无故跳转。当前源消失后，再按上次成功源和唯一在线源进行选择。
        if (activePresent) {
            target = refreshedActive;
            targetFound = true;
        } else if (m_lastSuccessfulPtzSelection != PtzProfileSelection::Auto) {
            targetFound = V4l2DeviceDetector::resolve(
                        m_lastSuccessfulPtzSelection,
                        &target,
                        &targetError,
                        nullptr);
        }

        if (!targetFound) {
            targetFound = V4l2DeviceDetector::resolve(
                        PtzProfileSelection::Auto,
                        &target,
                        &targetError,
                        nullptr);
        }
    } else if (activePresent
               && m_activePtzDevice.profile.selection == m_requestedPtzSelection) {
        target = refreshedActive;
        targetFound = true;
    } else {
        targetFound = V4l2DeviceDetector::resolve(
                    m_requestedPtzSelection,
                    &target,
                    &targetError,
                    nullptr);
    }

    if (!targetFound) {
        QString waitText;
        if (activePresent) {
            waitText = QStringLiteral("等待“%1”接入；当前保持%2")
                    .arg(ptzProfileSelectionName(m_requestedPtzSelection),
                         refreshedActive.profile.displayName);
            m_activePtzDevice = refreshedActive;
            m_ptzWaitingForDevice = true;
            updatePtzProfileSummary(waitText);
            if (m_ptzVideoStatusLabel) {
                m_ptzVideoStatusLabel->setText(waitText);
            }
        } else {
            waitText = QStringLiteral("等待PTZ设备：%1")
                    .arg(ptzProfileSelectionName(m_requestedPtzSelection));
            enterPtzWaitingState(waitText, true);
        }

        if (userInitiated) {
            m_lastAction = waitText;
            m_lastError = targetError.isEmpty()
                    ? QStringLiteral("none")
                    : QStringLiteral("PTZ配置：%1").arg(targetError);
        }
        if (m_lastPtzMonitorMessage != waitText) {
            qInfo() << "PTZ hotplug waiting:" << waitText
                    << "detail=" << targetError;
            m_lastPtzMonitorMessage = waitText;
        }
        updatePtzServiceButtons();
        updateStatusText();
        return false;
    }

    m_lastPtzMonitorMessage.clear();
    QString switchError;
    const bool ok = switchToResolvedPtzDevice(target,
                                               forceRestart,
                                               &switchError);
    if (!ok && userInitiated && !switchError.isEmpty()) {
        m_lastError = switchError;
    }
    return ok;
}
bool ControlPanelWindow::applyPtzProfileSelection(bool restartActiveServices)
{
    if (!m_ptzProfileCombo || !m_ptzCapture || !m_ptzControl) {
        return false;
    }

    m_requestedPtzSelection = static_cast<PtzProfileSelection>(
                m_ptzProfileCombo->currentData().toInt());
    savePtzSettings(m_requestedPtzSelection);
    m_lastAction = QStringLiteral("请求PTZ配置：%1")
            .arg(ptzProfileSelectionName(m_requestedPtzSelection));

    return reconcilePtzDevice(true, restartActiveServices);
}
void ControlPanelWindow::onApplyPtzProfileClicked()
{
    const bool anyServiceRunning = (m_ptzCapture && m_ptzCapture->isRunning())
            || (m_ptzControl && m_ptzControl->isRunning());
    (void)applyPtzProfileSelection(anyServiceRunning);
}
void ControlPanelWindow::onPtzPreviewClicked()
{
    if (!m_ptzCapture) {
        return;
    }

    if (m_ptzPreviewDesired) {
        m_ptzPreviewDesired = false;
        if (m_ptzCapture->isRunning()) {
            m_ptzCapture->stopCapture();
        }
        clearPtzPreview(QStringLiteral("PTZ视频预览已停止"));
        m_lastAction = QStringLiteral("停止PTZ视频预览/取消自动恢复");
    } else {
        m_ptzPreviewDesired = true;
        m_lastAction = QStringLiteral("启动PTZ视频预览/等待设备自动恢复");
        (void)reconcilePtzDevice(true, false);
    }
    updatePtzServiceButtons();
    updateStatusText();
}
void ControlPanelWindow::onPtzControlClicked()
{
    if (!m_ptzControl) {
        return;
    }

    if (m_ptzControlDesired) {
        m_ptzControlDesired = false;
        if (m_ptzControl->isRunning()) {
            m_ptzControl->stopControl();
        }
        m_lastAction = QStringLiteral("断开RS232 PTZ控制/取消自动恢复");
    } else {
        m_ptzControlDesired = true;
        m_lastAction = QStringLiteral("连接RS232 PTZ控制/等待设备自动恢复");
        (void)reconcilePtzDevice(true, false);
    }
    updatePtzServiceButtons();
    updateStatusText();
}
void ControlPanelWindow::onPtzDeviceMonitorTimer()
{
    if (m_ptzSwitchInProgress) {
        return;
    }

    // 只有用户仍希望预览/控制，或当前已有活动配置时才持续监测。
    if (!m_ptzPreviewDesired && !m_ptzControlDesired
            && !m_activePtzDevice.valid) {
        return;
    }
    (void)reconcilePtzDevice(false, false);
}
void ControlPanelWindow::onPtzCaptureDeviceLost(QString videoDevice,
                                                 QString reason)
{
    if (m_ptzSwitchInProgress || !m_ptzPreviewDesired) {
        return;
    }

    m_lastError = QStringLiteral("PTZ视频设备断开：%1").arg(reason);
    clearPtzPreview(QStringLiteral("检测到%1断开，等待重新枚举PTZ设备……")
                    .arg(videoDevice));
    updatePtzServiceButtons();
    updateStatusText();

    // 给udev/V4L2节点留出稳定时间，随后由统一重协调流程恢复。
    QTimer::singleShot(500, this, [this]() {
        (void)reconcilePtzDevice(false, false);
    });
}
void ControlPanelWindow::onPtzFrameTimer()
{
    if (!m_ptzCapture || !m_ptzPreviewLabel) {
        return;
    }

    QImage frame;
    if (!m_ptzCapture->takeLatestFrame(&frame) || frame.isNull()) {
        return;
    }

    // 保存原始预览帧，用于截图；界面缩放后的QPixmap不作为截图源。
    m_lastPtzFrame = frame.copy();

    const QSize target = m_ptzPreviewLabel->contentsRect().size();
    if (target.width() <= 0 || target.height() <= 0) {
        return;
    }

    QSize displaySize = target;
    const qreal sourceAspect = qreal(m_ptzDisplayAspectWidth)
            / qreal(m_ptzDisplayAspectHeight);
    const qreal targetAspect = qreal(target.width()) / qreal(target.height());
    if (targetAspect > sourceAspect) {
        displaySize.setWidth(qMax(1, qRound(target.height() * sourceAspect)));
    } else {
        displaySize.setHeight(qMax(1, qRound(target.width() / sourceAspect)));
    }

    // 720x480模拟视频使用非方形像素，按Profile的4:3比例显示；
    // HD USB PTZ仍按16:9显示。QLabel居中后自动形成必要黑边。
    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
                displaySize,
                Qt::IgnoreAspectRatio,
                Qt::FastTransformation);
    m_ptzPreviewLabel->setPixmap(pixmap);
}
void ControlPanelWindow::onSavePtzSnapshotClicked()
{
    if (m_lastPtzFrame.isNull()) {
        m_lastError = QStringLiteral("PTZ截图：当前没有可保存的预览帧");
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        updateStatusText();
        return;
    }

    QString storageError;
    if (!prepareSnapshotStorage(m_snapshotMountPoint,
                                m_manualSnapshotRoot,
                                m_detectedSnapshotRoot,
                                &storageError)) {
        m_lastError = QStringLiteral("PTZ截图：%1").arg(storageError);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        updateStatusText();
        return;
    }

    const QString fileName = QStringLiteral("PTZ_%1.jpg")
            .arg(QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString filePath = QDir(m_manualSnapshotRoot).filePath(fileName);

    QString errorMessage;
    if (!saveImageAsJpeg(m_lastPtzFrame, filePath, 92, &errorMessage)) {
        m_lastError = QStringLiteral("PTZ截图：%1").arg(errorMessage);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        updateStatusText();
        return;
    }

    m_lastSnapshotPath = filePath;
    m_lastAction = QStringLiteral("PTZ截图已保存：%1").arg(filePath);
    m_lastError = QStringLiteral("none");
    if (m_ptzVideoStatusLabel) {
        m_ptzVideoStatusLabel->setText(
                    QStringLiteral("截图已保存：%1").arg(filePath));
    }
    updateStatusText();
}
void ControlPanelWindow::onViewPtzSnapshotsClicked()
{
    QString storageError;
    if (!prepareSnapshotStorage(m_snapshotMountPoint,
                                m_manualSnapshotRoot,
                                m_detectedSnapshotRoot,
                                &storageError)) {
        m_lastError = QStringLiteral("查看抓拍：%1").arg(storageError);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(m_lastError);
        }
        QMessageBox::warning(this,
                             QStringLiteral("查看抓拍"),
                             m_lastError);
        updateStatusText();
        return;
    }

    const QFileInfoList snapshots = collectManualSnapshotFiles(
                m_manualSnapshotRoot,
                m_snapshotRoot);
    if (snapshots.isEmpty()) {
        const QString message = QStringLiteral(
                    "尚未找到手动抓拍图片。\n保存目录：%1")
                .arg(m_manualSnapshotRoot);
        if (m_ptzVideoStatusLabel) {
            m_ptzVideoStatusLabel->setText(message);
        }
        QMessageBox::information(this,
                                 QStringLiteral("查看抓拍"),
                                 message);
        m_lastAction = QStringLiteral("查看手动抓拍：目录为空");
        updateStatusText();
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("查看手动抓拍"));
    dialog.resize(1040, 650);
    dialog.setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    QLabel *imageLabel = new QLabel(QStringLiteral("正在加载图片..."), &dialog);
    imageLabel->setObjectName("ptzPreview");
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(820, 460);
    imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(imageLabel, 1);

    QLabel *fileLabel = new QLabel(&dialog);
    fileLabel->setObjectName("ptzState");
    fileLabel->setAlignment(Qt::AlignCenter);
    fileLabel->setWordWrap(true);
    layout->addWidget(fileLabel);

    QHBoxLayout *buttonRow = new QHBoxLayout();
    QPushButton *previousButton = new QPushButton(QStringLiteral("上一张"), &dialog);
    QPushButton *nextButton = new QPushButton(QStringLiteral("下一张"), &dialog);
    QPushButton *closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    previousButton->setObjectName("ptzButton");
    nextButton->setObjectName("ptzButton");
    closeButton->setObjectName("ptzButton");
    buttonRow->addWidget(previousButton);
    buttonRow->addWidget(nextButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    int currentIndex = 0;
    QImage currentImage;

    const auto refreshViewer = [&]() {
        if (currentIndex < 0 || currentIndex >= snapshots.size()) {
            return;
        }

        const QFileInfo &fileInfo = snapshots.at(currentIndex);
        QString loadError;
        QImage loaded;
        if (!loadJpegImage(fileInfo.absoluteFilePath(), &loaded, &loadError)) {
            currentImage = QImage();
            imageLabel->setPixmap(QPixmap());
            imageLabel->setText(QStringLiteral("图片读取失败\n%1")
                                .arg(loadError));
        } else {
            currentImage = loaded;
            QSize targetSize = imageLabel->contentsRect().size();
            if (targetSize.width() < 100 || targetSize.height() < 100) {
                targetSize = QSize(900, 500);
            }
            imageLabel->setText(QString());
            imageLabel->setPixmap(QPixmap::fromImage(currentImage).scaled(
                                      targetSize,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
        }

        fileLabel->setText(QStringLiteral(
                               "%1 / %2    %3\n拍摄时间：%4")
                           .arg(currentIndex + 1)
                           .arg(snapshots.size())
                           .arg(fileInfo.fileName())
                           .arg(fileInfo.lastModified().toString(
                                    QStringLiteral("yyyy-MM-dd HH:mm:ss"))));

        previousButton->setEnabled(currentIndex > 0);
        nextButton->setEnabled(currentIndex + 1 < snapshots.size());
    };

    connect(previousButton, &QPushButton::clicked, &dialog, [&]() {
        if (currentIndex > 0) {
            --currentIndex;
            refreshViewer();
        }
    });
    connect(nextButton, &QPushButton::clicked, &dialog, [&]() {
        if (currentIndex + 1 < snapshots.size()) {
            ++currentIndex;
            refreshViewer();
        }
    });
    connect(closeButton, &QPushButton::clicked,
            &dialog, &QDialog::accept);

    dialog.layout()->activate();
    refreshViewer();
    dialog.exec();

    m_lastAction = QStringLiteral("查看手动抓拍：共%1张")
            .arg(snapshots.size());
    m_lastError = QStringLiteral("none");
    updateStatusText();
}
void ControlPanelWindow::onPtzCaptureStateChanged(bool running, QString message)
{
    m_ptzCaptureRunning = running;
    m_ptzCaptureState = message;

    if (m_ptzVideoStatusLabel) {
        m_ptzVideoStatusLabel->setText(message);
    }
    if (!running && m_ptzPreviewLabel && message != "PTZ视频已停止") {
        m_ptzPreviewLabel->setText(message);
    }

    updatePtzServiceButtons();
    updateStatusText();

    // 非人工停止、非切换过程中的异常退出，由热插拔协调器重新解析和恢复。
    if (!running && m_ptzPreviewDesired && !m_ptzSwitchInProgress
            && message != QStringLiteral("PTZ视频已停止")) {
        QTimer::singleShot(700, this, [this]() {
            (void)reconcilePtzDevice(false, false);
        });
    }
}
void ControlPanelWindow::onPtzCaptureStatsUpdated(double captureFps,
                                                  double decodeFps,
                                                  double decodeAvgUs,
                                                  int failCount)
{
    m_ptzCaptureFps = captureFps;
    m_ptzDecodeFps = decodeFps;
    m_ptzDecodeAvgUs = decodeAvgUs;
    m_ptzDecodeFail = failCount;

    if (m_ptzStatsLabel) {
        m_ptzStatsLabel->setText(
                    QString("CAP:%1  DEC:%2  JPEG:%3 us  FAIL:%4")
                    .arg(captureFps, 0, 'f', 1)
                    .arg(decodeFps, 0, 'f', 1)
                    .arg(decodeAvgUs, 0, 'f', 0)
                    .arg(failCount));
    }

    updateStatusText();
}
void ControlPanelWindow::onPtzControlStateChanged(bool ready, QString message)
{
    m_ptzControlReady = ready;
    m_ptzControlState = message;

    if (m_ptzControlStatusLabel) {
        m_ptzControlStatusLabel->setText(message);
    }
    updatePtzServiceButtons();
    updateStatusText();
}
void ControlPanelWindow::onPtzControlError(QString message)
{
    m_lastError = QString("PTZ控制：%1").arg(message);
    if (m_ptzControlStatusLabel) {
        m_ptzControlStatusLabel->setText(m_lastError);
    }
    updateStatusText();
}
