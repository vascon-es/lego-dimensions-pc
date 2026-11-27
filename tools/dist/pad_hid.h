#pragma once

#include <windows.h>
#include <xinput.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace pad_hid {

// DualShock 4 e DualSense diretos, sem a necessidade de um driver XInput.
inline HANDLE Open(bool* dualsense, DWORD* report_bytes) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, nullptr, nullptr)) continue;

        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        const USHORT pid = HidD_GetAttributes(device, &attributes) && attributes.VendorID == 0x054C ? attributes.ProductID : 0;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool gamepad = pid && HidD_GetPreparsedData(device, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                             caps.UsagePage == 0x01 && caps.Usage == 0x05;
        if (preparsed) HidD_FreePreparsedData(preparsed);
        if (gamepad && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0 || pid == 0x0CE6 || pid == 0x0DF2)) {
            *dualsense = pid == 0x0CE6 || pid == 0x0DF2;
            *report_bytes = caps.InputReportByteLength;
            found = device;
        } else {
            CloseHandle(device);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

// USB e relatorio simples Bluetooth: 0x01. Bluetooth completo: 0x11 (DS4) ou 0x31 (DualSense).
inline bool Parse(const BYTE* report, DWORD size, bool dualsense, XINPUT_GAMEPAD* pad) {
    if (size < 10) return false;
    size_t stick = 0, buttons = 0;
    if (report[0] == 0x01) {
        stick = 1;
        buttons = dualsense && size >= 64 ? 8 : 5;
    } else if (report[0] == 0x11 && !dualsense) {
        stick = 3;
        buttons = 7;
    } else if (report[0] == 0x31 && dualsense && size >= 12) {
        stick = 2;
        buttons = 9;
    } else {
        return false;
    }

    constexpr WORD hat[] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
                            XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
                            XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
                            XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT};
    const BYTE face = report[buttons], side = report[buttons + 1];
    WORD out = (face & 0x0F) < 8 ? hat[face & 0x0F] : 0;
    if (face & 0x20) out |= XINPUT_GAMEPAD_A;
    if (face & 0x40) out |= XINPUT_GAMEPAD_B;
    if (face & 0x10) out |= XINPUT_GAMEPAD_X;
    if (face & 0x80) out |= XINPUT_GAMEPAD_Y;
    if (side & 0x20) out |= XINPUT_GAMEPAD_START;
    if ((side & 0x10) || (report[buttons + 2] & 0x02)) out |= XINPUT_GAMEPAD_BACK;
    if (side & 0x01) out |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (side & 0x02) out |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    pad->wButtons = out;
    pad->bLeftTrigger = (side & 0x04) ? 255 : 0;
    pad->bRightTrigger = (side & 0x08) ? 255 : 0;
    pad->sThumbLX = static_cast<SHORT>((report[stick] - 128) * 256);
    pad->sThumbLY = static_cast<SHORT>(std::clamp((128 - report[stick + 1]) * 256, -32768, 32767));
    return true;
}

inline void Read(std::atomic<XINPUT_GAMEPAD>* state, std::atomic<bool>* back_pressed) {
    for (;;) {
        bool dualsense = false;
        DWORD bytes = 0;
        HANDLE device = Open(&dualsense, &bytes);
        if (device == INVALID_HANDLE_VALUE) {
            Sleep(350);
            continue;
        }
        std::vector<BYTE> report(std::max<DWORD>(bytes, 64));
        DWORD got = 0;
        while (ReadFile(device, report.data(), static_cast<DWORD>(report.size()), &got, nullptr)) {
            XINPUT_GAMEPAD pad{};
            if (Parse(report.data(), got, dualsense, &pad)) {
                const auto previous = state->exchange(pad, std::memory_order_relaxed);
                if (back_pressed && (pad.wButtons & XINPUT_GAMEPAD_BACK) && !(previous.wButtons & XINPUT_GAMEPAD_BACK))
                    back_pressed->store(true, std::memory_order_relaxed);
            }
        }
        CloseHandle(device);
        state->store({}, std::memory_order_relaxed);
    }
}

} // namespace pad_hid
