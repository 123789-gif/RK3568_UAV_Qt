#ifndef PANGO_PCIE_UAPI_H
#define PANGO_PCIE_UAPI_H

#include <cstdint>
#include <sys/ioctl.h>

namespace pango_pcie {

constexpr unsigned long kReadDataCmd = _IOWR('S', 0, int);
constexpr unsigned long kMapAddrCmd  = _IOWR('S', 2, int);
constexpr unsigned long kUnmapAddrCmd = _IOWR('S', 7, int);
constexpr unsigned long kMapBar0Cmd = _IOWR('S', 10, int);
constexpr unsigned long kSetConfigCmd = _IOWR('S', 11, int);

constexpr std::uint32_t kFpgaControlAbiVersion = 1U;
constexpr std::uint32_t kFpgaDefaultTimeoutMs = 100U;

constexpr std::uint32_t kFpgaModeBypass = 0U;
constexpr std::uint32_t kFpgaModeInvert = 1U;
constexpr std::uint32_t kFpgaModeFixedColor = 2U;
constexpr std::uint32_t kFpgaModeGamma = 3U;
constexpr std::uint32_t kFpgaModeSobel = 4U;
constexpr std::uint32_t kFpgaModeDehaze = 5U;

constexpr std::uint32_t kFpgaStatusPending = (1U << 0);
constexpr std::uint32_t kFpgaStatusError = (1U << 1);

constexpr std::uint32_t kFpgaCapShadow = (1U << 0);
constexpr std::uint32_t kFpgaCapApplyAck = (1U << 1);
constexpr std::uint32_t kFpgaCapFrameBoundary = (1U << 2);
constexpr std::uint32_t kFpgaCapNoStop = (1U << 3);


constexpr std::uint32_t kFpgaEthAbiVersion = 1U;
constexpr std::uint32_t kFpgaEthDefaultTimeoutMs = 100U;

constexpr std::uint32_t kFpgaEthStatusPending = (1U << 0);
constexpr std::uint32_t kFpgaEthStatusError = (1U << 1);

struct FpgaCapsIoctl {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint32_t ip_version;
    std::uint32_t capability;
    std::uint32_t algorithm_bitmap;
    std::uint32_t stream_count;
    std::uint32_t reserved[2];
};

struct FpgaConfigIoctl {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint32_t sequence;
    std::uint32_t mode;
    std::uint32_t param0;
    std::uint32_t timeout_ms;

    std::uint32_t active_mode;
    std::uint32_t ack_sequence;
    std::uint32_t status;
    std::uint32_t error_status;
    std::uint32_t apply_latency_us;
    std::uint32_t reserved[5];
};

struct FpgaStatusIoctl {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint32_t status;
    std::uint32_t active_mode;
    std::uint32_t ack_sequence;
    std::uint32_t error_status;
    std::uint32_t frame_count;
    std::uint32_t last_apply_frame;
};

struct FpgaEthCapsIoctl {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint32_t ip_version;
    std::uint32_t capability;
    std::uint32_t output_width;
    std::uint32_t output_height;
    std::uint32_t output_fps;
    std::uint32_t destination_ipv4;
    std::uint32_t destination_port;
    std::uint32_t reserved[3];
};

struct FpgaEthConfigIoctl {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint32_t sequence;
    std::uint32_t enable;
    std::uint32_t timeout_ms;
    std::uint32_t active_enable;
    std::uint32_t ack_sequence;
    std::uint32_t status;
    std::uint32_t error_status;
    std::uint32_t apply_latency_us;
    std::uint32_t last_apply_frame;
    std::uint32_t reserved[5];
};

struct FpgaEthStatusIoctl {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint32_t status;
    std::uint32_t requested_enable;
    std::uint32_t active_enable;
    std::uint32_t ack_sequence;
    std::uint32_t error_status;
    std::uint32_t frame_count;
    std::uint32_t last_apply_frame;
    std::uint32_t reserved[3];
};

constexpr unsigned long kFpgaGetCapsCmd =
        _IOWR('S', 12, FpgaCapsIoctl);
constexpr unsigned long kFpgaApplyConfigCmd =
        _IOWR('S', 13, FpgaConfigIoctl);
constexpr unsigned long kFpgaGetStatusCmd =
        _IOWR('S', 14, FpgaStatusIoctl);

constexpr unsigned long kFpgaGetEthCapsCmd =
        _IOWR('S', 15, FpgaEthCapsIoctl);
constexpr unsigned long kFpgaSetEthOutputCmd =
        _IOWR('S', 16, FpgaEthConfigIoctl);
constexpr unsigned long kFpgaGetEthStatusCmd =
        _IOWR('S', 17, FpgaEthStatusIoctl);

constexpr unsigned int kDmaMaxPacketSize = 4096;
constexpr unsigned int kMaxBlockSize = 1024;

struct BarBaseInfo {
    unsigned long bar_base;
    unsigned long bar_len;
};

struct CapInfo {
    unsigned char flag;
    unsigned char id;
    unsigned char addr_offset;
    unsigned char next_offset;
};

struct CapList {
    unsigned char cap_status;
    unsigned char cap_error;
    CapInfo cap_buf[256];
};

struct PciDeviceInfo {
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int cmd_reg;
    unsigned int status_reg;
    unsigned int revision_id;
    unsigned int class_prog;
    unsigned int class_device;
    BarBaseInfo bar[6];
    unsigned int min_gnt;
    unsigned int max_lat;
    unsigned int link_speed;
    unsigned int link_width;
    unsigned int mps;
    unsigned int mrrs;
    unsigned int data[1024];
};

struct LoadDataInfo {
    unsigned int num_words;
    unsigned int block_words[kMaxBlockSize];
};

struct PciLoadInfo {
    unsigned char link_status;
    unsigned int crc;
    unsigned char axi_direction;
    unsigned char load_status;
    unsigned int total_num_words;
    LoadDataInfo data_block;
};

struct CommandOperation {
    unsigned char w_r;
    unsigned char step;
    unsigned int addr;
    unsigned int data;
    unsigned int cnt;
    unsigned int delay;
    PciDeviceInfo get_pci_dev_info;
    CapList cap_info;
    PciLoadInfo load_info;
};

struct DmaData {
    unsigned char read_buf[kDmaMaxPacketSize];
    unsigned char write_buf[kDmaMaxPacketSize];
};

struct DmaOperation {
    unsigned int current_len;
    unsigned int offset_addr;
    unsigned int cmd;
    DmaData data;
};

} // namespace pango_pcie

#endif // PANGO_PCIE_UAPI_H
