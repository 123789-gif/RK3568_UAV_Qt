#ifndef CONTROL_PANEL_WINDOW_H
#define CONTROL_PANEL_WINDOW_H

#include "video/drm_rga_display.h"
#include "npu/npu_task_config.h"

#include <QImage>
#include <QMainWindow>
#include <QString>
#include <QTimer>

class QLabel;
class QStackedWidget;
class QPushButton;
class QComboBox;
class QCheckBox;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class FanController;
class PtzCaptureWorker;
class PtzControlWorker;

QT_BEGIN_NAMESPACE
namespace Ui {
class ControlPanelWindow;
}
QT_END_NAMESPACE

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
    void onApplyFpgaConfigClicked();
    void onApplyAiConfigClicked();
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
                               int npuLoad,
                               double tempC);
    void onDisplayAiStatsUpdated(bool enabled,
                                 bool ready,
                                 double fps,
                                 int plateCount,
                                 int pedestrianCount,
                                 QString status);

    void onPtzPreviewClicked();
    void onPtzControlClicked();
    void onPtzFrameTimer();
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
    void onApplyFanConfigClicked();

private:
    void setupControlUi();
    QWidget *createNavPanel();
    QWidget *createVideoPage();
    QWidget *createPtzPage();
    QWidget *createFpgaPage();
    QWidget *createCpuPage();
    QWidget *createAiPage();
    QWidget *createStatusPage();
    void setCurrentPage(int index);
    void updateFanUi();
    void updateStatusText();
    void updateAiPanel(const NpuFrameResult &result);
    void appendAiResultRows(const NpuFrameResult &result);
    void clearAiResultTable();
    void loadAiSettings();
    void saveAiSettings(const NpuTaskConfig &config);
    void applyStyle();

private:
    Ui::ControlPanelWindow *ui = nullptr;

    DrmRgaDisplay *m_display = nullptr;
    PtzCaptureWorker *m_ptzCapture = nullptr;
    PtzControlWorker *m_ptzControl = nullptr;
    FanController *m_fanController = nullptr;

    QTimer *m_statusTimer = nullptr;
    QTimer *m_ptzFrameTimer = nullptr;
    QTimer *m_aiUiTimer = nullptr;

    QStackedWidget *m_stack = nullptr;
    QLabel *m_bottomStatus = nullptr;
    QLabel *m_statusDetail = nullptr;

    QPushButton *m_btnStartStop = nullptr;
    QPushButton *m_btnOsd = nullptr;

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
    QSlider *m_fanPwmSlider = nullptr;
    QSpinBox *m_fanPwmSpin = nullptr;
    QLabel *m_fanStatusLabel = nullptr;

    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_outputModeCombo = nullptr;
    QComboBox *m_sourceCombo = nullptr;
    QComboBox *m_ch1PreprocCombo = nullptr;
    QComboBox *m_ch2PreprocCombo = nullptr;
    QComboBox *m_ch3PreprocCombo = nullptr;
    QComboBox *m_ch4PreprocCombo = nullptr;
    QCheckBox *m_npuEnableCheck = nullptr;
    QComboBox *m_npuTaskCombo = nullptr;
    QSpinBox *m_npuFpsSpin = nullptr;
    QDoubleSpinBox *m_confSpin = nullptr;
    QDoubleSpinBox *m_nmsSpin = nullptr;
    QLabel *m_aiConfigSummaryLabel = nullptr;
    QLabel *m_aiRuntimeLabel = nullptr;
    QTableWidget *m_aiResultTable = nullptr;

    bool m_displayRunning = false;
    bool m_osdEnabled = true;
    bool m_ptzCaptureRunning = false;
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
    int m_npuLoad = -1;
    double m_tempC = -1.0;
    bool m_aiEnabled = false;
    bool m_aiReady = false;
    double m_aiFps = 0.0;
    int m_aiPlateCount = 0;
    int m_aiPedestrianCount = 0;
    QString m_aiStatus = "off";
    NpuTaskType m_aiTask = NpuTaskType::Pedestrian;
    double m_aiLastMs = 0.0;
    double m_aiPreMs = 0.0;
    int m_aiProcessed = 0;
    int m_aiDropped = 0;
    uint64_t m_lastAiResultFrameId = 0;
    int m_aiHistoryDepth = 50;

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
