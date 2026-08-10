#include "pcie/pcie_frame_receiver.h"
#include "pcie/pango_pci_uapi.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sstream>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr std::uint32_t kPioStartStopOffset = 0x000U;
constexpr std::uint32_t kPioStartValue = 0xffffffe5U;
constexpr std::uint32_t kPioStopValue = 0xffffff00U;

std::string errnoMessageWithCode(const char *operation, int errorCode)
{
    std::ostringstream os;
    os << operation << " failed: errno=" << errorCode
       << " (" << std::strerror(errorCode) << ')';
    return os.str();
}

std::string errnoMessage(const char *operation)
{
    return errnoMessageWithCode(operation, errno);
}

void assignError(std::string *target, const std::string &message)
{
    if (target != nullptr) {
        *target = message;
    }
}

} // namespace

PcieFrameReceiver::PcieFrameReceiver() = default;

PcieFrameReceiver::~PcieFrameReceiver()
{
    close();
}

bool PcieFrameReceiver::open(const std::string &devicePath)
{
    close();

    m_lastError.clear();
    m_lastWarning.clear();
    m_linkInfo = LinkInfo {};
    m_devicePath = devicePath;

    m_deviceFd = ::open(m_devicePath.c_str(), O_RDWR);
    if (m_deviceFd < 0) {
        const std::string operation = std::string("open ") + m_devicePath;
        setError(errnoMessage(operation.c_str()));
        return false;
    }

    // 读取链路信息失败不阻断视频接收，保持原工程行为。
    readLinkInfo();

    if (!configureDmaBuffers() || !mapFrameBuffers()) {
        close();
        return false;
    }

    m_ready = true;
    return true;
}

bool PcieFrameReceiver::startStream()
{
    if (!m_ready || m_deviceFd < 0) {
        setError("PCIe receiver is not ready");
        return false;
    }

    if (m_streamStarted) {
        return true;
    }

    // 原工程允许 BAR0 映射失败后继续接收，因此这里仅记录 warning。
    if (!mapBar0()) {
        return true;
    }

    volatile std::uint32_t *reg = reinterpret_cast<volatile std::uint32_t *>(
                const_cast<std::uint8_t *>(
                    reinterpret_cast<volatile std::uint8_t *>(m_barRegs) + kPioStartStopOffset));
    *reg = kPioStartValue;
    m_streamStarted = true;
    return true;
}

PcieFrameReceiver::WaitResult PcieFrameReceiver::waitNextFrame(FrameView &frame)
{
    frame = FrameView {};

    if (!m_ready || m_deviceFd < 0) {
        setError("waitNextFrame called while receiver is not ready");
        return WaitResult::Error;
    }

    char dummy = 0;
    const ssize_t result = ::read(m_deviceFd, &dummy, sizeof(dummy));

    // 驱动将帧有效标志和缓冲区编号编码在 read() 返回值中。
    // EINTR/EAGAIN 属于可恢复的“暂时无帧”；设备断开和 I/O 错误必须上报，
    // 否则显示线程会在永久故障后无提示空转。
    if (result < 0) {
        const int errorCode = errno;
        if (errorCode == EINTR || errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
            return WaitResult::NoFrame;
        }
        setError(errnoMessageWithCode("read PCIe frame", errorCode));
        return WaitResult::Error;
    }

    if (result == 0 || ((result & 0x1) == 0)) {
        return WaitResult::NoFrame;
    }

    const int index = static_cast<int>((result >> 1) & 0x3);
    if (index < 0 || index >= kBufferCount || m_buffers[static_cast<std::size_t>(index)] == nullptr) {
        setError("PCIe driver returned an invalid DMA buffer index");
        return WaitResult::Error;
    }

    frame.data = static_cast<const std::uint8_t *>(m_buffers[static_cast<std::size_t>(index)]);
    frame.bytes = kFrameBytes;
    frame.bufferIndex = index;
    return WaitResult::FrameReady;
}

void PcieFrameReceiver::stopStream()
{
    if (m_streamStarted && m_barRegs != nullptr) {
        volatile std::uint32_t *reg = reinterpret_cast<volatile std::uint32_t *>(
                    const_cast<std::uint8_t *>(
                        reinterpret_cast<volatile std::uint8_t *>(m_barRegs) + kPioStartStopOffset));
        *reg = kPioStopValue;
    }

    m_streamStarted = false;
    unmapBar0();
}

void PcieFrameReceiver::close()
{
    stopStream();

    for (void *&buffer : m_buffers) {
        if (buffer != nullptr && buffer != MAP_FAILED) {
            ::munmap(buffer, kFrameMapBytes);
        }
        buffer = nullptr;
    }

    if (m_deviceFd >= 0) {
        ::ioctl(m_deviceFd, pango_pcie::kUnmapAddrCmd, 0);
        ::close(m_deviceFd);
        m_deviceFd = -1;
    }

    m_ready = false;
}

bool PcieFrameReceiver::isOpen() const
{
    return m_deviceFd >= 0;
}

bool PcieFrameReceiver::isReady() const
{
    return m_ready;
}

bool PcieFrameReceiver::pioControlActive() const
{
    return m_streamStarted && m_barRegs != nullptr;
}

bool PcieFrameReceiver::getFpgaCaps(FpgaCaps &caps, std::string *errorMessage) const
{
    caps = FpgaCaps {};
    if (m_deviceFd < 0) {
        assignError(errorMessage, "FPGA GET_CAPS called while PCIe device is closed");
        return false;
    }

    pango_pcie::FpgaCapsIoctl raw {};
    raw.struct_size = sizeof(raw);
    raw.abi_version = pango_pcie::kFpgaControlAbiVersion;

    if (::ioctl(m_deviceFd, pango_pcie::kFpgaGetCapsCmd, &raw) < 0) {
        assignError(errorMessage, errnoMessage("ioctl PANGO_FPGA_GET_CAPS"));
        return false;
    }

    caps.valid = true;
    caps.ipVersion = raw.ip_version;
    caps.capability = raw.capability;
    caps.algorithmBitmap = raw.algorithm_bitmap;
    caps.streamCount = raw.stream_count;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool PcieFrameReceiver::getFpgaStatus(FpgaStatus &status, std::string *errorMessage) const
{
    status = FpgaStatus {};
    if (m_deviceFd < 0) {
        assignError(errorMessage, "FPGA GET_STATUS called while PCIe device is closed");
        return false;
    }

    pango_pcie::FpgaStatusIoctl raw {};
    raw.struct_size = sizeof(raw);
    raw.abi_version = pango_pcie::kFpgaControlAbiVersion;

    if (::ioctl(m_deviceFd, pango_pcie::kFpgaGetStatusCmd, &raw) < 0) {
        assignError(errorMessage, errnoMessage("ioctl PANGO_FPGA_GET_STATUS"));
        return false;
    }

    status.valid = true;
    status.status = raw.status;
    status.activeMode = raw.active_mode;
    status.ackSequence = raw.ack_sequence;
    status.errorStatus = raw.error_status;
    status.frameCount = raw.frame_count;
    status.lastApplyFrame = raw.last_apply_frame;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool PcieFrameReceiver::applyFpgaPreprocess(std::uint32_t mode,
                                            std::uint32_t param0,
                                            std::uint32_t timeoutMs,
                                            FpgaApplyResult &result,
                                            std::string *errorMessage) const
{
    result = FpgaApplyResult {};
    result.requestedMode = mode;

    if (m_deviceFd < 0) {
        assignError(errorMessage, "FPGA APPLY_CONFIG called while PCIe device is closed");
        return false;
    }

    pango_pcie::FpgaConfigIoctl raw {};
    raw.struct_size = sizeof(raw);
    raw.abi_version = pango_pcie::kFpgaControlAbiVersion;
    raw.sequence = 0; // 由驱动分配，避免与当前ACK序号冲突。
    raw.mode = mode;
    raw.param0 = param0;
    raw.timeout_ms = timeoutMs;

    errno = 0;
    const int ioctlResult = ::ioctl(m_deviceFd,
                                    pango_pcie::kFpgaApplyConfigCmd,
                                    &raw);
    const int savedErrno = errno;

    // 驱动在超时等失败路径也会尽量回填当前状态，始终保留这些信息。
    result.success = (ioctlResult == 0);
    result.systemError = (ioctlResult == 0) ? 0 : savedErrno;
    result.sequence = raw.sequence;
    result.activeMode = raw.active_mode;
    result.ackSequence = raw.ack_sequence;
    result.status = raw.status;
    result.errorStatus = raw.error_status;
    result.latencyUs = raw.apply_latency_us;

    if (ioctlResult < 0) {
        assignError(errorMessage,
                    errnoMessageWithCode("ioctl PANGO_FPGA_APPLY_CONFIG", savedErrno));
        return false;
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}


bool PcieFrameReceiver::getEthernetCaps(EthernetCaps &caps,
                                        std::string *errorMessage) const
{
    caps = EthernetCaps {};
    if (m_deviceFd < 0) {
        assignError(errorMessage, "FPGA GET_ETH_CAPS called while PCIe device is closed");
        return false;
    }

    pango_pcie::FpgaEthCapsIoctl raw {};
    raw.struct_size = sizeof(raw);
    raw.abi_version = pango_pcie::kFpgaEthAbiVersion;

    if (::ioctl(m_deviceFd, pango_pcie::kFpgaGetEthCapsCmd, &raw) < 0) {
        assignError(errorMessage, errnoMessage("ioctl PANGO_FPGA_GET_ETH_CAPS"));
        return false;
    }

    caps.valid = true;
    caps.ipVersion = raw.ip_version;
    caps.capability = raw.capability;
    caps.outputWidth = raw.output_width;
    caps.outputHeight = raw.output_height;
    caps.outputFps = raw.output_fps;
    caps.destinationIpv4 = raw.destination_ipv4;
    caps.destinationPort = raw.destination_port;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool PcieFrameReceiver::getEthernetStatus(EthernetStatus &status,
                                          std::string *errorMessage) const
{
    status = EthernetStatus {};
    if (m_deviceFd < 0) {
        assignError(errorMessage, "FPGA GET_ETH_STATUS called while PCIe device is closed");
        return false;
    }

    pango_pcie::FpgaEthStatusIoctl raw {};
    raw.struct_size = sizeof(raw);
    raw.abi_version = pango_pcie::kFpgaEthAbiVersion;

    if (::ioctl(m_deviceFd, pango_pcie::kFpgaGetEthStatusCmd, &raw) < 0) {
        assignError(errorMessage, errnoMessage("ioctl PANGO_FPGA_GET_ETH_STATUS"));
        return false;
    }

    status.valid = true;
    status.status = raw.status;
    status.requestedEnable = (raw.requested_enable != 0U);
    status.activeEnable = (raw.active_enable != 0U);
    status.ackSequence = raw.ack_sequence;
    status.errorStatus = raw.error_status;
    status.frameCount = raw.frame_count;
    status.lastApplyFrame = raw.last_apply_frame;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool PcieFrameReceiver::setEthernetOutput(bool enable,
                                          std::uint32_t timeoutMs,
                                          EthernetApplyResult &result,
                                          std::string *errorMessage) const
{
    result = EthernetApplyResult {};
    result.requestedEnable = enable;

    if (m_deviceFd < 0) {
        assignError(errorMessage, "FPGA SET_ETH_OUTPUT called while PCIe device is closed");
        return false;
    }

    pango_pcie::FpgaEthConfigIoctl raw {};
    raw.struct_size = sizeof(raw);
    raw.abi_version = pango_pcie::kFpgaEthAbiVersion;
    raw.sequence = 0;
    raw.enable = enable ? 1U : 0U;
    raw.timeout_ms = timeoutMs;

    errno = 0;
    const int ioctlResult = ::ioctl(m_deviceFd,
                                    pango_pcie::kFpgaSetEthOutputCmd,
                                    &raw);
    const int savedErrno = errno;

    result.success = (ioctlResult == 0);
    result.systemError = (ioctlResult == 0) ? 0 : savedErrno;
    result.sequence = raw.sequence;
    result.activeEnable = (raw.active_enable != 0U);
    result.ackSequence = raw.ack_sequence;
    result.status = raw.status;
    result.errorStatus = raw.error_status;
    result.latencyUs = raw.apply_latency_us;
    result.lastApplyFrame = raw.last_apply_frame;

    if (ioctlResult < 0) {
        assignError(errorMessage,
                    errnoMessageWithCode("ioctl PANGO_FPGA_SET_ETH_OUTPUT", savedErrno));
        return false;
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

const PcieFrameReceiver::LinkInfo &PcieFrameReceiver::linkInfo() const
{
    return m_linkInfo;
}

const std::string &PcieFrameReceiver::lastError() const
{
    return m_lastError;
}

const std::string &PcieFrameReceiver::lastWarning() const
{
    return m_lastWarning;
}

bool PcieFrameReceiver::readLinkInfo()
{
    pango_pcie::CommandOperation command {};
    if (::ioctl(m_deviceFd, pango_pcie::kReadDataCmd, &command) < 0) {
        setWarning(errnoMessage("ioctl PCI_READ_DATA_CMD"));
        return false;
    }

    const pango_pcie::PciDeviceInfo &info = command.get_pci_dev_info;
    m_linkInfo.valid = true;
    m_linkInfo.vendorId = info.vendor_id;
    m_linkInfo.deviceId = info.device_id;
    m_linkInfo.linkSpeed = info.link_speed;
    m_linkInfo.linkWidth = info.link_width;
    m_linkInfo.mps = info.mps;
    m_linkInfo.mrrs = info.mrrs;
    return true;
}

bool PcieFrameReceiver::configureDmaBuffers()
{
    pango_pcie::DmaOperation dma {};
    dma.current_len = static_cast<unsigned int>((kFrameWidth * kBytesPerPixel) >> 2);
    dma.offset_addr = 0;
    dma.cmd = 0;

    if (::ioctl(m_deviceFd, pango_pcie::kMapAddrCmd, &dma) < 0) {
        setError(errnoMessage("ioctl PCI_MAP_ADDR_CMD"));
        return false;
    }

    if (::ioctl(m_deviceFd, pango_pcie::kSetConfigCmd, &dma) < 0) {
        setError(errnoMessage("ioctl PCI_SET_CONFIG"));
        return false;
    }

    return true;
}

bool PcieFrameReceiver::mapFrameBuffers()
{
    for (int index = 0; index < kBufferCount; ++index) {
        const off_t offset = static_cast<off_t>(index) * static_cast<off_t>(kFrameMapBytes);
        void *mapped = ::mmap(nullptr,
                             kFrameMapBytes,
                             PROT_READ,
                             MAP_SHARED,
                             m_deviceFd,
                             offset);
        if (mapped == MAP_FAILED) {
            std::ostringstream os;
            os << "mmap video buffer[" << index << "] failed: errno="
               << errno << " (" << std::strerror(errno) << ')';
            setError(os.str());
            return false;
        }
        m_buffers[static_cast<std::size_t>(index)] = mapped;
        std::fprintf(stderr, "mmap video buffer %d at %p\n", index, mapped);
    }

    return true;
}

bool PcieFrameReceiver::mapBar0()
{
    pango_pcie::CommandOperation command {};
    if (::ioctl(m_deviceFd, pango_pcie::kMapBar0Cmd, &command) < 0) {
        setWarning(errnoMessage("ioctl PCI_MAP_BAR0_CMD"));
        return false;
    }

    const unsigned long barBase = command.get_pci_dev_info.bar[0].bar_base;
    const unsigned long barLength = command.get_pci_dev_info.bar[0].bar_len;
    if (barBase == 0 || barLength == 0) {
        std::ostringstream os;
        os << "invalid BAR0 base/len: base=0x" << std::hex << barBase
           << " len=0x" << barLength;
        setWarning(os.str());
        return false;
    }

    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        setWarning("sysconf(_SC_PAGESIZE) failed");
        return false;
    }

    const unsigned long pageMask = static_cast<unsigned long>(pageSize) - 1UL;
    const unsigned long pageBase = barBase & ~pageMask;
    const unsigned long pageOffset = barBase - pageBase;
    const std::size_t mapBytes = static_cast<std::size_t>(barLength + pageOffset);

    m_memFd = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (m_memFd < 0) {
        setWarning(errnoMessage("open /dev/mem"));
        return false;
    }

    void *mapped = ::mmap(nullptr,
                          mapBytes,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          m_memFd,
                          static_cast<off_t>(pageBase));
    if (mapped == MAP_FAILED) {
        setWarning(errnoMessage("mmap /dev/mem BAR0"));
        ::close(m_memFd);
        m_memFd = -1;
        return false;
    }

    m_barMapBase = mapped;
    m_barMapBytes = mapBytes;
    m_barRegs = static_cast<volatile std::uint8_t *>(mapped) + pageOffset;
    std::fprintf(stderr,
                 "BAR0 mapped: phys=0x%lx len=0x%lx virt=%p\n",
                 barBase,
                 barLength,
                 const_cast<std::uint8_t *>(
                     reinterpret_cast<volatile std::uint8_t *>(m_barRegs)));
    return true;
}

void PcieFrameReceiver::unmapBar0()
{
    if (m_barMapBase != nullptr && m_barMapBase != MAP_FAILED) {
        ::munmap(m_barMapBase, m_barMapBytes);
    }
    m_barMapBase = nullptr;
    m_barMapBytes = 0;
    m_barRegs = nullptr;

    if (m_memFd >= 0) {
        ::close(m_memFd);
        m_memFd = -1;
    }
}

void PcieFrameReceiver::setError(const std::string &message)
{
    m_lastError = message;
}

void PcieFrameReceiver::setWarning(const std::string &message)
{
    m_lastWarning = message;
}
