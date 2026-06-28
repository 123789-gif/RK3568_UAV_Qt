#include "ptz/visca_serial.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <poll.h>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

namespace {
constexpr uint8_t kPanLeft  = 0x01;
constexpr uint8_t kPanRight = 0x02;
constexpr uint8_t kPanStop  = 0x03;
constexpr uint8_t kTiltUp   = 0x01;
constexpr uint8_t kTiltDown = 0x02;
constexpr uint8_t kTiltStop = 0x03;

bool validPanDirection(uint8_t value) {
    return value == kPanLeft || value == kPanRight || value == kPanStop;
}

bool validTiltDirection(uint8_t value) {
    return value == kTiltUp || value == kTiltDown || value == kTiltStop;
}
}  // namespace

ViscaSerial::~ViscaSerial() {
    closePort();
}

void ViscaSerial::setError(const std::string& text) {
    last_error_ = text;
}

bool ViscaSerial::openPort(const std::string& device) {
    closePort();

    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        setError("open " + device + " failed: " + std::strerror(errno));
        return false;
    }

    device_ = device;
    rx_accumulator_.clear();

    if (!configure9600_8N1()) {
        closePort();
        return false;
    }

    tcflush(fd_, TCIOFLUSH);
    last_error_.clear();
    return true;
}

void ViscaSerial::closePort() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    device_.clear();
    rx_accumulator_.clear();
}

bool ViscaSerial::configure9600_8N1() {
    termios tty {};
    if (tcgetattr(fd_, &tty) != 0) {
        setError("tcgetattr failed: " + std::string(std::strerror(errno)));
        return false;
    }

    cfmakeraw(&tty);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (cfsetispeed(&tty, B9600) != 0 || cfsetospeed(&tty, B9600) != 0) {
        setError("setting 9600 baud failed: " + std::string(std::strerror(errno)));
        return false;
    }

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        setError("tcsetattr failed: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

bool ViscaSerial::writeCommand(const std::vector<uint8_t>& command) {
    if (fd_ < 0) {
        setError("serial port is not open");
        return false;
    }
    if (command.empty() || command.back() != 0xFF) {
        setError("VISCA command must end with 0xFF");
        return false;
    }

    size_t offset = 0;
    while (offset < command.size()) {
        const ssize_t written = ::write(fd_, command.data() + offset,
                                        command.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd {};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            const int ret = ::poll(&pfd, 1, 200);
            if (ret > 0) {
                continue;
            }
            setError(ret == 0 ? "serial write timeout"
                              : "serial poll(POLLOUT) failed: " + std::string(std::strerror(errno)));
            return false;
        }
        setError("serial write failed: " + std::string(std::strerror(errno)));
        return false;
    }

    if (tcdrain(fd_) != 0) {
        setError("tcdrain failed: " + std::string(std::strerror(errno)));
        return false;
    }
    last_error_.clear();
    return true;
}

void ViscaSerial::appendReceived(const uint8_t* data,
                                 size_t size,
                                 std::vector<std::vector<uint8_t>>& packets) {
    rx_accumulator_.insert(rx_accumulator_.end(), data, data + size);

    while (true) {
        const auto end = std::find(rx_accumulator_.begin(),
                                   rx_accumulator_.end(),
                                   uint8_t{0xFF});
        if (end == rx_accumulator_.end()) {
            break;
        }
        packets.emplace_back(rx_accumulator_.begin(), end + 1);
        rx_accumulator_.erase(rx_accumulator_.begin(), end + 1);
    }

    // A malformed/noisy link must not grow memory without bound.
    if (rx_accumulator_.size() > 1024) {
        rx_accumulator_.clear();
    }
}

std::vector<std::vector<uint8_t>> ViscaSerial::drainPackets() {
    std::vector<std::vector<uint8_t>> packets;
    if (fd_ < 0) {
        return packets;
    }

    uint8_t buffer[256];
    while (true) {
        const ssize_t count = ::read(fd_, buffer, sizeof(buffer));
        if (count > 0) {
            appendReceived(buffer, static_cast<size_t>(count), packets);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            setError("serial read failed: " + std::string(std::strerror(errno)));
        }
        break;
    }
    return packets;
}

std::vector<std::vector<uint8_t>> ViscaSerial::waitPackets(int timeout_ms) {
    std::vector<std::vector<uint8_t>> packets;
    if (fd_ < 0 || timeout_ms <= 0) {
        return packets;
    }

    int elapsed_ms = 0;
    while (elapsed_ms < timeout_ms) {
        pollfd pfd {};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int slice_ms = std::min(50, timeout_ms - elapsed_ms);
        const int ret = ::poll(&pfd, 1, slice_ms);
        elapsed_ms += slice_ms;

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            setError("serial poll(POLLIN) failed: " + std::string(std::strerror(errno)));
            break;
        }
        if (ret == 0) {
            continue;
        }

        auto newly_received = drainPackets();
        packets.insert(packets.end(),
                       newly_received.begin(),
                       newly_received.end());

        // For startup commands one complete packet is sufficient to prove the link.
        if (!packets.empty()) {
            break;
        }
    }
    return packets;
}

bool ViscaSerial::addressSet(int timeout_ms) {
    tcflush(fd_, TCIFLUSH);
    if (!writeCommand({0x88, 0x30, 0x01, 0xFF})) {
        return false;
    }
    return !waitPackets(timeout_ms).empty();
}

bool ViscaSerial::interfaceClear(int timeout_ms) {
    tcflush(fd_, TCIFLUSH);
    if (!writeCommand({0x88, 0x01, 0x00, 0x01, 0xFF})) {
        return false;
    }
    return !waitPackets(timeout_ms).empty();
}

bool ViscaSerial::home() {
    return writeCommand({0x81, 0x01, 0x06, 0x04, 0xFF});
}

bool ViscaSerial::panTiltDrive(uint8_t pan_speed,
                               uint8_t tilt_speed,
                               uint8_t pan_direction,
                               uint8_t tilt_direction) {
    pan_speed = std::clamp<uint8_t>(pan_speed, 0x01, 0x18);
    tilt_speed = std::clamp<uint8_t>(tilt_speed, 0x01, 0x14);

    if (!validPanDirection(pan_direction) ||
        !validTiltDirection(tilt_direction)) {
        setError("invalid VISCA pan/tilt direction");
        return false;
    }

    return writeCommand({0x81, 0x01, 0x06, 0x01,
                         pan_speed, tilt_speed,
                         pan_direction, tilt_direction,
                         0xFF});
}

bool ViscaSerial::panTiltStop(uint8_t pan_speed, uint8_t tilt_speed) {
    return panTiltDrive(pan_speed, tilt_speed, kPanStop, kTiltStop);
}

bool ViscaSerial::zoomTele() {
    return writeCommand({0x81, 0x01, 0x04, 0x07, 0x02, 0xFF});
}

bool ViscaSerial::zoomWide() {
    return writeCommand({0x81, 0x01, 0x04, 0x07, 0x03, 0xFF});
}

bool ViscaSerial::zoomStop() {
    return writeCommand({0x81, 0x01, 0x04, 0x07, 0x00, 0xFF});
}

std::string ViscaSerial::packetToHex(const std::vector<uint8_t>& packet) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < packet.size(); ++i) {
        if (i != 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned>(packet[i]);
    }
    return stream.str();
}
