#include "touchscreen_observer.h"

extern "C"
{
#include <hidsdi.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::touch_input
{
namespace
{
constexpr USAGE digitizer = 0x0d;
constexpr USAGE touchscreen = 0x04;
constexpr USAGE tipSwitch = 0x42;
constexpr USAGE contactId = 0x51;
constexpr USAGE desktop = 0x01;
constexpr USAGE xAxis = 0x30;
constexpr USAGE yAxis = 0x31;

struct ContactFields
{
    HIDP_VALUE_CAPS x, y;
};

struct TouchscreenDevice
{
    std::vector<std::byte> descriptor;
    std::vector<ContactFields> contacts;
    std::vector<ULONG> activeContacts, pressedIds, releasedIds;

    PHIDP_PREPARSED_DATA Data() noexcept
    {
        return reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor.data());
    }

    void Load(HANDLE device)
    {
        UINT size = 0;
        if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, nullptr, &size) == UINT(-1) || !size) return;
        descriptor.resize(size);
        if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, descriptor.data(), &size) == UINT(-1)) return;

        HIDP_CAPS caps{};
        if (HidP_GetCaps(Data(), &caps) != HIDP_STATUS_SUCCESS ||
            caps.UsagePage != digitizer || caps.Usage != touchscreen) return;
        USHORT xCount = caps.NumberInputValueCaps, yCount = xCount;
        std::vector<HIDP_VALUE_CAPS> xs(xCount), ys(yCount);
        if (!xCount || HidP_GetSpecificValueCaps(HidP_Input, desktop, 0, xAxis,
                xs.data(), &xCount, Data()) != HIDP_STATUS_SUCCESS ||
            HidP_GetSpecificValueCaps(HidP_Input, desktop, 0, yAxis,
                ys.data(), &yCount, Data()) != HIDP_STATUS_SUCCESS) return;
        for (const auto& x : std::span(xs).first(xCount))
        {
            if (x.LogicalMax <= x.LogicalMin) continue;
            const auto end = ys.begin() + yCount;
            const auto y = std::find_if(ys.begin(), end, [&](const auto& candidate) {
                return candidate.LinkCollection == x.LinkCollection && candidate.ReportID == x.ReportID &&
                    candidate.LogicalMax > candidate.LogicalMin;
            });
            if (y != end) contacts.push_back({x, *y});
        }
    }
};

std::unordered_map<HANDLE, TouchscreenDevice> devices;

std::wstring DeviceName(HANDLE device)
{
    UINT size = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size) == UINT(-1) || !size) return {};
    std::wstring name(size, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) == UINT(-1)) return {};
    name.resize(wcsnlen_s(name.data(), name.size()));
    return name;
}

std::optional<POINTER_DEVICE_INFO> PointerDevice(HANDLE device)
{
    POINTER_DEVICE_INFO info{};
    if (GetPointerDevice(device, &info) && info.pointerDeviceType == POINTER_DEVICE_TYPE_TOUCH && info.monitor) return info;

    UINT32 count = 0;
    if (!GetPointerDevices(&count, nullptr) || !count) return std::nullopt;
    std::vector<POINTER_DEVICE_INFO> pointers(count);
    if (!GetPointerDevices(&count, pointers.data())) return std::nullopt;
    const auto name = DeviceName(device);
    std::optional<POINTER_DEVICE_INFO> single;
    bool multiple = false;
    for (const auto& pointer : std::span(pointers).first(count))
    {
        if (pointer.pointerDeviceType != POINTER_DEVICE_TYPE_TOUCH || !pointer.monitor) continue;
        if (pointer.device == device || (!name.empty() && name == DeviceName(pointer.device))) return pointer;
        multiple |= single.has_value();
        single = pointer;
    }
    return multiple ? std::nullopt : single;
}

double Fraction(const HIDP_VALUE_CAPS& axis, ULONG value) noexcept
{
    if (axis.LogicalMin < 0 && axis.BitSize && axis.BitSize < 32 &&
        (value & (1ul << (axis.BitSize - 1)))) value |= ~0ul << axis.BitSize;
    const double signedValue = axis.LogicalMin < 0 ? static_cast<double>(static_cast<LONG>(value)) : value;
    return std::clamp((signedValue - axis.LogicalMin) / (static_cast<double>(axis.LogicalMax) - axis.LogicalMin), 0.0, 1.0);
}

std::optional<POINT> ScreenPosition(HANDLE device, double x, double y)
{
    const auto pointer = PointerDevice(device);
    const HMONITOR screenMonitor = pointer ? pointer->monitor : GetSystemMetrics(SM_CMONITORS) == 1
        ? MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY) : nullptr;
    MONITORINFO monitor{sizeof(monitor)};
    if (!screenMonitor || !GetMonitorInfoW(screenMonitor, &monitor)) return std::nullopt;
    const RECT screen = monitor.rcMonitor;
    switch (pointer ? pointer->displayOrientation : DISPLAYCONFIG_ROTATION_IDENTITY)
    {
    case DISPLAYCONFIG_ROTATION_ROTATE90: std::swap(x, y); x = 1 - x; break;
    case DISPLAYCONFIG_ROTATION_ROTATE180: x = 1 - x; y = 1 - y; break;
    case DISPLAYCONFIG_ROTATION_ROTATE270: std::swap(x, y); y = 1 - y; break;
    }
    return POINT{screen.left + static_cast<LONG>(x * (screen.right - screen.left - 1)),
        screen.top + static_cast<LONG>(y * (screen.bottom - screen.top - 1))};
}

std::optional<POINT> ReadReport(TouchscreenDevice& device, HANDLE handle, char* report, ULONG length)
{
    std::optional<POINT> pressed;
    device.releasedIds.clear();
    device.pressedIds.clear();
    for (const auto& fields : device.contacts)
    {
        const auto link = fields.x.LinkCollection;
        ULONG id = 0;
        if (static_cast<BYTE>(report[0]) != fields.x.ReportID ||
            HidP_GetUsageValue(HidP_Input, digitizer, link, contactId, &id,
                device.Data(), report, length) != HIDP_STATUS_SUCCESS) continue;

        std::array<USAGE, 16> buttons{};
        ULONG count = static_cast<ULONG>(buttons.size());
        if (HidP_GetUsages(HidP_Input, digitizer, link, buttons.data(), &count,
                device.Data(), report, length) != HIDP_STATUS_SUCCESS) continue;
        const auto end = buttons.begin() + count;
        if (std::find(buttons.begin(), end, tipSwitch) == end)
        {
            device.releasedIds.push_back(id);
            continue;
        }

        device.pressedIds.push_back(id);
        const bool hadContact = !device.activeContacts.empty();
        if (std::ranges::find(device.activeContacts, id) == device.activeContacts.end())
            device.activeContacts.push_back(id);
        if (hadContact) continue;
        ULONG x = 0, y = 0;
        if (HidP_GetUsageValue(HidP_Input, desktop, link, xAxis, &x, device.Data(), report, length) == HIDP_STATUS_SUCCESS &&
            HidP_GetUsageValue(HidP_Input, desktop, link, yAxis, &y, device.Data(), report, length) == HIDP_STATUS_SUCCESS)
            pressed = ScreenPosition(handle, Fraction(fields.x, x), Fraction(fields.y, y));
    }
    // Parallel reports can contain an unused collection with the same id.
    for (const auto id : device.releasedIds)
        if (std::ranges::find(device.pressedIds, id) == device.pressedIds.end()) std::erase(device.activeContacts, id);
    return pressed;
}
}

void RegisterTouchscreenObserver(HWND target) noexcept
{
    const RAWINPUTDEVICE device{digitizer, touchscreen, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, target};
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

void ForgetTouchscreenDevice(HANDLE device) noexcept { devices.erase(device); }

std::optional<POINT> ReadTouchscreenPress(HRAWINPUT input)
{
    constexpr auto reportOffset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
    UINT bytes = 0;
    if (GetRawInputData(input, RID_INPUT, nullptr, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        bytes < reportOffset) return std::nullopt;
    std::vector<std::byte> buffer(bytes);
    const UINT read = GetRawInputData(input, RID_INPUT, buffer.data(), &bytes, sizeof(RAWINPUTHEADER));
    if (read == UINT(-1) || read < reportOffset) return std::nullopt;
    auto* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
    const auto handle = raw->header.hDevice;
    if (raw->header.dwType != RIM_TYPEHID || !handle) return std::nullopt;
    const auto& hid = raw->data.hid;
    if (!hid.dwSizeHid || hid.dwCount > (read - reportOffset) / hid.dwSizeHid)
        return std::nullopt;
    auto [entry, added] = devices.try_emplace(handle);
    if (added) entry->second.Load(handle);
    std::optional<POINT> pressed;
    for (DWORD index = 0; index < hid.dwCount; ++index)
        if (const auto point = ReadReport(entry->second, handle,
                reinterpret_cast<char*>(raw->data.hid.bRawData + index * hid.dwSizeHid), hid.dwSizeHid); point && !pressed)
            pressed = point;
    return pressed;
}
}
