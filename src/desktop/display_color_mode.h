#pragma once


#include <windows.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::display_color
{
enum class Mode { Unknown, Sdr, WideColor, Hdr };

struct Configuration
{
    std::wstring sourceName;
    std::wstring displayName;
    Mode mode = Mode::Unknown;
    UINT32 bitsPerChannel = 0;
    bool acmEnabled = false;
    bool acmKnown = false;

    bool operator==(const Configuration&) const = default;
};

inline void ReadColorState(const DISPLAYCONFIG_PATH_TARGET_INFO& target, Configuration& config)
{
#if defined(NTDDI_WIN11_GA) && NTDDI_VERSION >= NTDDI_WIN11_GA
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color{};
    color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(color),
        target.adapterId, target.id};
    if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS)
    {
        config.mode = color.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG ? Mode::WideColor
            : color.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR ? Mode::Hdr : Mode::Sdr;
        config.bitsPerChannel = color.bitsPerColorChannel;
        config.acmKnown = true;
        config.acmEnabled = color.wideColorUserEnabled || config.mode == Mode::Hdr;
        return;
    }
#endif
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy{};
    legacy.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(legacy),
        target.adapterId, target.id};
    if (DisplayConfigGetDeviceInfo(&legacy.header) == ERROR_SUCCESS)
    {
        config.bitsPerChannel = legacy.bitsPerColorChannel;
        if (!legacy.advancedColorEnabled)
        {
            config.mode = Mode::Sdr;
            config.acmKnown = true;
        }
    }
}

inline std::vector<Configuration> ReadConfigurations()
{
    std::vector<Configuration> configurations;
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
        configurations.reserve(pathCount);
        for (UINT32 i = 0; i < pathCount; ++i)
        {
            const auto& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                path.sourceInfo.adapterId, path.sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

            Configuration config;
            config.sourceName = source.viewGdiDeviceName;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                path.targetInfo.adapterId, path.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
                config.displayName = target.monitorFriendlyDeviceName;

            ReadColorState(path.targetInfo, config);
            configurations.push_back(std::move(config));
        }
        break;
    }

    if (configurations.empty())
    {
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info))
            {
                auto& entries = *reinterpret_cast<std::vector<Configuration>*>(data);
                Configuration config;
                config.sourceName = info.szDevice;
                entries.push_back(std::move(config));
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&configurations));
    }
    return configurations;
}

inline const std::vector<Configuration>& Configurations()
{
    struct Snapshot { ULONGLONG nextRefresh = 0; std::vector<Configuration> entries; };
    static thread_local Snapshot snapshot;
    const auto now = GetTickCount64();
    if (now >= snapshot.nextRefresh)
    {
        snapshot.nextRefresh = now + 1000;
        snapshot.entries = ReadConfigurations();
    }
    return snapshot.entries;
}
} // namespace snowdesktop::display_color
