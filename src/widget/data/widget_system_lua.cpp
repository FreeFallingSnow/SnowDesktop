#include "widget_system_lua.h"

#include "common/utils.h"
#include "platform/deployment_context.h"
#include "widget/packages/widget_package.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/runtime/widget_engine.h"

#include <cstdint>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
constexpr char kSystemPerformancePermission[] =
    "system.performance.read";
constexpr char kSystemPowerPermission[] = "system.power.read";
constexpr char kSystemNetworkPermission[] = "system.network.read";

static void SetNumberField(lua_State* L, const char* key, lua_Number value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

static void SetBooleanField(lua_State* L, const char* key, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

int lua_SystemCpu(lua_State* L)
{
    if (!RequirePermission(L, kSystemPerformancePermission)) return 0;
    auto* host = BoundWidgetEngine(L);
    CpuSnapshot snapshot = host
        ? host->RuntimeGetCpuSnapshot(BoundWidgetId(L)) : CpuSnapshot{};
    lua_createtable(L, 0, 4);
    SetBooleanField(L, "available", snapshot.available);
    SetNumberField(L, "usagePercent", snapshot.usagePercent);
    SetNumberField(L, "logicalProcessors", snapshot.logicalProcessors);
    lua_pushstring(L, snapshot.name.c_str()); lua_setfield(L, -2, "name");
    return 1;
}

int lua_SystemMemory(lua_State* L)
{
    if (!RequirePermission(L, kSystemPerformancePermission)) return 0;
    auto* host = BoundWidgetEngine(L);
    MemorySnapshot snapshot = host
        ? host->RuntimeGetMemorySnapshot(BoundWidgetId(L)) : MemorySnapshot{};
    lua_createtable(L, 0, 5);
    SetBooleanField(L, "available", snapshot.available);
    SetNumberField(L, "totalBytes", static_cast<lua_Number>(snapshot.totalBytes));
    SetNumberField(L, "usedBytes", static_cast<lua_Number>(snapshot.usedBytes));
    SetNumberField(L, "freeBytes", static_cast<lua_Number>(snapshot.freeBytes));
    SetNumberField(L, "usagePercent", snapshot.usagePercent);
    return 1;
}

int lua_SystemBattery(lua_State* L)
{
    if (!RequirePermission(L, kSystemPowerPermission)) return 0;
    auto* host = BoundWidgetEngine(L);
    BatterySnapshot snapshot = host
        ? host->RuntimeGetBatterySnapshot(BoundWidgetId(L)) : BatterySnapshot{};
    lua_createtable(L, 0, 5);
    SetBooleanField(L, "available", snapshot.available);
    SetNumberField(L, "percent", snapshot.percent);
    SetBooleanField(L, "charging", snapshot.charging);
    SetBooleanField(L, "pluggedIn", snapshot.pluggedIn);
    SetBooleanField(L, "saver", snapshot.saver);
    return 1;
}

int lua_SystemNetwork(lua_State* L)
{
    if (!RequirePermission(L, kSystemNetworkPermission)) return 0;
    auto* host = BoundWidgetEngine(L);
    NetworkSnapshot snapshot = host
        ? host->RuntimeGetNetworkSnapshot(BoundWidgetId(L)) : NetworkSnapshot{};
    lua_createtable(L, 0, 7);
    SetBooleanField(L, "available", snapshot.available);
    SetBooleanField(L, "connected", snapshot.connected);
    SetNumberField(L, "downloadBytesPerSec", static_cast<lua_Number>(snapshot.downloadBytesPerSec));
    SetNumberField(L, "uploadBytesPerSec", static_cast<lua_Number>(snapshot.uploadBytesPerSec));
    SetNumberField(L, "receivedBytes", static_cast<lua_Number>(snapshot.receivedBytes));
    SetNumberField(L, "sentBytes", static_cast<lua_Number>(snapshot.sentBytes));
    return 1;
}

int lua_SystemGpu(lua_State* L)
{
    if (!RequirePermission(L, kSystemPerformancePermission)) return 0;
    auto* host = BoundWidgetEngine(L);
    GpuSnapshot snapshot = host
        ? host->RuntimeGetGpuSnapshot(BoundWidgetId(L)) : GpuSnapshot{};
    lua_createtable(L, 0, 5);
    SetBooleanField(L, "available", snapshot.available);
    lua_pushstring(L, snapshot.name.c_str()); lua_setfield(L, -2, "name");
    SetNumberField(L, "usagePercent", snapshot.usagePercent);
    SetNumberField(L, "vramTotalBytes", static_cast<lua_Number>(snapshot.vramTotalBytes));
    SetNumberField(L, "vramUsedBytes", static_cast<lua_Number>(snapshot.vramUsedBytes));
    return 1;
}

int lua_MediaCurrent(lua_State* L)
{
    if (!RequirePermission(L, "media.read")) return 0;
    auto* host = BoundWidgetEngine(L);
    MediaSnapshot snapshot = host
        ? host->RuntimeGetMediaSnapshot(BoundWidgetId(L)) : MediaSnapshot{};
    lua_createtable(L, 0, 10);
    SetBooleanField(L, "available", snapshot.available);
    lua_pushstring(L, WideToUtf8(snapshot.title).c_str()); lua_setfield(L, -2, "title");
    lua_pushstring(L, WideToUtf8(snapshot.artist).c_str()); lua_setfield(L, -2, "artist");
    lua_pushstring(L, WideToUtf8(snapshot.album).c_str()); lua_setfield(L, -2, "album");
    lua_pushstring(L, WideToUtf8(snapshot.sourceApp).c_str()); lua_setfield(L, -2, "sourceApp");
    lua_pushstring(L, snapshot.playbackStatus.c_str()); lua_setfield(L, -2, "playbackStatus");
    SetBooleanField(L, "canPlayPause", snapshot.canPlayPause);
    SetBooleanField(L, "canNext", snapshot.canNext);
    SetBooleanField(L, "canPrevious", snapshot.canPrevious);
    return 1;
}

int lua_MediaPlayPause(lua_State* L)
{
    if (!RequirePermission(L, "media.action")) return 0;
    auto* host = BoundWidgetEngine(L);
    lua_pushboolean(L, host && host->RuntimeMediaPlayPause());
    return 1;
}

int lua_MediaNext(lua_State* L)
{
    if (!RequirePermission(L, "media.action")) return 0;
    auto* host = BoundWidgetEngine(L);
    lua_pushboolean(L, host && host->RuntimeMediaNext());
    return 1;
}

int lua_MediaPrevious(lua_State* L)
{
    if (!RequirePermission(L, "media.action")) return 0;
    auto* host = BoundWidgetEngine(L);
    lua_pushboolean(L, host && host->RuntimeMediaPrevious());
    return 1;
}


static const char* ProcessArchitectureName()
{
#if defined(_M_ARM64)
    return "arm64";
#elif defined(_M_X64)
    return "x64";
#elif defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

static const char* NativeArchitectureName()
{
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x64";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    default:
        return "unknown";
    }
}

static DWORD WindowsBuildNumber()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module
        ? reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(module, "RtlGetVersion"))
        : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return function && function(&version) == 0
        ? version.dwBuildNumber : 0;
}

int lua_SystemInfoV2(lua_State* L)
{
    const bool packaged = snowdesktop::deployment::IsPackaged();
    lua_createtable(L, 0, 9);
    lua_pushliteral(L, "windows"); lua_setfield(L, -2, "osFamily");
    const DWORD build = WindowsBuildNumber();
    if (build != 0)
    {
        lua_pushinteger(L, static_cast<lua_Integer>(build));
        lua_setfield(L, -2, "osBuild");
    }
    lua_pushstring(L, ProcessArchitectureName());
    lua_setfield(L, -2, "processArchitecture");
    lua_pushstring(L, NativeArchitectureName());
    lua_setfield(L, -2, "nativeArchitecture");
    lua_pushliteral(L, SNOWDESKTOP_VERSION);
    lua_setfield(L, -2, "hostVersion");
    lua_pushinteger(L, snowdesktop::widget::kHostApiVersion);
    lua_setfield(L, -2, "apiVersion");
    lua_pushboolean(L, packaged ? 1 : 0);
    lua_setfield(L, -2, "packaged");
    lua_pushboolean(L, packaged ? 0 : 1);
    lua_setfield(L, -2, "portable");
    lua_pushstring(L, packaged ? "packaged" : "portable");
    lua_setfield(L, -2, "deploymentMode");
    return 1;
}

int lua_SystemUptime(lua_State* L)
{
    lua_createtable(L, 0, 2);
    const auto milliseconds = snowdesktop::widget_runtime::
            IsPreviewExecution()
        ? snowdesktop::widget_runtime::PreviewMonotonicMilliseconds
        : static_cast<std::int64_t>(GetTickCount64());
    lua_pushinteger(L, static_cast<lua_Integer>(milliseconds));
    lua_setfield(L, -2, "milliseconds");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "includesSleep");
    return 1;
}
}
