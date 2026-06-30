#ifndef PANGO_PCIE_UAPI_H
#define PANGO_PCIE_UAPI_H

#include <sys/ioctl.h>

namespace pango_pcie {

constexpr unsigned long kReadDataCmd = _IOWR('S', 0, int);
constexpr unsigned long kMapAddrCmd  = _IOWR('S', 2, int);
constexpr unsigned long kUnmapAddrCmd = _IOWR('S', 7, int);
constexpr unsigned long kMapBar0Cmd = _IOWR('S', 10, int);
constexpr unsigned long kSetConfigCmd = _IOWR('S', 11, int);

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
