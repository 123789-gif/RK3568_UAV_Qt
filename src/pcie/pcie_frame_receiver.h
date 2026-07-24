#ifndef PCIE_FRAME_RECEIVER_H
#define PCIE_FRAME_RECEIVER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class PcieFrameReceiver
{
public:
    static constexpr int kFrameWidth = 1920;
    static constexpr int kFrameHeight = 1080;
    static constexpr int kBytesPerPixel = 2;
    static constexpr std::size_t kFrameBytes =
            static_cast<std::size_t>(kFrameWidth) *
            static_cast<std::size_t>(kFrameHeight) *
            static_cast<std::size_t>(kBytesPerPixel);
    static constexpr std::size_t kFrameMapBytes =
            ((kFrameBytes + 4095U) / 4096U) * 4096U;
    static constexpr int kBufferCount = 4;

    struct LinkInfo {
        bool valid = false;
        unsigned int vendorId = 0;
        unsigned int deviceId = 0;
        unsigned int linkSpeed = 0;
        unsigned int linkWidth = 0;
        unsigned int mps = 0;
        unsigned int mrrs = 0;
    };

    struct FrameView {
        const std::uint8_t *data = nullptr;
        std::size_t bytes = 0;
        int bufferIndex = -1;
    };


    struct FpgaCaps {
        bool valid = false;
        std::uint32_t ipVersion = 0;
        std::uint32_t capability = 0;
        std::uint32_t algorithmBitmap = 0;
        std::uint32_t streamCount = 0;
    };

    struct FpgaStatus {
        bool valid = false;
        std::uint32_t status = 0;
        std::uint32_t activeMode = 0;
        std::uint32_t ackSequence = 0;
        std::uint32_t errorStatus = 0;
        std::uint32_t frameCount = 0;
        std::uint32_t lastApplyFrame = 0;
    };

    struct FpgaApplyResult {
        bool success = false;
        int systemError = 0;
        std::uint32_t requestedMode = 0;
        std::uint32_t sequence = 0;
        std::uint32_t activeMode = 0;
        std::uint32_t ackSequence = 0;
        std::uint32_t status = 0;
        std::uint32_t errorStatus = 0;
        std::uint32_t latencyUs = 0;
    };

    struct EthernetCaps {
        bool valid = false;
        std::uint32_t ipVersion = 0;
        std::uint32_t capability = 0;
        std::uint32_t outputWidth = 0;
        std::uint32_t outputHeight = 0;
        std::uint32_t outputFps = 0;
        std::uint32_t destinationIpv4 = 0;
        std::uint32_t destinationPort = 0;
    };

    struct EthernetStatus {
        bool valid = false;
        std::uint32_t status = 0;
        bool requestedEnable = false;
        bool activeEnable = false;
        std::uint32_t ackSequence = 0;
        std::uint32_t errorStatus = 0;
        std::uint32_t frameCount = 0;
        std::uint32_t lastApplyFrame = 0;
    };

    struct EthernetApplyResult {
        bool success = false;
        int systemError = 0;
        bool requestedEnable = false;
        std::uint32_t sequence = 0;
        bool activeEnable = false;
        std::uint32_t ackSequence = 0;
        std::uint32_t status = 0;
        std::uint32_t errorStatus = 0;
        std::uint32_t latencyUs = 0;
        std::uint32_t lastApplyFrame = 0;
    };

    enum class WaitResult {
        FrameReady,
        NoFrame,
        Error
    };

    PcieFrameReceiver();
    ~PcieFrameReceiver();

    PcieFrameReceiver(const PcieFrameReceiver &) = delete;
    PcieFrameReceiver &operator=(const PcieFrameReceiver &) = delete;

    bool open(const std::string &devicePath = "/dev/pango_pci_driver");
    bool startStream();
    WaitResult waitNextFrame(FrameView &frame);
    void stopStream();
    void close();

    bool isOpen() const;
    bool isReady() const;
    bool pioControlActive() const;

    // FPGA预处理控制复用当前视频接收fd，不额外open设备节点。
    bool getFpgaCaps(FpgaCaps &caps, std::string *errorMessage = nullptr) const;
    bool getFpgaStatus(FpgaStatus &status, std::string *errorMessage = nullptr) const;
    bool applyFpgaPreprocess(std::uint32_t mode,
                             std::uint32_t param0,
                             std::uint32_t timeoutMs,
                             FpgaApplyResult &result,
                             std::string *errorMessage = nullptr) const;

    // Ethernet H2/UDP视频输出控制同样复用当前视频接收fd。
    bool getEthernetCaps(EthernetCaps &caps,
                         std::string *errorMessage = nullptr) const;
    bool getEthernetStatus(EthernetStatus &status,
                           std::string *errorMessage = nullptr) const;
    bool setEthernetOutput(bool enable,
                           std::uint32_t timeoutMs,
                           EthernetApplyResult &result,
                           std::string *errorMessage = nullptr) const;

    const LinkInfo &linkInfo() const;
    const std::string &lastError() const;
    const std::string &lastWarning() const;

private:
    bool readLinkInfo();
    bool configureDmaBuffers();
    bool mapFrameBuffers();
    bool mapBar0();
    void unmapBar0();
    void setError(const std::string &message);
    void setWarning(const std::string &message);

private:
    int m_deviceFd = -1;
    std::array<void *, kBufferCount> m_buffers {};

    int m_memFd = -1;
    void *m_barMapBase = nullptr;
    std::size_t m_barMapBytes = 0;
    volatile std::uint8_t *m_barRegs = nullptr;

    bool m_ready = false;
    bool m_streamStarted = false;
    LinkInfo m_linkInfo;
    std::string m_devicePath;
    std::string m_lastError;
    std::string m_lastWarning;
};

#endif // PCIE_FRAME_RECEIVER_H
