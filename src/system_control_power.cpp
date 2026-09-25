#include "system_control_windows.h"
#include <powrprof.h>
#include <mutex>
#include <vector>

namespace snowdesktop::system_control::windows
{
namespace
{
constexpr GUID Efficiency{0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
constexpr GUID Performance{0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
constexpr GUID Personality{0x245d8541, 0x3943, 0x4422, {0xb0, 0x25, 0x13, 0xa7, 0x84, 0xf6, 0x79, 0xb7}};
using GetMode = DWORD(WINAPI*)(GUID*);
using SetMode = DWORD(WINAPI*)(const GUID*);
struct Modes
{
    HMODULE module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    GetMode getAc = module ? reinterpret_cast<GetMode>(GetProcAddress(module, "PowerGetUserConfiguredACPowerMode")) : nullptr;
    GetMode getDc = module ? reinterpret_cast<GetMode>(GetProcAddress(module, "PowerGetUserConfiguredDCPowerMode")) : nullptr;
    SetMode setAc = module ? reinterpret_cast<SetMode>(GetProcAddress(module, "PowerSetUserConfiguredACPowerMode")) : nullptr;
    SetMode setDc = module ? reinterpret_cast<SetMode>(GetProcAddress(module, "PowerSetUserConfiguredDCPowerMode")) : nullptr;
    ~Modes() { if (module) FreeLibrary(module); }
    bool Supported(const GUID& plan) const
    {
        DWORD personality = 0;
        return getAc && getDc && setAc && setDc && PowerReadACValueIndex(nullptr, &plan, nullptr, &Personality, &personality) == ERROR_SUCCESS && personality == 2;
    }
};
std::string ModeName(const GUID& mode)
{ return mode == GUID_NULL ? "balanced" : mode == Efficiency ? "efficiency" : mode == Performance ? "performance" : "unknown"; }
struct ShutdownPrivilege
{
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES previous{};
    bool enabled = false;
    ShutdownPrivilege()
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
        TOKEN_PRIVILEGES desired{}; desired.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &desired.Privileges[0].Luid)) return;
        desired.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD size = sizeof(previous); SetLastError(ERROR_SUCCESS);
        enabled = AdjustTokenPrivileges(token, FALSE, &desired, sizeof(previous), &previous, &size) && GetLastError() == ERROR_SUCCESS;
    }
    ~ShutdownPrivilege()
    { if (token) { if (enabled) AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr); CloseHandle(token); } }
};
class Power final : public Backend
{
    std::mutex mutex_;
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        std::lock_guard guard(mutex_);
        auto value = json::Object(); auto plans = json::Array();
        GUID* allocated = nullptr;
        const DWORD status = PowerGetActiveScheme(nullptr, &allocated);
        if (status != ERROR_SUCCESS || !allocated) return {{"system.power.plans", Missing("unavailable")}};
        const GUID current = *allocated; LocalFree(allocated);
        for (DWORD index = 0; index < 256 && !cancel.Stop(); ++index)
        {
            GUID id{}; DWORD size = sizeof(id);
            if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, reinterpret_cast<UCHAR*>(&id), &size) != ERROR_SUCCESS) break;
            DWORD bytes = 0;
            PowerReadFriendlyName(nullptr, &id, nullptr, nullptr, nullptr, &bytes);
            if (bytes == 0 || bytes > 65536) continue;
            std::vector<wchar_t> text((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t) + 1, 0);
            if (PowerReadFriendlyName(nullptr, &id, nullptr, nullptr, reinterpret_cast<UCHAR*>(text.data()), &bytes) != ERROR_SUCCESS) continue;
            auto plan = json::Object(); plan.object["id"] = json::Text(Guid(id)); plan.object["name"] = json::Text(Utf8(text.data()));
            plan.object["active"] = json::Boolean(id == current); plans.array.push_back(std::move(plan));
        }
        value.object["plans"] = std::move(plans); value.object["activePlanId"] = json::Text(Guid(current));
        Modes modes; const bool supported = modes.Supported(current);
        value.object["modeSupported"] = json::Boolean(supported);
        if (supported)
        {
            GUID ac{}, dc{};
            if (modes.getAc(&ac) == ERROR_SUCCESS) value.object["acMode"] = json::Text(ModeName(ac));
            if (modes.getDc(&dc) == ERROR_SUCCESS) value.object["dcMode"] = json::Text(ModeName(dc));
        }
        SYSTEM_POWER_STATUS battery{};
        if (GetSystemPowerStatus(&battery))
        {
            value.object["batteryPresent"] = json::Boolean(battery.BatteryFlag != 255 && (battery.BatteryFlag & 128) == 0);
            if (battery.ACLineStatus != 255) value.object["onAC"] = json::Boolean(battery.ACLineStatus == 1);
            if (battery.BatteryLifePercent != 255) value.object["batteryPercent"] = json::Number(battery.BatteryLifePercent);
        }
        return {{"system.power.plans", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        std::lock_guard guard(mutex_);
        if (cancel.Stop()) return cancel.Failure();
        if (request.name == "system.power.setPlan")
        {
            GUID id{}; if (!ParseGuid(Argument(request, "planId"), id)) return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
            DWORD status = PowerSetActiveScheme(nullptr, &id); if (status) return Error(status);
            GUID* actual = nullptr; status = PowerGetActiveScheme(nullptr, &actual);
            const bool matches = status == ERROR_SUCCESS && actual && *actual == id; if (actual) LocalFree(actual);
            return matches ? Result{true, {}, 0} : Error(status ? status : ERROR_INVALID_STATE, "stateMismatch");
        }
        if (request.name == "system.power.setMode")
        {
            const auto name = Argument(request, "mode");
            if (name != "balanced" && name != "efficiency" && name != "performance") return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
            Modes modes; GUID* plan = nullptr;
            const bool supported = PowerGetActiveScheme(nullptr, &plan) == ERROR_SUCCESS && plan && modes.Supported(*plan);
            if (plan) LocalFree(plan); if (!supported) return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
            const GUID target = name == "balanced" ? GUID_NULL : name == "efficiency" ? Efficiency : Performance;
            DWORD status = modes.setAc(&target); if (status) return Error(status);
            status = modes.setDc(&target); if (status) return Error(status);
            GUID ac{}, dc{};
            status = modes.getAc(&ac); if (status) return Error(status);
            status = modes.getDc(&dc); if (status) return Error(status);
            return ac == target && dc == target ? Result{true, {}, 0} : Error(ERROR_INVALID_STATE, "stateMismatch");
        }
        if (request.name == "system.power.lock") return LockWorkStation() ? Result{true, {}, 0} : Error(GetLastError());
        if (RequiresConfirmation(request.name) && !request.hostConfirmed) return Error(ERROR_CANCELLED, "confirmationRequired");
        ShutdownPrivilege privilege;
        if (!privilege.enabled) return Error(ERROR_PRIVILEGE_NOT_HELD);
        if (request.name == "system.power.sleep") return SetSuspendState(FALSE, FALSE, FALSE) ? Result{true, {}, 0} : Error(GetLastError());
        if (request.name == "system.power.shutdown" || request.name == "system.power.restart")
        {
            const UINT flags = request.name == "system.power.shutdown" ? EWX_POWEROFF : EWX_REBOOT;
            // Success acknowledges OS acceptance; completion cannot be observed
            // after this process exits. Never force other applications to close.
            return ExitWindowsEx(flags, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED) ? Result{true, {}, 0} : Error(GetLastError());
        }
        return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
    }
    void Release(std::string_view) override {}
};
}
std::shared_ptr<Backend> CreatePowerBackend() { return std::make_shared<Power>(); }
}
