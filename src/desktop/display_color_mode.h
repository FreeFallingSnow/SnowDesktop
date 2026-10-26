#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace snowdesktop::display_color
{
enum class Mode { Sdr, WideColor, Hdr };

// Query once per short interval rather than once per icon. Older systems and
// failed/unsupported queries retain the ordinary SDR drawing path.
inline Mode ModeForMonitor(HMONITOR monitor)
{
    struct Entry { std::wstring name; Mode mode; };
    struct Snapshot { ULONGLONG nextRefresh = 0; std::vector<Entry> entries; };
    static thread_local Snapshot snapshot;
    const auto now = GetTickCount64();
    if (now >= snapshot.nextRefresh)
    {
        snapshot.nextRefresh = now + 1000;
        snapshot.entries.clear();
#if defined(NTDDI_WIN11_GA) && NTDDI_VERSION >= NTDDI_WIN11_GA
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            UINT32 pathCount = 0, modeCount = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
                break;
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            const auto result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                &modeCount, modes.data(), nullptr);
            if (result == ERROR_INSUFFICIENT_BUFFER) continue;
            if (result != ERROR_SUCCESS) break;
            for (UINT32 i = 0; i < pathCount; ++i)
            {
                const auto& path = paths[i];
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                    path.sourceInfo.adapterId, path.sourceInfo.id};
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color{};
                color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(color),
                    path.targetInfo.adapterId, path.targetInfo.id};
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                    DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) continue;
                const auto mode = color.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG ? Mode::WideColor
                    : color.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR ? Mode::Hdr : Mode::Sdr;
                snapshot.entries.push_back({source.viewGdiDeviceName, mode});
            }
            break;
        }
#endif
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return Mode::Sdr;
    for (const auto& entry : snapshot.entries)
        if (_wcsicmp(entry.name.c_str(), info.szDevice) == 0) return entry.mode;
    return Mode::Sdr;
}

inline bool AnyWideColorMonitor()
{
    bool found = false;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            if (ModeForMonitor(monitor) != Mode::WideColor) return TRUE;
            *reinterpret_cast<bool*>(context) = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&found));
    return found;
}
}
