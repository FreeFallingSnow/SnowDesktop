#include "performance_environment.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <sstream>
#include <vector>
#include <locale>
#include <tuple>
#include <algorithm>

namespace snowdesktop::performance
{
namespace
{
struct Display { LONG x = 0, y = 0; DWORD width = 0, height = 0, refresh = 0; UINT dpiX = 0, dpiY = 0; bool primary = false; };
struct Inventory
{
    HMODULE library = nullptr;
    using DpiFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    DpiFunction dpi = nullptr;
    std::vector<Display> displays;
    bool truncated = false;
    Inventory()
    {
        displays.reserve(16);
        library = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library) { const auto address = GetProcAddress(library, "GetDpiForMonitor"); static_assert(sizeof(address)==sizeof(dpi)); std::memcpy(&dpi, &address, sizeof(dpi)); }
    }
    ~Inventory() { if (library) FreeLibrary(library); }
};
BOOL CALLBACK Collect(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto& inventory = *reinterpret_cast<Inventory*>(data);
    if (inventory.displays.size() >= 16) { inventory.truncated = true; return FALSE; }
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    Display display; display.x = info.rcMonitor.left; display.y = info.rcMonitor.top;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0))
    { display.width = mode.dmPelsWidth; display.height = mode.dmPelsHeight; display.refresh = mode.dmDisplayFrequency; }
    if (!inventory.dpi || FAILED(inventory.dpi(monitor, 0, &display.dpiX, &display.dpiY))) display.dpiX = display.dpiY = 0;
    inventory.displays.push_back(display); return TRUE;
}
}
std::string CaptureEnvironmentJson()
{
    Inventory inventory; EnumDisplayMonitors(nullptr, nullptr, Collect, reinterpret_cast<LPARAM>(&inventory));
    std::sort(inventory.displays.begin(), inventory.displays.end(), [](const auto& a, const auto& b) { return std::tie(a.x,a.y)<std::tie(b.x,b.y); });
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    const auto allProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    const bool haveMemory = GlobalMemoryStatusEx(&memory) != FALSE;
    SYSTEM_POWER_STATUS power{}; const bool havePower = GetSystemPowerStatus(&power) != FALSE;
    OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
    using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    VersionFunction getVersion = nullptr;
    const auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    static_assert(sizeof(address)==sizeof(getVersion)); std::memcpy(&getVersion, &address, sizeof(getVersion));
    const bool haveVersion = getVersion && getVersion(&version) >= 0;
    std::ostringstream out; out.imbue(std::locale::classic());
    const auto optional = [&](bool available, auto value) { if (available) out << value; else out << "null"; };
    out << "{\"osBuild\":"; optional(haveVersion, version.dwBuildNumber);
    out << ",\"processorArchitecture\":" << system.wProcessorArchitecture << ",\"logicalProcessors\":" << (allProcessors ? allProcessors : system.dwNumberOfProcessors);
    out << ",\"physicalMemoryBytes\":"; optional(haveMemory, memory.ullTotalPhys);
    out << ",\"acPower\":"; optional(havePower && power.ACLineStatus != 255, static_cast<unsigned>(power.ACLineStatus));
    out << ",\"batterySaver\":"; optional(havePower, static_cast<unsigned>(power.SystemStatusFlag));
    out << ",\"batteryPercent\":"; optional(havePower && power.BatteryLifePercent <= 100, static_cast<unsigned>(power.BatteryLifePercent));
    out << ",\"machineIdentity\":null,\"packageFingerprint\":null,\"layoutFingerprint\":null,\"workload\":null,\"displaysTruncated\":"
        << (inventory.truncated ? "true" : "false") << ",\"displays\":[";
    bool first = true;
    for (const auto& display : inventory.displays)
    {
        if (!first) out << ','; first = false;
        out << "{\"x\":" << display.x << ",\"y\":" << display.y << ",\"width\":"; optional(display.width>0, display.width);
        out << ",\"height\":"; optional(display.height>0, display.height);
        out << ",\"refreshHz\":"; optional(display.refresh>1, display.refresh);
        out << ",\"dpiX\":"; optional(display.dpiX>0, display.dpiX);
        out << ",\"dpiY\":"; optional(display.dpiY>0, display.dpiY);
        out << ",\"primary\":" << (display.primary ? "true" : "false") << '}';
    }
    out << "]}"; return out.str();
}
}
