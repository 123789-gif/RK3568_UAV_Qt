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
