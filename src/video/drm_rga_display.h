#ifndef DRM_RGA_DISPLAY_H
#define DRM_RGA_DISPLAY_H

#include "npu/npu_result_types.h"
#include "npu/npu_task_config.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class NpuInferenceWorker;

class DrmRgaDisplay : public QObject
{
    Q_OBJECT

public:
    explicit DrmRgaDisplay(QObject *parent = nullptr);
    ~DrmRgaDisplay();

    bool startDisplay();
    void stopDisplay();
    bool isRunning() const;

    void setOsdEnabled(bool enable);
    bool osdEnabled() const;

    void setDisplayMode(int mode);
    int displayMode() const;

    // 仅影响下一次 startDisplay。运行中修改分辨率需要先停止显示。
    bool setOutputResolution(int width, int height);
    int outputWidth() const;
    int outputHeight() const;

    void setNpuConfig(const NpuTaskConfig &config);
    NpuTaskConfig npuConfig() const;

    void setNpuEnabled(bool enable);
    void setNpuTargetFps(int fps);
    void setNpuConfidence(float confidence);
    void setNpuNmsThreshold(float threshold);
    void setNpuTask(NpuTaskType task);
    NpuFrameResult latestNpuResult() const;
    std::vector<NpuFrameResult> npuResultsAfter(uint64_t frameId, std::size_t maxResults = 64) const;

    QString lastErrorString() const;

signals:
    void stateChanged(QString state);
    void errorOccurred(QString msg);

    void statsUpdated(double readFps,
                      double validFps,
                      double rgaFps,
                      double flipFps,
                      double rgaAvgUs,
                      int failCount,
                      int npuLoad,
                      double tempC);
    void aiStatsUpdated(bool enabled,
                        bool ready,
                        double fps,
                        int plateCount,
                        int pedestrianCount,
                        QString status);

private:
    void workerMain();
    void setLastError(const QString &msg);

private:
    std::thread m_thread;
    std::atomic<bool> m_running {false};
    std::atomic<bool> m_stopReq {false};

    std::atomic<bool> m_osdEnabled {true};
    std::atomic<int>  m_displayMode {0};

    std::atomic<int> m_reqWidth {1920};
    std::atomic<int> m_reqHeight {1080};

    std::unique_ptr<NpuInferenceWorker> m_npuWorker;

    QString m_lastError = "none";
};

#endif // DRM_RGA_DISPLAY_H
