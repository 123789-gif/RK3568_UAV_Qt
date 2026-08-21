#ifndef DRM_RGA_DISPLAY_H
#define DRM_RGA_DISPLAY_H

#include "npu/npu_result_types.h"
#include "npu/npu_task_config.h"
#include "video/video_route_config.h"

#include <QObject>
#include <QString>
#include <QImage>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
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

    // AI 结果叠加层：检测框、车牌大字标签等。性能 HUD 独立显示。
    void setOsdEnabled(bool enable);
    bool osdEnabled() const;

    void setPerfHudEnabled(bool enable);
    bool perfHudEnabled() const;

    void setDisplayMode(int mode);
    int displayMode() const;

    void setVideoRoute(VideoRoute route);
    VideoRoute videoRoute() const;

    // 异步请求整帧FPGA预处理切换。实际ioctl由持有视频fd的控制线程执行。
    void requestFpgaPreprocess(int mode, std::uint32_t param0 = 0U);

    // 异步请求网口H2/UDP视频输出开关；同样复用当前视频fd。
    void requestEthernetOutput(bool enable);

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
    void setViolationConfig(const ViolationRuleConfig &config);
    ViolationRuleConfig violationConfig() const;
    void setWrongWayConfig(const VehicleDirectionConfig &config);
    VehicleDirectionConfig wrongWayConfig() const;
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
                      int cpuLoad,
                      int npuLoad,
                      double tempC,
                      double osdAvgUs,
                      double plateOsdAvgUs,
                      double npuPrepareAvgUs);
    void previewFrameReady(QImage image,
                           quint64 frameId,
                           int sourceChannel,
                           quint64 sourceRevision);

    void aiStatsUpdated(bool enabled,
                        bool ready,
                        double fps,
                        int plateCount,
                        int pedestrianCount,
                        int vehicleCount,
                        QString status);

    void fpgaControlInfoUpdated(bool available,
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

    void fpgaApplyFinished(bool success,
                           quint32 requestedMode,
                           quint32 activeMode,
                           quint32 sequence,
                           quint32 ackSequence,
                           quint32 status,
                           quint32 errorStatus,
                           quint32 latencyUs,
                           quint32 lastApplyFrame,
                           QString message);

    void ethernetControlInfoUpdated(bool available,
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

    void ethernetApplyFinished(bool success,
                               bool requestedEnable,
                               bool activeEnable,
                               quint32 sequence,
                               quint32 ackSequence,
                               quint32 status,
                               quint32 errorStatus,
                               quint32 latencyUs,
                               quint32 lastApplyFrame,
                               QString message);

private:
    void workerMain();
    void setLastError(const QString &msg);

private:
    std::mutex m_lifecycleMutex;
    std::thread m_thread;
    std::atomic<bool> m_running {false};
    std::atomic<bool> m_stopReq {false};

    std::atomic<bool> m_osdEnabled {true};
    std::atomic<bool> m_perfHudEnabled {true};
    std::atomic<int>  m_displayMode {0};
    // [63:8] route revision, [7:0] VideoRoute value.
    std::atomic<uint64_t> m_videoRouteState {(1ULL << 8)};

    std::atomic<int> m_reqWidth {1920};
    std::atomic<int> m_reqHeight {1080};

    std::mutex m_fpgaControlMutex;
    std::condition_variable m_fpgaControlCv;
    bool m_fpgaRequestPending = false;
    std::uint32_t m_fpgaRequestedMode = 0;
    std::uint32_t m_fpgaRequestedParam0 = 0;

    bool m_ethRequestPending = false;
    bool m_ethRequestedEnable = true;

    std::unique_ptr<NpuInferenceWorker> m_npuWorker;

    mutable std::mutex m_lastErrorMutex;
    QString m_lastError = "none";
};

#endif // DRM_RGA_DISPLAY_H
