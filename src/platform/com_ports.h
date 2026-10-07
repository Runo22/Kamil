#pragma once

#include <string>
#include <vector>

namespace kamil {

struct ComPort {
    std::wstring port;           // "COM7"
    std::wstring friendly_name;  // "USB Serial Port (COM7)"
    std::wstring hwid;           // "VID_0403&PID_6001" when known (USB), else empty
    std::wstring manufacturer;   // "FTDI"
};

// Present serial ports, sorted by number. Uses SetupAPI (friendly names, USB ids) and falls back
// to HKLM\HARDWARE\DEVICEMAP\SERIALCOMM for ports without a device node.
std::vector<ComPort> list_com_ports();

}  // namespace kamil
