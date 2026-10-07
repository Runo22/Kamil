#include "platform/com_ports.h"

#include "platform/win.h"

#include <setupapi.h>

#include <algorithm>
#include <cwctype>

namespace kamil {

namespace {

// GUID_DEVINTERFACE_COMPORT (ntddser.h)
const GUID kComPortInterface = {0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};

std::wstring device_property(HDEVINFO set, SP_DEVINFO_DATA* dev, DWORD prop) {
    wchar_t buf[512];
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, prop, &type, reinterpret_cast<BYTE*>(buf), sizeof(buf) - sizeof(wchar_t), nullptr))
        return {};
    buf[511] = 0;
    return buf;  // REG_MULTI_SZ: first string is enough (hardware id)
}

std::wstring usb_id(const std::wstring& hardware_id) {
    // "USB\VID_0403&PID_6001&REV_0600" or "FTDIBUS\VID_0403+PID_6001+..." -> "VID_0403&PID_6001"
    std::wstring up = hardware_id;
    for (auto& c : up) c = static_cast<wchar_t>(std::towupper(c));
    const size_t v = up.find(L"VID_");
    const size_t p = up.find(L"PID_");
    if (v == std::wstring::npos || p == std::wstring::npos) return {};
    return up.substr(v, 8) + L"&" + up.substr(p, 8);
}

int port_number(const std::wstring& name) {
    int n = 0;
    for (wchar_t c : name)
        if (c >= L'0' && c <= L'9') n = n * 10 + (c - L'0');
    return n;
}

}  // namespace

std::vector<ComPort> list_com_ports() {
    std::vector<ComPort> ports;
    HDEVINFO set = SetupDiGetClassDevsW(&kComPortInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            ComPort port;
            if (HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ); key != INVALID_HANDLE_VALUE) {
                wchar_t name[64];
                DWORD size = sizeof(name);
                if (RegQueryValueExW(key, L"PortName", nullptr, nullptr, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS)
                    port.port.assign(name, wcsnlen(name, size / sizeof(wchar_t)));
                RegCloseKey(key);
            }
            if (port.port.rfind(L"COM", 0) != 0) continue;  // LPT and others
            port.friendly_name = device_property(set, &dev, SPDRP_FRIENDLYNAME);
            port.manufacturer = device_property(set, &dev, SPDRP_MFG);
            port.hwid = usb_id(device_property(set, &dev, SPDRP_HARDWAREID));
            ports.push_back(std::move(port));
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    // Virtual / legacy ports without a device interface.
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t value_name[256];
            wchar_t data[64];
            DWORD name_len = 256, data_size = sizeof(data), type = 0;
            if (RegEnumValueW(key, i, value_name, &name_len, nullptr, &type, reinterpret_cast<BYTE*>(data), &data_size) != ERROR_SUCCESS)
                break;
            if (type != REG_SZ) continue;
            std::wstring port(data, wcsnlen(data, data_size / sizeof(wchar_t)));
            const bool known = std::any_of(ports.begin(), ports.end(), [&](const ComPort& p) { return p.port == port; });
            if (!known) ports.push_back(ComPort{port, port + L" (" + std::wstring(value_name, name_len) + L")", {}, {}});
        }
        RegCloseKey(key);
    }
    std::sort(ports.begin(), ports.end(), [](const ComPort& a, const ComPort& b) { return port_number(a.port) < port_number(b.port); });
    return ports;
}

}  // namespace kamil
