#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ViscaSerial {
public:
    ViscaSerial() = default;
    ~ViscaSerial();

    ViscaSerial(const ViscaSerial&) = delete;
    ViscaSerial& operator=(const ViscaSerial&) = delete;

    bool openPort(const std::string& device);
    void closePort();
    bool isOpen() const { return fd_ >= 0; }
    const std::string& device() const { return device_; }
    const std::string& lastError() const { return last_error_; }

    bool addressSet(int timeout_ms = 700);
    bool interfaceClear(int timeout_ms = 700);
    bool home();

    bool panTiltDrive(uint8_t pan_speed,
                      uint8_t tilt_speed,
                      uint8_t pan_direction,
                      uint8_t tilt_direction);
    bool panTiltStop(uint8_t pan_speed = 0x05,
                     uint8_t tilt_speed = 0x05);

    bool zoomTele();
    bool zoomWide();
    bool zoomStop();

    // Non-blocking receive. Complete VISCA packets end with 0xFF.
    std::vector<std::vector<uint8_t>> drainPackets();

    // Used for startup commands that need an immediate response check.
    std::vector<std::vector<uint8_t>> waitPackets(int timeout_ms);

    static std::string packetToHex(const std::vector<uint8_t>& packet);

private:
    bool configure9600_8N1();
    bool writeCommand(const std::vector<uint8_t>& command);
    void appendReceived(const uint8_t* data,
                        size_t size,
                        std::vector<std::vector<uint8_t>>& packets);
    void setError(const std::string& text);

    int fd_ = -1;
    std::string device_;
    std::string last_error_;
    std::vector<uint8_t> rx_accumulator_;
};
