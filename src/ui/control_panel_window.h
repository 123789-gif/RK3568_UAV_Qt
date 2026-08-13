#ifndef CONTROL_PANEL_WINDOW_H
#define CONTROL_PANEL_WINDOW_H

#include "video/drm_rga_display.h"
#include "npu/npu_task_config.h"
#include "ptz/v4l2_device_detector.h"
#include "npu/violation_rule_config.h"

#include <QImage>
#include <QMainWindow>
#include <QString>
#include <QTimer>

class QLabel;
class QStackedWidget;
class QPushButton;
class QComboBox;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QButtonGroup;
class FanController;
class PtzCaptureWorker;
class PtzControlWorker;
class ViolationPreviewWidget;

class ControlPanelWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ControlPanelWindow(QWidget *parent = nullptr);
    ~ControlPanelWindow();

    void showWindows();

private slots:
    void onStartStopClicked();
    void onOsdClicked();
    void onApplyVideoConfigClicked();
    void onApplyAiConfigClicked();
    void onStopAiInferenceClicked();
    void onStatusTimer();
    void onAiUiTimer();

    void onDisplayStateChanged(QString state);
    void onDisplayError(QString msg);
    void onDisplayStatsUpdated(double readFps,
                               double validFps,
                               double rgaFps,
                               double flipFps,
                               double rgaAvgUs,
                               int failCount,
                               int cpuLoad,
                               int npuLoad,
                               double tempC,
                               double osdAvgUs,
                               double plateOsdAvgUs,
                               double npuPrepareAvgUs);
    void onDisplayAiStatsUpdated(bool enabled,
                                 bool ready,
                                 double fps,
                                 int plateCount,
                                 int pedestrianCount,
                                 int vehicleCount,
                                 QString status);

    void onFpgaControlInfoUpdated(bool available,
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
                                  QString message);
    void onFpgaApplyFinished(bool success,
                             quint32 requestedMode,
                             quint32 activeMode,
                             quint32 sequence,
                             quint32 ackSequence,
                             quint32 status,
                             quint32 errorStatus,
                             quint32 latencyUs,
                             quint32 lastApplyFrame,
                             QString message);

    void onEthernetControlInfoUpdated(bool available,
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
                                      QString message);
    void onEthernetApplyFinished(bool success,
                                 bool requestedEnable,
                                 bool activeEnable,
                                 quint32 sequence,
                                 quint32 ackSequence,
                                 quint32 status,
                                 quint32 errorStatus,
                                 quint32 latencyUs,
                                 quint32 lastApplyFrame,
                                 QString message);

    void onApplyPtzProfileClicked();
    void onPtzPreviewClicked();
    void onPtzControlClicked();
    void onPtzFrameTimer();
    void onPtzDeviceMonitorTimer();
    void onPtzCaptureDeviceLost(QString videoDevice, QString reason);
    void onSavePtzSnapshotClicked();
    void onViewPtzSnapshotsClicked();
    void onPtzCaptureStateChanged(bool running, QString message);
    void onPtzCaptureStatsUpdated(double captureFps,
                                  double decodeFps,
                                  double decodeAvgUs,
                                  int failCount);
    void onPtzControlStateChanged(bool ready, QString message);
    void onPtzControlError(QString message);

    void onFanPowerClicked(bool checked);
    void onFanPwmChanged(int value);

private:
    void setupControlUi();
    QWidget *createNavPanel();
    QWidget *createVideoPage();
    QWidget *createPtzPage();
    QWidget *createFpgaPage();
    QWidget *createCpuPage();
    QWidget *createAiPage();
    QWidget *createViolationPage();
    QWidget *createStatusPage();
    void setCurrentPage(int index);
    void updateFanUi();
    void requestFpgaMode(quint32 mode);
    void updateFpgaModeButtons();
    void requestEthernetOutput(bool enable);
    void updateEthernetButtons();
    void updateFpgaUiAvailability();
    void updateStatusText();
    void updateAiPanel(const NpuFrameResult &result);
    void updateAiCurrentResult(const NpuFrameResult &result);
    void clearAiCurrentResult();
    void requestAiTask(NpuTaskType task);
    void updateAiTaskButtons();
    void loadAiSettings();
    void saveAiSettings(const NpuTaskConfig &config);
    void loadViolationSettings();
    bool saveViolationSettings();
    void startViolationDetection();
    void stopViolationDetection(bool restorePreviousConfig = true);
    void updateViolationPanel(const NpuFrameResult &result);
    void updateViolationButtons();
    void loadPtzSettings();
    void savePtzSettings(PtzProfileSelection selection);
    bool applyPtzProfileSelection(bool restartActiveServices);
    bool reconcilePtzDevice(bool userInitiated, bool forceRestart = false);
    bool switchToResolvedPtzDevice(const PtzResolvedDevice &resolved,
                                   bool forceRestart,
                                   QString *errorMessage = nullptr);
    void enterPtzWaitingState(const QString &message, bool clearActiveDevice);
    void clearPtzPreview(const QString &message);
    void updatePtzServiceButtons();
    void updatePtzProfileSummary(const QString &overrideText = QString());
    void applyStyle();

private:
    DrmRgaDisplay *m_display = nullptr;
    PtzCaptureWorker *m_ptzCapture = nullptr;
    PtzControlWorker *m_ptzControl = nullptr;
    FanController *m_fanController = nullptr;

    QTimer *m_statusTimer = nullptr;
    QTimer *m_ptzFrameTimer = nullptr;
    QTimer *m_ptzDeviceMonitorTimer = nullptr;
    QTimer *m_aiUiTimer = nullptr;

    QStackedWidget *m_stack = nullptr;
    QLabel *m_bottomStatus = nullptr;
    QLabel *m_statusDetail = nullptr;

    QPushButton *m_btnStartStop = nullptr;
    QPushButton *m_btnOsd = nullptr;

    QComboBox *m_ptzProfileCombo = nullptr;
    QPushButton *m_btnApplyPtzProfile = nullptr;
    QLabel *m_ptzProfileSummaryLabel = nullptr;
    QLabel *m_ptzPreviewLabel = nullptr;
    QLabel *m_ptzVideoStatusLabel = nullptr;
    QLabel *m_ptzControlStatusLabel = nullptr;
    QLabel *m_ptzStatsLabel = nullptr;
    QPushButton *m_btnPtzPreview = nullptr;
    QPushButton *m_btnPtzControl = nullptr;
    QSlider *m_ptzPanSpeedSlider = nullptr;
    QSpinBox *m_ptzPanSpeedSpin = nullptr;
    QSlider *m_ptzTiltSpeedSlider = nullptr;
    QSpinBox *m_ptzTiltSpeedSpin = nullptr;

    QPushButton *m_btnFanPower = nullptr;
    QPushButton *m_btnFanPwmMinus = nullptr;
    QPushButton *m_btnFanPwmPlus = nullptr;
    QSpinBox *m_fanPwmSpin = nullptr;
    QLabel *m_fanStatusLabel = nullptr;

    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_outputModeCombo = nullptr;
    QButtonGroup *m_videoRouteGroup = nullptr;
    QLabel *m_videoRouteSummaryLabel = nullptr;
    QButtonGroup *m_fpgaPreprocButtonGroup = nullptr;
    QLabel *m_fpgaCapabilityLabel = nullptr;
    QLabel *m_fpgaCurrentModeLabel = nullptr;
    QLabel *m_fpgaApplyStatusLabel = nullptr;
    QButtonGroup *m_ethernetOutputButtonGroup = nullptr;
    QLabel *m_ethernetInfoLabel = nullptr;
    QLabel *m_ethernetCurrentStateLabel = nullptr;
    QLabel *m_ethernetApplyStatusLabel = nullptr;
    QButtonGroup *m_npuTaskButtonGroup = nullptr;
    QButtonGroup *m_plateModelButtonGroup = nullptr;
    QButtonGroup *m_platePostprocessButtonGroup = nullptr;
    QPushButton *m_btnAiApply = nullptr;
    QPushButton *m_btnAiStop = nullptr;
    QSpinBox *m_npuFpsSpin = nullptr;
    QDoubleSpinBox *m_confSpin = nullptr;
    QDoubleSpinBox *m_nmsSpin = nullptr;
    QLabel *m_aiConfigSummaryLabel = nullptr;
    QLabel *m_aiRuntimeLabel = nullptr;
    QLabel *m_aiCurrentResultLabel = nullptr;

    ViolationPreviewWidget *m_violationPreview = nullptr;
    QButtonGroup *m_violationLightModeGroup = nullptr;
    QPushButton *m_btnViolationStart = nullptr;
    QPushButton *m_btnViolationStop = nullptr;
    QLabel *m_violationEditStatusLabel = nullptr;
    QLabel *m_violationRuntimeLabel = nullptr;
    QLabel *m_violationResultLabel = nullptr;

    bool m_displayRunning = false;
    bool m_violationRunning = false;
    bool m_violationConfigDirty = false;
    ViolationRuleConfig m_violationConfig;
    NpuTaskConfig m_violationPreviousAiConfig;
    bool m_violationPreviousAiConfigValid = false;
    std::uint64_t m_violationLastEventId = 0;
    bool m_osdEnabled = true;

    bool m_fpgaControlAvailable = false;
    bool m_fpgaApplyPending = false;
    quint32 m_fpgaRequestedMode = 0;
    quint32 m_fpgaIpVersion = 0;
    quint32 m_fpgaCapability = 0;
    quint32 m_fpgaAlgorithmBitmap = 0;
    quint32 m_fpgaActiveMode = 0;
    quint32 m_fpgaAckSequence = 0;
    quint32 m_fpgaStatus = 0;
    quint32 m_fpgaErrorStatus = 0;
    quint32 m_fpgaFrameCount = 0;
    quint32 m_fpgaLastApplyFrame = 0;
    bool m_ethernetControlAvailable = false;
    bool m_ethernetApplyPending = false;
    bool m_ethernetRequestedEnable = true;
    bool m_ethernetActiveEnable = true;
    quint32 m_ethernetIpVersion = 0;
    quint32 m_ethernetCapability = 0;
    quint32 m_ethernetOutputWidth = 0;
    quint32 m_ethernetOutputHeight = 0;
    quint32 m_ethernetOutputFps = 0;
    quint32 m_ethernetDestinationIpv4 = 0;
    quint32 m_ethernetDestinationPort = 0;
    quint32 m_ethernetAckSequence = 0;
    quint32 m_ethernetStatus = 0;
    quint32 m_ethernetErrorStatus = 0;
    quint32 m_ethernetFrameCount = 0;
    quint32 m_ethernetLastApplyFrame = 0;
    bool m_ptzCaptureRunning = false;
    PtzResolvedDevice m_activePtzDevice;
    PtzProfileSelection m_requestedPtzSelection = PtzProfileSelection::Auto;
    PtzProfileSelection m_lastSuccessfulPtzSelection = PtzProfileSelection::Auto;
    bool m_ptzPreviewDesired = true;
    bool m_ptzControlDesired = true;
    bool m_ptzSwitchInProgress = false;
    bool m_ptzWaitingForDevice = false;
    QString m_lastPtzMonitorMessage;
    int m_ptzDisplayAspectWidth = 16;
    int m_ptzDisplayAspectHeight = 9;
    bool m_ptzControlReady = false;

    int m_seconds = 0;
    int m_currentPage = 0;

    QString m_displayState = "RGA/DRM idle";
    QString m_ptzCaptureState = "PTZ视频未启动";
    QString m_ptzControlState = "RS232控制未启动";
    QString m_lastError = "none";
    QString m_lastAction = "none";

    double m_readFps = 0.0;
    double m_validFps = 0.0;
    double m_rgaFps = 0.0;
    double m_flipFps = 0.0;
    double m_rgaAvgUs = 0.0;
    int m_failCount = 0;
    int m_cpuLoad = -1;
    int m_npuLoad = -1;
    double m_tempC = -1.0;
    double m_osdAvgUs = 0.0;
    double m_plateOsdAvgUs = 0.0;
    double m_npuPrepareAvgUs = 0.0;
    bool m_aiEnabled = false;
    bool m_aiReady = false;
    double m_aiFps = 0.0;
    int m_aiPlateCount = 0;
    int m_aiPedestrianCount = 0;
    int m_aiVehicleCount = 0;
    QString m_aiStatus = "off";
    NpuTaskType m_aiTask = NpuTaskType::Pedestrian;
    NpuTaskType m_aiRequestedTask = NpuTaskType::Pedestrian;
    NpuPlateModelProfile m_aiRequestedPlateProfile = NpuPlateModelProfile::V291Candidate;
    NpuPlateModelProfile m_aiActivePlateProfile = NpuPlateModelProfile::V291Candidate;
    NpuPlatePostprocess m_aiRequestedPlatePostprocess = NpuPlatePostprocess::Stable;
    bool m_aiActivePlateProfileValid = false;
    bool m_aiPlateOptionsDirty = false;
    std::uint64_t m_aiRequestedConfigRevision = 0;
    bool m_aiTaskSwitchPending = false;
    double m_aiLastMs = 0.0;
    double m_aiPreMs = 0.0;
    int m_aiProcessed = 0;
    int m_aiDropped = 0;

    double m_ptzCaptureFps = 0.0;
    double m_ptzDecodeFps = 0.0;
    double m_ptzDecodeAvgUs = 0.0;
    int m_ptzDecodeFail = 0;

    QImage m_lastPtzFrame;
    const QString m_snapshotMountPoint = QStringLiteral("/mnt/ssd");
    const QString m_snapshotRoot = QStringLiteral("/mnt/ssd/ptz_snapshots");
    const QString m_manualSnapshotRoot = QStringLiteral("/mnt/ssd/ptz_snapshots/manual");
    const QString m_detectedSnapshotRoot = QStringLiteral("/mnt/ssd/ptz_snapshots/detected");
    QString m_lastSnapshotPath = QStringLiteral("none");
};

#endif // CONTROL_PANEL_WINDOW_H
